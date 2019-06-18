/*
  This file is part of TALER
  (C) 2014-2017 INRIA

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
 * @file backend/taler-merchant-httpd_track-transfer.c
 * @brief implement API for tracking transfers and wire transfers
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_track-transfer.h"


/**
 * How long to wait before giving up processing with the exchange?
 */
#define TRACK_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))

/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3

/**
 * Context used for handing /track/transfer requests.
 */
struct TrackTransferContext
{

  /**
   * This MUST be first!
   */
  struct TM_HandlerContext hc;

  /**
   * Handle to the exchange.
   */
  struct TALER_EXCHANGE_Handle *eh;

  /**
   * Handle for the /wire/transfers request.
   */
  struct TALER_EXCHANGE_TrackTransferHandle *wdh;

  /**
   * For which merchant instance is this tracking request?
   */
  struct MerchantInstance *mi;

  /**
   * HTTP connection we are handling.
   */
  struct MHD_Connection *connection;

  /**
   * Response to return upon resume.
   */
  struct MHD_Response *response;

  /**
   * Handle for operation to lookup /keys (and auditors) from
   * the exchange used for this transaction; NULL if no operation is
   * pending.
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Task run on timeout.
   */
  struct GNUNET_SCHEDULER_Task *timeout_task;

  /**
   * URL of the exchange.
   */
  char *url;

  /**
   * Wire method used for the transfer.
   */
  char *wire_method;

  /**
   * Pointer to the detail that we are currently
   * checking in #check_transfer().
   */
  const struct TALER_TrackTransferDetails *current_detail;

  /**
   * Argument for the /wire/transfers request.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Full original response we are currently processing.
   */
  const json_t *original_response;

  /**
   * Modified response to return to the frontend.
   */
  json_t *deposits_response;

  /**
   * Which transaction detail are we currently looking at?
   */
  unsigned int current_offset;

  /**
   * Response code to return.
   */
  unsigned int response_code;

  /**
   * #GNUNET_NO if we did not find a matching coin.
   * #GNUNET_SYSERR if we found a matching coin, but the amounts do not match.
   * #GNUNET_OK if we did find a matching coin.
   */
  int check_transfer_result;
};


/**
 * Represents an entry in the table used to sum up
 * individual deposits for each h_contract_terms.
 */
struct Entry
{

  /**
   * Sum accumulator for deposited value.
   */
  struct TALER_Amount deposit_value;

  /**
   * Sum accumulator for deposit fee.
   */
  struct TALER_Amount deposit_fee;

};


/**
 * Free the @a rctx.
 *
 * @param rctx data to free
 */
static void
free_transfer_track_context (struct TrackTransferContext *rctx)
{
  if (NULL != rctx->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (rctx->fo);
    rctx->fo = NULL;
  }
  if (NULL != rctx->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (rctx->timeout_task);
    rctx->timeout_task = NULL;
  }
  if (NULL != rctx->wdh)
  {
    TALER_EXCHANGE_track_transfer_cancel (rctx->wdh);
    rctx->wdh = NULL;
  }
  if (NULL != rctx->url)
  {
    GNUNET_free (rctx->url);
    rctx->url = NULL;
  }
  if (NULL != rctx->wire_method)
  {
    GNUNET_free (rctx->wire_method);
    rctx->wire_method = NULL;
  }
  GNUNET_free (rctx);
}

/**
 * Callback that frees all the elements in the hashmap
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct MerchantInstance`
 * @return #GNUNET_YES if the iteration should continue,
 *         #GNUNET_NO otherwise.
 */
static int
hashmap_free (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  struct TALER_Amount *amount = value;

  GNUNET_free (amount);
  return GNUNET_YES;
}


/**
 * Builds JSON response containing the summed-up amounts
 * from individual deposits.
 *
 * @param cls closure
 * @param key map's current key
 * @param map's current value
 * @return #GNUNET_YES if iteration is to be continued,
 *         #GNUNET_NO otherwise.
 */
