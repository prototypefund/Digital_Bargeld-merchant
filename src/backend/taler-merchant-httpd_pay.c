/*
  This file is part of TALER
  (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_pay.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <curl/curl.h>
#include <taler/taler_signatures.h>
#include <taler/taler_amount_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_mint_service.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_mints.h"
#include "taler_merchantdb_lib.h"


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
  struct TALER_MINT_DepositHandle *dh;

  /**
   * Denomination of this coin.
   */
  struct TALER_DenominationPublicKey denom;

  /**
   * Amount "f" that this coin contributes to the overall payment.
   */
  struct TALER_Amount percoin_amount;

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
   * Placeholder for #TMH_PARSE_post_json() to keep its internal state.
   */
  void *json_parse_context;

  /**
   * Mint URI given in @e root.
   */
  char *chosen_mint;

  /**
   * Transaction ID given in @e root.
   */
  uint64_t transaction_id;

  /**
   * Maximum fee from @e root.
   */
  struct TALER_Amount max_fee;

  /**
   * Amount from @e root.
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
  MHD_resume_connection (pc->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
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
 *   (200) for successful deposit; 0 if the mint's reply is bogus (fails
 *   to follow the protocol)
 * @param proof the received JSON reply,
 *   should be kept as proof (and, in case of errors, be forwarded to
 *   the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            json_t *proof)
{
  struct MERCHANT_DepositConfirmation *dc = cls;
  struct PayContext *pc = dc->pc;

  dc->dh = NULL;
  pc->pending--;
  if (MHD_HTTP_OK != http_status)
  {
    unsigned int i;

    /* Transaction failed; stop all other ongoing deposits */
    for (i=0;i<pc->coins_cnt;i++)
    {
      struct MERCHANT_DepositConfirmation *dci = &pc->dc[i];

      if (NULL != dci->dh)
      {
        TALER_MINT_deposit_cancel (dci->dh);
        dci->dh = NULL;
      }
    }
    /* Forward error including 'proof' for the body */
    resume_pay_with_response (pc,
                              http_status,
                              TMH_RESPONSE_make_json (proof));
    return;
  }
  /* FIXME: store result to DB here somewhere! */
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

  TMH_PARSE_post_cleanup_callback (pc->json_parse_context);
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct MERCHANT_DepositConfirmation *dc = &pc->dc[i];

    if (NULL != dc->dh)
    {
      TALER_MINT_deposit_cancel (dc->dh);
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
  if (NULL != pc->response)
  {
    MHD_destroy_response (pc->response);
    pc->response = NULL;
  }
  GNUNET_free (pc);
}


/**
 * Function called with the result of our mint lookup.
 *
 * @param cls the `struct PayContext`
 * @param mh NULL if mint was not found to be acceptable
 */
static void
process_pay_with_mint (void *cls,
                       struct TALER_MINT_Handle *mh)
{
  struct PayContext *pc = cls;
  struct TALER_Amount acc_fee;
  struct TALER_Amount coin_fee;
  const struct TALER_MINT_Keys *keys;
  unsigned int i;

  if (NULL == mh)
  {
    /* The mint on offer is not in the set of our (trusted)
       mints.  Reject the payment. */
    GNUNET_break_op (0);
    resume_pay_with_response (pc,
                              403, /* FIXME */
                              TMH_RESPONSE_make_external_error ("unknown mint"));
    return;
  }

  keys = TALER_MINT_get_keys (mh);
  if (NULL == keys)
  {
    GNUNET_break (0);
    resume_pay_with_response (pc,
                              UINT_MAX, /* FIXME */
                              TMH_RESPONSE_make_internal_error ("no keys"));
    return;
  }

  /* FIXME: do not just total up the fees, but also
     the value of the deposited coins! */
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct MERCHANT_DepositConfirmation *dc = &pc->dc[i];
    const struct TALER_MINT_DenomPublicKey *denom_details;

