/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016, 2017 GNUnet e.V. and INRIA

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
 * @file lib/merchant_api_proposal.c
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

struct TALER_MERCHANT_RefundIncreaseOperation
{
  /**
   * Complete URL where the backend offers /refund
   */
  char *url;

  /**
   * The request body
   */
  char *json_enc;

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
                                 const json_t *json)
{
  struct TALER_MERCHANT_RefundIncreaseOperation *rio = cls;
  char *error;
  char *hint;
  enum TALER_ErrorCode code;

  rio->job = NULL;
  switch (response_code)
  {
  case 0:
    /* Hard error */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Backend didn't even return from POST /refund\n");
    return;
  case MHD_HTTP_OK:
    rio->cb (rio->cb_cls,
             MHD_HTTP_OK,
             TALER_EC_NONE,
             json);
    break;
  default:
    /**
     * The backend gave response, but it's error, log it.
     * NOTE that json must be a Taler-specific error object (FIXME,
     * need a link to error objects at docs)
     */
    json_unpack ((json_t *) json,
                 "{s:s, s:I, s:s}",
                 "error", &error,
                 "code", &code,
                 "hint", &hint);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed POST /refund, error: %s, code: %d, hint: %s\n",
                error,
                code,
                hint);
  }
}

/**
 * Cancel a POST /refund request.
 *
 * @param rio the refund increasing operation to cancel
 */
void
TALER_MERCHANT_refund_increase_cancel (struct TALER_MERCHANT_RefundIncreaseOperation *rio)
{
  if (NULL != rio->job)
  {
    GNUNET_CURL_job_cancel (rio->job);
    rio->job = NULL;
  }
  GNUNET_free (rio->url);
  GNUNET_free (rio->json_enc);
  GNUNET_free (rio);
}

/**
 * Increase the refund associated to a order
 *
 * @param ctx the CURL context used to connect to the backend
 * @param backend_uri backend's base URL, including final "/"
 * @param order_id id of the order whose refund is to be increased
 * @param refund amount to which increase the refund
 * @param reason human-readable reason justifying the refund
 * @param instance id of the merchant instance issuing the request
 * @param cb callback processing the response from /refund
 * @param cb_cls closure for cb
 */
struct TALER_MERCHANT_RefundIncreaseOperation *
TALER_MERCHANT_refund_increase (struct GNUNET_CURL_Context *ctx,
                                const char *backend_uri,
                                const char *order_id,
                                const struct TALER_Amount *refund,
                                const char *reason,
                                const char *instance,
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
  GNUNET_asprintf (&rio->url,
                   "%s%s",
                   backend_uri,
                   "/refund");
  req = json_pack ("{s:o, s:s, s:s, s:s}",
                   "refund", TALER_JSON_from_amount (refund),
                   "order_id", order_id,
                   "reason", reason,
                   "instance", instance);
  eh = curl_easy_init ();
  rio->json_enc = json_dumps (req,
                              JSON_COMPACT);
  json_decref (req);
  if (NULL == rio->json_enc)
  {
    GNUNET_break (0);
    GNUNET_free (rio);
    return NULL;
  }
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   rio->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   rio->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (rio->json_enc)));
  rio->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_refund_increase_finished,
                                  rio);
  return rio;
}

/**
 * Process GET /refund response
 */
void
handle_refund_lookup_finished (void *cls,
                               long response_code,
                               const json_t *json)
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo = cls;
  char *error;
  char *hint;
  enum TALER_ErrorCode code;

  rlo->job = NULL;
  switch (response_code)
  {
  case 0:
    /* Hard error */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Backend didn't even return from GET /refund\n");
    return;
  case MHD_HTTP_OK:
    rlo->cb (rlo->cb_cls,
             MHD_HTTP_OK,
             TALER_EC_NONE,
             json);
    break;
  default:
    /**
     * The backend gave response, but it's error, log it.
     * NOTE that json must be a Taler-specific error object (FIXME,
     * need a link to error objects at docs)
     */
    json_unpack ((json_t *) json,
                 "{s:s, s:I, s:s}",
                 "error", &error,
                 "code", &code,
                 "hint", &hint);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed GET /refund, error: %s, code: %d, hint: %s\n",
                error,
                code,
                hint);
  }
}

/**
 * Does a GET /refund.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param cb callback which will work the response gotten from the backend
 * @param cb_cls closure to pass to the callback
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_RefundLookupOperation *
TALER_MERCHANT_refund_lookup (struct GNUNET_CURL_Context *ctx,
                              const char *backend_uri,
                              const char *order_id,
                              const char *instance,
                              TALER_MERCHANT_RefundLookupCallback cb,
                              void *cb_cls)
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo;
  CURL *eh;

  rlo = GNUNET_new (struct TALER_MERCHANT_RefundLookupOperation);
  rlo->ctx = ctx;
  rlo->cb = cb;
  rlo->cb_cls = cb_cls;

  GNUNET_asprintf (&rlo->url,
                   "%s/refund?instance=%s&order_id=%s",
                   backend_uri,
                   instance,
                   order_id);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    rlo->url))
  {
    GNUNET_break (0);
    return NULL;
  }

  if (NULL == (rlo->job = GNUNET_CURL_job_add (ctx,
                                               eh,
                                               GNUNET_NO,
                                               handle_refund_lookup_finished,
                                               rlo)))
  {
    GNUNET_break (0);
    return NULL;
  
  }

  return rlo;
}
