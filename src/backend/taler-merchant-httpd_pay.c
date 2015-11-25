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
   *
   */
  struct TALER_MINT_DepositHandle *dh;

  /**
   * The mint's response body (JSON). Mainly useful in case
   * some callback needs to send back to the to the wallet the
   * outcome of an erroneous coin. DEAD?
   */
  json_t *proof;

  /**
   * Denomination for this coin.
   */
  struct TALER_DenominationPublicKey denom;

  /**
   *
   */
  struct TALER_Amount percoin_amount;

  /**
   *
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   *
   */
  struct TALER_DenominationSignature ub_sig;

  /**
   *
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * True if this coin's outcome has been read from
   * its cb
   */
  unsigned int ackd;

  /**
   * The mint's response to this /deposit
   */
  unsigned int exit_status;

  /**
   * Offset of this coin into the array of all coins outcomes
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
   * Root node of the request body in JSON.
   */
  json_t *root;

  /**
   * Coins included in @e root.
   */
  json_t *coins;

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
   *
   */
  struct GNUNET_TIME_Absolute edate;

  /**
   * Response to return, NULL if we don't have one yet.
   */
  struct MHD_Response *response;

  /**
   * Number of coins this payment is made of.
   */
  unsigned int coins_cnt;

  /**
   * Number of transactions still pending.
   */
  unsigned int pending;

  /**
   * HTTP status code to use for the reply, i.e 200 for "OK".
   * Special value UINT_MAX is used to indicate hard errors
   * (no reply, return MHD_NO).
   */
  unsigned int response_code;

};


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

  /* FIXME: store to DB! */
  dc->ackd = 1;
  dc->exit_status = http_status;
  dc->proof = proof; /* FIXME: needs rc+1 */
  pc->pending--;
  if (0 != pc->pending)
    return; /* still more to do */

  pc->response
    = MHD_create_response_from_buffer (strlen ("All coins ack'd by the mint\n"),
                                       "All coins ack'd by the mint\n",
                                       MHD_RESPMEM_MUST_COPY);
  /* FIXME: move this logic into a function: */
  pc->response_code = MHD_HTTP_OK;
  MHD_resume_connection (pc->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
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

    /* FIXME: clean up 'dc'! */
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
 * @param mint NULL if mint was not found to be acceptable
 * @param mh NULL if mint was not found to be acceptable
 */
static void
process_pay_with_mint (void *cls,
                       struct MERCHANT_Mint *mint,
                       struct TALER_MINT_Handle *mh)
{
  struct PayContext *pc = cls;
  struct TALER_Amount acc_fee;
  struct TALER_Amount coin_fee;
  const struct TALER_MINT_Keys *keys;
  unsigned int i;

  if (NULL == mint)
  {
    /* The mint on offer is not in the set of our (trusted)
       mints.  Reject the payment. */
    MHD_resume_connection (pc->connection);
    pc->response_code = 403;
    pc->response = TMH_RESPONSE_make_external_error ("unknown mint");
    TMH_trigger_daemon ();
    return;
  }

  keys = TALER_MINT_get_keys (mh);
  if (NULL == keys)
  {
    GNUNET_break (0);
    pc->response_code = UINT_MAX;
    pc->response = TMH_RESPONSE_make_internal_error ("no keys");
    TMH_trigger_daemon ();
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
      pc->response_code = MHD_HTTP_BAD_REQUEST;
      pc->response
        = TMH_RESPONSE_make_json_pack ("{s:s, s:o}",
                                       "hint", "unknown denom to mint",
                                       "denom_pub", TALER_json_from_rsa_public_key (dc->denom.rsa_public_key));
      TMH_trigger_daemon ();
      return;
    }
    if (0 == i)
      acc_fee = denom_details->fee_deposit;
    else
      TALER_amount_add (&acc_fee,
                        &denom_details->fee_deposit,
			&coin_fee);
  }

  if (-1 == TALER_amount_cmp (&pc->max_fee,
                              &acc_fee))
  {
    pc->response_code = MHD_HTTP_NOT_ACCEPTABLE;
    pc->response = TMH_RESPONSE_make_external_error ("fees too high");
    TMH_trigger_daemon ();
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
      MHD_resume_connection (pc->connection);
      pc->response_code = MHD_HTTP_SERVICE_UNAVAILABLE;
      pc->response = TMH_RESPONSE_make_json_pack ("{s:s, s:i}",
                                                  "mint", pc->chosen_mint,
                                                  "transaction_id", pc->transaction_id);
      TMH_trigger_daemon ();
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
  int res;
  json_t *coin;
  unsigned int coins_index;

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
                             &pc->root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;  /* error parsing JSON */
  if ((GNUNET_NO == res) || (NULL == pc->root))
    return MHD_YES; /* the POST's body has to be further fetched */

  /* Got the JSON upload, parse it (FIXME: all of this here?) */

  /* We got no 'edate' from frontend. Generate it here; it will be
     timestamp plus the edate_delay supplied in config file */
  if (NULL == json_object_get (pc->root, "edate"))
  {
    pc->edate = GNUNET_TIME_absolute_add (pc->timestamp, // FIXME: uninit!
                                          edate_delay);
    if (-1 == json_object_set (pc->root,
                               "edate",
                               TALER_json_from_abs (pc->edate)))
      return MHD_NO;
  }

  {
    struct TMH_PARSE_FieldSpecification spec[] = {
      TMH_PARSE_member_array ("coins", &pc->coins),
      TMH_PARSE_member_string ("mint", &pc->chosen_mint),
      TMH_PARSE_member_amount ("max_fee", &pc->max_fee),
      TMH_PARSE_member_amount ("amount", &pc->amount),
      TMH_PARSE_member_time_abs ("edate", &pc->edate),
      TMH_PARSE_member_time_abs ("timestamp", &pc->timestamp),
      TMH_PARSE_member_time_abs ("refund_deadline", &pc->refund_deadline),
      TMH_PARSE_member_uint64 ("transaction_id", &pc->transaction_id),
      TMH_PARSE_member_fixed ("H_contract", &pc->h_contract),
      TMH_PARSE_MEMBER_END
    };

    res = TMH_PARSE_json_data (connection,
                               pc->root,
                               spec);
    if (GNUNET_YES != res)
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;

    if (0 == json_array_size (pc->coins))
      return TMH_RESPONSE_reply_external_error (connection,
                                                "no coins given");
  }
  pc->dc = GNUNET_new_array (json_array_size (pc->coins),
                             struct MERCHANT_DepositConfirmation);

  json_array_foreach (pc->coins, coins_index, coin)
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
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;


  }

  /* Find the responsible mint, this may take a while... */
  TMH_MINTS_find_mint (pc->chosen_mint,
                       &process_pay_with_mint,
                       pc);
  /* Suspend connection until the last coin has been ack'd or
     until we have encountered a hard error.
     Eventually, we will resume the connection and send back a response. */
  MHD_suspend_connection (connection);
  return MHD_YES;
}
