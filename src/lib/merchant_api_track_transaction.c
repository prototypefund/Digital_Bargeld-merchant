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
  TALER; see the file COPYING.LGPL.  If not, If not, see
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
   * base32 identifier being the 'witd' parameter required by the
   * exchange
   */
  char *wtid;

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
 * Function called when we're done processing the
 * HTTP /track/transaction request.
 *
 * @param cls the `struct TALER_MERCHANT_TrackTransactionHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_tracktransaction_finished (void *cls,
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
    {
    /* Work out argument for external callback from the body .. */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "200 returned from /track/transaction\n");
    /* FIXME: actually verify signature */
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
  /* FIXME: figure out which parameters ought to be passed back */
  tdo->cb (tdo->cb_cls,
           response_code,
           json);
}


/**
 * Request backend to return transactions associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_uri base URI of the backend
 * @param wtid base32 string indicating a wtid
 * @param exchange base URL of the exchange in charge of returning the wanted information
 * @param track_transaction_cb the callback to call when a reply for this request is available
 * @param track_transaction_cb_cls closure for @a contract_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransactionHandle *
TALER_MERCHANT_track_transaction (struct GNUNET_CURL_Context *ctx,
                                  const char *backend_uri,
                                  uint64_t transaction_id,
                                  const char *exchange_uri,
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
                   "%s/track/transaction?transaction=%llu&exchange=%s",
                   backend_uri,
                   (unsigned long long) transaction_id,
                   exchange_uri);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tdo->url));
  tdo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_tracktransaction_finished,
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
  GNUNET_free (tdo->wtid);
  GNUNET_free (tdo);
}

/* end of merchant_api_track_transaction.c */
