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
 * @file lib/merchant_api_patch_product.c
 * @brief Implementation of the PATCH /products/$ID request
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
 * Handle for a PATCH /products/$ID operation.
 */
struct TALER_MERCHANT_ProductPatchHandle
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
  TALER_MERCHANT_ProductPatchCallback cb;

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
 * HTTP PATCH /products/$ID request.
 *
 * @param cls the `struct TALER_MERCHANT_ProductPatchHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_patch_product_finished (void *cls,
                               long response_code,
                               const void *response)
{
  struct TALER_MERCHANT_ProductPatchHandle *pph = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  pph->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "PATCH /products/$ID completed with response code %u\n",
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
  case MHD_HTTP_CONFLICT:
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
  pph->cb (pph->cb_cls,
           &hr);
  TALER_MERCHANT_product_patch_cancel (pph);
}


/**
 * Make a PATCH /products request to update product details in the
 * inventory.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to add a product to,
 *                    NULL to query the default instance
 * @param product_id identifier to use for the product; the product must exist,
 *                    or the transaction will fail with a #MHD_HTTP_NOT_FOUND
 *                    HTTP status code
 * @param description description of the product
 * @param description_i18n Map from IETF BCP 47 language tags to localized descriptions
 * @param unit unit in which the product is measured (liters, kilograms, packages, etc.)
 * @param price the price for one @a unit of the product, zero is used to imply that
 *              this product is not sold separately or that the price is not fixed and
 *              must be supplied by the front-end.  If non-zero, price must include
 *              applicable taxes.
 * @param image base64-encoded product image
 * @param taxes list of taxes paid by the merchant
 * @param total_stocked in @a units, -1 to indicate "infinite" (i.e. electronic books),
 *               must be larger than previous values
 * @param total_lost in @a units, must be larger than previous values, and may
 *               not exceed total_stocked minus total_sold; if it does, the transaction
 *               will fail with a #MHD_HTTP_CONFLICT HTTP status code
 * @param location where the product is in stock
 * @param next_restock when the next restocking is expected to happen
 * @param cb function to call with the backend's result
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductPatchHandle *
TALER_MERCHANT_product_patch (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  const char *description,
  const json_t *description_i18n,
  const char *unit,
  const struct TALER_Amount *price,
  const json_t *image,
  const json_t *taxes,
  int64_t total_stocked,
  uint64_t total_lost,
  const json_t *address,
  struct GNUNET_TIME_Absolute next_restock,
  TALER_MERCHANT_ProductPatchCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_ProductPatchHandle *pph;
  json_t *req_obj;

  req_obj = json_pack ("{s:s, s:O, s:s, s:o, s:O,"
                       " s:O, s:I: s:I, s:O, s:o}",
                       "description",
                       description,
                       "description_i18n",
                       description_i18n,
                       "unit",
                       unit,
                       "price",
                       TALER_JSON_from_amount (price),
                       "image",
                       image,
                       /* End of first group of 5 */
                       "taxes",
                       taxes,
                       "total_stocked",
                       (json_int_t) total_stocked,
                       "total_lost",
                       (json_int_t) total_lost,
                       "address",
                       address,
                       "next_restock",
                       GNUNET_JSON_from_time_abs (next_restock));
  if (NULL == req_obj)
  {
    GNUNET_break (0);
    return NULL;
  }
  pph = GNUNET_new (struct TALER_MERCHANT_ProductPatchHandle);
  pph->ctx = ctx;
  pph->cb = cb;
  pph->cb_cls = cb_cls;
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
    pph->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == pph->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    json_decref (req_obj);
    GNUNET_free (pph);
    return NULL;
  }
  {
    CURL *eh;

    eh = curl_easy_init ();
    if (GNUNET_OK !=
        TALER_curl_easy_post (&pph->post_ctx,
                              eh,
                              req_obj))
    {
      GNUNET_break (0);
      json_decref (req_obj);
      GNUNET_free (pph);
      return NULL;
    }

    json_decref (req_obj);
    GNUNET_assert (CURLE_OK == curl_easy_setopt (eh,
                                                 CURLOPT_URL,
                                                 pph->url));
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_CUSTOMREQUEST,
                                     MHD_HTTP_METHOD_PATCH));
    pph->job = GNUNET_CURL_job_add2 (ctx,
                                     eh,
                                     pph->post_ctx.headers,
                                     &handle_patch_product_finished,
                                     pph);
  }
  return pph;
}


/**
 * Cancel PATCH /products/$ID request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param pph request to cancel.
 */
void
TALER_MERCHANT_product_patch_cancel (
  struct TALER_MERCHANT_ProductPatchHandle *pph)
{
  if (NULL != pph->job)
  {
    GNUNET_CURL_job_cancel (pph->job);
    pph->job = NULL;
  }
  TALER_curl_easy_post_finished (&pph->post_ctx);
  GNUNET_free (pph->url);
  GNUNET_free (pph);
}


/* end of merchant_api_patch_product.c */
