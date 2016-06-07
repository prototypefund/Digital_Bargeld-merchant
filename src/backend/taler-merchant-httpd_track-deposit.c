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
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_track-deposit.c
 * @brief implement API for tracking deposits and wire transfers
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
#include "taler-merchant-httpd_track-deposit.h"


/**
 * How long to wait before giving up processing with the exchange?
 */
#define TRACK_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))


/**
 * Context used for handing /track/deposit requests.
 */
struct DepositTrackContext
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
   * Handle for the /wire/deposits request.
   */
  struct TALER_EXCHANGE_WireDepositsHandle *wdh;

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
   * checking in #check_deposit().
   */
  const struct TALER_WireDepositDetails *current_detail;

  /**
   * Argument for the /wire/deposits request.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Response code to return.
   */
  unsigned int response_code;

  /**
   * #GNUNET_NO if we did not find a matching coin.
   * #GNUNET_SYSERR if we found a matching coin, but the amounts do not match.
   * #GNUNET_OK if we did find a matching coin.
   */
  int check_deposit_result;
};


/**
 * Free the @a rctx.
 *
 * @param rctx data to free
 */
static void
free_deposit_track_context (struct DepositTrackContext *rctx)
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
  if (NULL != rctx->uri)
  {
    GNUNET_free (rctx->uri);
    rctx->uri = NULL;
  }
  GNUNET_free (rctx);
}


/**
 * Resume the given /track/deposit operation and send the given response.
 * Stores the response in the @a rctx and signals MHD to resume
 * the connection.  Also ensures MHD runs immediately.
 *
 * @param rctx deposit tracking context
 * @param response_code response code to use
 * @param response response data to send back
 */
static void
resume_track_deposit_with_response (struct DepositTrackContext *rctx,
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
 * Custom cleanup routine for a `struct DepositTrackContext`.
 *
 * @param hc the `struct DepositTrackContext` to clean up.
 */
static void
track_deposit_cleanup (struct TM_HandlerContext *hc)
{
  struct DepositTrackContext *rctx = (struct DepositTrackContext *) hc;

  free_deposit_track_context (rctx);
}


/**
 * Function called with information about a coin that was deposited.
 * Verify that it matches the information claimed by the exchange.
 * Update the `check_deposit_result` field accordingly.
 *
 * @param cls closure with our `struct DepositTrackContext *`
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
check_deposit (void *cls,
               uint64_t transaction_id,
               const struct TALER_CoinSpendPublicKeyP *coin_pub,
               const struct TALER_Amount *amount_with_fee,
               const struct TALER_Amount *deposit_fee,
               const json_t *exchange_proof)
{
  struct DepositTrackContext *rctx = cls;
  const struct TALER_WireDepositDetails *wdd = rctx->current_detail;

  if (0 != memcmp (&wdd->coin_pub,
                   coin_pub,
                   sizeof (struct TALER_CoinSpendPublicKeyP)))
    return; /* not the coin we're looking for */
  if ( (0 != TALER_amount_cmp (amount_with_fee,
                               &wdd->coin_value)) ||
       (0 != TALER_amount_cmp (deposit_fee,
                               &wdd->coin_fee)) )
  {
    /* Disagreement between the exchange and us how much this
       coin is worth! */
    GNUNET_break_op (0);
    rctx->check_deposit_result = GNUNET_SYSERR;
    return;
  }
  rctx->check_deposit_result = GNUNET_OK;
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
 * @param wtid extracted wire transfer identifier, or NULL if the exchange could
 *             not provide any (set only if @a http_status is #MHD_HTTP_OK)
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
static void
wire_deposit_cb (void *cls,
                 unsigned int http_status,
                 const struct TALER_ExchangePublicKeyP *exchange_pub,
                 const json_t *json,
                 const struct GNUNET_HashCode *h_wire,
                 const struct TALER_Amount *total_amount,
                 unsigned int details_length,
                 const struct TALER_WireDepositDetails *details)
{
  struct DepositTrackContext *rctx = cls;
  unsigned int i;
  int ret;

  rctx->wdh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    resume_track_deposit_with_response
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
                                   exchange_pub,
                                   json))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to persist wire transfer proof in DB\n");
  }

  for (i=0;i<details_length;i++)
  {
    rctx->current_detail = &details[i];
    rctx->check_deposit_result = GNUNET_NO;
    ret = db->find_payments_by_id (rctx,
                                   details[i].transaction_id,
                                   &check_deposit,
                                   rctx);
    if (GNUNET_SYSERR == ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to verify existing payment data in DB\n");
    }
    if ( (GNUNET_NO == ret) ||
         (GNUNET_NO == rctx->check_deposit_result) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Failed to find payment data in DB\n");
    }
    if (GNUNET_SYSERR == rctx->check_deposit_result)
    {
      /* #check_deposit() failed, do something! */
      GNUNET_break (0);
      /* FIXME: generate nicer custom response */
      resume_track_deposit_with_response
        (rctx,
         MHD_HTTP_FAILED_DEPENDENCY,
         TMH_RESPONSE_make_json_pack ("{s:I, s:O}",
                                      "index", (json_int_t) i,
                                      "details", json));
      return;
    }
    ret = db->store_coin_to_transfer (db->cls,
                                      details[i].transaction_id,
                                      &details[i].coin_pub,
                                      &rctx->wtid);
    if (GNUNET_OK != ret)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to persist coin to wire transfer mapping in DB\n");
    }
  }
  /* FIXME: might want a more custom response here... */
  resume_track_deposit_with_response
    (rctx,
     MHD_HTTP_OK,
     TMH_RESPONSE_make_json_pack ("{s:I, s:O}",
                                  "exchange_status", (json_int_t) http_status,
                                  "details", json));
}