static int
build_deposits_response (void *cls,
                         const struct GNUNET_HashCode *key,
                         void *value)
{
  struct TrackTransferContext *rctx = cls;
  struct Entry *entry = value;
  json_t *element;
  json_t *contract_terms;
  json_t *order_id;

  db->preflight (db->cls);
  if (GNUNET_OK !=
      db->find_contract_terms_from_hash (db->cls,
                                         &contract_terms,
                                         key,
                                         &rctx->mi->pubkey))
  {
    GNUNET_break_op (0);
    return GNUNET_NO;
  }

  order_id = json_object_get (contract_terms,
                              "order_id");
  if (NULL == order_id)
  {
    GNUNET_break_op (0);
    json_decref (contract_terms);
    return GNUNET_NO;
  }
  element = json_pack ("{s:O, s:o, s:o}",
                       "order_id", order_id,
                       "deposit_value", TALER_JSON_from_amount (&entry->deposit_value),
                       "deposit_fee", TALER_JSON_from_amount (&entry->deposit_fee));
  json_decref (contract_terms);
  if (NULL == element)
  {
    GNUNET_break_op (0);
    return GNUNET_NO;
  }
  GNUNET_break (0 ==
                json_array_append_new (rctx->deposits_response,
                                       element));
  return GNUNET_YES;
}


/**
 * Transform /track/transfer result as gotten from the exchange
 * and transforms it in a format liked by the backoffice Web interface.
 *
 * @param result response from exchange's /track/transfer
 * @result pointer to new JSON, or NULL upon errors.
 */
static json_t *
transform_response (const json_t *result,
                    struct TrackTransferContext *rctx)
{
  json_t *deposits;
  json_t *value;
  json_t *result_mod = NULL;
  size_t index;
  const char *key;
  struct GNUNET_HashCode h_key;
  struct GNUNET_CONTAINER_MultiHashMap *map;
  struct TALER_Amount iter_value;
  struct TALER_Amount iter_fee;
  struct Entry *current_entry;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("deposit_value", &iter_value),
    TALER_JSON_spec_amount ("deposit_fee", &iter_fee),
    GNUNET_JSON_spec_string ("h_contract_terms", &key),
    GNUNET_JSON_spec_end ()
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Transforming /track/transfer response.\n");
  map = GNUNET_CONTAINER_multihashmap_create (1, GNUNET_NO);
  deposits = json_object_get (result,
                              "deposits");

