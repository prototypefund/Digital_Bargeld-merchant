/*
  This file is part of TALER
  Copyright (C) 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General
  Public License along with TALER; see the file COPYING.LGPL.
  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_lock_product.c
 * @brief Implementation of the POST /products/$ID/lock request
 *        of the merchant's HTTP API
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_json_lib.h>
#include <taler/taler_curl_lib.h>


/**
 * Handle for a POST /products/$ID/lock operation.
 */
struct TALER_MERCHANT_ProductLockHandle
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
  TALER_MERCHANT_ProductLockCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Minor context that holds body and headers.
   */
  struct TALER_CURL_PostContext post_ctx;

};


/**
 * Function called when we're done processing the
 * HTTP POST /products/$ID/lock request.
 *
 * @param cls the `struct TALER_MERCHANT_ProductLockHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_lock_product_finished (void *cls,
                              long response_code,
                              const void *response)
{
  struct TALER_MERCHANT_ProductLockHandle *plh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  plh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "LOCK /products/$ID completed with response code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case 0:
    hr.ec = TALER_EC_INVALID_RESPONSE;
    break;
  case MHD_HTTP_NO_CONTENT:
    break;
  case MHD_HTTP_BAD_REQUEST:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    GNUNET_break_op (0);
    /* This should never happen, either us
     * or the merchant is buggy (or API version conflict);
     * just pass JSON reply to the application */
    break;
  case MHD_HTTP_FORBIDDEN:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    /* Nothing really to verify, merchant says we tried to abort the payment
     * after it was successful. We should pass the JSON reply to the
     * application */
    break;
  case MHD_HTTP_NOT_FOUND:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_GONE:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    /* Server had an internal issue; we should retry,
       but this API leaves this to the application */
    break;
  default:
    TALER_MERCHANT_parse_error_details_ (json,
                                         response_code,
                                         &hr);
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    GNUNET_break_op (0);
    break;
  }
  plh->cb (plh->cb_cls,
           &hr);
  TALER_MERCHANT_product_lock_cancel (plh);
}


/**
 * Make a POST /products/$ID/lock request to reserve a certain
 * amount of product in inventory to a reservation UUID.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param product_id identifier of the product
 * @param uuid UUID that identifies the client holding the lock
 * @param duration how long should the lock be held
 * @param quantity how much product should be locked
 * @param cb function to call with the backend's lock status
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductLockHandle *
TALER_MERCHANT_product_lock (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *product_id,
  const struct GNUNET_Uuid *uuid,
  struct GNUNET_TIME_Relative duration,
  uint32_t quantity,
  TALER_MERCHANT_ProductLockCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_ProductLockHandle *plh;
  json_t *req_obj;

  req_obj = json_pack ("{s:o, s:o, s:I}",
                       "lock_uuid",
                       GNUNET_JSON_from_data_auto (uuid),
                       "duration",
                       GNUNET_JSON_from_time_rel (duration),
                       "quantity",
                       (json_int_t) quantity);
  if (NULL == req_obj)
  {
    GNUNET_break (0);
    return NULL;
  }
  plh = GNUNET_new (struct TALER_MERCHANT_ProductLockHandle);
  plh->ctx = ctx;
  plh->cb = cb;
  plh->cb_cls = cb_cls;
  {
    char *path;

    GNUNET_asprintf (&path,
                     "private/products/%s/lock",
                     product_id);
    plh->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == plh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    json_decref (req_obj);
    GNUNET_free (plh);
    return NULL;
  }
  {
    CURL *eh;

    eh = curl_easy_init ();
    if (GNUNET_OK !=
        TALER_curl_easy_post (&plh->post_ctx,
                              eh,
                              req_obj))
    {
      GNUNET_break (0);
      json_decref (req_obj);
      GNUNET_free (plh);
      return NULL;
    }

    json_decref (req_obj);
    GNUNET_assert (CURLE_OK == curl_easy_setopt (eh,
                                                 CURLOPT_URL,
                                                 plh->url));
    plh->job = GNUNET_CURL_job_add2 (ctx,
                                     eh,
                                     plh->post_ctx.headers,
                                     &handle_lock_product_finished,
                                     plh);
  }
  return plh;
}


/**
 * Cancel POST /products/$ID/lock request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param plh request to cancel.
 */
void
TALER_MERCHANT_product_lock_cancel (
  struct TALER_MERCHANT_ProductLockHandle *plh)
{
  if (NULL != plh->job)
  {
    GNUNET_CURL_job_cancel (plh->job);
    plh->job = NULL;
  }
  TALER_curl_easy_post_finished (&plh->post_ctx);
  GNUNET_free (plh->url);
  GNUNET_free (plh);
}


/* end of merchant_api_lock_product.c */
