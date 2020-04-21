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
 * @file lib/merchant_api_get_product.c
 * @brief Implementation of the GET /product/$ID request of the merchant's HTTP API
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
 * Handle for a GET /products/$ID operation.
 */
struct TALER_MERCHANT_ProductGetHandle
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
  TALER_MERCHANT_ProductGetCallback cb;

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
 * @param cls the `struct TALER_MERCHANT_ProductGetHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_get_product_finished (void *cls,
                             long response_code,
                             const void *response)
{
  struct TALER_MERCHANT_ProductGetHandle *pgh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  pgh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /products/$ID response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_OK:
    {
      const char *description;
      json_t *description_i18n;
      const char *unit;
      struct TALER_Amount price;
      json_t *image;
      json_t *taxes;
      int64_t total_stocked;
      uint64_t total_sold;
      uint64_t total_lost;
      json_t *location;
      struct GNUNET_TIME_Absolute next_restock;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_string ("description",
                                 &description),
        GNUNET_JSON_spec_json ("description_i18n",
                               &description_i18n),
        GNUNET_JSON_spec_string ("unit",
                                 &unit),
        TALER_JSON_spec_amount ("price_fee",
                                &price),
        GNUNET_JSON_spec_json ("image",
                               &image),
        GNUNET_JSON_spec_json ("taxes",
                               &taxes),
        GNUNET_JSON_spec_int64 ("total_stocked",
                                &total_stocked),
        GNUNET_JSON_spec_uint64 ("total_sold",
                                 &total_sold),
        GNUNET_JSON_spec_uint64 ("total_lost",
                                 &total_lost),
        GNUNET_JSON_spec_json ("location",
                               &location),
        GNUNET_JSON_spec_absolute_time ("next_restock",
                                        &next_restock),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK ==
          GNUNET_JSON_parse (json,
                             spec,
                             NULL, NULL))
      {
        pgh->cb (pgh->cb_cls,
                 &hr,
                 description,
                 description_i18n,
                 unit,
                 &price,
                 image,
                 taxes,
                 total_stocked,
                 total_sold,
                 total_lost,
                 location,
                 next_restock);
        GNUNET_JSON_parse_free (spec);
        TALER_MERCHANT_product_get_cancel (pgh);
        return;
      }
      hr.http_status = 0;
      hr.ec = TALER_EC_INVALID_RESPONSE;
      GNUNET_JSON_parse_free (spec);
      break;
    }
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
  pgh->cb (pgh->cb_cls,
           &hr,
           NULL,
           NULL,
           NULL,
           NULL,
           NULL,
           NULL,
           0,
           0,
           0,
           NULL,
           GNUNET_TIME_UNIT_FOREVER_ABS);
  TALER_MERCHANT_product_get_cancel (pgh);
}


/**
 * Make a GET /product/$ID request to get details about an
 * individual product.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to query about its products,
 *                    NULL to query the default instance
 * @param product_id identifier of the product to inquire about
 * @param cb function to call with the backend's product information
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductGetHandle *
TALER_MERCHANT_product_get (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  TALER_MERCHANT_ProductGetCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_ProductGetHandle *pgh;
  CURL *eh;

  pgh = GNUNET_new (struct TALER_MERCHANT_ProductGetHandle);
  pgh->ctx = ctx;
  pgh->cb = cb;
  pgh->cb_cls = cb_cls;
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
    pgh->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == pgh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (pgh);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              pgh->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   pgh->url));
  pgh->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_get_product_finished,
                                  pgh);
  return pgh;
}


/**
 * Cancel GET /product/$ID request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param pgh request to cancel.
 */
void
TALER_MERCHANT_product_get_cancel (
  struct TALER_MERCHANT_ProductGetHandle *pgh)
{
  if (NULL != pgh->job)
    GNUNET_CURL_job_cancel (pgh->job);
  GNUNET_free (pgh->url);
  GNUNET_free (pgh);
}
