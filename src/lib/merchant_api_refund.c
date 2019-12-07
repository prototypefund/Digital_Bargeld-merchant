/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016, 2017, 2019 Taler Systems SA

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
 * @file lib/merchant_api_refund.c
 * @brief Implementation of the /refund POST and GET
 * @author Christian Grothoff
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
#include <taler/taler_signatures.h>
#include <taler/taler_curl_lib.h>


struct TALER_MERCHANT_RefundLookupOperation
{
  /**
   * URL of the request, includes parameters
   */
  char *url;

  /**
   * Handle of the request
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the response
   */
  TALER_MERCHANT_RefundLookupCallback cb;

  /**
   * Closure for cb
   */
  void *cb_cls;

  /**
   * Reference to the execution context
   */
  struct GNUNET_CURL_Context *ctx;

};


/**
 * Cancel a /refund lookup operation
 *
 * @param
 */
void
TALER_MERCHANT_refund_lookup_cancel (struct
                                     TALER_MERCHANT_RefundLookupOperation *rlo)
{
  if (NULL != rlo->job)
  {
    GNUNET_CURL_job_cancel (rlo->job);
    rlo->job = NULL;
  }
  GNUNET_free (rlo->url);
  GNUNET_free (rlo);
}


/**
 * Process GET /refund response
 *
 * @param cls a `struct TALER_MERCHANT_RefundLookupOperation *`
 * @param response_code HTTP status, 0 for HTTP failure
 * @param response a `const json_t *` with the JSON of the HTTP body
 */
static void
handle_refund_lookup_finished (void *cls,
                               long response_code,
                               const void *response)
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo = cls;
  const json_t *json = response;

  rlo->job = NULL;
  switch (response_code)
  {
  case 0:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Backend didn't even return from GET /refund\n");
    rlo->cb (rlo->cb_cls,
             0,
             TALER_EC_INVALID_RESPONSE,
             NULL);
    break;
  case MHD_HTTP_OK:
  case MHD_HTTP_NOT_FOUND:
    rlo->cb (rlo->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             json);
    break;
  default:
    GNUNET_break_op (0); /* unexpected status code */
    rlo->cb (rlo->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             json);
    break;
  }
  TALER_MERCHANT_refund_lookup_cancel (rlo);
}


/**
 * Does a GET /refund.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param cb callback which will work the response gotten from the backend
 * @param cb_cls closure to pass to the callback
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_RefundLookupOperation *
TALER_MERCHANT_refund_lookup (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              const char *order_id,
                              TALER_MERCHANT_RefundLookupCallback cb,
                              void *cb_cls)
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo;
  CURL *eh;

  rlo = GNUNET_new (struct TALER_MERCHANT_RefundLookupOperation);
  rlo->ctx = ctx;
  rlo->cb = cb;
  rlo->cb_cls = cb_cls;
  rlo->url = TALER_url_join (backend_url,
                             "refund",
                             "order_id",
                             order_id,
                             NULL);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    rlo->url))
  {
    GNUNET_break (0);
    GNUNET_free (rlo->url);
    GNUNET_free (rlo);
    return NULL;
  }
  rlo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_NO,
                                  handle_refund_lookup_finished,
                                  rlo);
  if (NULL == rlo->job)
  {
    GNUNET_free (rlo->url);
    GNUNET_free (rlo);
    GNUNET_break (0);
    return NULL;
  }
  return rlo;
}
