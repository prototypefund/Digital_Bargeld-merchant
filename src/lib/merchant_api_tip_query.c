/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

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
 * @file lib/merchant_api_tip_query.c
 * @brief Implementation of the /tip-query request of the merchant's HTTP API
 * @author Florian Dold
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
 * @brief A handle for tracking /tip-pickup operations
 */
struct TALER_MERCHANT_TipQueryOperation
{
  /**
   * The url for this request.
   */
  char *url;

  /**
   * JSON encoding of the request to POST.
   */
  char *json_enc;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_TipQueryCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Expected number of planchets.
   */
  unsigned int num_planchets;
};


/**
 * We got a 200 response back from the exchange (or the merchant).
 * Now we need to parse the response and if it is well-formed,
 * call the callback (and set it to NULL afterwards).
 *
 * @param tqo handle of the original operation
 * @param json cryptographic proof returned by the exchange/merchant
 * @return #GNUNET_OK if response is valid
 */
static int
check_ok (struct TALER_MERCHANT_TipQueryOperation *tqo,
          const json_t *json)
{
  struct GNUNET_TIME_Absolute reserve_expiration;
  struct TALER_Amount amount_authorized;
  struct TALER_Amount amount_available;
  struct TALER_Amount amount_picked_up;
  struct TALER_ReservePublicKeyP reserve_pub;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("reserve_pub", &reserve_pub),
    GNUNET_JSON_spec_absolute_time ("reserve_expiration", &reserve_expiration),
    TALER_JSON_spec_amount ("amount_authorized", &amount_authorized),
    TALER_JSON_spec_amount ("amount_available", &amount_available),
    TALER_JSON_spec_amount ("amount_picked_up", &amount_picked_up),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  tqo->cb (tqo->cb_cls,
           MHD_HTTP_OK,
           TALER_JSON_get_error_code (json),
           json,
           reserve_expiration,
           &reserve_pub,
           &amount_authorized,
           &amount_available,
           &amount_picked_up);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /track/transaction request.
 *
 * @param cls the `struct TALER_MERCHANT_TipQueryOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_tip_query_finished (void *cls,
                           long response_code,
                           const void *response)
{
  struct TALER_MERCHANT_TipQueryOperation *tqo = cls;
  const json_t *json = response;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /tip-query response with status code %u\n",
              (unsigned int) response_code);

  tqo->job = NULL;
  switch (response_code)
  {
  case MHD_HTTP_OK:
    if (GNUNET_OK != check_ok (tqo,
                               json))
    {
      GNUNET_break_op (0);
      response_code = 0;
    }
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* legal, can happen if instance or tip reserve is unknown */
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
  if (MHD_HTTP_OK != response_code)
    tqo->cb (tqo->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             json,
             GNUNET_TIME_UNIT_ZERO_ABS,
             NULL,
             NULL,
             NULL,
             NULL);
  TALER_MERCHANT_tip_query_cancel (tqo);
}


/**
 * Issue a /tip-query request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipQueryOperation *
TALER_MERCHANT_tip_query (struct GNUNET_CURL_Context *ctx,
                          const char *backend_url,
                          TALER_MERCHANT_TipQueryCallback query_cb,
                          void *query_cb_cls)
{
  struct TALER_MERCHANT_TipQueryOperation *tqo;
  CURL *eh;

  tqo = GNUNET_new (struct TALER_MERCHANT_TipQueryOperation);
  tqo->ctx = ctx;
  tqo->cb = query_cb;
  tqo->cb_cls = query_cb_cls;
  tqo->url = TALER_url_join (backend_url,
                             "tip-query",
                             NULL);
  if (NULL == tqo->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (tqo);
    return NULL;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Requesting URL '%s'\n",
              tqo->url);

  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tqo->url));

  tqo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_tip_query_finished,
                                  tqo);
  return tqo;
}


/**
 * Cancel a /tip-query request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param tqo handle to the operation being cancelled
 */
void
TALER_MERCHANT_tip_query_cancel (struct TALER_MERCHANT_TipQueryOperation *tqo)
{
  if (NULL != tqo->job)
  {
    GNUNET_CURL_job_cancel (tqo->job);
    tqo->job = NULL;
  }
  GNUNET_free (tqo->url);
  GNUNET_free (tqo);
}


/* end of merchant_api_tip_query.c */
