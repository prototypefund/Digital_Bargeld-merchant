/*
  This file is part of TALER
  Copyright (C) 2014-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with TALER; see the file COPYING.LGPL.  If not,
  see <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_post_order_claim.c
 * @brief Implementation of POST /orders/$ID/claim
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


/**
 * Structure representing a POST /orders/$ID/claim operation.
 */
struct TALER_MERCHANT_OrderClaimHandle
{
  /**
   * Full URL, includes "/orders/$ID/claim".
   */
  char *url;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_OrderClaimCallback cb;

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
 * POST /orders/$ID/claim request.
 *
 * @param cls the `struct TALER_MERCHANT_OrderClaimHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, should be NULL
 */
static void
handle_post_order_claim_finished (void *cls,
                                  long response_code,
                                  const void *response)
{
  struct TALER_MERCHANT_OrderClaimHandle *och = cls;
  json_t *contract_terms;
  struct TALER_MerchantSignatureP sig;
  struct GNUNET_HashCode hash;
  const json_t *json = response;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("contract_terms",
                           &contract_terms),
    GNUNET_JSON_spec_fixed_auto ("sig",
                                 &sig),
    GNUNET_JSON_spec_end ()
  };
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  och->job = NULL;
  if (MHD_HTTP_OK != response_code)
  {
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Proposal lookup failed with HTTP status code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    och->cb (och->cb_cls,
             &hr,
             NULL,
             NULL,
             NULL);
    TALER_MERCHANT_order_claim_cancel (och);
    return;
  }

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Claiming order failed: could not parse JSON response\n");
    GNUNET_break_op (0);
    hr.ec = TALER_EC_INVALID_RESPONSE;
    hr.http_status = 0;
    och->cb (och->cb_cls,
             &hr,
             NULL,
             NULL,
             NULL);
    TALER_MERCHANT_order_claim_cancel (och);
    return;
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &hash))
  {
    GNUNET_break (0);
    hr.ec = TALER_EC_CLIENT_INTERNAL_FAILURE;
    hr.http_status = 0;
    GNUNET_JSON_parse_free (spec);
    och->cb (och->cb_cls,
             &hr,
             NULL,
             NULL,
             NULL);
    TALER_MERCHANT_order_claim_cancel (och);
    return;
  }

  och->cb (och->cb_cls,
           &hr,
           contract_terms,
           &sig,
           &hash);
  GNUNET_JSON_parse_free (spec);
  TALER_MERCHANT_order_claim_cancel (och);
}


/**
 * Calls the GET /proposal API at the backend.  That is,
 * retrieve a proposal data by providing its transaction id.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param nonce nonce used to perform the lookup
 * @param cb callback which will work the response gotten from the backend
 * @param cb_cls closure to pass to @a cb
 * @return handle for this handle, NULL upon errors
 */
struct TALER_MERCHANT_OrderClaimHandle *
TALER_MERCHANT_order_claim (struct GNUNET_CURL_Context *ctx,
                            const char *backend_url,
                            const char *order_id,
                            const struct GNUNET_CRYPTO_EddsaPublicKey *nonce,
                            TALER_MERCHANT_OrderClaimCallback cb,
                            void *cb_cls)
{
  struct TALER_MERCHANT_OrderClaimHandle *och;
  json_t *req_obj;

  req_obj = json_pack ("{s:o}",
                       "nonce",
                       GNUNET_JSON_from_data_auto (nonce));
  if (NULL == req_obj)
  {
    GNUNET_break (0);
    return NULL;
  }
  och = GNUNET_new (struct TALER_MERCHANT_OrderClaimHandle);
  och->ctx = ctx;
  och->cb = cb;
  och->cb_cls = cb_cls;
  {
    char *path;

    GNUNET_asprintf (&path,
                     "orders/%s/claim",
                     order_id);
    och->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == och->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    json_decref (req_obj);
    GNUNET_free (och);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Claiming order at %s\n",
              och->url);
  {
    CURL *eh;

    eh = curl_easy_init ();
    GNUNET_assert (NULL != eh);
    GNUNET_assert (GNUNET_OK ==
                   TALER_curl_easy_post (&och->post_ctx,
                                         eh,
                                         req_obj));
    json_decref (req_obj);
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_URL,
                                     och->url));
    och->job = GNUNET_CURL_job_add2 (ctx,
                                     eh,
                                     och->post_ctx.headers,
                                     &handle_post_order_claim_finished,
                                     och);
    GNUNET_assert (NULL != och->job);
  }
  return och;
}


/**
 * Cancel a POST /orders/$ID/claim request.
 *
 * @param och handle to the request to be canceled
 */
void
TALER_MERCHANT_order_claim_cancel (struct TALER_MERCHANT_OrderClaimHandle *och)
{
  if (NULL != och->job)
  {
    GNUNET_CURL_job_cancel (och->job);
    och->job = NULL;
  }
  TALER_curl_easy_post_finished (&och->post_ctx);
  GNUNET_free (och->url);
  GNUNET_free (och);
}


/* end of merchant_api_post_order_claim.c */
