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
 * @file merchant/backend/taler-merchant-httpd.c
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
#include "taler-mint-httpd.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "taler-mint-httpd_mhd.h"
#include "taler_merchantdb_lib.h"


/**
 * Outcome of a /deposit request for a coin. Typically forming an array enclosed
 * into the unique PayContext
 */
struct MERCHANT_DepositConfirmation
{
  /**
   * Reference to the per-deposit-handler Context. Needed by the
   * cleanup function to get it freed
   */
  struct DepositCallbackContext *dcc;

  /**
   * The mint's response body (JSON). Mainly useful in case
   * some callback needs to send back to the to the wallet the
   * outcome of an erroneous coin
   */
  json_t *proof;

  /**
   * True if this coin's outcome has been read from
   * its cb
   */
  unsigned int ackd;

  /**
   * The mint's response to this /deposit
   */
  unsigned int exit_status;

};



/**
 * Fetch the deposit fee related to the given coin aggregate.
 * @param connection the connection to send an error response to
 * @param coin_aggregate a coin "aggregate" is the JSON set of
 * values contained in a single cell of the 'coins' array sent
 * in a payment
 * @param deposit_fee where to store the resulting deposit fee
 * @param mint_index the index which points the chosen mint within
 * the global 'mints' array
 * @return GNUNET_OK if successful, GNUNET_NO if the data supplied
 * is invalid (including the case when the key is not found),
 * GNUNET_SYSERR upon internal errors
 */
static int
deposit_fee_from_coin_aggregate (struct MHD_Connection *connection,
                                 json_t *coin_aggregate,
                                 struct TALER_Amount *deposit_fee,
			         unsigned int mint_index)
{
  int res;
  const struct TALER_MINT_Keys *keys;
  const struct TALER_MINT_DenomPublicKey *denom_details;
  struct TALER_DenominationPublicKey denom;

  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_denomination_public_key ("denom_pub", &denom),
    TMH_PARSE_MEMBER_END
  };

  res = TMH_PARSE_json_data (connection,
                             coin_aggregate,
			     spec);
  if (GNUNET_OK != res)
    return res; /* may return GNUNET_NO */

  if (1 == mints[mint_index]->pending)
    return GNUNET_SYSERR;
  keys = TALER_MINT_get_keys (mints[mint_index]->conn);
  denom_details = TALER_MINT_get_denomination_key (keys, &denom);
  if (NULL == denom_details)
  {
    TMH_RESPONSE_reply_json_pack (connection,
                                  MHD_HTTP_BAD_REQUEST,
				  "{s:s, s:o}",
				  "hint", "unknown denom to mint",
				  "denom_pub", TALER_json_from_rsa_public_key (denom.rsa_public_key));
    return GNUNET_NO;
  }
  *deposit_fee = denom_details->fee_deposit;
  return GNUNET_OK;
}


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
   * Pointer to the global (malloc'd) array of all coins outcomes
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
   * Response to return, NULL if we don't have one yet.
   */
  struct MHD_Response *response;

  /**
   * Transaction id
   */
  uint64_t transaction_id;

  /**
   * How many coins this paymen is made of.
   */
  unsigned int coins_cnt;

  /**
   * HTTP status code to use for the reply, i.e 200 for "OK".
   * Special value UINT_MAX is used to indicate hard errors
   * (no reply, return MHD_NO).
   */
  unsigned int response_code;

};

/**
 * Information needed by a single /deposit callback to refer to its
 * own coin inside the confirmations array, namely `struct MERCHANT_DepositConfirmation *dc`
 * above. Note: this information can NOT be shared between all the callbacks.
 */
struct DepositCallbackContext
{

  /**
   * Offset of this coin into the array of all coins outcomes
   */
  unsigned int index;

  /**
   * Reference to the main PayContext
   */
  struct PayContext *pc;

};

