/*
  This file is part of TALER
  (C) 2014-2017 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
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
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_refund.h"


/**
 * How long to wait before giving up processing with the exchange?
 */
#define PAY_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))

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
   * This field MUST be first.
   * FIXME: Explain why!
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
   * payed for in this context.
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
   * Placeholder for #TMH_PARSE_post_json() to keep its internal state.
   */
  void *json_parse_context;

  /**
   * Hashed proposal.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * "H_wire" from @e contract_terms.  Used to identify the instance's
   * wire transfer method.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Maximum fee the merchant is willing to pay, from @e root.
   * Note that IF the total fee of the exchange is higher, that is
   * acceptable to the merchant if the customer is willing to
   * pay the difference (i.e. amount - max_fee <= actual-amount - actual-fee).
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
   * wire transfer to be executed? (Can be given by the frontend
   * or be determined by our configuration via #wire_transfer_delay.)
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

  /**
   * Optional session id given in @e root.
   * NULL if not given.
   */
  char *session_id;

  /**
   * Transaction ID given in @e root.
   */
  char *order_id;
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
    if (GNUNET_YES == pc->suspended)
    {
      pc->suspended = GNUNET_SYSERR;
      MHD_resume_connection (pc->connection);
    }
  }
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
 * Abort all pending /deposit operations.
 *
 * @param pc pay context to abort
 */
static void
abort_deposit (struct PayContext *pc)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aborting pending /deposit operations\n");
  for (unsigned int i=0;i<pc->coins_cnt;i++)
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
 * Generate a response that indicates payment success.
 *
 * @param pc payment context
 * @return the mhd response
 */
