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


/**
 * How long to wait before giving up processing with the exchange?
 */
#define PAY_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))


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
   * Array with @e coins_cnt coins we are despositing.
   */
  struct DepositConfirmation *dc;

  /**
   * MHD connection to return to
   */
  struct MHD_Connection *connection;

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
   * Placeholder for #TMH_PARSE_post_json() to keep its internal state.
   */
  void *json_parse_context;

  /**
   * Exchange URI given in @e root.
   */
  char *chosen_exchange;

  /**
   * Transaction ID given in @e root.
   */
  const char *order_id;

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
   * Number of transactions that the wire fees are expected to be
   * amortized over.  Never zero, defaults (conservateively) to 1.
   * May be higher if merchants expect many small transactions to
   * be aggregated and thus wire fees to be reasonably amortized
   * due to aggregation.
   */
  uint32_t wire_fee_amortization;

  /**
   * Amount from @e root.  This is the amount the merchant expects
   * to make, minus @e max_fee.
   */
  struct TALER_Amount amount;

  /**
   * Timestamp from @e proposal_data.
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * Refund deadline from @e proposal_data.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * Deadline for the customer to pay for this proposal.
   */
  struct GNUNET_TIME_Absolute pay_deadline;

  /**
   * Hashed proposal.
   */
  struct GNUNET_HashCode h_proposal_data;

  /**
   * "H_wire" from @e proposal_data.  Used to identify the instance's
   * wire transfer method.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Wire transfer deadline. How soon would the merchant like the
   * wire transfer to be executed? (Can be given by the frontend
   * or be determined by our configuration via #wire_transfer_delay.)
   */
  struct GNUNET_TIME_Absolute wire_transfer_deadline;

  /**
   * Response to return, NULL if we don't have one yet.
   */
  struct MHD_Response *response;

  /**
   * Number of coins this payment is made of.  Length
   * of the @e dc array.
   */
  unsigned int coins_cnt;

  /**
   * Number of transactions still pending.  Initially set to
   * @e coins_cnt, decremented on each transaction that
   * successfully finished.
   */
  unsigned int pending;

  /**
   * HTTP status code to use for the reply, i.e 200 for "OK".
   * Special value UINT_MAX is used to indicate hard errors
   * (no reply, return #MHD_NO).
   */
  unsigned int response_code;

  /**
   * Task called when the (suspended) processing for
   * the /pay request times out.
   * Happens when we don't get a response from the exchange.
   */
  struct GNUNET_SCHEDULER_Task *timeout_task;

  /**
   * #GNUNET_NO if the transaction is not in our database,
   * #GNUNET_YES if the transaction is known to our database,
   * #GNUNET_SYSERR if the transaction ID is used for a different
   * transaction in our database.
   */
  int transaction_exists;

  /**
   * Instance of the payment's instance (in JSON format)
   */
  struct MerchantInstance *mi;

  /**
   * Proposal data for the proposal that is being
   * payed for in this context.
   */
  json_t *proposal_data;

};


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
  unsigned int i;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Aborting pending /deposit operations\n");
  for (i=0;i<pc->coins_cnt;i++)
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
struct MHD_Response *
sign_success_response (struct PayContext *pc)
{
  struct GNUNET_CRYPTO_EddsaSignature sig;
  struct PaymentResponsePS mr;

  mr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
  mr.purpose.size = htonl (sizeof (mr));
  mr.h_proposal_data = pc->h_proposal_data;

  GNUNET_CRYPTO_eddsa_sign (&pc->mi->privkey.eddsa_priv,
                            &mr.purpose,
			    &sig);

  return TMH_RESPONSE_make_json_pack ("{s:O, s:o, s:o}",
                                      "proposal_data",
                                      pc->proposal_data,
                                      "sig",
                                      GNUNET_JSON_from_data_auto (&sig),
                                      "h_proposal_data",
                                      GNUNET_JSON_from_data (&pc->h_proposal_data,
                                                             sizeof (struct GNUNET_HashCode)));
}


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
 * @param sign_key which key did the exchange use to sign the @a proof
 * @param proof the received JSON reply,
 *   should be kept as proof (and, in case of errors, be forwarded to
 *   the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
	    enum TALER_ErrorCode ec,
            const struct TALER_ExchangePublicKeyP *sign_key,
            const json_t *proof)
{
  struct DepositConfirmation *dc = cls;
  struct PayContext *pc = dc->pc;

  dc->dh = NULL;
  pc->pending--;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
		"Deposit operation failed with HTTP code %u\n",
		http_status);
    /* Transaction failed; stop all other ongoing deposits */
    abort_deposit (pc);

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
              "Storing successful payment for h_proposal_data '%s'\n",
              GNUNET_h2s (&pc->h_proposal_data));

  if (GNUNET_OK !=
      db->store_deposit (db->cls,
			 &pc->h_proposal_data,
			 &pc->mi->pubkey,
			 &dc->coin_pub,
			 &dc->amount_with_fee,
			 &dc->deposit_fee,
                         sign_key,
			 proof))
  {
    GNUNET_break (0);
    /* internal error */
    abort_deposit (pc);
    /* Forward error including 'proof' for the body */
    resume_pay_with_response (pc,
                              MHD_HTTP_INTERNAL_SERVER_ERROR,
                              TMH_RESPONSE_make_internal_error (TALER_EC_PAY_DB_STORE_PAY_ERROR,
								"Merchant database error"));
    return;
  }

  if (0 != pc->pending)
    return; /* still more to do */
  resume_pay_with_response (pc,
                            MHD_HTTP_OK,
                            sign_success_response (pc));
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
  unsigned int i;

  if (NULL != pc->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (pc->timeout_task);
    pc->timeout_task = NULL;
  }

  TMH_PARSE_post_cleanup_callback (pc->json_parse_context);
  for (i=0;i<pc->coins_cnt;i++)
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
  if (NULL != pc->chosen_exchange)
  {
    GNUNET_free (pc->chosen_exchange);
    pc->chosen_exchange = NULL;
  }
  if (NULL != pc->proposal_data)
  {
    json_decref (pc->proposal_data);
    pc->proposal_data = NULL;
  }
  GNUNET_free (pc);
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
  struct TALER_Amount acc_fee;
  struct TALER_Amount acc_amount;
  struct TALER_Amount wire_fee_delta;
  struct TALER_Amount wire_fee_customer_contribution;
  const struct TALER_EXCHANGE_Keys *keys;
  unsigned int i;

  pc->fo = NULL;
  if (NULL == mh)
  {
    /* The exchange on offer is not in the set of our (trusted)
       exchanges.  Reject the payment. */
    GNUNET_break_op (0);
    resume_pay_with_response (pc,
                              MHD_HTTP_PRECONDITION_FAILED,
                              TMH_RESPONSE_make_external_error (TALER_EC_PAY_EXCHANGE_REJECTED,
								"exchange not supported"));
    return;
  }
  pc->mh = mh;
  keys = TALER_EXCHANGE_get_keys (mh);
  if (NULL == keys)
  {
    GNUNET_break (0);
    resume_pay_with_response (pc,
                              MHD_HTTP_INTERNAL_SERVER_ERROR,
                              TMH_RESPONSE_make_internal_error (TALER_EC_PAY_EXCHANGE_KEYS_FAILURE,
								"no keys"));
    return;
  }

  /* Total up the fees and the value of the deposited coins! */
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];
    const struct TALER_EXCHANGE_DenomPublicKey *denom_details;

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
    if (0 == i)
    {
      acc_fee = denom_details->fee_deposit;
      acc_amount = dc->amount_with_fee;
    }
    else
    {
      if ( (GNUNET_OK !=
	    TALER_amount_add (&acc_fee,
			      &denom_details->fee_deposit,
			      &acc_fee)) ||
	   (GNUNET_OK !=
	    TALER_amount_add (&acc_amount,
			      &dc->amount_with_fee,
			      &acc_amount)) )
      {
	GNUNET_break_op (0);
	/* Overflow in these amounts? Very strange. */
	resume_pay_with_response (pc,
				  MHD_HTTP_BAD_REQUEST,
				  TMH_RESPONSE_make_internal_error (TALER_EC_PAY_AMOUNT_OVERFLOW,
								    "Overflow adding up amounts"));
	return;
      }
    }
    if (1 ==
	TALER_amount_cmp (&dc->deposit_fee,
                          &dc->amount_with_fee))
    {
      GNUNET_break_op (0);
      /* fee higher than residual coin value, makes no sense. */
      resume_pay_with_response (pc,
				MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:o, s:o}",
                                                             "hint", "fee higher than coin value",
							     "code", (json_int_t) TALER_EC_PAY_FEES_EXCEED_PAYMENT,
                                                             "f" /* FIXME */, TALER_JSON_from_amount (&dc->amount_with_fee),
                                                             "fee_deposit", TALER_JSON_from_amount (&denom_details->fee_deposit)));
      return;
    }
  }

  /* Now compare exchange wire fee compared to what we are willing to pay */
  if (GNUNET_YES !=
      TALER_amount_cmp_currency (wire_fee,
                                 &pc->max_wire_fee))
  {
    GNUNET_break (0);
    resume_pay_with_response (pc,
                              MHD_HTTP_INTERNAL_SERVER_ERROR,
                              TMH_RESPONSE_make_internal_error (TALER_EC_PAY_WIRE_FEE_CURRENCY_MISSMATCH,
                                                                "wire_fee"));
    return;
  }

  if (GNUNET_OK ==
      TALER_amount_subtract (&wire_fee_delta,
                             wire_fee,
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
                   TALER_amount_get_zero (wire_fee->currency,
                                          &wire_fee_customer_contribution));

  }

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
      resume_pay_with_response (pc,
                                MHD_HTTP_INTERNAL_SERVER_ERROR,
                                TMH_RESPONSE_make_internal_error (TALER_EC_PAY_AMOUNT_OVERFLOW,
								  "overflow"));
      return;
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
      resume_pay_with_response (pc,
                                MHD_HTTP_METHOD_NOT_ACCEPTABLE,
                                TMH_RESPONSE_make_external_error (TALER_EC_PAY_PAYMENT_INSUFFICIENT_DUE_TO_FEES,
								  "insufficient funds (including excessive exchange fees to be covered by customer)"));
      return;
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
      if (GNUNET_SYSERR ==
          TALER_amount_subtract (&acc_amount,
                                 &acc_amount,
                                 &wire_fee_customer_contribution))
      {
        GNUNET_break_op (0);
        resume_pay_with_response (pc,
                                  MHD_HTTP_METHOD_NOT_ACCEPTABLE,
                                  TMH_RESPONSE_make_external_error (TALER_EC_PAY_PAYMENT_INSUFFICIENT_DUE_TO_FEES,
                                                                    "insufficient funds (including excessive exchange fees to be covered by customer)"));
        return;
      }
    }

    /* fees are acceptable, merchant covers them all; let's check the amount */
    if (-1 == TALER_amount_cmp (&acc_amount,
                                &pc->amount))
    {
      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_METHOD_NOT_ACCEPTABLE,
                                TMH_RESPONSE_make_external_error (TALER_EC_PAY_PAYMENT_INSUFFICIENT,
								  "insufficient funds"));
      return;
    }
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Exchange and fee structure OK. Initiating deposit operation for coins\n");

  /* Initiate /deposit operation for all coins */
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if (GNUNET_YES == dc->found_in_db)
      continue;
    GNUNET_assert (NULL != pc->mi->j_wire);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Timing for this payment, wire_deadline: %llu, refund_deadline: %llu\n",
                (unsigned long long) pc->wire_transfer_deadline.abs_value_us,
                (unsigned long long) pc->refund_deadline.abs_value_us);
    dc->dh = TALER_EXCHANGE_deposit (mh,
                                     &dc->amount_with_fee,
                                     pc->wire_transfer_deadline,
                                     pc->mi->j_wire,
                                     &pc->h_proposal_data,
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
  }
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
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /pay with error after timeout\n");
  if (NULL != pc->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (pc->fo);
    pc->fo = NULL;
  }
  resume_pay_with_response (pc,
                            MHD_HTTP_SERVICE_UNAVAILABLE,
                            TMH_RESPONSE_make_internal_error (TALER_EC_PAY_EXCHANGE_TIMEOUT,
							      "exchange not reachable"));
}