/**
 * Callback to handle a deposit permission's response.
 *
 * @param cls see `struct MERCHANT_DepositConfirmationCls` (i.e. a poinetr to the global
 * array of confirmations and an index for this call in that array). That way, the last
 * executed callback can detect that no other confirmations are on the way, and can pack
 * a response for the wallet
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 * 0 if the mint's reply is bogus (fails to follow the protocol)
 * @param proof the received JSON reply, should be kept as proof (and, in case of errors,
 * be forwarded to the customer)
 */
static void
deposit_cb (void *cls,
            unsigned int http_status,
            json_t *proof)
{
  struct DepositCallbackContext *dcc = cls;
  int i;

  /*FIXME the index is the same for every individual cb */
  if (GNUNET_SYSERR ==
      MERCHANT_DB_update_deposit_permission (db_conn,
                                             dcc->pc->transaction_id,
	                                     0))
    /* TODO */
    printf ("db error\n");
  dcc->pc->dc[dcc->index].ackd = 1;
  dcc->pc->dc[dcc->index].exit_status = http_status;
  dcc->pc->dc[dcc->index].proof = proof;

  /* loop through the confirmation array and return accordingly */
  for (i = 0; i < dcc->pc->coins_cnt; i++)
  {
    /* just return if there is at least one coin to be still
      confirmed */
    if (! dcc->pc->dc[i].ackd)
    {
      printf ("still vacant coins\n");
      return;
    }
  }

  printf ("All /deposit(s) ack'd\n");

  dcc->pc->response = MHD_create_response_from_buffer (strlen ("All coins ack'd by the mint\n"),
                                                               "All coins ack'd by the mint\n",
                                                               MHD_RESPMEM_MUST_COPY);
  dcc->pc->response_code = MHD_HTTP_OK;
  /* Clean up what we can already */
  MHD_resume_connection (dcc->pc->connection);
  TM_trigger_daemon (); /* we resumed, kick MHD */
}


static void
pay_context_cleanup (struct TM_HandlerContext *hc)
{
  int i;
  struct PayContext *pc = (struct PayContext *) hc;

  for (i = 0; i < pc->coins_cnt; i++)
    GNUNET_free_non_null (pc->dc[i].dcc);

  TMH_PARSE_post_cleanup_callback (pc->json_parse_context);

  #if 0
  if (NULL != pc->response)
  {
    /* undestroyable regardless of the other MHD_destroy_response called
    in this source, FIXME */

    MHD_destroy_response (pc->response);
    pc->response = NULL;
  }
  #endif

  GNUNET_free_non_null (pc->dc);
  GNUNET_free (pc);
}


