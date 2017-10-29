/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

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
 * @file lib/merchant_api_tip_enable.c
 * @brief Implementation of the /tip-enable request of the merchant's HTTP API
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
#include "merchant_api_common.h"


/**
 * @brief A handle for tracking transactions.
 */
struct TALER_MERCHANT_TipEnableOperation
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
  TALER_MERCHANT_TipEnableCallback cb;

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
 * @param cls the `struct TALER_MERCHANT_TipEnableOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_tip_enable_finished (void *cls,
                            long response_code,
                            const json_t *json)
{
  struct TALER_MERCHANT_TipEnableOperation *teo = cls;

  teo->job = NULL;
  switch (response_code)
  {
  case MHD_HTTP_OK:
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
  teo->cb (teo->cb_cls,
           response_code,
	   TALER_JSON_get_error_code (json));
  TALER_MERCHANT_tip_enable_cancel (teo);
}


/**
 * Issue a /tip-enable request to the backend.  Informs the backend
 * that a reserve is now available for tipping.  Note that the
 * respective @a reserve_priv must also be bound to one or more
 * instances (together with the URI of the exchange) via the backend's
 * configuration file before it can be used.  Usually, the process
 * is that one first configures an exchange and a @a reserve_priv for
 * an instance, and then enables (or re-enables) the reserve by
 * performing wire transfers and informs the backend about it using
 * this API.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param amount amount that was credited to the reserve
 * @param expiration when will the reserve expire
 * @param reserve_priv private key of the reserve
 * @param credit_uuid unique ID of the wire transfer
 * @param enable_cb callback which will work the response gotten from the backend
 * @param enable_cb_cls closure to pass to @a enable_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipEnableOperation *
TALER_MERCHANT_tip_enable (struct GNUNET_CURL_Context *ctx,
                           const char *backend_uri,
                           const struct TALER_Amount *amount,
                           struct GNUNET_TIME_Absolute expiration,
                           const struct TALER_ReservePrivateKeyP *reserve_priv,
                           const struct GNUNET_HashCode *credit_uuid,
                           TALER_MERCHANT_TipEnableCallback enable_cb,
                           void *enable_cb_cls)
{
  struct TALER_MERCHANT_TipEnableOperation *teo;
  CURL *eh;
  json_t *te_obj;

  teo = GNUNET_new (struct TALER_MERCHANT_TipEnableOperation);
  teo->ctx = ctx;
  teo->cb = enable_cb;
  teo->cb_cls = enable_cb_cls;
  teo->url = MAH_path_to_url_ (backend_uri,
                               "/tip-enable");
  te_obj = json_pack ("{"
                      " s:o," /* amount */
                      " s:o," /* expiration */
                      " s:o," /* credit_uuid */
                      " s:o," /* reserve_priv */
                      "}",
                      "credit", TALER_JSON_from_amount (amount),
                      "expiration", GNUNET_JSON_from_time_abs (expiration),
                      "credit_uuid", GNUNET_JSON_from_data_auto (credit_uuid),
                      "reserve_priv", GNUNET_JSON_from_data_auto (reserve_priv));
  if (NULL == te_obj)
  {
    GNUNET_break (0);
    GNUNET_free (teo->url);
    GNUNET_free (teo);
    return NULL;
  }
  if (NULL == (teo->json_enc =
               json_dumps (te_obj,
                           JSON_COMPACT)))
  {
    GNUNET_break (0);
    json_decref (te_obj);
    GNUNET_free (teo->url);
    GNUNET_free (teo);
    return NULL;
  }
  json_decref (te_obj);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URI '%s'\n",
              teo->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   teo->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   teo->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (teo->json_enc)));
  teo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_tip_enable_finished,
                                  teo);
  return teo;
}


/**
 * Cancel a /track/transaction request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param teo handle to the tracking operation being cancelled
 */
void
TALER_MERCHANT_tip_enable_cancel (struct TALER_MERCHANT_TipEnableOperation *teo)
{
  if (NULL != teo->job)
  {
    GNUNET_CURL_job_cancel (teo->job);
    teo->job = NULL;
  }
  GNUNET_free_non_null (teo->json_enc);
  GNUNET_free (teo->url);
  GNUNET_free (teo);
}

/* end of merchant_api_tip_enable.c */
