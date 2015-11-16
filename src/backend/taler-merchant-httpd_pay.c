/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
#include "taler-mint-httpd.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "taler-mint-httpd_mhd.h"
#include "merchant_db.h"
#include "merchant.h"
#include "taler_merchant_lib.h"

extern struct MERCHANT_Mint *mints;
extern const struct MERCHANT_WIREFORMAT_Sepa *wire;
extern PGconn *db_conn;
extern long long salt;
extern unsigned int nmints;
extern struct GNUNET_TIME_Relative edate_delay;
extern struct GNUNET_CRYPTO_EddsaPrivateKey privkey;
extern struct GNUNET_SCHEDULER_Task *poller_task;
extern void context_task (void *cls,
                          const struct GNUNET_SCHEDULER_TaskContext *tc);

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
int
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

  /*printf ("mint %s (%d), pends: %d\n",
           mints[mint_index].hostname,
	   mint_index,
	   mints[mint_index].pending);*/

  if (1 == mints[mint_index].pending) 
    return GNUNET_SYSERR;
  keys = TALER_MINT_get_keys (mints[mint_index].conn);
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
 * Callback to handle a deposit permission's response.
 * @param cls see `struct MERCHANT_DepositConfirmationCls` (i.e. a poinetr to the global
 * array of confirmations and an index for this call in that array). That way, the last
 * executed callback can detect that no other confirmations are on the way, and can pack
 * a response for the wallet
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 * 0 if the mint's reply is bogus (fails to follow the protocol)
 * @param proof the received JSON reply, should be kept as proof (and, in case of errors,
 * be forwarded to the customer)
 */
void
deposit_cb (void *cls, unsigned int http_status, json_t *proof)
{
  /* NOTE: what if the mint doesn't respond? Does this callback get
    called? */
  int i;
  struct MERCHANT_DepositConfirmationCls *dccls;
  dccls = (struct MERCHANT_DepositConfirmationCls *) cls;

  printf ("deposit cb\n");
  if (GNUNET_SYSERR ==
      MERCHANT_DB_update_deposit_permission (db_conn,
                                             dccls->transaction_id,
	                                     0))
    /* TODO */
    printf ("db error\n");
  printf ("/deposit ack'd\n");
  dccls->dc[dccls->index].ackd = 1;
  dccls->dc[dccls->index].exit_status = http_status;
  dccls->dc[dccls->index].proof = proof;
  
  /* loop through the confirmation array and return accordingly */
  for (i = 0; i < dccls->coins_cnt; i++)
  {
    /* just return if there is at least one coin to be still
      confirmed */
    if (!dccls->dc[i].ackd)
    {
      printf ("still vacant coins\n");
      return;
    }
  }

  printf ("All /deposit(s) ack'd\n");
  MHD_resume_connection (dccls->connection);
  i = TMH_RESPONSE_reply_json_pack (dccls->connection,
                                    MHD_HTTP_OK,
	                            "{s:s}",
				    "result", "all conins ack'd (and connection resumed)");
  printf ("mhd res %d\n", i);
  /* TODO at this point, any coin has been confirmed by the mint. So
    check for errors .. */

  /* Finally */
  GNUNET_free (dccls->dc);
  GNUNET_free (dccls);
}

/**
 * Accomplish this payment.
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
  struct MERCHANT_DepositConfirmationCls *dccls;

  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_array ("coins", &coins),
    TMH_PARSE_member_string ("mint", &chosen_mint),
    TMH_PARSE_member_amount ("max_fee", &max_fee),
    TMH_PARSE_member_time_abs ("timestamp", &timestamp),
    TMH_PARSE_member_time_abs ("refund_deadline", &refund_deadline),
    TMH_PARSE_member_uint64 ("transaction_id", &transaction_id),
    TMH_PARSE_member_fixed ("H_contract", &h_contract),
    TMH_PARSE_MEMBER_END
  };

  struct TMH_PARSE_FieldSpecification coin_aggregate_spec[] = {
    TMH_PARSE_member_amount ("f", &amount),
    TMH_PARSE_member_fixed ("coin_pub", &coin_pub.eddsa_pub),
    TMH_PARSE_member_denomination_public_key ("denom_pub", &denom_pub),
    TMH_PARSE_member_denomination_signature ("ub_sig", &ub_sig),
    TMH_PARSE_member_fixed ("coin_sig", &coin_sig.eddsa_signature),
    TMH_PARSE_MEMBER_END
  };

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
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
    if (0 == strcmp (mints[mint_index].hostname,
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
  GNUNET_CRYPTO_eddsa_key_get_public (&privkey, &pubkey.eddsa_pub);
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
					                   mints[mint_index].hostname))
      return TMH_RESPONSE_reply_internal_error (connection, "internal DB failure");
    res = TMH_PARSE_json_data (connection,
                               coin_aggregate,
                               coin_aggregate_spec);
    if (GNUNET_OK != res)
      return res; /* may return GNUNET_NO */

    dccls = GNUNET_malloc (sizeof (struct MERCHANT_DepositConfirmationCls));
    dccls->index = coins_index;
    dccls->dc = dc;
    dccls->coins_cnt = coins_cnt;
    dccls->transaction_id = transaction_id;
    dccls->connection = connection;
    
    dh = TALER_MINT_deposit (mints[mint_index].conn,
                             &amount,
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
			     deposit_cb,
			     dccls); /*may be destroyed by the time the cb gets called..*/
    if (NULL == dh)
    {
      MHD_resume_connection (connection);
      return TMH_RESPONSE_reply_json_pack (connection,
                                           MHD_HTTP_SERVICE_UNAVAILABLE,
		                           "{s:s, s:i}",
				           "mint", mints[mint_index].hostname,
				           "transaction_id", transaction_id);
    }
  }

  printf ("poller task: %p\n", poller_task);
  GNUNET_SCHEDULER_cancel (poller_task);
  GNUNET_SCHEDULER_add_now (context_task, mints[mint_index].ctx);
  return MHD_YES;

  /* 4 Return response code: success, or whatever data the
    mint sent back regarding some bad coin */
}