/**
 * Accomplish this payment.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure
 * (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a
 * upload_data
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
  json_t *root;
  json_t *coins;
  char *chosen_mint;
  json_t *coin_aggregate;
  json_t *wire_details;
  unsigned int mint_index; /*a cell in the global array*/
  unsigned int coins_index;
  unsigned int coins_cnt;
  uint64_t transaction_id;
  int res;
  struct TALER_MINT_DepositHandle *dh;
  struct TALER_Amount max_fee;
  struct TALER_Amount acc_fee;
  struct TALER_Amount coin_fee;
  struct TALER_Amount amount;
  struct TALER_Amount percoin_amount;
  struct GNUNET_TIME_Absolute edate;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct TALER_MerchantPublicKeyP pubkey;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  struct TALER_DenominationPublicKey denom_pub;
  struct TALER_DenominationSignature ub_sig;
  struct TALER_CoinSpendSignatureP coin_sig;
  struct GNUNET_HashCode h_contract;
  struct MERCHANT_DepositConfirmation *dc;

  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_array ("coins", &coins),
    TMH_PARSE_member_string ("mint", &chosen_mint),
    TMH_PARSE_member_amount ("max_fee", &max_fee),
    TMH_PARSE_member_amount ("amount", &amount),
    TMH_PARSE_member_time_abs ("timestamp", &timestamp),
    TMH_PARSE_member_time_abs ("refund_deadline", &refund_deadline),
    TMH_PARSE_member_uint64 ("transaction_id", &transaction_id),
    TMH_PARSE_member_fixed ("H_contract", &h_contract),
    TMH_PARSE_MEMBER_END
  };

  struct TMH_PARSE_FieldSpecification coin_aggregate_spec[] = {
    TMH_PARSE_member_amount ("f", &percoin_amount),
    TMH_PARSE_member_fixed ("coin_pub", &coin_pub.eddsa_pub),
    TMH_PARSE_member_denomination_public_key ("denom_pub", &denom_pub),
    TMH_PARSE_member_denomination_signature ("ub_sig", &ub_sig),
    TMH_PARSE_member_fixed ("coin_sig", &coin_sig.eddsa_signature),
    TMH_PARSE_MEMBER_END
  };

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
    if (UINT_MAX == pc->response_code)
      return MHD_NO; /* hard error */
    /* We are *done* processing the request, just queue the response (!) */
    res = MHD_queue_response (connection,
                              pc->response_code,
                              pc->response);
    #if 0
    if (pc->response != NULL)
    {
      /* undestroyable regardless of the other MHD_destroy_response called
        in this source, FIXME */
      MHD_destroy_response (pc->response);
      pc->response = NULL;
    }
    #endif
    return res;
  }

  res = TMH_PARSE_post_json (connection,
                             &pc->json_parse_context,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES;

  res = TMH_PARSE_json_data (connection,
                             root,
			     spec);

  if (GNUNET_YES != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;

  /* 1 Check if the chosen mint is among the merchant's preferred.

    An error in this case could be due to:

    * the wallet indicated a non existent mint
    * the wallet indicated a non trusted mint

    NOTE: by preventively checking this, the merchant
    avoids getting HTTP response codes from random
    websites that may mislead the wallet in the way
    of managing the error. Of course, that protect the
    merchant from POSTing coins to untrusted mints.

   */

  for (mint_index = 0; mint_index <= nmints; mint_index++)
  {
    /* no mint found in array */
    if (mint_index == nmints)
    {
      mint_index = -1;
      break;
    }

    /* test it by checking public key */
    if (0 == strcmp (mints[mint_index]->hostname,
                     chosen_mint))
      break;

  }

  if (-1 == mint_index)
    return TMH_RESPONSE_reply_external_error (connection, "unknown mint");

  /* no 'edate' from frontend. Generate it here; it will be timestamp
    + a edate delay supplied in config file */
  if (NULL == json_object_get (root, "edate"))
  {
    edate = GNUNET_TIME_absolute_add (timestamp, edate_delay);
    if (-1 == json_object_set (root, "edate", TALER_json_from_abs (edate)))
      return MHD_NO;
  }

  coins_cnt = json_array_size (coins);

  if (0 == coins_cnt)
    return TMH_RESPONSE_reply_external_error (connection, "no coins given");

  json_array_foreach (coins, coins_index, coin_aggregate)
  {
    res = deposit_fee_from_coin_aggregate (connection,
                                           coin_aggregate,
		                           &coin_fee,
				           mint_index);
    if (GNUNET_NO == res)
      return MHD_YES;
    if (GNUNET_SYSERR == res)
      return MHD_NO;

    if (0 == coins_index)
      acc_fee = coin_fee;
    else
      TALER_amount_add (&acc_fee,
                        &acc_fee,
			&coin_fee);
  }


  if (-1 == TALER_amount_cmp (&max_fee, &acc_fee))
    return MHD_HTTP_NOT_ACCEPTABLE;

  /* cutting off unneeded fields from deposit permission as
    gotten from the wallet */
  if (-1 == json_object_del (root, "mint"))
    return TMH_RESPONSE_reply_external_error (connection,
                                              "malformed/non-existent 'mint' field");
  if (-1 == json_object_del (root, "coins"))
    return TMH_RESPONSE_reply_external_error (connection,
                                              "malformed/non-existent 'coins' field");

  /* adding our public key to deposit permission */
  GNUNET_CRYPTO_eddsa_key_get_public (privkey, &pubkey.eddsa_pub);
  json_object_set_new (root,
                       "merchant_pub",
		       TALER_json_from_data (&pubkey, sizeof (pubkey)));

  wire_details = MERCHANT_get_wire_json (wire, salt);
  /* since memory is zero'd out by GNUNET_malloc, any 'ackd' field will be
    (implicitly) set to false */
  dc = GNUNET_malloc (coins_cnt * sizeof (struct MERCHANT_DepositConfirmation));
  if (NULL == dc)
    return TMH_RESPONSE_reply_internal_error (connection, "memory failure");

  /* DEBUG CHECKPOINT: return a provisory fullfilment page to the wallet
    to test the reception of coins array */

  #ifdef COINSCHECKPOINT
  rh->data = "Coins received\n";
  return TMH_MHD_handler_static_response (rh,
                                          connection,
	                                  connection_cls,
				          upload_data,
				          upload_data_size);

  #endif

  /* suspend connection until the last coin has been ack'd to the cb.
    That last cb will finally resume the connection and send back a response */
  MHD_suspend_connection (connection);

  pc->dc = dc;
  pc->coins_cnt = coins_cnt;
  pc->transaction_id = transaction_id;


  json_array_foreach (coins, coins_index, coin_aggregate)
  {

    /* 3 For each coin in DB

         a. Generate a deposit permission
         b. store it in DB
         c. POST to the mint (see mint-lib for this)
            (retry until getting a persisten state)
    */

    /* a */
    if (-1 == json_object_update (root, coin_aggregate))
      return TMH_RESPONSE_reply_internal_error (connection,
                                                "deposit permission not generated for storing");

    /* b */
    char *deposit_permission_str = json_dumps (root, JSON_COMPACT);
    if (GNUNET_OK != MERCHANT_DB_store_deposit_permission (db_conn,
                                                           deposit_permission_str,
			                                   transaction_id,
					                   1,
					                   mints[mint_index]->hostname))
      return TMH_RESPONSE_reply_internal_error (connection, "internal DB failure");
    res = TMH_PARSE_json_data (connection,
                               coin_aggregate,
                               coin_aggregate_spec);
    if (GNUNET_OK != res)
      return res; /* may return GNUNET_NO */

    /* c */
    struct DepositCallbackContext *percoin_dcc = GNUNET_new (struct DepositCallbackContext);
    pc->dc[coins_index].dcc = percoin_dcc;
    percoin_dcc->index = coins_index;
    percoin_dcc->pc = pc;

    dh = TALER_MINT_deposit (mints[mint_index]->conn,
                             &percoin_amount,
			     edate,
			     wire_details,
			     &h_contract,
			     &coin_pub,
			     &ub_sig,
			     &denom_pub,
			     timestamp,
                             transaction_id,
                             &pubkey,
			     refund_deadline,
			     &coin_sig,
			     &deposit_cb,
			     percoin_dcc); /*FIXME TODO instantiate an individual cls for each
			            cb: each of them needs an index which points the
				    array of all the confirmations */
    if (NULL == dh)
    {
      MHD_resume_connection (connection);
      return TMH_RESPONSE_reply_json_pack (connection,
                                           MHD_HTTP_SERVICE_UNAVAILABLE,
		                           "{s:s, s:i}",
				           "mint", mints[mint_index]->hostname,
				           "transaction_id", transaction_id);
    }
  }

  GNUNET_SCHEDULER_cancel (mints[mint_index]->poller_task);
  GNUNET_SCHEDULER_add_now (context_task, mints[mint_index]->ctx);
  return MHD_YES;

  /* 4 Return response code: success, or whatever data the
    mint sent back regarding some bad coin */
}
