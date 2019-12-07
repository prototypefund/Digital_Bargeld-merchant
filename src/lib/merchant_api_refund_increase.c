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


struct TALER_MERCHANT_RefundIncreaseOperation
{
  /**
   * Complete URL where the backend offers /refund
   */
  char *url;

  /**
   * Minor context that holds body and headers.
   */
  struct TEAH_PostContext post_ctx;

  /**
   * The CURL context to connect to the backend
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * The callback to pass the backend response to
   */
  TALER_MERCHANT_RefundIncreaseCallback cb;

  /**
   * Clasure to pass to the callback
   */
  void *cb_cls;

  /**
   * Handle for the request
   */
  struct GNUNET_CURL_Job *job;
};


/**
 * Callback to process POST /refund response
 *
 * @param cls the `struct TALER_MERCHANT_RefundIncreaseOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not JSON
 */
static void
handle_refund_increase_finished (void *cls,
                                 long response_code,
                                 const void *response)
{
  struct TALER_MERCHANT_RefundIncreaseOperation *rio = cls;
  const json_t *json = response;

  rio->job = NULL;
  switch (response_code)
  {
  case 0:
    rio->cb (rio->cb_cls,
             0,
             TALER_EC_INVALID_RESPONSE,
             NULL);
    break;
  case MHD_HTTP_OK:
  case MHD_HTTP_BAD_REQUEST:
  case MHD_HTTP_NOT_FOUND:
    rio->cb (rio->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             json);
    break;
  default:
    GNUNET_break_op (0); /* unexpected status code */
    rio->cb (rio->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             json);
    break;
  }
  TALER_MERCHANT_refund_increase_cancel (rio);
}


/**
 * Cancel a POST /refund request.
 *
 * @param rio the refund increasing operation to cancel
 */
void
TALER_MERCHANT_refund_increase_cancel (struct
                                       TALER_MERCHANT_RefundIncreaseOperation *
                                       rio)
{
  if (NULL != rio->job)
  {
    GNUNET_CURL_job_cancel (rio->job);
    rio->job = NULL;
  }
  TALER_curl_easy_post_finished (&rio->post_ctx);
  GNUNET_free (rio->url);
  GNUNET_free (rio);
}


/**
 * Increase the refund associated to a order
 *
 * @param ctx the CURL context used to connect to the backend
 * @param backend_url backend's base URL, including final "/"
 * @param order_id id of the order whose refund is to be increased
 * @param refund amount to which increase the refund
 * @param reason human-readable reason justifying the refund
 * @param cb callback processing the response from /refund
 * @param cb_cls closure for cb
 */
struct TALER_MERCHANT_RefundIncreaseOperation *
TALER_MERCHANT_refund_increase (struct GNUNET_CURL_Context *ctx,
                                const char *backend_url,
                                const char *order_id,
                                const struct TALER_Amount *refund,
                                const char *reason,
                                TALER_MERCHANT_RefundIncreaseCallback cb,
                                void *cb_cls)
{
  struct TALER_MERCHANT_RefundIncreaseOperation *rio;
  json_t *req;
  CURL *eh;

  rio = GNUNET_new (struct TALER_MERCHANT_RefundIncreaseOperation);
  rio->ctx = ctx;
  rio->cb = cb;
  rio->cb_cls = cb_cls;
  rio->url = TALER_url_join (backend_url,
                             "refund",
                             NULL);
  if (NULL == rio->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (rio);
    return NULL;
  }

  req = json_pack ("{s:o, s:s, s:s}",
                   "refund", TALER_JSON_from_amount (refund),
                   "order_id", order_id,
                   "reason", reason);
  eh = curl_easy_init ();

  if (GNUNET_OK != TALER_curl_easy_post (&rio->post_ctx,
                                         eh,
                                         req))
  {
    GNUNET_break (0);
    json_decref (req);
    GNUNET_free (rio->url);
    GNUNET_free (rio);
    return NULL;
  }
  json_decref (req);
  GNUNET_assert (CURLE_OK == curl_easy_setopt (eh,
                                               CURLOPT_URL,
                                               rio->url));
  rio->job = GNUNET_CURL_job_add2
               (ctx,
               eh,
               rio->post_ctx.headers,
               &handle_refund_increase_finished,
               rio);
  if (NULL == rio->job)
  {
    GNUNET_free (rio->url);
    GNUNET_free (rio);
    return NULL;
  }
  return rio;
}