/**
 * Function called with the result of our exchange lookup.
 *
 * @param cls the `struct DepositTrackContext`
 * @param eh NULL if exchange was not found to be acceptable
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_track_deposit_with_exchange (void *cls,
                                     struct TALER_EXCHANGE_Handle *eh,
                                     int exchange_trusted)
{
  struct DepositTrackContext *rctx = cls;

  rctx->fo = NULL;
  rctx->eh = eh;
  rctx->wdh = TALER_EXCHANGE_wire_deposits (eh,
                                            &rctx->wtid,
                                            &wire_deposit_cb,
                                            rctx);
}


/**
 * Handle a timeout for the processing of the track deposit request.
 *
 * @param cls closure
 */
static void
handle_track_deposit_timeout (void *cls)
{
  struct DepositTrackContext *rctx = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /track/deposit with error after timeout\n");
  rctx->timeout_task = NULL;

  if (NULL != rctx->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (rctx->fo);
    rctx->fo = NULL;
  }
  resume_track_deposit_with_response (rctx,
                                      MHD_HTTP_SERVICE_UNAVAILABLE,
                                      TMH_RESPONSE_make_internal_error ("exchange not reachable"));
}


/**
 * Function called with information about a wire transfer identifier.
 * Generate a response based on the given @a proof.
 *
 * @param cls closure
 * @param proof proof from exchange about what the wire transfer was for
 */
static void
proof_cb (void *cls,
          const json_t *proof)
{
  struct DepositTrackContext *rctx = cls;

  rctx->response_code = MHD_HTTP_OK;
  /* FIXME: might want a more custom response here... */
  rctx->response = TMH_RESPONSE_make_json_pack ("{s:I, s:O}",
                                                "exchange_status", (json_int_t) MHD_HTTP_OK,
                                                "details", proof);
}


/**
 * Manages a /track/deposit call, thus it calls the /track/wtid
 * offered by the exchange in order to return the set of deposits
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
MH_handler_track_deposit (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  struct DepositTrackContext *rctx;
  const char *str;
  const char *uri;
  int ret;

  if (NULL == *connection_cls)
  {
    rctx = GNUNET_new (struct DepositTrackContext);
    rctx->hc.cc = &track_deposit_cleanup;
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
                "Queueing response (%u) for /track/deposit (%s).\n",
                (unsigned int) rctx->response_code,
                ret ? "OK" : "FAILED");
    return ret;
  }

  uri = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "exchange");
  if (NULL == uri)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "exchange argument missing");
  rctx->uri = GNUNET_strdup (uri);

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
    return ret;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /track/deposit handling while working with the exchange\n");
  MHD_suspend_connection (connection);
  rctx->fo = TMH_EXCHANGES_find_exchange (uri,
                                          &process_track_deposit_with_exchange,
                                          rctx);
  rctx->timeout_task = GNUNET_SCHEDULER_add_delayed (TRACK_TIMEOUT,
                                                     &handle_track_deposit_timeout,
                                                     rctx);
  return MHD_NO;
}

/* end of taler-merchant-httpd_track-deposit.c */
