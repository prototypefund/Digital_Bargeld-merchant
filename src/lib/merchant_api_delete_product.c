/*
  This file is part of TALER
  Copyright (C) 2014-2018, 2020 Taler Systems SA

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
 * @file lib/merchant_api_delete_product.c
 * @brief Implementation of the DELETE /product/$ID request of the merchant's HTTP API
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
 * Handle for a DELETE /products/$ID operation.
 */
struct TALER_MERCHANT_ProductDeleteHandle
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
  TALER_MERCHANT_ProductDeleteCallback cb;

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
 * HTTP GET /products/$ID request.
 *
 * @param cls the `struct TALER_MERCHANT_ProductDeleteHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_delete_product_finished (void *cls,
                                long response_code,
                                const void *response)
{
  struct TALER_MERCHANT_ProductDeleteHandle *pdh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  pdh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /products/$ID response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_NO_CONTENT:
    break;
  case MHD_HTTP_NOT_FOUND:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_CONFLICT:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  default:
    /* unexpected response code */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    break;
  }
  pdh->cb (pdh->cb_cls,
           &hr);
  TALER_MERCHANT_product_delete_cancel (pdh);
}


/**
 * Make a DELETE /products/$ID request to delete a product from our
 * inventory.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to query about its products,
 *                    NULL to query the default instance
 * @param product_id identifier of the product
 * @param cb function to call with the backend's deletion status
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductDeleteHandle *
TALER_MERCHANT_product_delete (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  TALER_MERCHANT_ProductDeleteCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_ProductDeleteHandle *pdh;

  pdh = GNUNET_new (struct TALER_MERCHANT_ProductDeleteHandle);
  pdh->ctx = ctx;
  pdh->cb = cb;
  pdh->cb_cls = cb_cls;
  {
    char *path;

    if (NULL == instance_id)
      GNUNET_asprintf (&path,
                       "products/%s",
                       product_id);
    else
      GNUNET_asprintf (&path,
                       "instances/%s/products/%s",
                       instance_id,
                       product_id);
    pdh->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == pdh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (pdh);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              pdh->url);
  {
    CURL *eh;

    eh = curl_easy_init ();
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_URL,
                                     pdh->url));
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_CUSTOMREQUEST,
                                     MHD_HTTP_METHOD_DELETE));
    pdh->job = GNUNET_CURL_job_add (ctx,
                                    eh,
                                    GNUNET_YES,
                                    &handle_delete_product_finished,
                                    pdh);
  }
  return pdh;
}


/**
 * Cancel DELETE /product/$ID request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param pdh request to cancel.
 */
void
TALER_MERCHANT_product_delete_cancel (
  struct TALER_MERCHANT_ProductDeleteHandle *pdh)
{
  if (NULL != pdh->job)
    GNUNET_CURL_job_cancel (pdh->job);
  GNUNET_free (pdh->url);
  GNUNET_free (pdh);
}
