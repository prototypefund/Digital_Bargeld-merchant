/*
  This file is part of TALER
  (C) 2014-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation; either version 3,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not,
  see <http://www.gnu.org/licenses/>
*/

/**
 * @file backend/taler-merchant-httpd_pay.c
 * @brief handling of /pay requests
 * @author Marcello Stanisci
 * @author Christian Grothoff
 * @author Florian Dold
 */
#include "platform.h"
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_exchange_service.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_refund.h"


/**
 * How long to wait before giving up processing with the exchange?
 */
#define PAY_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, \
                                                    30))

/**
 * How often do we retry the (complex!) database transaction?
 */
#define MAX_RETRIES 5

/**
 * Information we keep for an individual call to the /pay handler.
 */
struct PayContext;

/**
 * Information kept during a /pay request for each coin.
 */
struct DepositConfirmation
{

  /**
   * Reference to the main PayContext
   */
  struct PayContext *pc;

  /**
   * Handle to the deposit operation we are performing for
   * this coin, NULL after the operation is done.
   */
  struct TALER_EXCHANGE_DepositHandle *dh;

  /**
   * URL of the exchange that issued this coin.
   */
  char *exchange_url;

  /**
   * Denomination of this coin.
   */
  struct TALER_DenominationPublicKey denom;

  /**
   * Amount this coin contributes to the total purchase price.
   * This amount includes the deposit fee.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Fee charged by the exchange for the deposit operation of this coin.
   */
  struct TALER_Amount deposit_fee;

  /**
   * Fee charged by the exchange for the refund operation of this coin.
   */
  struct TALER_Amount refund_fee;

  /**
   * Wire fee charged by the exchange of this coin.
   */
  struct TALER_Amount wire_fee;

  /**
   * Public key of the coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Signature using the @e denom key over the @e coin_pub.
   */
  struct TALER_DenominationSignature ub_sig;

  /**
   * Signature of the coin's private key over the contract.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Offset of this coin into the `dc` array of all coins in the
   * @e pc.
   */
  unsigned int index;

  /**
   * #GNUNET_YES if we found this coin in the database.
   */
  int found_in_db;

  /**
   * #GNUNET_YES if this coin was refunded.
   */
  int refunded;

};


/**
 * Information we keep for an individual call to the /pay handler.
 */
struct PayContext
{

  /**
   * This field MUST be first for handle_mhd_completion_callback() to work
   * when it treats this struct as a `struct TM_HandlerContext`.
   */
  struct TM_HandlerContext hc;

  /**
   * Stored in a DLL.
   */
  struct PayContext *next;

  /**
   * Stored in a DLL.
   */
  struct PayContext *prev;

  /**
   * Array with @e coins_cnt coins we are despositing.
   */
  struct DepositConfirmation *dc;

  /**
   * MHD connection to return to
   */
  struct MHD_Connection *connection;

  /**
   * Instance of the payment's instance (in JSON format)
   */
  struct MerchantInstance *mi;

  /**
   * What wire method (of the @e mi) was selected by the wallet?
   * Set in #parse_pay().
   */
  struct WireMethod *wm;

  /**
   * Proposal data for the proposal that is being
   * paid for in this context.
   */
  json_t *contract_terms;

  /**
   * Task called when the (suspended) processing for
   * the /pay request times out.
   * Happens when we don't get a response from the exchange.
   */
  struct GNUNET_SCHEDULER_Task *timeout_task;

  /**
   * Response to return, NULL if we don't have one yet.
   */
  struct MHD_Response *response;

  /**
   * Handle to the exchange that we are doing the payment with.
   * (initially NULL while @e fo is trying to find a exchange).
   */
  struct TALER_EXCHANGE_Handle *mh;

  /**
   * Handle for operation to lookup /keys (and auditors) from
   * the exchange used for this transaction; NULL if no operation is
   * pending.
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * URL of the exchange used for the last @e fo.
   */
  const char *current_exchange;

  /**
   * Placeholder for #TALER_MHD_parse_post_json() to keep its internal state.
   */
  void *json_parse_context;

  /**
   * Optional session id given in @e root.
   * NULL if not given.
   */
  char *session_id;

  /**
   * Transaction ID given in @e root.
   */
  char *order_id;

  /**
   * Fulfillment URL from @e contract_terms.
   */
  char *fulfillment_url;

  /**
   * Hashed proposal.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * "h_wire" from @e contract_terms.  Used to identify
   * the instance's wire transfer method.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Maximum fee the merchant is willing to pay, from @e root.
   * Note that IF the total fee of the exchange is higher, that is
   * acceptable to the merchant if the customer is willing to
   * pay the difference
   * (i.e. amount - max_fee <= actual-amount - actual-fee).
   */
  struct TALER_Amount max_fee;

  /**
   * Maximum wire fee the merchant is willing to pay, from @e root.
   * Note that IF the total fee of the exchange is higher, that is
   * acceptable to the merchant if the customer is willing to
   * pay the amorized difference.  Wire fees are charged over an
   * aggregate of several translations, hence unlike the deposit
   * fees, they are amortized over several customer's transactions.
   * The contract specifies under @e wire_fee_amortization how many
   * customer's transactions he expects the wire fees to be amortized
   * over on average.  Thus, if the wire fees are larger than
   * @e max_wire_fee, each customer is expected to contribute
   * $\frac{actual-wire-fee - max_wire_fee}{wire_fee_amortization}$.
   * The customer's contribution may be further reduced by the
   * difference between @e max_fee and the sum of the deposit fees.
   *
   * Default is that the merchant is unwilling to pay any wire fees.
   */
  struct TALER_Amount max_wire_fee;

  /**
   * Amount from @e root.  This is the amount the merchant expects
   * to make, minus @e max_fee.
   */
  struct TALER_Amount amount;

  /**
   * Considering all the coins with the "found_in_db" flag
   * set, what is the total amount we were so far paid on
   * this contract?
   */
  struct TALER_Amount total_paid;

  /**
   * Considering all the coins with the "found_in_db" flag
   * set, what is the total amount we had to pay in deposit
   * fees so far on this contract?
   */
  struct TALER_Amount total_fees_paid;

  /**
   * Considering all the coins with the "found_in_db" flag
   * set, what is the total amount we already refunded?
   */
  struct TALER_Amount total_refunded;

  /**
   * Wire transfer deadline. How soon would the merchant like the
   * wire transfer to be executed?
   */
  struct GNUNET_TIME_Absolute wire_transfer_deadline;

  /**
   * Timestamp from @e contract_terms.
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * Refund deadline from @e contract_terms.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * Deadline for the customer to pay for this proposal.
   */
  struct GNUNET_TIME_Absolute pay_deadline;

  /**
   * Number of transactions that the wire fees are expected to be
   * amortized over.  Never zero, defaults (conservateively) to 1.
   * May be higher if merchants expect many small transactions to
   * be aggregated and thus wire fees to be reasonably amortized
   * due to aggregation.
   */
  uint32_t wire_fee_amortization;

  /**
   * Number of coins this payment is made of.  Length
   * of the @e dc array.
   */
  unsigned int coins_cnt;

  /**
   * How often have we retried the 'main' transaction?
   */
  unsigned int retry_counter;

  /**
   * Number of transactions still pending.  Initially set to
   * @e coins_cnt, decremented on each transaction that
   * successfully finished.
   */
  unsigned int pending;

  /**
   * Number of transactions still pending for the currently selected
   * exchange.  Initially set to the number of coins started at the
   * exchange, decremented on each transaction that successfully
   * finished.  Once it hits zero, we pick the next exchange.
   */
  unsigned int pending_at_ce;

  /**
   * HTTP status code to use for the reply, i.e 200 for "OK".
   * Special value UINT_MAX is used to indicate hard errors
   * (no reply, return #MHD_NO).
   */
  unsigned int response_code;

