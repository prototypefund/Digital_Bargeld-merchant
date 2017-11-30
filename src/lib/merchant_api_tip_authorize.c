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
 * @file lib/merchant_api_tip_authorize.c
 * @brief Implementation of the /tip-authorize request of the merchant's HTTP API
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
struct TALER_MERCHANT_TipAuthorizeOperation
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
  TALER_MERCHANT_TipAuthorizeCallback cb;

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
 * We got a 200 response back from the exchange (or the merchant).
 * Now we need to parse the response and if it is well-formed,
 * call the callback (and set it to NULL afterwards).
 *
 * @param tao handle of the original authorization operation
 * @param json cryptographic proof returned by the exchange/merchant
 * @return #GNUNET_OK if response is valid
 */
static int
check_ok (struct TALER_MERCHANT_TipAuthorizeOperation *tao,
          const json_t *json)
{
  struct GNUNET_HashCode tip_id;
  struct GNUNET_TIME_Absolute tip_expiration;
  const char *exchange_uri;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_absolute_time ("expiration", &tip_expiration),
    GNUNET_JSON_spec_fixed_auto ("tip_id", &tip_id),
    GNUNET_JSON_spec_string ("exchange_url", &exchange_uri),
    GNUNET_JSON_spec_end()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  tao->cb (tao->cb_cls,
           MHD_HTTP_OK,
           TALER_JSON_get_error_code (json),
           &tip_id,
           tip_expiration,
           exchange_uri);
  tao->cb = NULL; /* do not call twice */
  GNUNET_JSON_parse_free (spec);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /track/transaction request.
 *
 * @param cls the `struct TALER_MERCHANT_TipAuthorizeOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_tip_authorize_finished (void *cls,
                               long response_code,
                               const json_t *json)
{
  struct TALER_MERCHANT_TipAuthorizeOperation *tao = cls;

  tao->job = NULL;
  switch (response_code)
  {
  case MHD_HTTP_OK:
    if (GNUNET_OK != check_ok (tao,
                              json))
    {
      GNUNET_break_op (0);
      response_code = 0;
    }
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Well-defined status code, pass on to application! */
    break;
  case MHD_HTTP_PRECONDITION_FAILED:
    /* Well-defined status code, pass on to application! */
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
  if (NULL != tao->cb)
    tao->cb (tao->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             NULL,
             GNUNET_TIME_UNIT_ZERO_ABS,
             NULL);
  TALER_MERCHANT_tip_authorize_cancel (tao);
}


/**
 * Issue a /tip-authorize request to the backend.  Informs the backend
 * that a tip should be created.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param amount amount to be handed out as a tip
 * @param instance which backend instance should create the tip (identifies the reserve and exchange)
 * @param justification which justification should be stored (human-readable reason for the tip)
 * @param authorize_cb callback which will work the response gotten from the backend
 * @param authorize_cb_cls closure to pass to @a authorize_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipAuthorizeOperation *
TALER_MERCHANT_tip_authorize (struct GNUNET_CURL_Context *ctx,
                              const char *backend_uri,
                              const struct TALER_Amount *amount,
                              const char *instance,
                              const char *justification,
                              TALER_MERCHANT_TipAuthorizeCallback authorize_cb,
                              void *authorize_cb_cls)
{
  struct TALER_MERCHANT_TipAuthorizeOperation *tao;
  CURL *eh;
  json_t *te_obj;

  tao = GNUNET_new (struct TALER_MERCHANT_TipAuthorizeOperation);
  tao->ctx = ctx;
  tao->cb = authorize_cb;
  tao->cb_cls = authorize_cb_cls;
  tao->url = MAH_path_to_url_ (backend_uri,
                               "/tip-authorize");
  te_obj = json_pack ("{"
                      " s:o," /* amount */
                      " s:s," /* instance */
                      " s:s," /* justification */
                      "}",
                      "amount", TALER_JSON_from_amount (amount),
                      "instance", instance,
                      "justification", justification);
  if (NULL == te_obj)
  {
    GNUNET_break (0);
    GNUNET_free (tao->url);
    GNUNET_free (tao);
    return NULL;
  }
  if (NULL == (tao->json_enc =
               json_dumps (te_obj,
                           JSON_COMPACT)))
  {
    GNUNET_break (0);
    json_decref (te_obj);
    GNUNET_free (tao->url);
    GNUNET_free (tao);
    return NULL;
  }
  json_decref (te_obj);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URI '%s'\n",
              tao->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tao->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   tao->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (tao->json_enc)));
  tao->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_tip_authorize_finished,
                                  tao);
  return tao;
}


/**
 * Cancel a /track/transaction request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param tao handle to the tracking operation being cancelled
 */
void
TALER_MERCHANT_tip_authorize_cancel (struct TALER_MERCHANT_TipAuthorizeOperation *tao)
{
  if (NULL != tao->job)
  {
    GNUNET_CURL_job_cancel (tao->job);
    tao->job = NULL;
  }
  GNUNET_free_non_null (tao->json_enc);
  GNUNET_free (tao->url);
  GNUNET_free (tao);
}

/* end of merchant_api_tip_authorize.c */
