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
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
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
struct MERCHANT_DepositConfirmation
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
   * Amount "f" that this coin contributes to the overall payment.
   * This amount includes the deposit fee.
   */
  struct TALER_Amount percoin_amount;

  /**
   * Amount this coin contributes to the total purchase price.
   */
  struct TALER_Amount amount_without_fee;

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
  struct MERCHANT_DepositConfirmation *dc;

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
   * "H_contract" from @e root.
   */
  struct GNUNET_HashCode h_contract;

  /**
   * Execution date. How soon would the merchant like the
   * transaction to be executed? (Can be given by the frontend
   * or be determined by our configuration via #edate_delay.)
   */
  struct GNUNET_TIME_Absolute edate;

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
  buf_size = GNUNET_CRYPTO_rsa_public_key_encode (dk->rsa_public_key, &buf);
  buf2 = GNUNET_STRINGS_data_to_string_alloc (buf, buf_size);
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
    struct MERCHANT_DepositConfirmation *dci = &pc->dc[i];

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
 * @param cls a `struct MERCHANT_DepositConfirmation` (i.e. a pointer
 *   into the global array of confirmations and an index for this call
 *   in that array). That way, the last executed callback can detect
 *   that no other confirmations are on the way, and can pack a response
 *   for the wallet
 * @param http_status HTTP response code, #MHD_HTTP_OK
 *   (200) for successful deposit; 0 if the exchange's reply is bogus (fails
 *   to follow the protocol)
 * @param proof the received JSON reply,
 *   should be kept as proof (and, in case of errors, be forwarded to
 *   the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            const json_t *proof)
{
  struct MERCHANT_DepositConfirmation *dc = cls;
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
      /* FIXME: any useful information we can include? */
      resume_pay_with_response (pc,
                                MHD_HTTP_SERVICE_UNAVAILABLE,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:s}",
                                                             "error", "exchange failed",
                                                             "hint", "The exchange provided an unexpected response"));
    }
    else
    {
      /* Forward error, adding the "coin_pub" for which the
         error was being generated */
      json_t *eproof;

      eproof = json_copy ((json_t *) proof);
      json_object_set (eproof,
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
      db->store_payment (db->cls,
			 &pc->h_contract,
			 &h_wire,
			 pc->transaction_id,
			 pc->timestamp,
			 pc->refund_deadline,
			 &dc->amount_without_fee,
			 &dc->coin_pub,
			 proof))
  {
    GNUNET_break (0);
    /* internal error */
    abort_deposit (pc);
    /* Forward error including 'proof' for the body */
    resume_pay_with_response (pc,
                              MHD_HTTP_INTERNAL_SERVER_ERROR,
                              TMH_RESPONSE_make_internal_error ("Merchant database error"));
    return;
  }
  if (0 != pc->pending)
    return; /* still more to do */
  resume_pay_with_response (pc,
                            MHD_HTTP_OK,
                            MHD_create_response_from_buffer (0,
                                                             NULL,
                                                             MHD_RESPMEM_PERSISTENT));
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
    struct MERCHANT_DepositConfirmation *dc = &pc->dc[i];

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
                              TMH_RESPONSE_make_external_error ("exchange not supported"));
    return;
  }
  pc->mh = mh;

  keys = TALER_EXCHANGE_get_keys (mh);
  if (NULL == keys)
  {
    GNUNET_break (0);
    resume_pay_with_response (pc,
                              MHD_HTTP_INTERNAL_SERVER_ERROR,
                              TMH_RESPONSE_make_internal_error ("no keys"));
    return;
  }

  /* Total up the fees and the value of the deposited coins! */
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct MERCHANT_DepositConfirmation *dc = &pc->dc[i];
    const struct TALER_EXCHANGE_DenomPublicKey *denom_details;

    denom_details = TALER_EXCHANGE_get_denomination_key (keys,
							 &dc->denom);
    if (NULL == denom_details)
    {
      char *denom_enc;
      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:o, s:o}",
                                                             "error", "denomination not found",
                                                             "denom_pub", GNUNET_JSON_from_rsa_public_key (dc->denom.rsa_public_key),
                                                             "exchange_keys", TALER_EXCHANGE_get_keys_raw (mh)));
      denom_enc = denomination_to_string_alloc (&dc->denom);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "unknown denom to exchange: %s\n", denom_enc);
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
                                TMH_RESPONSE_make_json_pack ("{s:s, s:o}",
                                                             "error", "invalid denomination",
                                                             "denom_pub", GNUNET_JSON_from_rsa_public_key (dc->denom.rsa_public_key)));
      denom_enc = denomination_to_string_alloc (&dc->denom);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "client offered invalid denomination: %s\n", denom_enc);
      GNUNET_free (denom_enc);
      return;
    }
    if (0 == i)
    {
      acc_fee = denom_details->fee_deposit;
      acc_amount = dc->percoin_amount;
    }
    else
    {
      if ( (GNUNET_OK !=
	    TALER_amount_add (&acc_fee,
			      &denom_details->fee_deposit,
			      &acc_fee)) ||
	   (GNUNET_OK !=
	    TALER_amount_add (&acc_amount,
			      &dc->percoin_amount,
			      &acc_amount)) )
      {
	GNUNET_break_op (0);
	/* Overflow in these amounts? Very strange. */
	resume_pay_with_response (pc,
				  MHD_HTTP_BAD_REQUEST,
				  TMH_RESPONSE_make_internal_error ("Overflow adding up amounts"));
	return;
      }
    }
    if (GNUNET_SYSERR ==
	TALER_amount_subtract (&dc->amount_without_fee,
			       &dc->percoin_amount,
			       &denom_details->fee_deposit))
    {
      GNUNET_break_op (0);
      /* fee higher than residual coin value, makes no sense. */
      resume_pay_with_response (pc,
				MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:o, s:o}",
                                                             "hint", "fee higher than coin value",
                                                             "f", TALER_JSON_from_amount (&dc->percoin_amount),
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
                                TMH_RESPONSE_make_internal_error ("overflow"));
      return;
    }
    /* check if total payment sufficies */
    if (-1 == TALER_amount_cmp (&acc_amount,
                                &total_needed))
    {
      GNUNET_break_op (0);
      resume_pay_with_response (pc,
                                MHD_HTTP_NOT_ACCEPTABLE,
                                TMH_RESPONSE_make_external_error ("insufficient funds (including excessive exchange fees to be covered by customer)"));
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
                                MHD_HTTP_NOT_ACCEPTABLE,
                                TMH_RESPONSE_make_external_error ("insufficient funds"));
      return;
    }
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Exchange and fee structure OK. Initiating deposit operation for coins\n");
  /* Initiate /deposit operation for all coins */
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct MERCHANT_DepositConfirmation *dc = &pc->dc[i];

    GNUNET_assert (NULL != j_wire);

    dc->dh = TALER_EXCHANGE_deposit (mh,
                                     &dc->percoin_amount,
                                     pc->edate,
                                     j_wire,
                                     &pc->h_contract,
                                     &dc->coin_pub,
                                     &dc->ub_sig,
                                     &dc->denom,
                                     pc->timestamp,
                                     pc->transaction_id,
                                     &pubkey,
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
                                TMH_RESPONSE_make_json_pack ("{s:s, s:i}",
                                                             "hint", "Coin signature invalid.",
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

  pc->timeout_task = NULL;

  if (NULL != pc->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (pc->fo);
    pc->fo = NULL;
  }

  resume_pay_with_response (pc,
                            MHD_HTTP_SERVICE_UNAVAILABLE,
                            TMH_RESPONSE_make_internal_error ("exchange not reachable"));
}


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

  res = TMH_PARSE_post_json (connection,
                             &pc->json_parse_context,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
  {
    GNUNET_break (0);
    return MHD_NO;  /* error parsing JSON */
  }
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES; /* the POST's body has to be further fetched */

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
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_CONTRACT,
                                    &cp.purpose,
                                    &merchant_sig.eddsa_sig,
                                    &pubkey.eddsa_pub))
    {
      GNUNET_break (0);
      json_decref (root);
      return TMH_RESPONSE_reply_external_error (connection,
                                                "invalid merchant signature supplied");
    }

    /* 'edate' is optional, if it is not present, generate it here; it
       will be timestamp plus the edate_delay supplied in config
       file */
    if (NULL == json_object_get (root, "edate"))
    {
      pc->edate = GNUNET_TIME_absolute_add (pc->timestamp,
                                            edate_delay);
    }
    else
    {
      struct GNUNET_JSON_Specification espec[] = {
        GNUNET_JSON_spec_absolute_time ("edate", &pc->edate),
        GNUNET_JSON_spec_end()
      };

      res = TMH_PARSE_json_data (connection,
                                 root,
                                 espec);
      if (GNUNET_YES != res)
      {
        json_decref (root);
        GNUNET_break (0);
        return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }
    }


    pc->coins_cnt = json_array_size (coins);
    if (0 == pc->coins_cnt)
    {
      json_decref (root);
      return TMH_RESPONSE_reply_external_error (connection,
                                                "no coins given");
    }
    /* note: 1 coin = 1 deposit confirmation expected */
    pc->dc = GNUNET_new_array (pc->coins_cnt,
                               struct MERCHANT_DepositConfirmation);

    json_array_foreach (coins, coins_index, coin)
    {
      struct MERCHANT_DepositConfirmation *dc = &pc->dc[coins_index];
      struct GNUNET_JSON_Specification spec[] = {
        TALER_JSON_spec_denomination_public_key ("denom_pub", &dc->denom),
        TALER_JSON_spec_amount ("f", &dc->percoin_amount),
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
        json_decref (root);
        GNUNET_break (0);
        return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }

      {
        char *s;
        s = TALER_amount_to_string (&dc->percoin_amount);
        GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                    "Coin #%i has f %s\n",
                    coins_index,
                    s);
        GNUNET_free (s);
      }

      dc->index = coins_index;
      dc->pc = pc;
    }
  }

  /* Check if this payment attempt has already taken place */
  if (GNUNET_OK ==
      db->check_payment (db->cls,
                         pc->transaction_id))
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
    return ret;
  }

  /* Find the responsible exchange, this may take a while... */
  pc->pending = pc->coins_cnt;
  pc->fo = TMH_EXCHANGES_find_exchange (pc->chosen_exchange,
                                        &process_pay_with_exchange,
                                        pc);

  /* ... so we suspend connection until the last coin has been ack'd
     or until we have encountered a hard error.  Eventually, we will
     resume the connection and send back a response using
     #resume_pay_with_response(). */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /pay handling while working with the exchange\n");
  MHD_suspend_connection (connection);
  GNUNET_SCHEDULER_add_delayed (PAY_TIMEOUT, handle_pay_timeout, pc);
  json_decref (root);
  return MHD_YES;
}

/* end of taler-merchant-httpd_pay.c */