  /**
   * #GNUNET_NO if the @e connection was not suspended,
   * #GNUNET_YES if the @e connection was suspended,
   * #GNUNET_SYSERR if @e connection was resumed to as
   * part of #MH_force_pc_resume during shutdown.
   */
  int suspended;

  /**
   * Which operational mode is the /pay request made in?
   */
  enum { PC_MODE_PAY, PC_MODE_ABORT_REFUND } mode;

};


/**
 * Head of active pay context DLL.
 */
static struct PayContext *pc_head;

/**
 * Tail of active pay context DLL.
 */
static struct PayContext *pc_tail;


/**
 * Abort all pending /deposit operations.
 *
 * @param pc pay context to abort
 */
static void
abort_deposit (struct PayContext *pc)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aborting pending /deposit operations\n");
  for (unsigned int i = 0; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dci = &pc->dc[i];

    if (NULL != dci->dh)
    {
      TALER_EXCHANGE_deposit_cancel (dci->dh);
      dci->dh = NULL;
    }
  }
}


/**
 * Force all pay contexts to be resumed as we are about
 * to shut down MHD.
 */
void
MH_force_pc_resume ()
{
  for (struct PayContext *pc = pc_head;
       NULL != pc;
       pc = pc->next)
  {
    abort_deposit (pc);
    if (NULL != pc->timeout_task)
    {
      GNUNET_SCHEDULER_cancel (pc->timeout_task);
      pc->timeout_task = NULL;
    }
    if (GNUNET_YES == pc->suspended)
    {
      pc->suspended = GNUNET_SYSERR;
      MHD_resume_connection (pc->connection);
    }
  }
}


/**
 * Function called to resume suspended connections.
 *
 * @param cls NULL
 * @param key key in the #payment_trigger_map
 * @param value a `struct TMH_SuspendedConnection` to resume
 * @return #GNUNET_OK (continue to iterate)
 */
static int
resume_operation (void *cls,
                  const struct GNUNET_HashCode *key,
                  void *value)
{
  struct TMH_SuspendedConnection *sc = value;

  (void) cls;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming operation suspended pending payment on key %s\n",
              GNUNET_h2s (key));
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_remove (payment_trigger_map,
                                                       key,
                                                       sc));
  GNUNET_assert (sc ==
                 GNUNET_CONTAINER_heap_remove_node (sc->hn));
  sc->hn = NULL;
  MHD_resume_connection (sc->con);
  return GNUNET_OK;
}


/**
 * Find out if we have any clients long-polling for @a order_id to be
 * confirmed at merchant @a mpub, and if so, tell them to resume.
 *
 * @param order_id the order that was paid
 * @param mpub the merchant's public key of the instance where the payment happened
 */
static void
resume_suspended_payment_checks (const char *order_id,
                                 const struct TALER_MerchantPublicKeyP *mpub)
{
  struct GNUNET_HashCode key;

  TMH_compute_pay_key (order_id,
                       mpub,
                       &key);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming operations suspended pending payment on key %s\n",
              GNUNET_h2s (&key));
  GNUNET_CONTAINER_multihashmap_get_multiple (payment_trigger_map,
                                              &key,
                                              &resume_operation,
                                              NULL);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%u operations remain suspended pending payment\n",
              GNUNET_CONTAINER_multihashmap_size (payment_trigger_map));
}


/**
 * Resume the given pay context and send the given response.
 * Stores the response in the @a pc and signals MHD to resume
 * the connection.  Also ensures MHD runs immediately.
 *
 * @param pc payment context
 * @param response_code response code to use
 * @param response response data to send back
 */
static void
resume_pay_with_response (struct PayContext *pc,
                          unsigned int response_code,
                          struct MHD_Response *response)
{
  pc->response_code = response_code;
  pc->response = response;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /pay handling as exchange interaction is done (%u)\n",
              response_code);
  if (NULL != pc->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (pc->timeout_task);
    pc->timeout_task = NULL;
  }
  GNUNET_assert (GNUNET_YES == pc->suspended);
  pc->suspended = GNUNET_NO;
  MHD_resume_connection (pc->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
}


/**
 * Resume payment processing with an error.
 *
 * @param pc operation to resume
 * @param http_status http status code to return
 * @param ec taler error code to return
 * @param msg human readable error message
 */
static void
resume_pay_with_error (struct PayContext *pc,
                       unsigned int http_status,
                       enum TALER_ErrorCode ec,
                       const char *msg)
{
  resume_pay_with_response (pc,
                            http_status,
                            TALER_MHD_make_error (ec,
                                                  msg));
}


/**
 * Generate a response that indicates payment success.
 *
 * @param pc payment context
 */
static void
generate_success_response (struct PayContext *pc)
{
  json_t *refunds;
  struct GNUNET_CRYPTO_EddsaSignature sig;

  /* Check for applicable refunds */
  {
    enum TALER_ErrorCode ec;
    const char *errmsg;

    refunds = TM_get_refund_json (pc->mi,
                                  &pc->h_contract_terms,
                                  &ec,
                                  &errmsg);
    /* We would get an EMPTY array back on success if there
       are no refunds, but not NULL. So NULL is always an error. */
    if (NULL == refunds)
    {
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             ec,
                             errmsg);
      return;
    }
  }

  /* Sign on our end (as the payment did go through, even if it may
     have been refunded already) */
  {
    struct PaymentResponsePS mr = {
      .purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK),
      .purpose.size = htonl (sizeof (mr)),
      .h_contract_terms = pc->h_contract_terms
    };

    GNUNET_CRYPTO_eddsa_sign (&pc->mi->privkey.eddsa_priv,
                              &mr,
                              &sig);
  }

  /* Build the response */
  {
    json_t *resp;

    resp = json_pack ("{s:O, s:o, s:o, s:o}",
                      "contract_terms",
                      pc->contract_terms,
                      "sig",
                      GNUNET_JSON_from_data_auto (&sig),
                      "h_contract_terms",
                      GNUNET_JSON_from_data (&pc->h_contract_terms,
                                             sizeof (struct GNUNET_HashCode)),
                      "refund_permissions",
                      refunds);
    if (NULL == resp)
    {
      GNUNET_break (0);
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             TALER_EC_JSON_ALLOCATION_FAILURE,
                             "could not build final response");
      return;
    }
    resume_pay_with_response (pc,
                              MHD_HTTP_OK,
                              TALER_MHD_make_json (resp));
    json_decref (resp);
  }
}


/**
 * Custom cleanup routine for a `struct PayContext`.
 *
 * @param hc the `struct PayContext` to clean up.
 */
static void
pay_context_cleanup (struct TM_HandlerContext *hc)
{
  struct PayContext *pc = (struct PayContext *) hc;

  if (NULL != pc->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (pc->timeout_task);
    pc->timeout_task = NULL;
  }
  TALER_MHD_parse_post_cleanup_callback (pc->json_parse_context);
  abort_deposit (pc);
  for (unsigned int i = 0; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (NULL != dc->denom.rsa_public_key)
    {
      GNUNET_CRYPTO_rsa_public_key_free (dc->denom.rsa_public_key);
      dc->denom.rsa_public_key = NULL;
    }
    if (NULL != dc->ub_sig.rsa_signature)
    {
      GNUNET_CRYPTO_rsa_signature_free (dc->ub_sig.rsa_signature);
      dc->ub_sig.rsa_signature = NULL;
    }
    GNUNET_free_non_null (dc->exchange_url);
  }
  GNUNET_free_non_null (pc->dc);
  if (NULL != pc->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (pc->fo);
    pc->fo = NULL;
  }
  if (NULL != pc->response)
  {
    MHD_destroy_response (pc->response);
    pc->response = NULL;
  }
  if (NULL != pc->contract_terms)
  {
    json_decref (pc->contract_terms);
    pc->contract_terms = NULL;
  }
  GNUNET_free_non_null (pc->order_id);
  GNUNET_free_non_null (pc->session_id);
  GNUNET_free_non_null (pc->fulfillment_url);
  GNUNET_CONTAINER_DLL_remove (pc_head,
                               pc_tail,
                               pc);
  GNUNET_free (pc);
}