  json_array_foreach (deposits, index, value)
  {
    if (GNUNET_OK !=
        GNUNET_JSON_parse (value,
                           spec,
                           NULL,
                           NULL))
    {
      GNUNET_break_op (0);
      return NULL;
    }
    GNUNET_CRYPTO_hash_from_string (key,
                                    &h_key);

    if (NULL != (current_entry =
		 GNUNET_CONTAINER_multihashmap_get (map,
						    &h_key)))
    {
      /* The map already knows this h_contract_terms*/
      if ( (GNUNET_SYSERR ==
            TALER_amount_add (&current_entry->deposit_value,
                              &current_entry->deposit_value,
                              &iter_value)) ||
           (GNUNET_SYSERR ==
            TALER_amount_add (&current_entry->deposit_fee,
                              &current_entry->deposit_fee,
                              &iter_fee)) )
      {
        GNUNET_JSON_parse_free (spec);
        goto cleanup;
      }
    }
    else
    {
      /* First time in the map for this h_contract_terms*/
      current_entry = GNUNET_new (struct Entry);
      current_entry->deposit_value = iter_value;
      current_entry->deposit_fee = iter_fee;

      if (GNUNET_SYSERR ==
          GNUNET_CONTAINER_multihashmap_put (map,
                                             &h_key,
                                             current_entry,
                                             GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
      {
        GNUNET_JSON_parse_free (spec);
        goto cleanup;
      }
    }
    GNUNET_JSON_parse_free (spec);
  }
  rctx->deposits_response = json_array ();

  if (GNUNET_SYSERR ==
      GNUNET_CONTAINER_multihashmap_iterate (map,
                                             &build_deposits_response,
                                             rctx))
    goto cleanup;

  result_mod = json_copy ((struct json_t *) result);
  json_object_del (result_mod,
                   "deposits");
  json_object_set_new (result_mod,
                       "deposits_sums",
                       rctx->deposits_response);
  rctx->deposits_response = NULL;
 cleanup:
  GNUNET_CONTAINER_multihashmap_iterate (map,
                                         &hashmap_free,
                                         NULL);
  GNUNET_CONTAINER_multihashmap_destroy (map);
  return result_mod;
}


/**
 * Resume the given /track/transfer operation and send the given response.
 * Stores the response in the @a rctx and signals MHD to resume
 * the connection.  Also ensures MHD runs immediately.
 *
 * @param rctx transfer tracking context
 * @param response_code response code to use
 * @param response response data to send back
 */
static void
resume_track_transfer_with_response (struct TrackTransferContext *rctx,
                                     unsigned int response_code,
                                     struct MHD_Response *response)
{
  rctx->response_code = response_code;
  rctx->response = response;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /track/transfer handling as exchange interaction is done (%u)\n",
              response_code);
  if (NULL != rctx->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (rctx->timeout_task);
    rctx->timeout_task = NULL;
  }
  MHD_resume_connection (rctx->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
}


/**
 * Custom cleanup routine for a `struct TrackTransferContext`.
 *
 * @param hc the `struct TrackTransferContext` to clean up.
 */
static void
track_transfer_cleanup (struct TM_HandlerContext *hc)
{
  struct TrackTransferContext *rctx = (struct TrackTransferContext *) hc;

  free_transfer_track_context (rctx);
}


/**
 * This function checks that the information about the coin which
 * was paid back by _this_ wire transfer matches what _we_ (the merchant)
 * knew about this coin.
 *
 * @param cls closure with our `struct TrackTransferContext *`
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param exchange_url URL of the exchange that issued @a coin_pub
 * @param amount_with_fee amount the exchange will transfer for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param refund_fee fee the exchange will charge for refunding this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
check_transfer (void *cls,
                const struct GNUNET_HashCode *h_contract_terms,
                const struct TALER_CoinSpendPublicKeyP *coin_pub,
                const char *exchange_url,
                const struct TALER_Amount *amount_with_fee,
                const struct TALER_Amount *deposit_fee,
                const struct TALER_Amount *refund_fee,
                const struct TALER_Amount *wire_fee,
                const json_t *exchange_proof)
{
  struct TrackTransferContext *rctx = cls;
  const struct TALER_TrackTransferDetails *ttd = rctx->current_detail;

  if (GNUNET_SYSERR == rctx->check_transfer_result)
    return; /* already had a serious issue; odd that we're called more than once as well... */
  if ( (0 != TALER_amount_cmp (amount_with_fee,
                               &ttd->coin_value)) ||
       (0 != TALER_amount_cmp (deposit_fee,
                               &ttd->coin_fee)) )
  {
    /* Disagreement between the exchange and us about how much this
       coin is worth! */
    GNUNET_break_op (0);
    rctx->check_transfer_result = GNUNET_SYSERR;
    /* Build the `TrackTransferConflictDetails` */
    rctx->response
      = TMH_RESPONSE_make_json_pack ("{s:I, s:s, s:o, s:I, s:o, s:o, s:s, s:o, s:o}",
                                     "code", (json_int_t) TALER_EC_TRACK_TRANSFER_CONFLICTING_REPORTS,
                                     "hint", "disagreement about deposit valuation",
                                     "exchange_deposit_proof", exchange_proof,
                                     "conflict_offset", (json_int_t) rctx->current_offset,
                                     "exchange_transfer_proof", rctx->original_response,
                                     "coin_pub", GNUNET_JSON_from_data_auto (coin_pub),
                                     "h_contract_terms", GNUNET_JSON_from_data_auto (&ttd->h_contract_terms),
                                     "amount_with_fee", TALER_JSON_from_amount (amount_with_fee),
                                     "deposit_fee", TALER_JSON_from_amount (deposit_fee));
    return;
  }
  rctx->check_transfer_result = GNUNET_OK;
}


/**
 * Check that the given @a wire_fee is what the
 * @a exchange_pub should charge at the @a execution_time.
 * If the fee is correct (according to our database),
 * return #GNUNET_OK.  If we do not have the fee structure
 * in our DB, we just accept it and return #GNUNET_NO;
 * if we have proof that the fee is bogus, we respond with
 * the proof to the client and return #GNUNET_SYSERR.
 *
 * @param rctx context of the transfer to respond to
 * @param json response from the exchange
 * @param execution_time time of the wire transfer
 * @param wire_fee fee claimed by the exchange
 * @return #GNUNET_SYSERR if we returned hard proof of
 *   missbehavior from the exchange to the client
 */
static int
check_wire_fee (struct TrackTransferContext *rctx,
                const json_t *json,
                struct GNUNET_TIME_Absolute execution_time,
                const struct TALER_Amount *wire_fee)
{
  const struct TALER_MasterPublicKeyP *master_pub;
  struct GNUNET_HashCode h_wire_method;
  struct TALER_Amount expected_fee;
  struct TALER_Amount closing_fee;
  struct TALER_MasterSignatureP master_sig;
  struct GNUNET_TIME_Absolute start_date;
  struct GNUNET_TIME_Absolute end_date;
  enum GNUNET_DB_QueryStatus qs;
  const struct TALER_EXCHANGE_Keys *keys;

  keys = TALER_EXCHANGE_get_keys (rctx->eh);
  if (NULL == keys)
  {
    GNUNET_break (0);
    return GNUNET_NO;
  }
  master_pub = &keys->master_pub;
  GNUNET_CRYPTO_hash (rctx->wire_method,
                      strlen (rctx->wire_method) + 1,
                      &h_wire_method);
  db->preflight (db->cls);
  qs = db->lookup_wire_fee (db->cls,
                            master_pub,
                            &h_wire_method,
                            execution_time,
                            &expected_fee,
                            &closing_fee,
                            &start_date,
                            &end_date,
                            &master_sig);
  if (0 >= qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to find wire fee for `%s' and method `%s' at %s in DB, accepting blindly that the fee is %s\n",
                TALER_B2S (master_pub),
                rctx->wire_method,
                GNUNET_STRINGS_absolute_time_to_string (execution_time),
                TALER_amount2s (wire_fee));
    return GNUNET_NO;
  }
  if (0 <= TALER_amount_cmp (&expected_fee,
                             wire_fee))
    return GNUNET_OK; /* expected_fee >= wire_fee */