static struct MHD_Response *
sign_success_response (struct PayContext *pc)
{
  json_t *refunds;
  enum TALER_ErrorCode ec;
  const char *errmsg;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  struct PaymentResponsePS mr;
  json_t *resp;
  struct MHD_Response *mret;

  refunds = TM_get_refund_json (pc->mi,
				&pc->h_contract_terms,
				&ec,
				&errmsg);

  if (NULL == refunds)
    return TMH_RESPONSE_make_error (ec,
				    errmsg);

  mr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
  mr.purpose.size = htonl (sizeof (mr));
  mr.h_contract_terms = pc->h_contract_terms;
  GNUNET_CRYPTO_eddsa_sign (&pc->mi->privkey.eddsa_priv,
                            &mr.purpose,
			    &sig);
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

  if (NULL != pc->session_id)
  {
    struct GNUNET_CRYPTO_EddsaSignature session_sig;
    struct TALER_MerchantPaySessionSigPS mps;

    GNUNET_assert (NULL != pc->order_id);
    mps.purpose.size = htonl (sizeof (struct TALER_MerchantPaySessionSigPS));
    mps.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAY_SESSION);
    GNUNET_CRYPTO_hash (pc->order_id,
                        strlen (pc->order_id),
                        &mps.h_order_id);
    GNUNET_CRYPTO_hash (pc->session_id,
                        strlen (pc->session_id),
                        &mps.h_session_id);

    GNUNET_CRYPTO_eddsa_sign (&pc->mi->privkey.eddsa_priv,
                              &mps.purpose,
                              &session_sig);
    json_object_set_new (resp,
                         "session_sig",
                         GNUNET_JSON_from_data_auto (&session_sig));
  }

  mret = TMH_RESPONSE_make_json (resp);
  json_decref (resp);
  return mret;
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
			    TMH_RESPONSE_make_error (ec,
						     msg));
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
  TMH_PARSE_post_cleanup_callback (pc->json_parse_context);
  for (unsigned int i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (NULL != dc->dh)
    {
      TALER_EXCHANGE_deposit_cancel (dc->dh);
      dc->dh = NULL;
    }
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
 * @return taler error code, #TALER_EC_NONE if amount is sufficient
 */
static enum TALER_ErrorCode
check_payment_sufficient (struct PayContext *pc)
{
  struct TALER_Amount acc_fee;
  struct TALER_Amount acc_amount;
  struct TALER_Amount wire_fee_delta;
  struct TALER_Amount wire_fee_customer_contribution;
  struct TALER_Amount total_wire_fee;

  if (0 == pc->coins_cnt)
    return TALER_EC_PAY_PAYMENT_INSUFFICIENT;

  acc_fee = pc->dc[0].deposit_fee;
  total_wire_fee = pc->dc[0].wire_fee;
  acc_amount = pc->dc[0].amount_with_fee;
  for (unsigned int i=1;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    GNUNET_assert (GNUNET_YES == dc->found_in_db);
    if ( (GNUNET_OK !=
	  TALER_amount_add (&acc_fee,
			    &dc->deposit_fee,
			    &acc_fee)) ||
	 (GNUNET_OK !=
	  TALER_amount_add (&acc_amount,
			    &dc->amount_with_fee,
			    &acc_amount)) )
    {
      GNUNET_break_op (0);
      /* Overflow in these amounts? Very strange. */
      return TALER_EC_PAY_AMOUNT_OVERFLOW;
    }
    if (1 ==
	TALER_amount_cmp (&dc->deposit_fee,
                          &dc->amount_with_fee))
    {
      GNUNET_break_op (0);
      /* fee higher than residual coin value, makes no sense. */
      return TALER_EC_PAY_FEES_EXCEED_PAYMENT;
    }

    /* If exchange differs, add wire fee */
    {
      int new_exchange = GNUNET_YES;

      for (unsigned int j=0;j<i;j++)
	if (0 == strcasecmp (dc->exchange_url,
			     pc->dc[j].exchange_url))
	{
	  new_exchange = GNUNET_NO;
	  break;
	}
      if (GNUNET_YES == new_exchange)
      {
	if (GNUNET_OK !=
	    TALER_amount_add (&total_wire_fee,
			      &total_wire_fee,
			      &dc->wire_fee))
	{
	  GNUNET_break_op (0);
	  return TALER_EC_PAY_EXCHANGE_REJECTED;
	}
      }
    }
  }

  /* Now compare exchange wire fee compared to what we are willing to
     pay */
  if (GNUNET_YES !=
      TALER_amount_cmp_currency (&total_wire_fee,
                                 &pc->max_wire_fee))
  {
    GNUNET_break (0);
    return TALER_EC_PAY_WIRE_FEE_CURRENCY_MISSMATCH;
  }

  if (GNUNET_OK ==
      TALER_amount_subtract (&wire_fee_delta,
                             &total_wire_fee,
                             &pc->max_wire_fee))
  {
    /* Actual wire fee is indeed higher than our maximum, compute
       how much the customer is expected to cover! */
    TALER_amount_divide (&wire_fee_customer_contribution,
                         &wire_fee_delta,
                         pc->wire_fee_amortization);
  }
  else
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (total_wire_fee.currency,
                                          &wire_fee_customer_contribution));

  }

  /* Do not count any refunds towards the payment */
  GNUNET_assert (GNUNET_SYSERR !=
		 TALER_amount_subtract (&acc_amount,
					&acc_amount,
					&pc->total_refunded));
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Subtracting total refunds from paid amount: %s\n",
              TALER_amount_to_string (&pc->total_refunded));
  /* Now check that the customer paid enough for the full contract */
  if (-1 == TALER_amount_cmp (&pc->max_fee,
                              &acc_fee))
  {
    /* acc_fee > max_fee, customer needs to cover difference */
    struct TALER_Amount excess_fee;
    struct TALER_Amount total_needed;

    /* compute fee amount to be covered by customer */
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_subtract (&excess_fee,
                                          &acc_fee,
                                          &pc->max_fee));
    /* add that to the total */
    if (GNUNET_OK !=
        TALER_amount_add (&total_needed,
                          &excess_fee,
                          &pc->amount))
    {
      GNUNET_break (0);
      return TALER_EC_PAY_AMOUNT_OVERFLOW;
    }
    /* add wire fee contribution to the total */
    if (GNUNET_OK ==
        TALER_amount_add (&total_needed,
                          &total_needed,
                          &wire_fee_customer_contribution))

    /* check if total payment sufficies */
    if (-1 == TALER_amount_cmp (&acc_amount,
                                &total_needed))
    {
      GNUNET_break_op (0);
      return TALER_EC_PAY_PAYMENT_INSUFFICIENT_DUE_TO_FEES;
    }
  }
  else
  {
    struct TALER_Amount deposit_fee_savings;

    /* Compute how much the customer saved by not going to the
       limit on the deposit fees, as this amount is counted against
       what we expect him to cover for the wire fees */
    GNUNET_assert (GNUNET_SYSERR !=
                   TALER_amount_subtract (&deposit_fee_savings,
                                          &pc->max_fee,
                                          &acc_fee));
    /* See how much of wire fee contribution is covered by fee_savings */
    if (-1 == TALER_amount_cmp (&deposit_fee_savings,
                                &wire_fee_customer_contribution))
    {
      /* wire_fee_customer_contribution > deposit_fee_savings */
      GNUNET_assert (GNUNET_SYSERR !=
                     TALER_amount_subtract (&wire_fee_customer_contribution,
                                            &wire_fee_customer_contribution,
                                            &deposit_fee_savings));
      /* subtract remaining wire fees from total contribution */
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Subtract remaining wire fees from total contribution: %s",
                  TALER_amount_to_string (&wire_fee_customer_contribution));
      if (GNUNET_SYSERR ==
          TALER_amount_subtract (&acc_amount,
                                 &acc_amount,
                                 &wire_fee_customer_contribution))
      {
        GNUNET_break_op (0);
        return TALER_EC_PAY_PAYMENT_INSUFFICIENT_DUE_TO_FEES;
      }
    }

    /* fees are acceptable, merchant covers them all; let's check the amount */
    if (-1 == TALER_amount_cmp (&acc_amount,
                                &pc->amount))
    {
      GNUNET_break_op (0);
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "price vs. sent: %s vs. %s\n",
                  TALER_amount_to_string (&pc->amount),
                  TALER_amount_to_string (&acc_amount));
      return TALER_EC_PAY_PAYMENT_INSUFFICIENT;
    }
  }
  return TALER_EC_NONE;
}


