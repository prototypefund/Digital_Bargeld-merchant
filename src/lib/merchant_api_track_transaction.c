/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_track_transaction.c
 * @brief Implementation of the /track/transaction request of the
 * merchant's HTTP API
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>


/**
 * @brief A handle for tracking transactions.
 */
struct TALER_MERCHANT_TrackTransactionHandle
{

  /**
   * The url for this request.
   */
  char *url;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_TrackTransactionCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;
};


/**
 * Free data in @a transfers.
 *
 * @param num_transfers length of the @a transfers array
 * @param transfers information about wire transfers to free
 */
static void
free_transfers (unsigned int num_transfers,
                struct TALER_MERCHANT_TransactionWireTransfer *transfers)
{
  unsigned int i;

  for (i=0;i<num_transfers;i++)
    GNUNET_free (transfers[i].coins);
}


/**
 * Handle #MHD_HTTP_OK response to /track/transaction.
 * Parse @a json and if successful call the callback in @a tdo.
 *
 * @param tdo handle of the operation
 * @param json json to parse
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
static int
parse_track_transaction_ok (struct TALER_MERCHANT_TrackTransactionHandle *tdo,
                            const json_t *json)
{
  unsigned int num_transfers = json_array_size (json);
  struct TALER_MERCHANT_TransactionWireTransfer transfers[num_transfers];
  unsigned int i;

  if (0 == num_transfers)
  {
    /* zero transfers is not a valid reply */
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  for (i=0;i<num_transfers;i++)
  {
    struct TALER_MERCHANT_TransactionWireTransfer *transfer = &transfers[i];
    json_t *coins;
    unsigned int j;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("wtid",
                                   &transfer->wtid),
      GNUNET_JSON_spec_absolute_time ("execution_time",
                                      &transfer->execution_time),
      GNUNET_JSON_spec_json ("coins",
                             &coins),
      GNUNET_JSON_spec_end()
    };

    if (GNUNET_OK !=
        GNUNET_JSON_parse (json_array_get (json, i),
                           spec,
                           NULL, NULL))
    {
      GNUNET_break_op (0);
      return GNUNET_SYSERR;
    }
    transfer->num_coins = json_array_size (coins);
    transfer->coins = GNUNET_new_array (transfer->num_coins,
                                        struct TALER_MERCHANT_CoinWireTransfer);
    for (j=0;j<transfer->num_coins;j++)
    {
      struct TALER_MERCHANT_CoinWireTransfer *coin = &transfer->coins[j];
      struct GNUNET_JSON_Specification coin_spec[] = {
        GNUNET_JSON_spec_fixed_auto ("coin_pub", &coin->coin_pub),
        TALER_JSON_spec_amount ("amount_with_fee", &coin->amount_with_fee),
        TALER_JSON_spec_amount ("deposit_fee", &coin->deposit_fee),
        GNUNET_JSON_spec_end()
      };
      if (GNUNET_OK !=
          GNUNET_JSON_parse (json_array_get (coins, j),
                             coin_spec,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        GNUNET_JSON_parse_free (spec);
        free_transfers (i,
                        transfers);
        return GNUNET_SYSERR;
      }
    }
    GNUNET_JSON_parse_free (spec);
  }
  tdo->cb (tdo->cb_cls,
           MHD_HTTP_OK,
	   TALER_EC_NONE,
           json,
           num_transfers,
           transfers);
  free_transfers (num_transfers,
                  transfers);
  TALER_MERCHANT_track_transaction_cancel (tdo);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /track/transaction request.
 *
 * @param cls the `struct TALER_MERCHANT_TrackTransactionHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_track_transaction_finished (void *cls,
                                   long response_code,
                                   const json_t *json)
{
  struct TALER_MERCHANT_TrackTransactionHandle *tdo = cls;

  tdo->job = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK ==
        parse_track_transaction_ok (tdo,
                                    json))
      return;
    response_code = 0;
    break;
  case MHD_HTTP_ACCEPTED:
    {
      /* Expect time stamp of when the transfer is supposed to happen */
    }
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "track transaction URI not found\n");
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                (unsigned int) response_code);
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  tdo->cb (tdo->cb_cls,
           response_code,
	   TALER_JSON_get_error_code (json),
           json,
           0,
           NULL);
  TALER_MERCHANT_track_transaction_cancel (tdo);
}


/**
 * Request backend to return transactions associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_uri base URI of the backend
 * @param receiver which merchant instance is going to be tracked
 * @param transaction_id which transaction should we trace
 * @param track_transaction_cb the callback to call when a reply for this request is available
 * @param track_transaction_cb_cls closure for @a track_transaction_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransactionHandle *
TALER_MERCHANT_track_transaction (struct GNUNET_CURL_Context *ctx,
                                  const char *backend_uri,
                                  const char *receiver,
                                  uint64_t transaction_id,
                                  TALER_MERCHANT_TrackTransactionCallback track_transaction_cb,
                                  void *track_transaction_cb_cls)
{
  struct TALER_MERCHANT_TrackTransactionHandle *tdo;
  CURL *eh;

  tdo = GNUNET_new (struct TALER_MERCHANT_TrackTransactionHandle);
  tdo->ctx = ctx;
  tdo->cb = track_transaction_cb;
  tdo->cb_cls = track_transaction_cb_cls;
  GNUNET_asprintf (&tdo->url,
                   "%s/track/transaction?id=%llu&receiver=%s",
                   backend_uri,
                   (unsigned long long) transaction_id,
                   receiver);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tdo->url));
  tdo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_track_transaction_finished,
                                  tdo);
  return tdo;
}


/**
 * Cancel a /track/transaction request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param co the transaction's tracking handle
 */
void
TALER_MERCHANT_track_transaction_cancel (struct TALER_MERCHANT_TrackTransactionHandle *tdo)
{
  if (NULL != tdo->job)
  {
    GNUNET_CURL_job_cancel (tdo->job);
    tdo->job = NULL;
  }
  GNUNET_free (tdo->url);
  GNUNET_free (tdo);
}

/* end of merchant_api_track_transaction.c */