  /* Wire fee check failed, export proof to client */
  resume_track_transfer_with_response
    (rctx,
     MHD_HTTP_INTERNAL_SERVER_ERROR,
     TMH_RESPONSE_make_json_pack ("{s:I, s:o, s:o, s:o, s:o, s:o, s:o, s:o, s:o, s:O}",
                                  "code", (json_int_t) TALER_EC_TRACK_TRANSFER_JSON_BAD_WIRE_FEE,
                                  "wire_fee", TALER_JSON_from_amount (wire_fee),
                                  "execution_time", GNUNET_JSON_from_time_abs (execution_time),
                                  "expected_wire_fee", TALER_JSON_from_amount (&expected_fee),
                                  "expected_closing_fee", TALER_JSON_from_amount (&closing_fee),
                                  "start_date", GNUNET_JSON_from_time_abs (start_date),
                                  "end_date", GNUNET_JSON_from_time_abs (end_date),
                                  "master_sig", GNUNET_JSON_from_data_auto (&master_sig),
                                  "master_pub", GNUNET_JSON_from_data_auto (master_pub),
                                  "json", json));
  return GNUNET_SYSERR;
}


/**
 * Function called with detailed wire transfer data, including all
 * of the coin transactions that were combined into the wire transfer.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param ec taler-specific error code for the operation, #TALER_EC_NONE on success
 * @param exchange_pub public key of the exchange used to sign @a json
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param h_wire hash of the wire transfer address the transfer went to, or NULL on error
 * @param execution_time time when the exchange claims to have performed the wire transfer
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param wire_fee wire fee that was charged by the exchange
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
static void
wire_transfer_cb (void *cls,
                  unsigned int http_status,
                  enum TALER_ErrorCode ec,
                  const struct TALER_ExchangePublicKeyP *exchange_pub,
                  const json_t *json,
                  const struct GNUNET_HashCode *h_wire,
                  struct GNUNET_TIME_Absolute execution_time,
                  const struct TALER_Amount *total_amount,
                  const struct TALER_Amount *wire_fee,
                  unsigned int details_length,
                  const struct TALER_TrackTransferDetails *details)
{
  struct TrackTransferContext *rctx = cls;
  json_t *jresponse;
  enum GNUNET_DB_QueryStatus qs;

  rctx->wdh = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Got response code %u from exchange for /track/transfer\n",
              http_status);
  if (MHD_HTTP_OK != http_status)
  {
    resume_track_transfer_with_response
      (rctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack ("{s:I, s:I, s:I, s:O}",
                                    "code", (json_int_t) TALER_EC_TRACK_TRANSFER_EXCHANGE_ERROR,
                                    "exchange-code", (json_int_t) ec,
                                    "exchange-http-status", (json_int_t) http_status,
                                    "details", json));
    return;
  }
  for (unsigned int i=0;i<MAX_RETRIES;i++)
  {
    db->preflight (db->cls);
    qs = db->store_transfer_to_proof (db->cls,
                                      rctx->url,
                                      &rctx->wtid,
                                      execution_time,
                                      exchange_pub,
                                      json);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    /* Special report if retries insufficient */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    resume_track_transfer_with_response
      (rctx,
       MHD_HTTP_INTERNAL_SERVER_ERROR,
       TMH_RESPONSE_make_json_pack ("{s:I, s:s}",
                                    "code", (json_int_t) TALER_EC_TRACK_TRANSFER_DB_STORE_TRANSFER_ERROR,
                                    "details", "failed to store response from exchange to local database"));
    return;
  }
  rctx->original_response = json;

  if (GNUNET_SYSERR ==
      check_wire_fee (rctx,
                      json,
                      execution_time,
                      wire_fee))
    return;

  /* Now we want to double-check that any (Taler coin) deposit
   * which is accounted into _this_ wire transfer, does exist
   * into _our_ database.  This is the rationale: if the
   * exchange paid us for it, we must have received it _beforehands_!
   *
   * details_length is how many (Taler coin) deposits have been
   * aggregated into _this_ wire transfer.
   */
  for (unsigned int i=0;i<details_length;i++)
  {
    rctx->current_offset = i;
    rctx->current_detail = &details[i];
    /* Set the coin as "never seen" before. */
    rctx->check_transfer_result = GNUNET_NO;
    db->preflight (db->cls);
    qs = db->find_payments_by_hash_and_coin (db->cls,
                                             &details[i].h_contract_terms,
                                             &rctx->mi->pubkey,
                                             &details[i].coin_pub,
                                             &check_transfer,
                                             rctx);
    if (0 > qs)
    {
      /* single, read-only SQL statements should never cause
	 serialization problems */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      resume_track_transfer_with_response
        (rctx,
         MHD_HTTP_INTERNAL_SERVER_ERROR,
         TMH_RESPONSE_make_json_pack ("{s:I, s:s}",
                                      "code", (json_int_t) TALER_EC_TRACK_TRANSFER_DB_FETCH_DEPOSIT_ERROR,
                                      "details", "failed to obtain deposit data from local database"));
      return;
    }
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    {
      /* The exchange says we made this deposit, but WE do not
         recall making it (corrupted / unreliable database?)!
         Well, let's say thanks and accept the money! */
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Failed to find payment data in DB\n");
      rctx->check_transfer_result = GNUNET_OK;
    }
    if (GNUNET_NO == rctx->check_transfer_result)
    {
      /* Internal error: how can we have called #check_transfer()
         but still have no result? */
      GNUNET_break (0);
      resume_track_transfer_with_response
        (rctx,
         MHD_HTTP_INTERNAL_SERVER_ERROR,
         TMH_RESPONSE_make_json_pack ("{s:I, s:s, s:I, s:s}",
                                      "code", (json_int_t) TALER_EC_TRACK_TRANSFER_DB_INTERNAL_LOGIC_ERROR,
                                      "details", "internal logic error",
                                      "line", (json_int_t) __LINE__,
                                      "file", __FILE__));
      return;
    }
    if (GNUNET_SYSERR == rctx->check_transfer_result)
    {
      /* #check_transfer() failed, report conflict! */
      GNUNET_break_op (0);
      GNUNET_assert (NULL != rctx->response);
      resume_track_transfer_with_response
        (rctx,
         MHD_HTTP_FAILED_DEPENDENCY,
         rctx->response);
      rctx->response = NULL;
      return;
    }
    /* Response is consistent with the /deposit we made,
       remember it for future reference */
    for (unsigned int r=0;r<MAX_RETRIES;r++)
    {
      db->preflight (db->cls);
      qs = db->store_coin_to_transfer (db->cls,
                                       &details[i].h_contract_terms,
                                       &details[i].coin_pub,
                                       &rctx->wtid);
      if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
        break;
    }
    if (0 > qs)
    {
      /* Special report if retries insufficient */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      resume_track_transfer_with_response
        (rctx,
         MHD_HTTP_INTERNAL_SERVER_ERROR,
         TMH_RESPONSE_make_json_pack ("{s:I, s:s}",
                                      "code", (json_int_t) TALER_EC_TRACK_TRANSFER_DB_STORE_COIN_ERROR,
                                      "details", "failed to store response from exchange to local database"));
      return;
    }
  }
  rctx->original_response = NULL;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "About to call tracks transformator.\n");

  if (NULL == (jresponse =
               transform_response (json,
                                   rctx)))
  {
    resume_track_transfer_with_response
      (rctx,
       MHD_HTTP_INTERNAL_SERVER_ERROR,
       TMH_RESPONSE_make_error (TALER_EC_TRACK_TRANSFER_JSON_RESPONSE_ERROR,
                                "Fail to elaborate the response."));
    return;
  }

  resume_track_transfer_with_response (rctx,
                                       MHD_HTTP_OK,
                                       TMH_RESPONSE_make_json (jresponse));
  json_decref (jresponse);
}