    denom_details = TALER_MINT_get_denomination_key (keys,
                                                     &dc->denom);
    if (NULL == denom_details)
    {
      resume_pay_with_response (pc,
                                MHD_HTTP_BAD_REQUEST,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:o}",
                                                             "hint", "unknown denom to mint",
                                                             "denom_pub", TALER_json_from_rsa_public_key (dc->denom.rsa_public_key)));
      return;
    }
    if (0 == i)
      acc_fee = denom_details->fee_deposit;
    else
      TALER_amount_add (&acc_fee,
                        &denom_details->fee_deposit,
			&coin_fee);
  }

  /* FIXME: we should check that the total matches as well... */
  if (-1 == TALER_amount_cmp (&pc->max_fee,
                              &acc_fee))
  {
    resume_pay_with_response (pc,
                              MHD_HTTP_NOT_ACCEPTABLE,
                              TMH_RESPONSE_make_external_error ("fees too high"));
    return;
  }

  /* Initiate /deposit operation for all coins */
  for (i=0;i<pc->coins_cnt;i++)
  {
    struct MERCHANT_DepositConfirmation *dc = &pc->dc[i];

    dc->dh = TALER_MINT_deposit (mh,
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
      resume_pay_with_response (pc,
                                MHD_HTTP_SERVICE_UNAVAILABLE,
                                TMH_RESPONSE_make_json_pack ("{s:s, s:i}",
                                                             "mint", pc->chosen_mint,
                                                             "transaction_id", pc->transaction_id));
      return;
    }
  }
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
      return MHD_NO; /* hard error */
    res = MHD_queue_response (connection,
                              pc->response_code,
                              pc->response);
    if (NULL != pc->response)
    {
      MHD_destroy_response (pc->response);
      pc->response = NULL;
    }
    return res;
  }

  res = TMH_PARSE_post_json (connection,
                             &pc->json_parse_context,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;  /* error parsing JSON */
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES; /* the POST's body has to be further fetched */

  /* We got no 'edate' from frontend. Generate it here; it will be
     timestamp plus the edate_delay supplied in config file */
  if (NULL == json_object_get (root, "edate"))
  {
    pc->edate = GNUNET_TIME_absolute_add (pc->timestamp, // FIXME: uninit!
                                          edate_delay);
    if (-1 ==
        json_object_set (root,
                         "edate",
                         TALER_json_from_abs (pc->edate)))
    {
      GNUNET_break (0);
      json_decref (root);
      return MHD_NO;
    }
  }

  /* Got the JSON upload, parse it */
  {
    json_t *coins;
    json_t *coin;
    unsigned int coins_index;
    struct TMH_PARSE_FieldSpecification spec[] = {
      TMH_PARSE_member_array ("coins", &coins),
      TMH_PARSE_member_string ("mint", &pc->chosen_mint),
      TMH_PARSE_member_uint64 ("transaction_id", &pc->transaction_id),
      TMH_PARSE_member_amount ("max_fee", &pc->max_fee),
      TMH_PARSE_member_amount ("amount", &pc->amount),
      TMH_PARSE_member_time_abs ("timestamp", &pc->timestamp),
      TMH_PARSE_member_time_abs ("refund_deadline", &pc->refund_deadline),
      TMH_PARSE_member_fixed ("H_contract", &pc->h_contract),
      TMH_PARSE_member_time_abs ("edate", &pc->edate),
      TMH_PARSE_MEMBER_END
    };

    res = TMH_PARSE_json_data (connection,
                               root,
                               spec);
    if (GNUNET_YES != res)
    {
      json_decref (root);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }

    pc->coins_cnt = json_array_size (coins);
    if (0 == pc->coins_cnt)
    {
      json_decref (root);
      return TMH_RESPONSE_reply_external_error (connection,
                                                "no coins given");
    }
    pc->dc = GNUNET_new_array (pc->coins_cnt,
                               struct MERCHANT_DepositConfirmation);

    json_array_foreach (coins, coins_index, coin)
    {
      struct MERCHANT_DepositConfirmation *dc = &pc->dc[coins_index];
      struct TMH_PARSE_FieldSpecification spec[] = {
        TMH_PARSE_member_denomination_public_key ("denom_pub", &dc->denom),
        TMH_PARSE_member_amount ("f", &dc->percoin_amount),
        TMH_PARSE_member_fixed ("coin_pub", &dc->coin_pub),
        TMH_PARSE_member_denomination_signature ("ub_sig", &dc->ub_sig),
        TMH_PARSE_member_fixed ("coin_sig", &dc->coin_sig),
        TMH_PARSE_MEMBER_END
      };

      res = TMH_PARSE_json_data (connection,
                                 coin,
                                 spec);
      if (GNUNET_YES != res)
      {
        json_decref (root);
        return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
      }
      dc->index = coins_index;
    }
  }

  /* Find the responsible mint, this may take a while... */
  pc->pending = pc->coins_cnt;
  TMH_MINTS_find_mint (pc->chosen_mint,
                       &process_pay_with_mint,
                       pc);

  /* ... so we suspend connection until the last coin has been ack'd
     or until we have encountered a hard error.  Eventually, we will
     resume the connection and send back a response using
     #resume_pay_with_response(). */
  MHD_suspend_connection (connection);
  json_decref (root);
  return MHD_YES;
}