/**
 * Generate full error response based on the @a ec
 *
 * @param pc context for which to generate the error
 * @param ec error code identifying the issue
 */
static void
generate_error_response (struct PayContext *pc,
			 enum TALER_ErrorCode ec)
{
  switch (ec)
  {
  case TALER_EC_PAY_AMOUNT_OVERFLOW:
    resume_pay_with_error (pc,
			   MHD_HTTP_BAD_REQUEST,
			   ec,
			   "Overflow adding up amounts");
    break;
  case TALER_EC_PAY_FEES_EXCEED_PAYMENT:
    resume_pay_with_error (pc,
			   MHD_HTTP_BAD_REQUEST,
			   ec,
			   "Deposit fees exceed coin's contribution");
    break;
  case TALER_EC_PAY_PAYMENT_INSUFFICIENT_DUE_TO_FEES:
    resume_pay_with_error (pc,
			   MHD_HTTP_METHOD_NOT_ACCEPTABLE,
			   ec,
			   "insufficient funds (including excessive exchange fees to be covered by customer)");
    break;
  case TALER_EC_PAY_PAYMENT_INSUFFICIENT:
    resume_pay_with_error (pc,
			   MHD_HTTP_METHOD_NOT_ACCEPTABLE,
			   ec,
			   "insufficient funds");
    break;
  case TALER_EC_PAY_WIRE_FEE_CURRENCY_MISSMATCH:
    resume_pay_with_error (pc,
			   MHD_HTTP_INTERNAL_SERVER_ERROR,
			   ec,
			   "wire_fee currency does not match");
    break;
  case TALER_EC_PAY_EXCHANGE_REJECTED:
    resume_pay_with_error (pc,
			   MHD_HTTP_PRECONDITION_FAILED,
			   ec,
			   "exchange charges incompatible wire fee");
    break;
  default:
    resume_pay_with_error (pc,
			   MHD_HTTP_INTERNAL_SERVER_ERROR,
			   ec,
			   "unexpected error code");
    GNUNET_break (0);
    break;
  }
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
 * @param http_status HTTP response code, #MHD_HTTP_OK
 *   (200) for successful deposit; 0 if the exchange's reply is bogus (fails
 *   to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param exchange_sig signature from the exchange over the deposit confirmation
 * @param sign_key which key did the exchange use to sign the @a proof
 * @param proof the received JSON reply,
 *   should be kept as proof (and, in case of errors, be forwarded to
 *   the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
	    enum TALER_ErrorCode ec,
            const struct TALER_ExchangeSignatureP *exchange_sig,
	    const struct TALER_ExchangePublicKeyP *sign_key,
            const json_t *proof)
{
  struct DepositConfirmation *dc = cls;
  struct PayContext *pc = dc->pc;
  enum GNUNET_DB_QueryStatus qs;

  dc->dh = NULL;
  GNUNET_assert (GNUNET_YES == pc->suspended);
  pc->pending_at_ce--;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
		"Deposit operation failed with HTTP code %u\n",
		http_status);
    /* Transaction failed; stop all other ongoing deposits */
    abort_deposit (pc);
    db->rollback (db->cls);