/**
 * Function called with the result of our exchange lookup.
 *
 * @param cls the `struct TrackTransferContext`
 * @param eh NULL if exchange was not found to be acceptable
 * @param wire_fee NULL (we did not specify a wire method)
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_track_transfer_with_exchange (void *cls,
                                      struct TALER_EXCHANGE_Handle *eh,
                                      const struct TALER_Amount *wire_fee,
                                      int exchange_trusted)
{
  struct TrackTransferContext *rctx = cls;

  rctx->fo = NULL;
  rctx->eh = eh;
  rctx->wdh = TALER_EXCHANGE_track_transfer (eh,
                                             &rctx->wtid,
                                             &wire_transfer_cb,
                                             rctx);
  if (NULL == rctx->wdh)
  {
    GNUNET_break (0);
    resume_track_transfer_with_response
      (rctx,
       MHD_HTTP_INTERNAL_SERVER_ERROR,
       TMH_RESPONSE_make_json_pack ("{s:I, s:s}",
				    "code", (json_int_t) TALER_EC_TRACK_TRANSFER_REQUEST_ERROR,
                                    "error", "failed to run /track/transfer on exchange"));
  }
}


/**
 * Handle a timeout for the processing of the track transfer request.
 *
 * @param cls closure
 */
static void
handle_track_transfer_timeout (void *cls)
{
  struct TrackTransferContext *rctx = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /track/transfer with error after timeout\n");
  rctx->timeout_task = NULL;

  if (NULL != rctx->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (rctx->fo);
    rctx->fo = NULL;
  }
  resume_track_transfer_with_response (rctx,
                                       MHD_HTTP_SERVICE_UNAVAILABLE,
                                       TMH_RESPONSE_make_error (TALER_EC_TRACK_TRANSFER_EXCHANGE_TIMEOUT,
									 "exchange not reachable"));
}


