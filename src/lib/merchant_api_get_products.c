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
 * @file lib/merchant_api_get_products.c
 * @brief Implementation of the GET /products request of the merchant's HTTP API
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
 * Handle for a GET /products operation.
 */
struct TALER_MERCHANT_ProductsGetHandle
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
  TALER_MERCHANT_ProductsGetCallback cb;

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
 * Parse product information from @a ia.
 *
 * @param ia JSON array (or NULL!) with product data
 * @param pgh operation handle
 * @return #GNUNET_OK on success
 */
static int
parse_products (const json_t *ia,
                struct TALER_MERCHANT_ProductsGetHandle *pgh)
{
  unsigned int ies_len = json_array_size (ia);
  struct TALER_MERCHANT_InventoryEntry ies[ies_len];
  size_t index;
  json_t *value;
  int ret;

  ret = GNUNET_OK;
  json_array_foreach (ia, index, value) {
    struct TALER_MERCHANT_InventoryEntry *ie = &ies[index];
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("product_id",
                               &ie->product_id),
      GNUNET_JSON_spec_end ()
    };

    if (GNUNET_OK !=
        GNUNET_JSON_parse (value,
                           spec,
                           NULL, NULL))
    {
      GNUNET_break_op (0);
      ret = GNUNET_SYSERR;
      continue;
    }
    if (GNUNET_SYSERR == ret)
      break;
  }
  if (GNUNET_OK == ret)
  {
    struct TALER_MERCHANT_HttpResponse hr = {
      .http_status = MHD_HTTP_OK
    };

    pgh->cb (pgh->cb_cls,
             &hr,
             ies_len,
             ies);
    pgh->cb = NULL; /* just to be sure */
  }
  return ret;
}


/**
 * Function called when we're done processing the
 * HTTP /products request.
 *
 * @param cls the `struct TALER_MERCHANT_ProductsGetHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_get_products_finished (void *cls,
                              long response_code,
                              const void *response)
{
  struct TALER_MERCHANT_ProductsGetHandle *pgh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  pgh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /products response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_OK:
    {
      json_t *products;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_json ("products",
                               &products),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (json,
                             spec,
                             NULL, NULL))
      {
        hr.http_status = 0;
        hr.ec = TALER_EC_INVALID_RESPONSE;
      }
      else
      {
        if ( (! json_is_array (products)) ||
             (GNUNET_OK ==
              parse_products (products,
                              pgh)) )
        {
          GNUNET_JSON_parse_free (spec);
          TALER_MERCHANT_products_get_cancel (pgh);
          return;
        }
        else
        {
          hr.http_status = 0;
          hr.ec = TALER_EC_INVALID_RESPONSE;
        }
      }
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
           0,
           NULL);
  TALER_MERCHANT_products_get_cancel (pgh);
}


/**
 * Make a GET /products request.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param cb function to call with the backend's inventory information
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductsGetHandle *
TALER_MERCHANT_products_get (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  TALER_MERCHANT_ProductsGetCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_ProductsGetHandle *pgh;
  CURL *eh;

  pgh = GNUNET_new (struct TALER_MERCHANT_ProductsGetHandle);
  pgh->ctx = ctx;
  pgh->cb = cb;
  pgh->cb_cls = cb_cls;
  pgh->url = TALER_url_join (backend_url,
                             "private/products",
                             NULL);
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
                                  &handle_get_products_finished,
                                  pgh);
  return pgh;
}


/**
 * Cancel /products request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param pgh request to cancel.
 */
void
TALER_MERCHANT_products_get_cancel (
  struct TALER_MERCHANT_ProductsGetHandle *pgh)
{
  if (NULL != pgh->job)
    GNUNET_CURL_job_cancel (pgh->job);
  GNUNET_free (pgh->url);
  GNUNET_free (pgh);
}