/**
 * Check whether the amount paid is sufficient to cover
 * the contract.
 *
 * @param pc payment context to check
 * @return #GNUNET_OK if the payment is sufficient, #GNUNET_SYSERR if it is
 *         insufficient
 */
static int
check_payment_sufficient (struct PayContext *pc)
{
  struct TALER_Amount acc_fee;
  struct TALER_Amount acc_amount;
  struct TALER_Amount final_amount;
  struct TALER_Amount wire_fee_delta;
  struct TALER_Amount wire_fee_customer_contribution;
  struct TALER_Amount total_wire_fee;
  struct TALER_Amount total_needed;

  if (0 == pc->coins_cnt)
  {
    GNUNET_break_op (0);
    resume_pay_with_error (pc,
                           MHD_HTTP_BAD_REQUEST,
                           TALER_EC_PAY_PAYMENT_INSUFFICIENT,
                           "insufficient funds (no coins!)");
    return GNUNET_SYSERR;
  }

  acc_fee = pc->dc[0].deposit_fee;
  total_wire_fee = pc->dc[0].wire_fee;
  acc_amount = pc->dc[0].amount_with_fee;

  /**
   * This loops calculates what are the deposit fee / total
   * amount with fee / and wire fee, for all the coins.
   */
  for (unsigned int i = 1; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    GNUNET_assert (GNUNET_YES == dc->found_in_db);
    if ( (0 >
          TALER_amount_add (&acc_fee,
                            &dc->deposit_fee,
                            &acc_fee)) ||
         (0 >
          TALER_amount_add (&acc_amount,
                            &dc->amount_with_fee,
                            &acc_amount)) )
    {
      GNUNET_break (0);
      /* Overflow in these amounts? Very strange. */
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             TALER_EC_PAY_AMOUNT_OVERFLOW,
                             "Overflow adding up amounts");
    }
    if (1 ==
        TALER_amount_cmp (&dc->deposit_fee,
                          &dc->amount_with_fee))
    {
      GNUNET_break_op (0);
      resume_pay_with_error (pc,
                             MHD_HTTP_BAD_REQUEST,
                             TALER_EC_PAY_FEES_EXCEED_PAYMENT,
                             "Deposit fees exceed coin's contribution");
      return GNUNET_SYSERR;
    }

    /* If exchange differs, add wire fee */
    {
      int new_exchange = GNUNET_YES;

      for (unsigned int j = 0; j<i; j++)
        if (0 == strcasecmp (dc->exchange_url,
                             pc->dc[j].exchange_url))
        {
          new_exchange = GNUNET_NO;
          break;
        }
      if (GNUNET_YES == new_exchange)
      {
        if (GNUNET_OK !=
            TALER_amount_cmp_currency (&total_wire_fee,
                                       &dc->wire_fee))
        {
          GNUNET_break_op (0);
          resume_pay_with_error (pc,
                                 MHD_HTTP_PRECONDITION_FAILED,
                                 TALER_EC_PAY_WIRE_FEE_CURRENCY_MISMATCH,
                                 "exchange wire in different currency");
          return GNUNET_SYSERR;
        }
        if (0 >
            TALER_amount_add (&total_wire_fee,
                              &total_wire_fee,
                              &dc->wire_fee))
        {
          GNUNET_break (0);
          resume_pay_with_error (pc,
                                 MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 TALER_EC_PAY_EXCHANGE_WIRE_FEE_ADDITION_FAILED,
                                 "could not add exchange wire fee to total");
          return GNUNET_SYSERR;
        }
      }
    }
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Amount received from wallet: %s\n",
              TALER_amount2s (&acc_amount));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Deposit fee for all coins: %s\n",
              TALER_amount2s (&acc_fee));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Total wire fee: %s\n",
              TALER_amount2s (&total_wire_fee));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Max wire fee: %s\n",
              TALER_amount2s (&pc->max_wire_fee));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Deposit fee limit for merchant: %s\n",
              TALER_amount2s (&pc->max_fee));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Total refunded amount: %s\n",
              TALER_amount2s (&pc->total_refunded));

  /* Now compare exchange wire fee compared to
   * what we are willing to pay */
  if (GNUNET_YES !=
      TALER_amount_cmp_currency (&total_wire_fee,
                                 &pc->max_wire_fee))
  {
    resume_pay_with_error (pc,
                           MHD_HTTP_PRECONDITION_FAILED,
                           TALER_EC_PAY_WIRE_FEE_CURRENCY_MISMATCH,
                           "exchange wire does not match our currency");
    return GNUNET_SYSERR;
  }

  switch (TALER_amount_subtract (&wire_fee_delta,
                                 &total_wire_fee,
                                 &pc->max_wire_fee))
  {
  case TALER_AAR_RESULT_POSITIVE:
    /* Actual wire fee is indeed higher than our maximum,
       compute how much the customer is expected to cover!  */
    TALER_amount_divide (&wire_fee_customer_contribution,
                         &wire_fee_delta,
                         pc->wire_fee_amortization);
    break;
  case TALER_AAR_RESULT_ZERO:
  case TALER_AAR_INVALID_NEGATIVE_RESULT:
    /* Wire fee threshold is still above the wire fee amount.
       Customer is not going to contribute on this.  */
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (total_wire_fee.currency,
                                          &wire_fee_customer_contribution));
    break;
  default:
    GNUNET_assert (0);
  }

  /* add wire fee contribution to the total fees */
  if (0 >
      TALER_amount_add (&acc_fee,
                        &acc_fee,
                        &wire_fee_customer_contribution))
  {
    GNUNET_break (0);
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_AMOUNT_OVERFLOW,
                           "Overflow adding up amounts");
    return GNUNET_SYSERR;
  }
  if (-1 == TALER_amount_cmp (&pc->max_fee,
                              &acc_fee))
  {
    /**
     * Sum of fees of *all* the different exchanges of all the coins are
     * higher than the fixed limit that the merchant is willing to pay.  The
     * difference must be paid by the customer.
     *///
    struct TALER_Amount excess_fee;

    /* compute fee amount to be covered by customer */
    GNUNET_assert (TALER_AAR_RESULT_POSITIVE ==
                   TALER_amount_subtract (&excess_fee,
                                          &acc_fee,
                                          &pc->max_fee));
    /* add that to the total */
    if (0 >
        TALER_amount_add (&total_needed,
                          &excess_fee,
                          &pc->amount))
    {
      GNUNET_break (0);
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             TALER_EC_PAY_AMOUNT_OVERFLOW,
                             "Overflow adding up amounts");
      return GNUNET_SYSERR;
    }
  }
  else
  {
    /* Fees are fully covered by the merchant, all we require
       is that the total payment is not below the contract's amount */
    total_needed = pc->amount;
  }

  /* Do not count refunds towards the payment */
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Subtracting total refunds from paid amount: %s\n",
              TALER_amount2s (&pc->total_refunded));
  if (0 >
      TALER_amount_subtract (&final_amount,
                             &acc_amount,
                             &pc->total_refunded))
  {
    GNUNET_break (0);
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_REFUNDS_EXCEED_PAYMENTS,
                           "refunded amount exceeds total payments");
    return GNUNET_SYSERR;
  }

  if (-1 == TALER_amount_cmp (&final_amount,
                              &total_needed))
  {
    /* acc_amount < total_needed */
    if (-1 < TALER_amount_cmp (&acc_amount,
                               &total_needed))
    {
      resume_pay_with_error (pc,
                             MHD_HTTP_PAYMENT_REQUIRED,
                             TALER_EC_PAY_REFUNDED,
                             "contract not paid up due to refunds");
    }
    else if (-1 < TALER_amount_cmp (&acc_amount,
                                    &pc->amount))
    {
      GNUNET_break_op (0);
      resume_pay_with_error (pc,
                             MHD_HTTP_BAD_REQUEST,
                             TALER_EC_PAY_PAYMENT_INSUFFICIENT_DUE_TO_FEES,
                             "contract not paid up due to fees (client may have calculated them badly)");
    }
    else
    {
      GNUNET_break_op (0);
      resume_pay_with_error (pc,
                             MHD_HTTP_BAD_REQUEST,
                             TALER_EC_PAY_PAYMENT_INSUFFICIENT,
                             "payment insufficient");

    }
    return GNUNET_SYSERR;
  }


  return GNUNET_OK;
}