/**
 * Function called with information about a wire transfer identifier.
 * Generate a response based on the given @a proof.
 *
 * @param cls closure
 * @param proof proof from exchange about what the wire transfer was for.
 *              should match the `TrackTransactionResponse` format
 *              of the exchange
 */
static void
proof_cb (void *cls,
          const json_t *proof)
{
  struct TrackTransferContext *rctx = cls;
  json_t *transformed_response;

  if (NULL == (transformed_response =
	       transform_response (proof,
				   rctx)))
  {
    rctx->response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
    rctx->response
      = TMH_RESPONSE_make_error (TALER_EC_TRACK_TRANSFER_JSON_RESPONSE_ERROR,
				 "Fail to elaborate response.");
    return;
  }

  rctx->response_code = MHD_HTTP_OK;
  rctx->response = TMH_RESPONSE_make_json (transformed_response);
  json_decref (transformed_response);
}


/**
 * Manages a /track/transfer call, thus it calls the /track/wtid
 * offered by the exchange in order to return the set of transfers
 * (of coins) associated with a given wire transfer.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_track_transfer (struct TMH_RequestHandler *rh,
                           struct MHD_Connection *connection,
                           void **connection_cls,
                           const char *upload_data,
                           size_t *upload_data_size)
{
  struct TrackTransferContext *rctx;
  const char *str;
  const char *url;
  const char *instance_str;
  const char *wire_method;
  int ret;
  enum GNUNET_DB_QueryStatus qs;

  if (NULL == *connection_cls)
  {
    rctx = GNUNET_new (struct TrackTransferContext);
    rctx->hc.cc = &track_transfer_cleanup;
    rctx->connection = connection;
    *connection_cls = rctx;
  }
  else
  {
    /* not first call, recover state */
    rctx = *connection_cls;
  }

  if (0 != rctx->response_code)
  {
    /* We are *done* processing the request, just queue the response (!) */
    if (UINT_MAX == rctx->response_code)
    {
      GNUNET_break (0);
      return MHD_NO; /* hard error */
    }
    ret = MHD_queue_response (connection,
                              rctx->response_code,
                              rctx->response);
    if (NULL != rctx->response)
    {
      MHD_destroy_response (rctx->response);
      rctx->response = NULL;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Queueing response (%u) for /track/transfer (%s).\n",
                (unsigned int) rctx->response_code,
                ret ? "OK" : "FAILED");
    return ret;
  }
  if ( (NULL != rctx->fo) ||
       (NULL != rctx->eh) )
  {
    /* likely old MHD version */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Not sure why we are here, should be suspended\n");
    return MHD_YES; /* still work in progress */
  }

  url = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "exchange");
  if (NULL == url)
    return TMH_RESPONSE_reply_arg_missing (connection,
                                           TALER_EC_PARAMETER_MISSING,
                                           "exchange");
  rctx->url = GNUNET_strdup (url);

  /* FIXME: change again: we probably don't want the wire_method
     but rather the _account_ (section) here! */
  wire_method = MHD_lookup_connection_value (connection,
                                             MHD_GET_ARGUMENT_KIND,
                                             "wire_method");
  if (NULL == wire_method)
  {
    if (1)
    {
      /* temporary work-around until demo is adjusted... */
      GNUNET_break (0);
      wire_method = "x-taler-bank";
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  "Client needs fixing, see API change for #4943!\n");
    }
    else
      return TMH_RESPONSE_reply_arg_missing (connection,
					     TALER_EC_PARAMETER_MISSING,
					     "wire_method");
  }
  rctx->wire_method = GNUNET_strdup (wire_method);

  instance_str = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "instance");
  if (NULL == instance_str)
    instance_str = "default";

  rctx->mi = TMH_lookup_instance (instance_str);
  if (NULL == rctx->mi)
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_TRACK_TRANSFER_INSTANCE_UNKNOWN,
                                         "instance unknown");
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "wtid");
  if (NULL == str)
    return TMH_RESPONSE_reply_arg_missing (connection,
                                           TALER_EC_PARAMETER_MISSING,
                                           "wtid");
  if (GNUNET_OK !=
      GNUNET_STRINGS_string_to_data (str,
                                     strlen (str),
                                     &rctx->wtid,
                                     sizeof (rctx->wtid)))
  {
    return TMH_RESPONSE_reply_arg_invalid (connection,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "wtid");
  }

  /* Check if reply is already in database! */
  db->preflight (db->cls);
  qs = db->find_proof_by_wtid (db->cls,
                               rctx->url,
                               &rctx->wtid,
                               &proof_cb,
                               rctx);
  if (0 > qs)
  {
    /* Simple select queries should not cause serialization issues */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_TRACK_TRANSFER_DB_FETCH_FAILED,
                                              "Fail to query database about proofs");
  }
  if (0 != rctx->response_code)
  {
    ret = MHD_queue_response (connection,
                              rctx->response_code,
                              rctx->response);
    if (NULL != rctx->response)
    {
      MHD_destroy_response (rctx->response);
      rctx->response = NULL;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Queueing response (%u) for /track/transfer (%s).\n",
                (unsigned int) rctx->response_code,
                ret ? "OK" : "FAILED");
    return ret;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /track/transfer handling while working with the exchange\n");
  MHD_suspend_connection (connection);
  rctx->fo = TMH_EXCHANGES_find_exchange (url,
                                          NULL,
                                          &process_track_transfer_with_exchange,
                                          rctx);
  rctx->timeout_task
    = GNUNET_SCHEDULER_add_delayed (TRACK_TIMEOUT,
                                    &handle_track_transfer_timeout,
                                    rctx);
  return MHD_YES;
}

/* end of taler-merchant-httpd_track-transfer.c */
