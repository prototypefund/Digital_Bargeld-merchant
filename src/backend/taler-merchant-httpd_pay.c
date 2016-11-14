/*
  This file is part of TALER
  (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

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
 * The instance which is working this request
 */
struct MerchantInstance *mi;


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
  uint64_t transaction_id;

  /**
   * Maximum fee the merchant is willing to pay, from @e root.
   * Note that IF the total fee of the exchange is higher, that is
   * acceptable to the merchant if the customer is willing to
   * pay the difference (i.e. amount - max_fee <= actual-amount - actual-fee).
   */
  struct TALER_Amount max_fee;

  /**
   * Amount from @e root.  This is the amount the merchant expects
   * to make, minus @e max_fee.
   */
  struct TALER_Amount amount;

  /**
   * Timestamp from @e root.
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * Refund deadline from @e root.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * Deadline for the customer to pay for this contract.
   */
  struct GNUNET_TIME_Absolute pay_deadline;

  /**
   * "H_contract" from @e root.
   */
  struct GNUNET_HashCode h_contract;

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
  int transaction_exits;

  /**
   * Instance of the payment's instance (in JSON format)
   */
  struct MerchantInstance *mi;

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
 * Convert denomination key to its base32 representation
 *
 * @param dk denomination key to convert
 * @return 0-terminated base32 encoding of @a dk, to be deallocated
 */