/**
 * Find the exchange we need to talk to for the next
 * pending deposit permission.
 *
 * @param pc payment context we are processing
 */
static void
find_next_exchange (struct PayContext *pc);


/**
 * Begin of the DB transaction.  If required (from
 * soft/serialization errors), the transaction can be
 * restarted here.
 *
 * @param pc payment context to transact
 */
static void
begin_transaction (struct PayContext *pc);


/**
 * Callback to handle a deposit permission's response.
 *
 * @param cls a `struct DepositConfirmation` (i.e. a pointer
 *   into the global array of confirmations and an index for this call
 *   in that array). That way, the last executed callback can detect
 *   that no other confirmations are on the way, and can pack a response
 *   for the wallet
 * @param hr HTTP response code details
 * @param exchange_sig signature from the exchange over the deposit confirmation
 * @param sign_key which key did the exchange use to sign the @a proof
 */
static void
deposit_cb (void *cls,
            const struct TALER_EXCHANGE_HttpResponse *hr,
            const struct TALER_ExchangeSignatureP *exchange_sig,
            const struct TALER_ExchangePublicKeyP *sign_key)
{
  struct DepositConfirmation *dc = cls;
  struct PayContext *pc = dc->pc;
  enum GNUNET_DB_QueryStatus qs;

  dc->dh = NULL;
  GNUNET_assert (GNUNET_YES == pc->suspended);
  pc->pending_at_ce--;
  if (MHD_HTTP_OK != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Deposit operation failed with HTTP code %u/%d\n",
                hr->http_status,
                (int) hr->ec);
    /* Transaction failed; stop all other ongoing deposits */
    abort_deposit (pc);

    if (5 == hr->http_status / 100)
    {
      /* internal server error at exchange */
      resume_pay_with_response (pc,
                                MHD_HTTP_SERVICE_UNAVAILABLE,
                                TALER_MHD_make_json_pack (
                                  "{s:s, s:I, s:I, s:I}",
                                  "hint",
                                  "exchange had an internal server error",
                                  "code",
                                  (json_int_t) TALER_EC_PAY_EXCHANGE_FAILED,
                                  "exchange-code",
                                  (json_int_t) hr->ec,
                                  "exchange-http-status",
                                  (json_int_t) hr->http_status));
    }
    else if (NULL == hr->reply)
    {
      /* We can't do anything meaningful here, the exchange did something wrong */
      resume_pay_with_response (pc,
                                MHD_HTTP_FAILED_DEPENDENCY,
                                TALER_MHD_make_json_pack (
                                  "{s:s, s:I, s:I, s:I}",
                                  "hint",
                                  "exchange failed, response body not even in JSON",
                                  "code",
                                  (json_int_t) TALER_EC_PAY_EXCHANGE_FAILED,
                                  "exchange-code",
                                  (json_int_t) hr->ec,
                                  "exchange-http-status",
                                  (json_int_t) hr->http_status));
    }
    else
    {
      /* Forward error, adding the "coin_pub" for which the
         error was being generated */
      if (TALER_EC_DEPOSIT_INSUFFICIENT_FUNDS == hr->ec)
        resume_pay_with_response (
          pc,
          MHD_HTTP_CONFLICT,
          TALER_MHD_make_json_pack ("{s:s, s:I, s:I, s:I, s:o, s:O}",
                                    "hint",
                                    "exchange failed on deposit of a coin",
                                    "code",
                                    (json_int_t) TALER_EC_PAY_EXCHANGE_FAILED,
                                    "exchange-code",
                                    (json_int_t) hr->ec,
                                    "exchange-http-status",
                                    (json_int_t) hr->http_status,
                                    "coin_pub",
                                    GNUNET_JSON_from_data_auto (&dc->coin_pub),
                                    "exchange-reply",
                                    hr->reply));
      else
        resume_pay_with_response (
          pc,
          MHD_HTTP_FAILED_DEPENDENCY,
          TALER_MHD_make_json_pack ("{s:s, s:I, s:I, s:I, s:o, s:O}",
                                    "hint",
                                    "exchange failed on deposit of a coin",
                                    "code",
                                    (json_int_t) TALER_EC_PAY_EXCHANGE_FAILED,
                                    "exchange-code",
                                    (json_int_t) hr->ec,
                                    "exchange-http-status",
                                    (json_int_t) hr->http_status,
                                    "coin_pub",
                                    GNUNET_JSON_from_data_auto (&dc->coin_pub),
                                    "exchange-reply",
                                    hr->reply));
    }
    return;
  }
  /* store result to DB */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Storing successful payment for h_contract_terms `%s' and merchant `%s'\n",
              GNUNET_h2s (&pc->h_contract_terms),
              TALER_B2S (&pc->mi->pubkey));
  /* NOTE: not run in any transaction block, simply as a
     transaction by itself! */
  db->preflight (db->cls);
  qs = db->store_deposit (db->cls,
                          &pc->h_contract_terms,
                          &pc->mi->pubkey,
                          &dc->coin_pub,
                          dc->exchange_url,
                          &dc->amount_with_fee,
                          &dc->deposit_fee,
                          &dc->refund_fee,
                          &dc->wire_fee,
                          sign_key,
                          hr->reply);
  if (0 > qs)
  {
    /* Special report if retries insufficient */
    abort_deposit (pc);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      begin_transaction (pc);
      return;
    }
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    /* Forward error including 'proof' for the body */
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_DB_STORE_PAY_ERROR,
                           "Merchant database error");
    return;
  }
  dc->found_in_db = GNUNET_YES;
  pc->pending--;

  if (0 != pc->pending_at_ce)
    return; /* still more to do with current exchange */
  find_next_exchange (pc);
}


/**
 * Function called with the result of our exchange lookup.
 *
 * @param cls the `struct PayContext`
 * @param mh NULL if exchange was not found to be acceptable
 * @param wire_fee current applicable fee for dealing with @a mh,
 *        NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is
 *        trusted by config
 * @param ec error code, #TALER_EC_NONE on success
 * @param http_status the HTTP status we got from the exchange
 * @param error_reply the full reply from the exchange, NULL if
 *        the response was NOT in JSON or on success
 */