    if (NULL == proof)
    {
      /* We can't do anything meaningful here, the exchange did something wrong */
      resume_pay_with_response (pc,
                                MHD_HTTP_SERVICE_UNAVAILABLE,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:I, s:s}",
                                                             "error", "exchange failed",
							     "code", (json_int_t) TALER_EC_PAY_EXCHANGE_FAILED,
							     "exchange-code", (json_int_t) ec,
							     "exchange-http-status", (json_int_t) http_status,
                                                             "hint", "The exchange provided an unexpected response"));
    }
    else
    {
      /* Forward error, adding the "coin_pub" for which the
         error was being generated */
      json_t *eproof;

      eproof = json_copy ((json_t *) proof);
      json_object_set_new (eproof,
                           "coin_pub",
                           GNUNET_JSON_from_data_auto (&dc->coin_pub));
      resume_pay_with_response (pc,
                                http_status,
                                TMH_RESPONSE_make_json (eproof));
      json_decref (eproof);
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
			  proof);
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
 * @param wire_fee current applicable fee for dealing with @a mh, NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_pay_with_exchange (void *cls,
                           struct TALER_EXCHANGE_Handle *mh,
                           const struct TALER_Amount *wire_fee,
                           int exchange_trusted)
{
  struct PayContext *pc = cls;
  const struct TALER_EXCHANGE_Keys *keys;

  pc->fo = NULL;
  GNUNET_assert (GNUNET_YES == pc->suspended);
  if (NULL == mh)
  {
    /* The exchange on offer is not in the set of our (trusted)
       exchanges.  Reject the payment. */
    GNUNET_break_op (0);
    resume_pay_with_error (pc,
			   MHD_HTTP_PRECONDITION_FAILED,
			   TALER_EC_PAY_EXCHANGE_REJECTED,
			   "exchange not supported");
    return;
  }
  pc->mh = mh;
  keys = TALER_EXCHANGE_get_keys (mh);
  if (NULL == keys)
  {
    GNUNET_break (0);
    resume_pay_with_error (pc,
			   MHD_HTTP_INTERNAL_SERVER_ERROR,
			   TALER_EC_PAY_EXCHANGE_KEYS_FAILURE,
			   "no keys");
    return;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Found transaction data for proposal `%s' of merchant `%s', initiating deposits\n",
              GNUNET_h2s (&pc->h_contract_terms),
              TALER_B2S (&pc->mi->pubkey));

  /* Initiate /deposit operation for all coins of
     the current exchange (!) */
  GNUNET_assert (0 == pc->pending_at_ce);
  for (unsigned int i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];
    const struct TALER_EXCHANGE_DenomPublicKey *denom_details;

    if (GNUNET_YES == dc->found_in_db)
      continue;
    if (0 != strcmp (dc->exchange_url,
		     pc->current_exchange))
      continue;
    denom_details = TALER_EXCHANGE_get_denomination_key (keys,
							 &dc->denom);
    if (NULL == denom_details)
    {
      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:o, s:o}",
                                                             "error", "denomination not found",
							     "code", TALER_EC_PAY_DENOMINATION_KEY_NOT_FOUND,
                                                             "denom_pub", GNUNET_JSON_from_rsa_public_key (dc->denom.rsa_public_key),
                                                             "exchange_keys", TALER_EXCHANGE_get_keys_raw (mh)));
      return;
    }
    if (GNUNET_OK !=
        TMH_AUDITORS_check_dk (mh,
                               denom_details,
                               exchange_trusted))
    {
      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:o}",
                                                             "error", "invalid denomination",
							     "code", (json_int_t) TALER_EC_PAY_DENOMINATION_KEY_AUDITOR_FAILURE,
                                                             "denom_pub", GNUNET_JSON_from_rsa_public_key (dc->denom.rsa_public_key)));
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
      /* Signature was invalid.  If the exchange was unavailable,
       * we'd get that information in the callback. */
      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_UNAUTHORIZED,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:i}",
                                                             "hint", "Coin signature invalid.",
							     "code", (json_int_t) TALER_EC_PAY_COIN_SIGNATURE_INVALID,

                                                             "coin_idx", i));
      return;
    }
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
  for (unsigned int i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (GNUNET_YES != dc->found_in_db)
    {
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
			       TALER_EC_PAY_EXCHANGE_FAILED,
			       "Failed to lookup exchange by URL");
	return;
      }
      return;
    }
  }
  pc->current_exchange = NULL;
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
			 MHD_HTTP_SERVICE_UNAVAILABLE,
			 TALER_EC_PAY_EXCHANGE_TIMEOUT,
			 "exchange not reachable");
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

  if (0 != memcmp (&pc->h_contract_terms,
                   h_contract_terms,
                   sizeof (struct GNUNET_HashCode)))
  {
    GNUNET_break (0);
    return;
  }
  for (unsigned int i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (GNUNET_YES == dc->found_in_db)
      continue; /* processed earlier */

    /* Get matching coin from results*/
    if ( (0 != memcmp (coin_pub,
                       &dc->coin_pub,
                       sizeof (struct TALER_CoinSpendPublicKeyP))) ||
         (0 != TALER_amount_cmp (amount_with_fee,
                                 &dc->amount_with_fee)) )
      continue;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Coin (%s) already found in our DB.\n",
                TALER_b2s (coin_pub,
			   sizeof (*coin_pub)));
    if (GNUNET_OK !=
	TALER_amount_add (&pc->total_paid,
			  &pc->total_paid,
			  amount_with_fee))
    {
      /* We accepted this coin for payment on this contract before,
	 and now we can't even add the amount!? */
      GNUNET_break (0);
      continue;
    }
    if (GNUNET_OK !=
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
  json_t *coin;
  json_t *merchant;
  unsigned int coins_index;
  const char *order_id;
  const char *mode;
  struct TALER_MerchantPublicKeyP merchant_pub;
  int res;
  char *last_session_id;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("mode",
			     &mode),
    GNUNET_JSON_spec_json ("coins",
			   &coins),
    GNUNET_JSON_spec_string ("order_id",
			     &order_id),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub",
				 &merchant_pub),
    GNUNET_JSON_spec_end()
  };
  enum GNUNET_DB_QueryStatus qs;
  const char *session_id;
  struct GNUNET_TIME_Relative used_wire_transfer_delay;

  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  if (GNUNET_YES != res)
  {
    GNUNET_break (0);
    return res;
  }

  session_id = json_string_value (json_object_get (root,
                                                   "session_id"));
  if (NULL != session_id)
    pc->session_id = GNUNET_strdup (session_id);
  pc->order_id = GNUNET_strdup (order_id);
  GNUNET_assert (NULL == pc->contract_terms);
  qs = db->find_contract_terms (db->cls,
                                &pc->contract_terms,
                                &last_session_id,
                                order_id,
                                &merchant_pub);
  if (0 > qs)
  {
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_PAY_DB_FETCH_PAY_ERROR,
					      "db error to previous /pay data");

  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_JSON_parse_free (spec);
    if (MHD_YES !=
        TMH_RESPONSE_reply_not_found (connection,
                                      TALER_EC_PAY_DB_STORE_PAY_ERROR,
                                      "Proposal not found"))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
    return GNUNET_NO;
  }

  GNUNET_free (last_session_id);

  if (GNUNET_OK !=
      TALER_JSON_hash (pc->contract_terms,
                       &pc->h_contract_terms))
  {
    GNUNET_break (0);
    GNUNET_JSON_parse_free (spec);
    if (MHD_YES !=
        TMH_RESPONSE_reply_internal_error (connection,
                                           TALER_EC_PAY_FAILED_COMPUTE_PROPOSAL_HASH,
                                           "Failed to hash proposal"))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
    return GNUNET_NO;
  }

  merchant = json_object_get (pc->contract_terms,
                              "merchant");
  if (NULL == merchant)
  {
    /* invalid contract */
    GNUNET_break (0);
    GNUNET_JSON_parse_free (spec);
    if (MHD_YES !=
        TMH_RESPONSE_reply_internal_error (connection,
                                           TALER_EC_PAY_MERCHANT_FIELD_MISSING,
                                           "No merchant field in proposal"))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
    return GNUNET_NO;
  }
  if (0 != strcasecmp ("abort-refund",
		       mode))
    pc->mode = PC_MODE_PAY;
  else
    pc->mode = PC_MODE_ABORT_REFUND;
  pc->mi = TMH_lookup_instance_json (merchant);
  if (NULL == pc->mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unable to find the specified instance\n");
    GNUNET_JSON_parse_free (spec);
    if (MHD_NO ==
	TMH_RESPONSE_reply_not_found (connection,
				      TALER_EC_PAY_INSTANCE_UNKNOWN,
				      "Unknown instance given"))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
    return GNUNET_NO;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "/pay: picked instance %s\n",
              pc->mi->id);

  {
    struct GNUNET_JSON_Specification espec[] = {
      GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                      &pc->refund_deadline),
      GNUNET_JSON_spec_absolute_time ("pay_deadline",
                                      &pc->pay_deadline),
      GNUNET_JSON_spec_absolute_time ("timestamp",
                                      &pc->timestamp),
      TALER_JSON_spec_amount ("max_fee",
                              &pc->max_fee),
      TALER_JSON_spec_amount ("amount",
                              &pc->amount),
      GNUNET_JSON_spec_fixed_auto ("H_wire",
                                   &pc->h_wire),
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
                               pc->contract_terms,
                               espec);
    if (GNUNET_YES != res)
    {
      GNUNET_JSON_parse_free (spec);
      GNUNET_break (0);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }

    /* Use the value from config as default.  */
    used_wire_transfer_delay = wire_transfer_delay;

    if (NULL != json_object_get (pc->contract_terms,
                                 "wire_transfer_delay"))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Frontend specified wire transfer delay\n");

      struct GNUNET_JSON_Specification wspec[] = {
        GNUNET_JSON_spec_relative_time ("wire_transfer_delay",
                                        &used_wire_transfer_delay),
        GNUNET_JSON_spec_end()
      };

      res = TMH_PARSE_json_data (connection,
                                 pc->contract_terms,
                                 wspec);
      if (GNUNET_YES != res)
      {
        GNUNET_JSON_parse_free (spec);
        GNUNET_break (0);
        return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }
    }

    pc->wire_transfer_deadline
      = GNUNET_TIME_absolute_add (pc->timestamp,
                                  used_wire_transfer_delay);

    if (pc->wire_transfer_deadline.abs_value_us < pc->refund_deadline.abs_value_us)
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return TMH_RESPONSE_reply_external_error (connection,
                                                TALER_EC_PAY_REFUND_DEADLINE_PAST_WIRE_TRANSFER_DEADLINE,
                                                "refund deadline after wire transfer deadline");
    }
  }

  /* find wire method */
  {
    struct WireMethod *wm;

    wm = pc->mi->wm_head;
    while (0 != memcmp (&pc->h_wire,
                        &wm->h_wire,
                        sizeof (struct GNUNET_HashCode)))
      wm = wm->next;
    if (NULL == wm)
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return TMH_RESPONSE_reply_internal_error (connection,
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
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
                               pc->contract_terms,
                               espec);
    if (GNUNET_YES != res)
    {
      GNUNET_break_op (0); /* invalid input, use default */
      /* default is we cover no fee */
      GNUNET_assert (GNUNET_OK ==
                     TALER_amount_get_zero (pc->max_fee.currency,
                                            &pc->max_wire_fee));
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
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
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
    return TMH_RESPONSE_reply_arg_invalid (connection,
                                           TALER_EC_PAY_COINS_ARRAY_EMPTY,
                                           "coins");
  }
  /* note: 1 coin = 1 deposit confirmation expected */
  pc->dc = GNUNET_new_array (pc->coins_cnt,
                             struct DepositConfirmation);

  /* This loop populates the array 'dc' in 'pc' */
  json_array_foreach (coins, coins_index, coin)
  {
    struct DepositConfirmation *dc = &pc->dc[coins_index];
    const char *exchange_url;
    struct GNUNET_JSON_Specification spec[] = {
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
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
                               coin,
                               spec);
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
 * @param reason human-readable explaination of the refund
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

  for (unsigned int i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    /* Get matching coin from results*/
    if (0 != memcmp (coin_pub,
		     &dc->coin_pub,
		     sizeof (struct TALER_CoinSpendPublicKeyP)))
    {
      dc->refunded = GNUNET_YES;
      GNUNET_break (GNUNET_OK ==
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
    resume_pay_with_response (pc,
			      MHD_HTTP_INTERNAL_SERVER_ERROR,
			      TMH_RESPONSE_make_json_pack ("{s:I, s:s}",
							   "code", (json_int_t) TALER_EC_PAY_DB_STORE_TRANSACTION_ERROR,
							   "hint", "Soft merchant database error: retry counter exceeded"));
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
			   "Merchant database error (could not start transaction)");
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
			   "Merchant database error");
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
    qs = db->increase_refund_for_contract (db->cls,
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
			     "Merchant database error");
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
      for (unsigned int i=0;i<pc->coins_cnt;i++)
      {
	struct TALER_RefundRequestPS rr;
	struct TALER_MerchantSignatureP msig;
	uint64_t rtransactionid;

        /* Will only work with coins found in DB.  */
	if (GNUNET_YES != pc->dc[i].found_in_db)
	  continue;

	rtransactionid = 0;
        rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
	rr.purpose.size = htonl (sizeof (struct TALER_RefundRequestPS));
	rr.h_contract_terms = pc->h_contract_terms;
	rr.coin_pub = pc->dc[i].coin_pub;
	rr.merchant = pc->mi->pubkey;
	rr.rtransaction_id = GNUNET_htonll (rtransactionid);
	TALER_amount_hton (&rr.refund_amount,
			   &pc->dc[i].amount_with_fee);
	TALER_amount_hton (&rr.refund_fee,
			   &pc->dc[i].refund_fee);

	if (GNUNET_OK !=
	    GNUNET_CRYPTO_eddsa_sign (&pc->mi->privkey.eddsa_priv,
				      &rr.purpose,
				      &msig.eddsa_sig))
	{
	  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		      "Failed to sign successful refund confirmation\n");
	  json_decref (refunds);
	  resume_pay_with_error (pc,
				 MHD_HTTP_INTERNAL_SERVER_ERROR,
				 TALER_EC_PAY_REFUND_SIGNATURE_FAILED,
				 "Refund approved, but failed to sign confirmation");
	  return;
	}

        /* Pack refund for i-th coin.  */
	json_array_append_new (refunds,
			       json_pack ("{s:I, s:o, s:o s:o s:o}",
					  "rtransaction_id", (json_int_t) rtransactionid,
					  "coin_pub", GNUNET_JSON_from_data_auto (&rr.coin_pub),
					  "merchant_sig", GNUNET_JSON_from_data_auto (&msig),
                                          "refund_amount", TALER_JSON_from_amount_nbo (&rr.refund_amount),
                                          "refund_fee", TALER_JSON_from_amount_nbo (&rr.refund_fee)));
      }

      /* Resume and send back the response.  */
      resume_pay_with_response
        (pc,
	 MHD_HTTP_OK,
	 TMH_RESPONSE_make_json_pack
           ("{s:o, s:o, s:o}",
            /* Refunds pack.  */
	    "refund_permissions", refunds,
	    "merchant_pub",
            GNUNET_JSON_from_data_auto (&pc->mi->pubkey),
            "h_contract_terms",
            GNUNET_JSON_from_data_auto (&pc->h_contract_terms)));
    }
    return;
  }
  /* Default PC_MODE_PAY mode */

  /* Final termination case: all coins already known, just
     generate ultimate outcome. */
  if (0 == pc->pending)
  {
    enum TALER_ErrorCode ec;

    ec = check_payment_sufficient (pc);
    if (TALER_EC_NONE == ec)
    {
      /* Payment succeeded, commit! */
      qs = db->mark_proposal_paid (db->cls,
				   &pc->h_contract_terms,
				   &pc->mi->pubkey,
                                   pc->session_id);
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
	resume_pay_with_error (pc,
			       MHD_HTTP_INTERNAL_SERVER_ERROR,
			       TALER_EC_PAY_DB_STORE_PAYMENTS_ERROR,
			       "Merchant database error: could not mark proposal as 'paid'");
	return;
      }
      resume_pay_with_response (pc,
				MHD_HTTP_OK,
				sign_success_response (pc));
      return;
    }
    generate_error_response (pc,
			     ec);
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
  int ret;

  ret = parse_pay (connection,
                   root,
                   pc);
  if (GNUNET_OK != ret)
    return (GNUNET_NO == ret) ? MHD_YES : MHD_NO;
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
 * @return MHD result code
 */
int
MH_handler_pay (struct TMH_RequestHandler *rh,
		struct MHD_Connection *connection,
		void **connection_cls,
		const char *upload_data,
		size_t *upload_data_size)
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
  }
  else
  {
    /* not the first call, recover state */
    pc = *connection_cls;
  }
  if (0 != pc->response_code)
  {
    /* We are *done* processing the request, just queue the response (!) */
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

  res = TMH_PARSE_post_json (connection,
			     &pc->json_parse_context,
			     upload_data,
			     upload_data_size,
			     &root);
  if (GNUNET_SYSERR == res)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_invalid_json (connection);
  }
  if ( (GNUNET_NO == res) ||
       (NULL == root) )
    return MHD_YES; /* the POST's body has to be further fetched */

  res = handler_pay_json (connection,
                          root,
                          pc);
  json_decref (root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  return MHD_YES;
}

/* end of taler-merchant-httpd_pay.c */