static char *
denomination_to_string_alloc (struct TALER_DenominationPublicKey *dk)
{
  char *buf;
  char *buf2;
  size_t buf_size;

  buf_size = GNUNET_CRYPTO_rsa_public_key_encode (dk->rsa_public_key,
                                                  &buf);
  buf2 = GNUNET_STRINGS_data_to_string_alloc (buf,
                                              buf_size);
  GNUNET_free (buf);
  return buf2;
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
  struct GNUNET_CRYPTO_EddsaSignature sig;
  struct PaymentResponsePS mr;

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
  if (GNUNET_OK !=
      db->store_deposit (db->cls,
			 pc->transaction_id,
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


  mr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
  mr.purpose.size = htonl (sizeof (mr));
  mr.h_contract = pc->h_contract;

  GNUNET_CRYPTO_eddsa_sign (&mi->privkey.eddsa_priv,
                            &mr.purpose,
			    &sig);
  resume_pay_with_response (pc,
                            MHD_HTTP_OK,
                            TMH_RESPONSE_make_json_pack ("{s:s, s:o}",
                                                         "merchant_sig",
							 json_string_value (GNUNET_JSON_from_data_auto (&sig)),
                                                         "h_contract",
                                                         GNUNET_JSON_from_data (&pc->h_contract,
                                                                                sizeof (struct GNUNET_HashCode))));
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
  GNUNET_free (pc);
}


/**
 * Function called with the result of our exchange lookup.
 *
 * @param cls the `struct PayContext`
 * @param mh NULL if exchange was not found to be acceptable
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_pay_with_exchange (void *cls,
                           struct TALER_EXCHANGE_Handle *mh,
                           int exchange_trusted)
{
  struct PayContext *pc = cls;
  struct TALER_Amount acc_fee;
  struct TALER_Amount acc_amount;
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
      char *denom_enc;

      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:o, s:o}",
                                                             "error", "denomination not found",
							     "code", TALER_EC_PAY_DENOMINATION_KEY_NOT_FOUND,
                                                             "denom_pub", GNUNET_JSON_from_rsa_public_key (dc->denom.rsa_public_key),
                                                             "exchange_keys", TALER_EXCHANGE_get_keys_raw (mh)));
      denom_enc = denomination_to_string_alloc (&dc->denom);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "unknown denom to exchange: %s\n",
                  denom_enc);
      GNUNET_free (denom_enc);
      return;
    }
    if (GNUNET_OK !=
        TMH_AUDITORS_check_dk (mh,
                               denom_details,
                               exchange_trusted))
    {
      char *denom_enc;

      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:o}",
                                                             "error", "invalid denomination",
							     "code", (json_int_t) TALER_EC_PAY_DENOMINATION_KEY_AUDITOR_FAILURE,
                                                             "denom_pub", GNUNET_JSON_from_rsa_public_key (dc->denom.rsa_public_key)));
      denom_enc = denomination_to_string_alloc (&dc->denom);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Client offered invalid denomination: %s\n",
                  denom_enc);
      GNUNET_free (denom_enc);
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
    /* fees are acceptable, we cover them all; let's check the amount */
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
                                     &pc->h_contract,
                                     &dc->coin_pub,
                                     &dc->ub_sig,
                                     &dc->denom,
                                     pc->timestamp,
                                     pc->transaction_id,
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
 * @param cls closure
 */
static void
handle_pay_timeout (void *cls)
{
  struct PayContext *pc = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /pay with error after timeout\n");

  pc->timeout_task = NULL;

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
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
check_coin_paid (void *cls,
                 uint64_t transaction_id,
                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                 const struct TALER_Amount *amount_with_fee,
                 const struct TALER_Amount *deposit_fee,
                 const json_t *exchange_proof)
{
  struct PayContext *pc = cls;
  unsigned int i;

  if (pc->transaction_id != transaction_id)
  {
    GNUNET_break (0);
    return;
  }
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct DepositConfirmation *dc = &pc->dc[i];

    if ( (0 != memcmp (coin_pub,
                       &dc->coin_pub,
                       sizeof (struct TALER_CoinSpendPublicKeyP))) ||
         (0 != TALER_amount_cmp (amount_with_fee,
                                 &dc->amount_with_fee)) )
      continue;
    dc->found_in_db = GNUNET_YES;
    pc->pending--;
  }
}


/**
 * Check if the existing transaction matches our transaction.
 * Update `transaction_exits` accordingly.
 *
 * @param cls closure with the `struct PayContext`
 * @param transaction_id of the contract
 * @param merchant_pub merchant's public key
 * @param exchange_uri URI of the exchange
 * @param h_contract hash of the contract
 * @param h_xwire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
static void
check_transaction_exists (void *cls,
			  uint64_t transaction_id,
			  const struct TALER_MerchantPublicKeyP *merchant_pub,
			  const char *exchange_uri,
			  const struct GNUNET_HashCode *h_contract,
			  const struct GNUNET_HashCode *h_xwire,
			  struct GNUNET_TIME_Absolute timestamp,
			  struct GNUNET_TIME_Absolute refund,
			  const struct TALER_Amount *total_amount)
{
  struct PayContext *pc = cls;

  if ( (0 == memcmp (h_contract,
		     &pc->h_contract,
		     sizeof (struct GNUNET_HashCode))) &&
       (0 == memcmp (h_xwire,
		     &pc->mi->h_wire,
		     sizeof (struct GNUNET_HashCode))) &&
       (timestamp.abs_value_us == pc->timestamp.abs_value_us) &&
       (refund.abs_value_us == pc->refund_deadline.abs_value_us) &&
       (0 == TALER_amount_cmp (total_amount,
			       &pc->amount) ) )
  {
    pc->transaction_exits = GNUNET_YES;
  }
  else
  {
    GNUNET_break_op (0);
    pc->transaction_exits = GNUNET_SYSERR;
  }
}

extern struct MerchantInstance *
get_instance (struct json_t *json);


/**
 * Accomplish this payment.
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
  struct GNUNET_TIME_Absolute now;

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
    if (NULL != pc->response)
    {
      MHD_destroy_response (pc->response);
      pc->response = NULL;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"Queueing response (%u) for /pay (%s).\n",
		(unsigned int) pc->response_code,
		res ? "OK" : "FAILED");
    return res;
  }
  if (NULL != pc->chosen_exchange)
  {
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
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES; /* the POST's body has to be further fetched */

  mi = get_instance (root);

  /* Got the JSON upload, parse it */
  {
    json_t *coins;
    json_t *coin;
    unsigned int coins_index;
    struct TALER_MerchantSignatureP merchant_sig;
    struct TALER_ContractPS cp;
    const char *chosen_exchange;
    struct GNUNET_JSON_Specification spec[] = {
      TALER_JSON_spec_amount ("amount", &pc->amount),
      GNUNET_JSON_spec_json ("coins", &coins),
      GNUNET_JSON_spec_fixed_auto ("H_contract", &pc->h_contract),
      TALER_JSON_spec_amount ("max_fee", &pc->max_fee),
      GNUNET_JSON_spec_fixed_auto ("merchant_sig", &merchant_sig),
      GNUNET_JSON_spec_string ("exchange", &chosen_exchange),
      GNUNET_JSON_spec_absolute_time ("refund_deadline", &pc->refund_deadline),
      GNUNET_JSON_spec_absolute_time ("pay_deadline", &pc->pay_deadline),
      GNUNET_JSON_spec_absolute_time ("timestamp", &pc->timestamp),
      GNUNET_JSON_spec_uint64 ("transaction_id", &pc->transaction_id),
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_json_data (connection,
			       root,
			       spec);
    if (GNUNET_YES != res)
    {
      json_decref (root);
      GNUNET_break (0);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
    pc->mi = get_instance (root);
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		"/pay: picked instance %s\n",
		pc->mi->id);

    if (NULL == pc->mi)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  "Not able to find the specified instance\n");
      json_decref (root);
      return TMH_RESPONSE_reply_not_found (connection,
					   TALER_EC_PAY_INSTANCE_UNKNOWN,
					   "Unknown instance given");
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"The instance for this deposit is '%s', whose bank details are '%s'\n",
		pc->mi->id,
		json_dumps (pc->mi->j_wire,
			    JSON_COMPACT));
    pc->chosen_exchange = GNUNET_strdup (chosen_exchange);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"Parsed JSON for /pay.\n");
    cp.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
    cp.purpose.size = htonl (sizeof (struct TALER_ContractPS));
    cp.transaction_id = GNUNET_htonll (pc->transaction_id);
    TALER_amount_hton (&cp.total_amount,
		       &pc->amount);
    TALER_amount_hton (&cp.max_fee,
		       &pc->max_fee);
    cp.h_contract = pc->h_contract;
    cp.merchant_pub = pc->mi->pubkey;
    if (GNUNET_OK !=
	GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_CONTRACT,
				    &cp.purpose,
				    &merchant_sig.eddsa_sig,
				    &pc->mi->pubkey.eddsa_pub))
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      json_decref (root);
      return TMH_RESPONSE_reply_external_error (connection,
						TALER_EC_PAY_MERCHANT_SIGNATURE_INVALID,
						"invalid merchant signature supplied");
    }

    /* 'wire_transfer_deadline' is optional, if it is not present,
       generate it here; it will be timestamp plus the
       wire_transfer_delay supplied in config file */
    if (NULL == json_object_get (root,
				 "wire_transfer_deadline"))
    {
      pc->wire_transfer_deadline
	= GNUNET_TIME_absolute_add (pc->timestamp,
				    wire_transfer_delay);
      if (pc->wire_transfer_deadline.abs_value_us < pc->refund_deadline.abs_value_us)
      {
	/* Refund value very large, delay wire transfer accordingly */
	pc->wire_transfer_deadline = pc->refund_deadline;
      }
    }
    else
    {
      struct GNUNET_JSON_Specification espec[] = {
	GNUNET_JSON_spec_absolute_time ("wire_transfer_deadline",
					&pc->wire_transfer_deadline),
	GNUNET_JSON_spec_end()
      };

      res = TMH_PARSE_json_data (connection,
				 root,
				 espec);
      if (GNUNET_YES != res)
      {
	GNUNET_JSON_parse_free (spec);
	json_decref (root);
	GNUNET_break (0);
	return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }
      if (pc->wire_transfer_deadline.abs_value_us < pc->refund_deadline.abs_value_us)
      {
	GNUNET_break (0);
	GNUNET_JSON_parse_free (spec);
	json_decref (root);
	return TMH_RESPONSE_reply_external_error (connection,
						  TALER_EC_PAY_REFUND_DEADLINE_PAST_WIRE_TRANSFER_DEADLINE,
						  "refund deadline after wire transfer deadline");
      }
    }


    pc->coins_cnt = json_array_size (coins);
    if (0 == pc->coins_cnt)
    {
      GNUNET_JSON_parse_free (spec);
      json_decref (root);
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
	json_decref (root);
	GNUNET_break (0);
	return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }

      {
	char *s;

	s = TALER_amount_to_string (&dc->amount_with_fee);
	GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		    "Coin #%i has f %s\n",
		    coins_index,
		    s);
	GNUNET_free (s);
     }

      dc->index = coins_index;
      dc->pc = pc;
    }
    GNUNET_JSON_parse_free (spec);
  } /* end of parsing of JSON upload */
  pc->pending = pc->coins_cnt;

  /* Check if this payment attempt has already succeeded */
  if (GNUNET_SYSERR ==
      db->find_payments (db->cls,
		         pc->transaction_id,
                         &pc->mi->pubkey,
		         &check_coin_paid,
		         pc))
  {
    GNUNET_break (0);
    json_decref (root);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
					      "Merchant database error");
  }
  if (0 == pc->pending)
  {
    struct MHD_Response *resp;
    int ret;

    /* Payment succeeded in the past; take short cut
       and accept immediately */
    resp = MHD_create_response_from_buffer (0,
					    NULL,
					    MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response (connection,
			      MHD_HTTP_OK,
			      resp);
    MHD_destroy_response (resp);
    json_decref (root);
    return ret;
  }
  /* Check if transaction is already known, if not store it. */
  if (GNUNET_SYSERR ==
      db->find_transaction (db->cls,
			    pc->transaction_id,
			    &pc->mi->pubkey,
			    &check_transaction_exists,
                            pc))
  {
    GNUNET_break (0);
    json_decref (root);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                               "Merchant database error");
  }
  if (GNUNET_SYSERR == pc->transaction_exits)
  {
    GNUNET_break (0);
    json_decref (root);
    return TMH_RESPONSE_reply_external_error (connection,
                                              TALER_EC_PAY_DB_TRANSACTION_ID_CONFLICT,
					      "Transaction ID reused with different transaction details");
  }
  if (GNUNET_NO == pc->transaction_exits)
  {
    /* #4521 goes here: Check if the customer respects pay_deadline */
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

    if (GNUNET_OK !=
        db->store_transaction (db->cls,
                               pc->transaction_id,
                               &pc->mi->pubkey,
                               pc->chosen_exchange,
                               &pc->h_contract,
                               &pc->mi->h_wire,
                               pc->timestamp,
                               pc->refund_deadline,
                               &pc->amount))
    {
      GNUNET_break (0);
      json_decref (root);
      return TMH_RESPONSE_reply_internal_error (connection,
						TALER_EC_PAY_DB_STORE_TRANSACTION_ERROR,
						"Merchant database error");
    }
  }

  MHD_suspend_connection (connection);

  /* Find the responsible exchange, this may take a while... */
  pc->fo = TMH_EXCHANGES_find_exchange (pc->chosen_exchange,
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
  json_decref (root);
  return MHD_YES;
}

/* end of taler-merchant-httpd_pay.c */
