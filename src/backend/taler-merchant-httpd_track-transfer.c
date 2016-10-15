/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

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
   * URI of the exchange.
   */
  char *uri;

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
  if (NULL != rctx->uri)
  {
    GNUNET_free (rctx->uri);
    rctx->uri = NULL;
  }
  GNUNET_free (rctx);
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
              "Resuming /track/transaction handling as exchange interaction is done (%u)\n",
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
 * Function called with information about a coin that was transfered.
 * Verify that it matches the information claimed by the exchange.
 * Update the `check_transfer_result` field accordingly.
 *
 * @param cls closure with our `struct TrackTransferContext *`
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will transfer for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
check_transfer (void *cls,
                uint64_t transaction_id,
                const struct TALER_CoinSpendPublicKeyP *coin_pub,
                const struct TALER_Amount *amount_with_fee,
                const struct TALER_Amount *deposit_fee,
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
      = TMH_RESPONSE_make_json_pack ("{s:s, s:O, s:I, s:O, s:o, s:I, s:o, s:o}",
                                     "hint", "disagreement about deposit valuation",
                                     "exchange_deposit_proof", exchange_proof,
                                     "conflict_offset", (json_int_t) rctx->current_offset,
                                     "exchange_transfer_proof", rctx->original_response,
                                     "coin_pub", GNUNET_JSON_from_data_auto (coin_pub),
                                     "transaction_id", (json_int_t) transaction_id,
                                     "amount_with_fee", TALER_JSON_from_amount (amount_with_fee),
                                     "deposit_fee", TALER_JSON_from_amount (deposit_fee));
    return;
  }
  rctx->check_transfer_result = GNUNET_OK;
}


/**
 * Function called with detailed wire transfer data, including all
 * of the coin transactions that were combined into the wire transfer.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param exchange_pub public key of the exchange used to sign @a json
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param h_wire hash of the wire transfer address the transfer went to, or NULL on error
 * @param execution_time time when the exchange claims to have performed the wire transfer
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
static void
wire_transfer_cb (void *cls,
                  unsigned int http_status,
                  const struct TALER_ExchangePublicKeyP *exchange_pub,
                  const json_t *json,
                  const struct GNUNET_HashCode *h_wire,
                  struct GNUNET_TIME_Absolute execution_time,
                  const struct TALER_Amount *total_amount,
                  unsigned int details_length,
                  const struct TALER_TrackTransferDetails *details)
{
  struct TrackTransferContext *rctx = cls;
  unsigned int i;
  int ret;

  rctx->wdh = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Got response code %u from exchange for /track/transfer\n",
              http_status);
  if (MHD_HTTP_OK != http_status)
  {
    resume_track_transfer_with_response
      (rctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack ("{s:I, s:O}",
                                    "exchange_status", (json_int_t) http_status,
                                    "details", json));
    return;
  }

  if (GNUNET_OK !=
      db->store_transfer_to_proof (db->cls,
                                   rctx->uri,
                                   &rctx->wtid,
                                   execution_time,
                                   exchange_pub,
                                   json))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to persist wire transfer proof in DB\n");
    resume_track_transfer_with_response
      (rctx,
       MHD_HTTP_INTERNAL_SERVER_ERROR,
       TMH_RESPONSE_make_json_pack ("{s:s}",
                                    "details", "failed to store response from exchange to local database"));
    return;
  }
  rctx->original_response = json;
  for (i=0;i<details_length;i++)
  {
    rctx->current_offset = i;
    rctx->current_detail = &details[i];
    rctx->check_transfer_result = GNUNET_NO;
    ret = db->find_payments_by_id_and_coin (db->cls,
                                            details[i].transaction_id,
                                            &rctx->mi->pubkey,
                                            &details[i].coin_pub,
                                            &check_transfer,
                                            rctx);
    if (GNUNET_SYSERR == ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to obtain existing payment data from DB\n");
      resume_track_transfer_with_response
        (rctx,
         MHD_HTTP_INTERNAL_SERVER_ERROR,
         TMH_RESPONSE_make_json_pack ("{s:s}",
                                      "details", "failed to obtain deposit data from local database"));
      return;
    }
    if (GNUNET_NO == ret)
    {
      /* The exchange says we made this deposit, but WE do not
         recall making it! Well, let's say thanks and accept the
         money! */
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
         TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:s}",
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
         MHD_HTTP_CONFLICT,
         rctx->response);
      rctx->response = NULL;
      return;
    }
    /* Response is consistent with the /deposit we made, remember
       it for future reference */
    ret = db->store_coin_to_transfer (db->cls,
                                      details[i].transaction_id,
                                      &details[i].coin_pub,
                                      &rctx->wtid);
    if (GNUNET_OK != ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to persist coin to wire transfer mapping in DB\n");
      resume_track_transfer_with_response
        (rctx,
         MHD_HTTP_INTERNAL_SERVER_ERROR,
         TMH_RESPONSE_make_json_pack ("{s:s}",
                                      "details", "failed to store response from exchange to local database"));
      return;
    }
  }
  rctx->original_response = NULL;
  resume_track_transfer_with_response
    (rctx,
     MHD_HTTP_OK,
     TMH_RESPONSE_make_json (json));
}


/**
 * Function called with the result of our exchange lookup.
 *
 * @param cls the `struct TrackTransferContext`
 * @param eh NULL if exchange was not found to be acceptable
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_track_transfer_with_exchange (void *cls,
                                      struct TALER_EXCHANGE_Handle *eh,
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
       TMH_RESPONSE_make_json_pack ("{s:s}",
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
                                       TMH_RESPONSE_make_internal_error ("exchange not reachable"));
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

  rctx->response_code = MHD_HTTP_OK;
  rctx->response = TMH_RESPONSE_make_json (proof);
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
  const char *uri;
  const char *receiver_str;
  int ret;

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

  uri = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "exchange");
  if (NULL == uri)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "exchange argument missing");
  rctx->uri = GNUNET_strdup (uri);

  receiver_str = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "receiver");
  if (NULL == receiver_str)
    receiver_str = "default";
  rctx->mi = TMH_lookup_instance (receiver_str);
  if (NULL == rctx->mi)
    return TMH_RESPONSE_reply_not_found (connection,
                                         "instance unknown");
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "wtid");
  if (NULL == str)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "wtid argument missing");
  if (GNUNET_OK !=
      GNUNET_STRINGS_string_to_data (str,
                                     strlen (str),
                                     &rctx->wtid,
                                     sizeof (rctx->wtid)))
  {
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "wtid argument malformed");
  }

  /* Check if reply is already in database! */
  ret = db->find_proof_by_wtid (db->cls,
                                rctx->uri,
                                &rctx->wtid,
                                &proof_cb,
                                rctx);
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
  rctx->fo = TMH_EXCHANGES_find_exchange (uri,
                                          &process_track_transfer_with_exchange,
                                          rctx);
  rctx->timeout_task
    = GNUNET_SCHEDULER_add_delayed (TRACK_TIMEOUT,
                                    &handle_track_transfer_timeout,
                                    rctx);
  return MHD_YES;
}

/* end of taler-merchant-httpd_track-transfer.c */