static void
process_pay_with_exchange (void *cls,
                           struct TALER_EXCHANGE_Handle *mh,
                           const struct TALER_Amount *wire_fee,
                           int exchange_trusted,
                           enum TALER_ErrorCode ec,
                           unsigned int http_status,
                           const json_t *error_reply)
{
  struct PayContext *pc = cls;
  const struct TALER_EXCHANGE_Keys *keys;

  pc->fo = NULL;
  GNUNET_assert (GNUNET_YES == pc->suspended);
  if (MHD_HTTP_OK != http_status)
  {
    /* The request failed somehow */
    GNUNET_break_op (0);
    resume_pay_with_response (
      pc,
      MHD_HTTP_FAILED_DEPENDENCY,
      TALER_MHD_make_json_pack (
        (NULL != error_reply)
        ? "{s:s, s:I, s:I, s:I, s:O}"
        : "{s:s, s:I, s:I, s:I}",
        "hint",
        "failed to obtain meta-data from exchange",
        "code",
        (json_int_t) TALER_EC_PAY_EXCHANGE_KEYS_FAILURE,
        "exchange-http-status",
        (json_int_t) http_status,
        "exchange-code",
        (json_int_t) ec,
        "exchange-reply",
        error_reply));
    return;
  }
  pc->mh = mh;
  keys = TALER_EXCHANGE_get_keys (mh);
  if (NULL == keys)
  {
    GNUNET_break (0); /* should not be possible if HTTP status is #MHD_HTTP_OK */
    resume_pay_with_error (pc,
                           MHD_HTTP_FAILED_DEPENDENCY,
                           TALER_EC_PAY_EXCHANGE_KEYS_FAILURE,
                           "no keys");
    return;
  }

  GNUNET_log (
    GNUNET_ERROR_TYPE_DEBUG,
    "Found transaction data for proposal `%s' of merchant `%s', initiating deposits\n",
    GNUNET_h2s (&pc->h_contract_terms),
    TALER_B2S (&pc->mi->pubkey));

  /* Initiate /deposit operation for all coins of
     the current exchange (!) */
  GNUNET_assert (0 == pc->pending_at_ce);
  for (unsigned int i = 0; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];
    const struct TALER_EXCHANGE_DenomPublicKey *denom_details;
    enum TALER_ErrorCode ec;
    unsigned int hc;

    if (GNUNET_YES == dc->found_in_db)
      continue;
    if (0 != strcmp (dc->exchange_url,
                     pc->current_exchange))
      continue;
    denom_details = TALER_EXCHANGE_get_denomination_key (keys,
                                                         &dc->denom);
    if (NULL == denom_details)
    {
      /* FIXME: #6136 applies HERE */
      struct GNUNET_HashCode h_denom;

      GNUNET_CRYPTO_rsa_public_key_hash (dc->denom.rsa_public_key,
                                         &h_denom);
      resume_pay_with_response (
        pc,
        MHD_HTTP_FAILED_DEPENDENCY,
        TALER_MHD_make_json_pack (
          "{s:s, s:I, s:o, s:o}",
          "hint", "coin's denomination not found",
          "code", TALER_EC_PAY_DENOMINATION_KEY_NOT_FOUND,
          "h_denom_pub", GNUNET_JSON_from_data_auto (&h_denom),
          "exchange_keys", TALER_EXCHANGE_get_keys_raw (mh)));
      return;
    }
    if (GNUNET_OK !=
        TMH_AUDITORS_check_dk (mh,
                               denom_details,
                               exchange_trusted,
                               &hc,
                               &ec))
    {
      resume_pay_with_response (
        pc,
        hc,
        TALER_MHD_make_json_pack ("{s:s, s:I, s:o}",
                                  "hint", "denomination not accepted",
                                  "code", (json_int_t) ec,
                                  "h_denom_pub", GNUNET_JSON_from_data_auto (
                                    &denom_details->h_key)));
      return;
    }

    dc->deposit_fee = denom_details->fee_deposit;
    dc->refund_fee = denom_details->fee_refund;
    dc->wire_fee = *wire_fee;

    GNUNET_assert (NULL != pc->wm);
    GNUNET_assert (NULL != pc->wm->j_wire);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Timing for this payment, wire_deadline: %llu, refund_deadline: %llu\n",
                (unsigned long long) pc->wire_transfer_deadline.abs_value_us,
                (unsigned long long) pc->refund_deadline.abs_value_us);
    db->preflight (db->cls);
    dc->dh = TALER_EXCHANGE_deposit (mh,
                                     &dc->amount_with_fee,
                                     pc->wire_transfer_deadline,
                                     pc->wm->j_wire,
                                     &pc->h_contract_terms,
                                     &dc->coin_pub,
                                     &dc->ub_sig,
                                     &dc->denom,
                                     pc->timestamp,
                                     &pc->mi->pubkey,
                                     pc->refund_deadline,
                                     &dc->coin_sig,
                                     &deposit_cb,
                                     dc);
    if (NULL == dc->dh)
    {
      /* Signature was invalid or some other constraint was not satisfied.  If
         the exchange was unavailable, we'd get that information in the
         callback. */
      GNUNET_break_op (0);
      resume_pay_with_response (
        pc,
        MHD_HTTP_UNAUTHORIZED,
        TALER_MHD_make_json_pack (
          "{s:s, s:I, s:i}",
          "hint", "deposit signature invalid",
          "code", (json_int_t) TALER_EC_PAY_COIN_SIGNATURE_INVALID,
          "coin_idx", i));
      return;
    }
    if (TMH_force_audit)
      TALER_EXCHANGE_deposit_force_dc (dc->dh);
    pc->pending_at_ce++;
  }
}


/**
 * Find the exchange we need to talk to for the next
 * pending deposit permission.
 *
 * @param pc payment context we are processing
 */
static void
find_next_exchange (struct PayContext *pc)
{
  for (unsigned int i = 0; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (GNUNET_YES != dc->found_in_db)
    {
      db->preflight (db->cls);
      pc->current_exchange = dc->exchange_url;
      pc->fo = TMH_EXCHANGES_find_exchange (pc->current_exchange,
                                            pc->wm->wire_method,
                                            &process_pay_with_exchange,
                                            pc);
      if (NULL == pc->fo)
      {
        GNUNET_break (0);
        resume_pay_with_error (pc,
                               MHD_HTTP_INTERNAL_SERVER_ERROR,
                               TALER_EC_PAY_EXCHANGE_LOOKUP_FAILED,
                               "Failed to lookup exchange by URL");
        return;
      }
      return;
    }
  }
  pc->current_exchange = NULL;
  db->preflight (db->cls);
  /* We are done with all the HTTP requests, go back and try
     the 'big' database transaction! (It should work now!) */
  begin_transaction (pc);
}


/**
 * Handle a timeout for the processing of the pay request.
 *
 * @param cls our `struct PayContext`
 */
static void
handle_pay_timeout (void *cls)
{
  struct PayContext *pc = cls;

  pc->timeout_task = NULL;
  GNUNET_assert (GNUNET_YES == pc->suspended);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /pay with error after timeout\n");
  if (NULL != pc->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (pc->fo);
    pc->fo = NULL;
  }
  resume_pay_with_error (pc,
                         MHD_HTTP_REQUEST_TIMEOUT,
                         TALER_EC_PAY_EXCHANGE_TIMEOUT,
                         "likely the exchange did not reply quickly enough");
}


/**
 * Function called with information about a coin that was deposited.
 *
 * @param cls closure
 * @param h_contract_terms hashed proposal data
 * @param coin_pub public key of the coin
 * @param exchange_url URL of the exchange that issued @a coin_pub
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param refund_fee fee the exchange will charge for refunding this coin
 * @param wire_fee wire fee the exchange of this coin charges
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
check_coin_paid (void *cls,
                 const struct GNUNET_HashCode *h_contract_terms,
                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                 const char *exchange_url,
                 const struct TALER_Amount *amount_with_fee,
                 const struct TALER_Amount *deposit_fee,
                 const struct TALER_Amount *refund_fee,
                 const struct TALER_Amount *wire_fee,
                 const json_t *exchange_proof)
{
  struct PayContext *pc = cls;

  if (0 != GNUNET_memcmp (&pc->h_contract_terms,
                          h_contract_terms))
  {
    GNUNET_break (0);
    return;
  }
  for (unsigned int i = 0; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (GNUNET_YES == dc->found_in_db)
      continue; /* processed earlier */

    /* Get matching coin from results*/
    if ( (0 != GNUNET_memcmp (coin_pub,
                              &dc->coin_pub)) ||
         (0 != TALER_amount_cmp (amount_with_fee,
                                 &dc->amount_with_fee)) )
      continue;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Coin (%s) already found in our DB.\n",
                TALER_b2s (coin_pub,
                           sizeof (*coin_pub)));
    if (0 >
        TALER_amount_add (&pc->total_paid,
                          &pc->total_paid,
                          amount_with_fee))
    {
      /* We accepted this coin for payment on this contract before,
         and now we can't even add the amount!? */
      GNUNET_break (0);
      continue;
    }
    if (0 >
        TALER_amount_add (&pc->total_fees_paid,
                          &pc->total_fees_paid,
                          deposit_fee))
    {
      /* We accepted this coin for payment on this contract before,
         and now we can't even add the amount!? */
      GNUNET_break (0);
      continue;
    }
    dc->deposit_fee = *deposit_fee;
    dc->refund_fee = *refund_fee;
    dc->wire_fee = *wire_fee;
    dc->amount_with_fee = *amount_with_fee;
    dc->found_in_db = GNUNET_YES;
    pc->pending--;
  }
}


