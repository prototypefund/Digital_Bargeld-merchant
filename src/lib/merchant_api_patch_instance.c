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
 * @file lib/merchant_api_patch_instance.c
 * @brief Implementation of the PATCH /instances/$ID request
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
 * Handle for a PATCH /instances/$ID operation.
 */
struct TALER_MERCHANT_InstancePatchHandle
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
  TALER_MERCHANT_InstancePatchCallback cb;

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
 * HTTP PATCH /instances/$ID request.
 *
 * @param cls the `struct TALER_MERCHANT_InstancePatchHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_patch_instance_finished (void *cls,
                                long response_code,
                                const void *response)
{
  struct TALER_MERCHANT_InstancePatchHandle *iph = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  iph->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "PATCH /instances/$ID completed with response code %u\n",
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
  iph->cb (iph->cb_cls,
           &hr);
  TALER_MERCHANT_instance_patch_cancel (iph);
}


/**
 * Update instance configuration.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id identity of the instance to get information about
 * @param payto_uris_length length of the @a accounts array
 * @param payto_uris URIs of the bank accounts of the merchant instance
 * @param name name of the merchant instance
 * @param address physical address of the merchant instance
 * @param jurisdiction jurisdiction of the merchant instance
 * @param default_max_wire_fee default maximum wire fee merchant is willing to fully pay
 * @param default_wire_fee_amortization default amortization factor for excess wire fees
 * @param default_max_deposit_fee default maximum deposit fee merchant is willing to pay
 * @param default_wire_transfer_delay default wire transfer delay merchant will ask for
 * @param default_pay_delay default validity period for offers merchant makes
 * @param cb function to call with the
 *        backend's instances information
 * @param cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstancePatchHandle *
TALER_MERCHANT_instance_patch (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  unsigned int accounts_length,
  const char *payto_uris[],
  const char *name,
  const json_t *address,
  const json_t *jurisdiction,
  const struct TALER_Amount *default_max_wire_fee,
  uint32_t default_wire_fee_amortization,
  const struct TALER_Amount *default_max_deposit_fee,
  struct GNUNET_TIME_Relative default_wire_transfer_delay,
  struct GNUNET_TIME_Relative default_pay_delay,
  TALER_MERCHANT_InstancePatchCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_InstancePatchHandle *iph;
  json_t *jpayto_uris;
  json_t *req_obj;

  jpayto_uris = json_array ();
  if (NULL == jpayto_uris)
  {
    GNUNET_break (0);
    return NULL;
  }
  for (unsigned int i = 0; i<accounts_length; i++)
  {
    if (0 !=
        json_array_append_new (jpayto_uris,
                               json_string (payto_uris[i])))
    {
      GNUNET_break (0);
      json_decref (jpayto_uris);
      return NULL;
    }
  }
  req_obj = json_pack ("{s:o, s:s, s:O, s:O"
                       " s:o, s:I: s:o, s:o, s:o}",
                       "payto_uris",
                       jpayto_uris,
                       "name",
                       name,
                       "address",
                       address,
                       "jurisdiction",
                       jurisdiction,
                       /* end of group of 4 */
                       "default_max_wire_fee",
                       TALER_JSON_from_amount (default_max_wire_fee),
                       "default_wire_fee_amortization",
                       (json_int_t) default_wire_fee_amortization,
                       "default_max_deposit_fee",
                       TALER_JSON_from_amount (default_max_deposit_fee),
                       "default_wire_transfer_delay",
                       GNUNET_JSON_from_time_rel (default_wire_transfer_delay),
                       "default_pay_delay",
                       GNUNET_JSON_from_time_rel (default_pay_delay));
  if (NULL == req_obj)
  {
    GNUNET_break (0);
    return NULL;
  }
  iph = GNUNET_new (struct TALER_MERCHANT_InstancePatchHandle);
  iph->ctx = ctx;
  iph->cb = cb;
  iph->cb_cls = cb_cls;
  {
    char *path;

    GNUNET_asprintf (&path,
                     "instances/%s",
                     instance_id);
    iph->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == iph->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    json_decref (req_obj);
    GNUNET_free (iph);
    return NULL;
  }
  {
    CURL *eh;

    eh = curl_easy_init ();
    if (GNUNET_OK !=
        TALER_curl_easy_post (&iph->post_ctx,
                              eh,
                              req_obj))
    {
      GNUNET_break (0);
      json_decref (req_obj);
      GNUNET_free (iph);
      return NULL;
    }

    json_decref (req_obj);
    GNUNET_assert (CURLE_OK == curl_easy_setopt (eh,
                                                 CURLOPT_URL,
                                                 iph->url));
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_CUSTOMREQUEST,
                                     MHD_HTTP_METHOD_PATCH));
    iph->job = GNUNET_CURL_job_add2 (ctx,
                                     eh,
                                     iph->post_ctx.headers,
                                     &handle_patch_instance_finished,
                                     iph);
  }
  return iph;
}


/**
 * Cancel PATCH /instances/$ID request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param iph request to cancel.
 */
void
TALER_MERCHANT_instance_patch_cancel (
  struct TALER_MERCHANT_InstancePatchHandle *iph)
{
  if (NULL != iph->job)
  {
    GNUNET_CURL_job_cancel (iph->job);
    iph->job = NULL;
  }
  TALER_curl_easy_post_finished (&iph->post_ctx);
  GNUNET_free (iph->url);
  GNUNET_free (iph);
}


/* end of merchant_api_patch_instance.c */