/**
 * Function called with information about a coin that was deposited.
 *
 * @param cls closure
 * @param h_proposal_data hashed proposal data
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
check_coin_paid (void *cls,
                 const struct GNUNET_HashCode *h_proposal_data,
                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                 const struct TALER_Amount *amount_with_fee,
                 const struct TALER_Amount *deposit_fee,
                 const json_t *exchange_proof)
{
  struct PayContext *pc = cls;
  unsigned int i;

  if (0 != memcmp (&pc->h_proposal_data,
                   h_proposal_data,
                   sizeof (struct GNUNET_HashCode)))
  {
    GNUNET_break (0);
    return;
  }
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];
    /* Get matching coin from results*/
    if ( (0 != memcmp (coin_pub,
                       &dc->coin_pub,
                       sizeof (struct TALER_CoinSpendPublicKeyP))) ||
         (0 != TALER_amount_cmp (amount_with_fee,
                                 &dc->amount_with_fee)) )
      continue;

    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Coin (%s) already found in our DB.\n",
                TALER_b2s (coin_pub, sizeof (*coin_pub)));

    dc->found_in_db = GNUNET_YES;
    /**
     * What happens if a (mad) wallet sends new coins on a
     * contract that it already paid for?
     */
    pc->pending--;
  }
}


/**
 * Check if the existing transaction matches our transaction.
 * Update `transaction_exists` accordingly.
 *
 * @param cls closure with the `struct PayContext`
 * @param merchant_pub merchant's public key
 * @param exchange_uri URI of the exchange
 * @param h_proposal_data hashed proposal data
 * @param h_xwire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
static void
check_transaction_exists (void *cls,
			  const struct TALER_MerchantPublicKeyP *merchant_pub,
			  const char *exchange_uri,
			  const struct GNUNET_HashCode *h_proposal_data,
			  const struct GNUNET_HashCode *h_xwire,
			  struct GNUNET_TIME_Absolute timestamp,
			  struct GNUNET_TIME_Absolute refund,
			  const struct TALER_Amount *total_amount)
{
  struct PayContext *pc = cls;

  if ( (0 == memcmp (h_proposal_data,
		     &pc->h_proposal_data,
                     sizeof (struct GNUNET_HashCode))) &&
       (0 == memcmp (h_xwire,
		     &pc->mi->h_wire,
		     sizeof (struct GNUNET_HashCode))) &&
       (timestamp.abs_value_us == pc->timestamp.abs_value_us) &&
       (refund.abs_value_us == pc->refund_deadline.abs_value_us) &&
       (0 == TALER_amount_cmp (total_amount,
			       &pc->amount) ) )
  {
    pc->transaction_exists = GNUNET_YES;
  }
  else
  {
    GNUNET_break_op (0);
    pc->transaction_exists = GNUNET_SYSERR;
  }
}


// FIXME: declare in proper header!
extern struct MerchantInstance *
get_instance (struct json_t *json);


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
  const char *chosen_exchange;
  const char *order_id;
  struct TALER_MerchantPublicKeyP merchant_pub;
  int res;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("coins", &coins),
    GNUNET_JSON_spec_string ("exchange", &chosen_exchange),
    GNUNET_JSON_spec_string ("order_id", &order_id),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &merchant_pub),
    GNUNET_JSON_spec_end()
  };

  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  if (GNUNET_YES != res)
  {
    GNUNET_break (0);
    return res;
  }
  res = db->find_proposal_data (db->cls,
                                &pc->proposal_data,
                                order_id,
                                &merchant_pub);
  if (GNUNET_OK != res)
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

  if (GNUNET_OK !=
      TALER_JSON_hash (pc->proposal_data,
                       &pc->h_proposal_data))
  {
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

  merchant = json_object_get (pc->proposal_data,
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
  pc->mi = get_instance (merchant);
  if (NULL == pc->mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unable to find the specified instance\n");
    GNUNET_JSON_parse_free (spec);
    if (MHD_NO == TMH_RESPONSE_reply_not_found (connection,
                                                TALER_EC_PAY_INSTANCE_UNKNOWN,
                                                "Unknown instance given"))
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }
    return GNUNET_NO;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "/pay: picked instance %s with key %s\n",
              pc->mi->id,
              GNUNET_STRINGS_data_to_string_alloc (&pc->mi->pubkey,
                                                   sizeof (pc->mi->pubkey)));

  pc->chosen_exchange = GNUNET_strdup (chosen_exchange);
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
                               pc->proposal_data,
                               espec);
    if (GNUNET_YES != res)
    {
      GNUNET_JSON_parse_free (spec);
      GNUNET_break (0);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }

    pc->wire_transfer_deadline
      = GNUNET_TIME_absolute_add (pc->timestamp,
                                  wire_transfer_delay);

    if (pc->wire_transfer_deadline.abs_value_us < pc->refund_deadline.abs_value_us)
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return TMH_RESPONSE_reply_external_error (connection,
                                                TALER_EC_PAY_REFUND_DEADLINE_PAST_WIRE_TRANSFER_DEADLINE,
                                                "refund deadline after wire transfer deadline");
    }
  }

  /* NOTE: In the future, iterate over all wire hashes
     available to a given instance here! (#4939) */
  if (0 != memcmp (&pc->h_wire,
                   &pc->mi->h_wire,
                   sizeof (struct GNUNET_HashCode)))
  {
    GNUNET_break (0);
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PAY_WIRE_HASH_UNKNOWN,
                                              "Did not find matching wire details");
  }

  /* parse optional details */
  if (NULL != json_object_get (pc->proposal_data,
                               "max_wire_fee"))
  {
    struct GNUNET_JSON_Specification espec[] = {
      TALER_JSON_spec_amount ("max_wire_fee",
                              &pc->max_wire_fee),
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
                               pc->proposal_data,
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
  if (NULL != json_object_get (pc->proposal_data,
                               "wire_fee_amortization"))
  {
    struct GNUNET_JSON_Specification espec[] = {
      GNUNET_JSON_spec_uint32 ("wire_fee_amortization",
                              &pc->wire_fee_amortization),
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
                               pc->proposal_data,
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
    struct GNUNET_JSON_Specification spec[] = {
      TALER_JSON_spec_denomination_public_key ("denom_pub", &dc->denom),
      TALER_JSON_spec_amount ("f" /* FIXME */, &dc->amount_with_fee),
      GNUNET_JSON_spec_fixed_auto ("coin_pub", &dc->coin_pub),
      TALER_JSON_spec_denomination_signature ("ub_sig", &dc->ub_sig),
      GNUNET_JSON_spec_fixed_auto ("coin_sig", &dc->coin_sig),
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
                               coin,
                               spec);
    if (GNUNET_YES != res)
    {
      GNUNET_JSON_parse_free (spec);
      GNUNET_break (0);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }

    dc->index = coins_index;
    dc->pc = pc;
  }
  pc->pending = pc->coins_cnt;
  GNUNET_JSON_parse_free (spec);
  return GNUNET_OK;
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

  /* Check if this payment attempt has already succeeded */
  if (GNUNET_SYSERR ==
      db->find_payments (db->cls,
		         &pc->h_proposal_data,
                         &pc->mi->pubkey,
		         &check_coin_paid,
		         pc))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
					      "Merchant database error");
  }
  if (0 == pc->pending)
  {
    struct MHD_Response *resp;
    int ret;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Payment succeeded in the past; take short cut"
                " and accept immediately.\n");

    /* Payment succeeded in the past; take short cut
       and accept immediately */
    resp = MHD_create_response_from_buffer (0,
					    NULL,
					    MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response (connection,
			      MHD_HTTP_OK,
			      sign_success_response (pc));
    MHD_destroy_response (resp);
    return ret;
  }
  /* Check if transaction is already known, if not store it. */
  if (GNUNET_SYSERR ==
      db->find_transaction (db->cls,
			    &pc->h_proposal_data,
			    &pc->mi->pubkey,
			    &check_transaction_exists,
                            pc))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                               "Merchant database error");
  }
  if (GNUNET_SYSERR == pc->transaction_exists)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_external_error (connection,
                                              TALER_EC_PAY_DB_TRANSACTION_ID_CONFLICT,
					      "Transaction ID reused with different transaction details");
  }
  if (GNUNET_NO == pc->transaction_exists)
  {
    struct GNUNET_TIME_Absolute now;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Dealing with new transaction '%s'\n",
                GNUNET_h2s (&pc->h_proposal_data));

    now = GNUNET_TIME_absolute_get ();
    if (now.abs_value_us > pc->pay_deadline.abs_value_us)
    {
      /* Time expired, we don't accept this payment now! */
      const char *pd_str;
      pd_str = GNUNET_STRINGS_absolute_time_to_string (pc->pay_deadline);

      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Attempt to get coins for expired contract. Deadline: '%s'\n",
		  pd_str);

      return TMH_RESPONSE_reply_bad_request (connection,
					     TALER_EC_PAY_OFFER_EXPIRED,
                                             "The time to pay for this contract has expired.");
    }

    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Storing transaction '%s'\n",
                GNUNET_h2s (&pc->h_proposal_data));
    if (GNUNET_OK !=
        db->store_transaction (db->cls,
                               &pc->h_proposal_data,
                               &pc->mi->pubkey,
                               pc->chosen_exchange,
                               &pc->mi->h_wire,
                               pc->timestamp,
                               pc->refund_deadline,
                               &pc->amount))
    {
      GNUNET_break (0);
      return TMH_RESPONSE_reply_internal_error (connection,
						TALER_EC_PAY_DB_STORE_TRANSACTION_ERROR,
						"Merchant database error");
    }
  }

  MHD_suspend_connection (connection);

  /* Find the responsible exchange, this may take a while... */
  pc->fo = TMH_EXCHANGES_find_exchange (pc->chosen_exchange,
                                        pc->mi->wire_method,
                                        &process_pay_with_exchange,
                                        pc);

  /* ... so we suspend connection until the last coin has been ack'd
     or until we have encountered a hard error.  Eventually, we will
     resume the connection and send back a response using
     #resume_pay_with_response(). */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /pay handling while working with the exchange\n");
  pc->timeout_task = GNUNET_SCHEDULER_add_delayed (PAY_TIMEOUT,
                                                   &handle_pay_timeout,
                                                   pc);
  return MHD_YES;
}


/**
 * Process a payment for a proposal.
 * Takes data from the given MHD connection.
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
  if (NULL != pc->chosen_exchange)
  {
    // FIXME: explain in comment why this could happen!
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"Shouldn't be here. Old MHD version?\n");
    return MHD_YES;
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