/**
 * Try to parse the pay request into the given pay context.
 * Schedules an error response in the connection on failure.
 *
 * @param connection HTTP connection we are receiving payment on
 * @param root JSON upload with payment data
 * @param pc context we use to handle the payment
 * @return #GNUNET_OK on success,
 *         #GNUNET_NO on failure (response was queued with MHD)
 *         #GNUNET_SYSERR on hard error (MHD connection must be dropped)
 */
static int
parse_pay (struct MHD_Connection *connection,
           const json_t *root,
           struct PayContext *pc)
{
  json_t *coins;
  const char *order_id;
  const char *mode;
  struct TALER_MerchantPublicKeyP merchant_pub;
  int res;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("mode",
                             &mode),
    GNUNET_JSON_spec_json ("coins",
                           &coins),
    GNUNET_JSON_spec_string ("order_id",
                             &order_id),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub",
                                 &merchant_pub),
    GNUNET_JSON_spec_end ()
  };
  enum GNUNET_DB_QueryStatus qs;

  res = TALER_MHD_parse_json_data (connection,
                                   root,
                                   spec);
  if (GNUNET_YES != res)
  {
    GNUNET_break_op (0);
    return res;
  }

  if (0 != GNUNET_memcmp (&merchant_pub,
                          &pc->mi->pubkey))
  {
    GNUNET_JSON_parse_free (spec);
    TALER_LOG_INFO (
      "Unknown merchant public key included in payment (usually wrong instance chosen)\n");
    return
      (MHD_YES ==
       TALER_MHD_reply_with_error (connection,
                                   MHD_HTTP_BAD_REQUEST,
                                   TALER_EC_PAY_WRONG_INSTANCE,
                                   "merchant_pub in contract does not match this instance"))
      ? GNUNET_NO
      : GNUNET_SYSERR;
  }

  {
    const char *session_id;

    session_id = json_string_value (json_object_get (root,
                                                     "session_id"));
    if (NULL != session_id)
      pc->session_id = GNUNET_strdup (session_id);
  }
  GNUNET_assert (NULL == pc->order_id);
  pc->order_id = GNUNET_strdup (order_id);
  GNUNET_assert (NULL == pc->contract_terms);
  qs = db->find_contract_terms (db->cls,
                                &pc->contract_terms,
                                order_id,
                                &merchant_pub);
  if (0 > qs)
  {
    GNUNET_JSON_parse_free (spec);
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return
      (MHD_YES ==
       TALER_MHD_reply_with_error (connection,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR,
                                   TALER_EC_PAY_DB_FETCH_PAY_ERROR,
                                   "Failed to obtain contract terms from DB"))
      ? GNUNET_NO
      : GNUNET_SYSERR;
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_JSON_parse_free (spec);
    return
      (MHD_YES ==
       TALER_MHD_reply_with_error (connection,
                                   MHD_HTTP_NOT_FOUND,
                                   TALER_EC_PAY_PROPOSAL_NOT_FOUND,
                                   "Proposal not found"))
      ? GNUNET_NO
      : GNUNET_SYSERR;
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (pc->contract_terms,
                       &pc->h_contract_terms))
  {
    GNUNET_break (0);
    GNUNET_JSON_parse_free (spec);
    return
      (MHD_YES ==
       TALER_MHD_reply_with_error (connection,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR,
                                   TALER_EC_PAY_FAILED_COMPUTE_PROPOSAL_HASH,
                                   "Failed to hash proposal"))
      ? GNUNET_NO
      : GNUNET_SYSERR;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Handling /pay for order `%s' with contract hash `%s'\n",
              order_id,
              GNUNET_h2s (&pc->h_contract_terms));

  if (NULL == json_object_get (pc->contract_terms,
                               "merchant"))
  {
    /* invalid contract */
    GNUNET_JSON_parse_free (spec);
    return
      (MHD_YES ==
       TALER_MHD_reply_with_error (connection,
                                   MHD_HTTP_INTERNAL_SERVER_ERROR,
                                   TALER_EC_PAY_MERCHANT_FIELD_MISSING,
                                   "No merchant field in proposal"))
      ? GNUNET_NO
      : GNUNET_SYSERR;
  }
  if (0 != strcasecmp ("abort-refund",
                       mode))
    pc->mode = PC_MODE_PAY;
  else
    pc->mode = PC_MODE_ABORT_REFUND;
  {
    const char *fulfillment_url;
    struct GNUNET_JSON_Specification espec[] = {
      GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                      &pc->refund_deadline),
      GNUNET_JSON_spec_absolute_time ("pay_deadline",
                                      &pc->pay_deadline),
      GNUNET_JSON_spec_absolute_time ("wire_transfer_deadline",
                                      &pc->wire_transfer_deadline),
      GNUNET_JSON_spec_absolute_time ("timestamp",
                                      &pc->timestamp),
      TALER_JSON_spec_amount ("max_fee",
                              &pc->max_fee),
      TALER_JSON_spec_amount ("amount",
                              &pc->amount),
      GNUNET_JSON_spec_string ("fulfillment_url",
                               &fulfillment_url),
      GNUNET_JSON_spec_fixed_auto ("h_wire",
                                   &pc->h_wire),
      GNUNET_JSON_spec_end ()
    };

    res = TALER_MHD_parse_json_data (connection,
                                     pc->contract_terms,
                                     espec);
    if (GNUNET_YES != res)
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return res;
    }

    pc->fulfillment_url = GNUNET_strdup (fulfillment_url);
    if (pc->wire_transfer_deadline.abs_value_us <
        pc->refund_deadline.abs_value_us)
    {
      /* This should already have been checked when creating the
         order! */
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PAY_REFUND_DEADLINE_PAST_WIRE_TRANSFER_DEADLINE,
                                         "refund deadline after wire transfer deadline");
    }

    if (pc->pay_deadline.abs_value_us <
        GNUNET_TIME_absolute_get ().abs_value_us)
    {
      /* too late */
      GNUNET_JSON_parse_free (spec);
      return
        (MHD_YES ==
         TALER_MHD_reply_with_error (connection,
                                     MHD_HTTP_GONE,
                                     TALER_EC_PAY_OFFER_EXPIRED,
                                     "The payment deadline has past and the offer is no longer valid"))
        ? GNUNET_NO
        : GNUNET_SYSERR;
    }

  }

  /* find wire method */
  {
    struct WireMethod *wm;

    wm = pc->mi->wm_head;
    while (0 != GNUNET_memcmp (&pc->h_wire,
                               &wm->h_wire))
      wm = wm->next;
    if (NULL == wm)
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PAY_WIRE_HASH_UNKNOWN,
                                         "Did not find matching wire details");
    }
    pc->wm = wm;
  }

  /* parse optional details */
  if (NULL != json_object_get (pc->contract_terms,
                               "max_wire_fee"))
  {
    struct GNUNET_JSON_Specification espec[] = {
      TALER_JSON_spec_amount ("max_wire_fee",
                              &pc->max_wire_fee),
      GNUNET_JSON_spec_end ()
    };

    res = TALER_MHD_parse_json_data (connection,
                                     pc->contract_terms,
                                     espec);
    if (GNUNET_YES != res)
    {
      GNUNET_break_op (0); /* invalid input, fail */
      GNUNET_JSON_parse_free (spec);
      return res;
    }
  }
  else
  {
    /* default is we cover no fee */
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (pc->max_fee.currency,
                                          &pc->max_wire_fee));
  }

  if (NULL != json_object_get (pc->contract_terms,
                               "wire_fee_amortization"))
  {
    struct GNUNET_JSON_Specification espec[] = {
      GNUNET_JSON_spec_uint32 ("wire_fee_amortization",
                               &pc->wire_fee_amortization),
      GNUNET_JSON_spec_end ()
    };

    res = TALER_MHD_parse_json_data (connection,
                                     pc->contract_terms,
                                     espec);
    if ( (GNUNET_YES != res) ||
         (0 == pc->wire_fee_amortization) )
    {
      GNUNET_break_op (0); /* invalid input, use default */
      /* default is no amortization */
      pc->wire_fee_amortization = 1;
    }
  }
  else
  {
    pc->wire_fee_amortization = 1;
  }

  pc->coins_cnt = json_array_size (coins);
  if (0 == pc->coins_cnt)
  {
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PAY_COINS_ARRAY_EMPTY,
                                       "coins");
  }
  /* note: 1 coin = 1 deposit confirmation expected */
  pc->dc = GNUNET_new_array (pc->coins_cnt,
                             struct DepositConfirmation);

  /* This loop populates the array 'dc' in 'pc' */
  {
    unsigned int coins_index;
    json_t *coin;
    json_array_foreach (coins, coins_index, coin)
    {
      struct DepositConfirmation *dc = &pc->dc[coins_index];
      const char *exchange_url;
      struct GNUNET_JSON_Specification ispec[] = {
        TALER_JSON_spec_denomination_public_key ("denom_pub",
                                                 &dc->denom),
        TALER_JSON_spec_amount ("contribution",
                                &dc->amount_with_fee),
        GNUNET_JSON_spec_string ("exchange_url",
                                 &exchange_url),
        GNUNET_JSON_spec_fixed_auto ("coin_pub",
                                     &dc->coin_pub),
        TALER_JSON_spec_denomination_signature ("ub_sig",
                                                &dc->ub_sig),
        GNUNET_JSON_spec_fixed_auto ("coin_sig",
                                     &dc->coin_sig),
        GNUNET_JSON_spec_end ()
      };

      res = TALER_MHD_parse_json_data (connection,
                                       coin,
                                       ispec);
      if (GNUNET_YES != res)
      {
        GNUNET_JSON_parse_free (spec);
        GNUNET_break_op (0);
        return res;
      }
      dc->exchange_url = GNUNET_strdup (exchange_url);
      dc->index = coins_index;
      dc->pc = pc;
    }
  }
  pc->pending = pc->coins_cnt;
  GNUNET_JSON_parse_free (spec);
  return GNUNET_OK;
}


