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
 * @file lib/merchant_api_contract.c
 * @brief Implementation of the /history request of the merchant's HTTP API
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_json_lib.h>


/**
 * @brief A Contract Operation Handle
 */
struct TALER_MERCHANT_HistoryOperation
{

  /**
   * The url for this request, including parameters.
   */
  char *url;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_HistoryOperationCallback cb;

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
 * Cancel a pending /history request
 *
 * @param handle from the operation to cancel
 */
void
TALER_MERCHANT_history_cancel (struct TALER_MERCHANT_HistoryOperation *ho)
{
  if (NULL != ho->job)
  {
    GNUNET_CURL_job_cancel (ho->job);
    ho->job = NULL;
  }
  GNUNET_free (ho->url);
  GNUNET_free (ho);
}


/**
 * Function called when we're done processing the
 * HTTP /history request.
 *
 * @param cls the `struct TALER_MERCHANT_TrackTransactionHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
history_raw_cb (void *cls,
                long response_code,
                const json_t *json)
{
  struct TALER_MERCHANT_HistoryOperation *ho = cls;

  ho->job = NULL;

  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    ho->cb (ho->cb_cls,
            MHD_HTTP_OK,
	    TALER_EC_NONE,
            json);
    return;
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		"/history URI not found\n");
    break;
  case MHD_HTTP_BAD_REQUEST:
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		"Wrong/missing URL parameter\n");
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
  ho->cb (ho->cb_cls,
          response_code,
	  TALER_JSON_get_error_code (json),
          json);
}


/**
 * Issue a /history request to the backend.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param date only transactions younger than/equals to date will be returned
 * @param history_cb callback which will work the response gotten from the backend
 * @param history_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_HistoryOperation *
TALER_MERCHANT_history (struct GNUNET_CURL_Context *ctx,
                        const char *backend_uri,
                        const char *instance,
                        struct GNUNET_TIME_Absolute date,
                        TALER_MERCHANT_HistoryOperationCallback history_cb,
                        void *history_cb_cls)
{
  struct TALER_MERCHANT_HistoryOperation *ho;
  uint64_t seconds;
  CURL *eh;

  ho = GNUNET_new (struct TALER_MERCHANT_HistoryOperation);
  ho->ctx = ctx;
  ho->cb = history_cb;
  ho->cb_cls = history_cb_cls;
  seconds = date.abs_value_us / 1000LL / 1000LL;
  GNUNET_asprintf (&ho->url,
                   "%s/history?date=%llu&instance=%s",
                   backend_uri,
                   seconds,
                   instance);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    ho->url))
  {
    GNUNET_break (0);  
    return NULL;
  }    

  if (NULL == (ho->job = GNUNET_CURL_job_add (ctx,
                                              eh,
                                              GNUNET_YES,
                                              &history_raw_cb,
                                              ho)))
  {
    GNUNET_break (0);
    return NULL;
  }
  return ho;
}                        


/* end of merchant_api_contract.c */