/**
 * Function called with information about a refund.
 * Check if this coin was claimed by the wallet for the
 * transaction, and if so add the refunded amount to the
 * pc's "total_refunded" amount.
 *
 * @param cls closure with a `struct PayContext`
 * @param coin_pub public coin from which the refund comes from
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explanation of the refund
 * @param refund_amount refund amount which is being taken from coin_pub
 * @param refund_fee cost of this refund operation
 */
static void
check_coin_refunded (void *cls,
                     const struct TALER_CoinSpendPublicKeyP *coin_pub,
                     uint64_t rtransaction_id,
                     const char *reason,
                     const struct TALER_Amount *refund_amount,
                     const struct TALER_Amount *refund_fee)
{
  struct PayContext *pc = cls;

  for (unsigned int i = 0; i<pc->coins_cnt; i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    /* Get matching coin from results*/
    if (0 == GNUNET_memcmp (coin_pub,
                            &dc->coin_pub))
    {
      dc->refunded = GNUNET_YES;
      GNUNET_assert (0 <=
                     TALER_amount_add (&pc->total_refunded,
                                       &pc->total_refunded,
                                       refund_amount));
    }
  }
}


/**
 * Begin of the DB transaction.  If required (from
 * soft/serialization errors), the transaction can be
 * restarted here.
 *
 * @param pc payment context to transact
 */
static void
begin_transaction (struct PayContext *pc)
{
  enum GNUNET_DB_QueryStatus qs;

  /* Avoid re-trying transactions on soft errors forever! */
  if (pc->retry_counter++ > MAX_RETRIES)
  {
    GNUNET_break (0);
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_DB_STORE_TRANSACTION_ERROR,
                           "Soft merchant database error: retry counter exceeded");
    return;
  }
  GNUNET_assert (GNUNET_YES == pc->suspended);

  /* Init. some price accumulators.  */
  GNUNET_break (GNUNET_OK ==
                TALER_amount_get_zero (pc->amount.currency,
                                       &pc->total_paid));
  GNUNET_break (GNUNET_OK ==
                TALER_amount_get_zero (pc->amount.currency,
                                       &pc->total_fees_paid));
  GNUNET_break (GNUNET_OK ==
                TALER_amount_get_zero (pc->amount.currency,
                                       &pc->total_refunded));

  /* First, try to see if we have all we need already done */
  db->preflight (db->cls);
  if (GNUNET_OK !=
      db->start (db->cls,
                 "run pay"))
  {
    GNUNET_break (0);
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                           "Merchant database error (could not begin transaction)");
    return;
  }

  /* Check if some of these coins already succeeded for _this_ contract.  */
  qs = db->find_payments (db->cls,
                          &pc->h_contract_terms,
                          &pc->mi->pubkey,
                          &check_coin_paid,
                          pc);
  if (0 > qs)
  {
    db->rollback (db->cls);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      begin_transaction (pc);
      return;
    }
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                           "Merchant database error");
    return;
  }

  /* Check if we refunded some of the coins */
  qs = db->get_refunds_from_contract_terms_hash (db->cls,
                                                 &pc->mi->pubkey,
                                                 &pc->h_contract_terms,
                                                 &check_coin_refunded,
                                                 pc);
  if (0 > qs)
  {
    db->rollback (db->cls);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      begin_transaction (pc);
      return;
    }
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    resume_pay_with_error (pc,
                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                           TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                           "Merchant database error checking for refunds");
    return;
  }

  /* All the coins known to the database have
   * been processed, now delve into specific case
   * (pay vs. abort) */

  if (PC_MODE_ABORT_REFUND == pc->mode)
  {
    json_t *terms;

    /* The wallet is going for a refund,
       (on aborted operation)! */

    /* check payment was indeed incomplete */
    qs = db->find_paid_contract_terms_from_hash (db->cls,
                                                 &terms,
                                                 &pc->h_contract_terms,
                                                 &pc->mi->pubkey);
    if (0 > qs)
    {
      db->rollback (db->cls);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      {
        begin_transaction (pc);
        return;
      }
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             TALER_EC_PAY_DB_STORE_PAY_ERROR,
                             "Merchant database error");
      return;
    }
    if (0 < qs)
    {
      /* Payment had been complete! */
      json_decref (terms);
      db->rollback (db->cls);
      resume_pay_with_error (pc,
                             MHD_HTTP_FORBIDDEN,
                             TALER_EC_PAY_ABORT_REFUND_REFUSED_PAYMENT_COMPLETE,
                             "Payment complete, refusing to abort");
      return;
    }

    /* Store refund in DB */
    qs = db->increase_refund_for_contract_NT (db->cls,
                                              &pc->h_contract_terms,
                                              &pc->mi->pubkey,
                                              &pc->total_paid,
                                              /* justification */
                                              "incomplete payment aborted");
    if (0 > qs)
    {
      db->rollback (db->cls);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      {
        begin_transaction (pc);
        return;
      }
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             TALER_EC_PAY_DB_STORE_PAY_ERROR,
                             "Merchant database error storing abort-refund");
      return;
    }
    qs = db->commit (db->cls);
    if (0 > qs)
    {
      db->rollback (db->cls);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      {
        begin_transaction (pc);
        return;
      }
      resume_pay_with_error (pc,
                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                             TALER_EC_PAY_DB_STORE_PAY_ERROR,
                             "Merchant database error: could not commit");
      return;
    }
    /* At this point, the refund got correctly committed
     * into the database.  */
    {
      json_t *refunds;

      refunds = json_array ();
      if (NULL == refunds)
      {
        GNUNET_break (0);
        resume_pay_with_error (pc,
                               MHD_HTTP_INTERNAL_SERVER_ERROR,
                               TALER_EC_JSON_ALLOCATION_FAILURE,
                               "could not create JSON array");
        return;
      }
      for (unsigned int i = 0; i<pc->coins_cnt; i++)
      {
        struct TALER_MerchantSignatureP msig;
        struct TALER_RefundRequestPS rr = {
          .purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND),
          .purpose.size = htonl (sizeof (rr)),
          .h_contract_terms = pc->h_contract_terms,
          .coin_pub = pc->dc[i].coin_pub,
          .merchant = pc->mi->pubkey,
          .rtransaction_id = GNUNET_htonll (0)
        };

        if (GNUNET_YES != pc->dc[i].found_in_db)
          continue; /* Skip coins not found in DB.  */
        TALER_amount_hton (&rr.refund_amount,
                           &pc->dc[i].amount_with_fee);
        TALER_amount_hton (&rr.refund_fee,
                           &pc->dc[i].refund_fee);

        GNUNET_CRYPTO_eddsa_sign (&pc->mi->privkey.eddsa_priv,
                                  &rr,
                                  &msig.eddsa_sig);
        /* Pack refund for i-th coin.  */
        if (0 !=
            json_array_append_new (
              refunds,
              json_pack ("{s:I, s:o, s:o s:o s:o}",
                         "rtransaction_id",
                         (json_int_t) 0,
                         "coin_pub",
                         GNUNET_JSON_from_data_auto (&rr.coin_pub),
                         "merchant_sig",
                         GNUNET_JSON_from_data_auto (&msig),
                         "refund_amount",
                         TALER_JSON_from_amount_nbo (&rr.refund_amount),
                         "refund_fee",
                         TALER_JSON_from_amount_nbo (&rr.refund_fee))))
        {
          json_decref (refunds);
          GNUNET_break (0);
          resume_pay_with_error (pc,
                                 MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 TALER_EC_JSON_ALLOCATION_FAILURE,
                                 "could not create JSON array");
          return;
        }
      }

      /* Resume and send back the response.  */
      resume_pay_with_response (
        pc,
        MHD_HTTP_OK,
        TALER_MHD_make_json_pack (
          "{s:o, s:o, s:o}",
          /* Refunds pack.  */
          "refund_permissions", refunds,
          "merchant_pub",
          GNUNET_JSON_from_data_auto (&pc->mi->pubkey),
          "h_contract_terms",
          GNUNET_JSON_from_data_auto (&pc->h_contract_terms)));
    }
    return;
  } /* End of PC_MODE_ABORT_REFUND */

  /* Default PC_MODE_PAY mode */

  /* Final termination case: all coins already known, just
     generate ultimate outcome. */
  if (0 == pc->pending)
  {
    if (GNUNET_OK != check_payment_sufficient (pc))
    {
      db->rollback (db->cls);
      return;
    }
    /* Payment succeeded, save in database */
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Contract `%s' was fully paid\n",
                GNUNET_h2s (&pc->h_contract_terms));
    qs = db->mark_proposal_paid (db->cls,
                                 &pc->h_contract_terms,
                                 &pc->mi->pubkey);
    if (qs < 0)
    {
      db->rollback (db->cls);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      {
        begin_transaction (pc);
        return;
      }
      resume_pay_with_error (
        pc,
        MHD_HTTP_INTERNAL_SERVER_ERROR,
        TALER_EC_PAY_DB_STORE_PAYMENTS_ERROR,
        "Merchant database error: could not mark proposal as 'paid'");
      return;
    }

    if ( (NULL != pc->session_id) &&
         (NULL != pc->fulfillment_url) )
    {
      qs = db->insert_session_info (db->cls,
                                    pc->session_id,
                                    pc->fulfillment_url,
                                    pc->order_id,
                                    &pc->mi->pubkey);
    }

    /* Now commit! */
    if (0 <= qs)
      qs = db->commit (db->cls);
    else
      db->rollback (db->cls);
    if (0 > qs)
    {
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      {
        begin_transaction (pc);
        return;
      }
      resume_pay_with_error (
        pc,
        MHD_HTTP_INTERNAL_SERVER_ERROR,
        TALER_EC_PAY_DB_STORE_PAYMENTS_ERROR,
        "Merchant database error: could not commit to mark proposal as 'paid'");
      return;
    }
    resume_suspended_payment_checks (pc->order_id,
                                     &pc->mi->pubkey);
    generate_success_response (pc);
    return;
  }


  /* we made no DB changes,
     so we can just rollback */
  db->rollback (db->cls);

  /* Ok, we need to first go to the network.
     Do that interaction in *tiny* transactions. */
  find_next_exchange (pc);
}


/**
 * Process a payment for a proposal.
 *
 * @param connection HTTP connection we are receiving payment on
 * @param root JSON upload with payment data
 * @param pc context we use to handle the payment
 * @return value to return to MHD (#MHD_NO to drop connection,
 *         #MHD_YES to keep handling it)
 */
static int
handler_pay_json (struct MHD_Connection *connection,
                  const json_t *root,
                  struct PayContext *pc)
{
  {
    int ret;

    ret = parse_pay (connection,
                     root,
                     pc);
    if (GNUNET_OK != ret)
      return (GNUNET_NO == ret) ? MHD_YES : MHD_NO;
  }

  /* Payment not finished, suspend while we interact with the exchange */
  MHD_suspend_connection (connection);
  pc->suspended = GNUNET_YES;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /pay handling while working with the exchange\n");
  pc->timeout_task = GNUNET_SCHEDULER_add_delayed (PAY_TIMEOUT,
                                                   &handle_pay_timeout,
                                                   pc);
  begin_transaction (pc);
  return MHD_YES;
}


/**
 * Process a payment for a proposal.  Takes data from the given MHD
 * connection.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure
 *       (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a
 *       upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_pay (struct TMH_RequestHandler *rh,
                struct MHD_Connection *connection,
                void **connection_cls,
                const char *upload_data,
                size_t *upload_data_size,
                struct MerchantInstance *mi)
{
  struct PayContext *pc;
  int res;
  json_t *root;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "In handler for /pay.\n");
  if (NULL == *connection_cls)
  {
    pc = GNUNET_new (struct PayContext);
    GNUNET_CONTAINER_DLL_insert (pc_head,
                                 pc_tail,
                                 pc);
    pc->hc.cc = &pay_context_cleanup;
    pc->connection = connection;
    *connection_cls = pc;
    pc->mi = mi;
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "/pay: picked instance %s\n",
                mi->id);
  }
  else
  {
    /* not the first call, recover state */
    pc = *connection_cls;
  }
  if (GNUNET_SYSERR == pc->suspended)
    return MHD_NO; /* during shutdown, we don't generate any more replies */
  if (0 != pc->response_code)
  {
    /* We are *done* processing the request,
       just queue the response (!) */
    if (UINT_MAX == pc->response_code)
    {
      GNUNET_break (0);
      return MHD_NO; /* hard error */
    }
    res = MHD_queue_response (connection,
                              pc->response_code,
                              pc->response);
    MHD_destroy_response (pc->response);
    pc->response = NULL;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Queueing response (%u) for /pay (%s).\n",
                (unsigned int) pc->response_code,
                res ? "OK" : "FAILED");
    return res;
  }

  res = TALER_MHD_parse_post_json (connection,
                                   &pc->json_parse_context,
                                   upload_data,
                                   upload_data_size,
                                   &root);
  if (GNUNET_SYSERR == res)
  {
    GNUNET_break (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_JSON_INVALID,
                                       "could not parse JSON");
  }
  if ( (GNUNET_NO == res) ||
       (NULL == root) )
    return MHD_YES; /* the POST's body has to be further fetched */

  res = handler_pay_json (connection,
                          root,
                          pc);
  json_decref (root);
  return res;
}


/* end of taler-merchant-httpd_pay.c */
