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
 * @file lib/merchant_api_proposal.c
 * @brief Implementation of the /proposal POST
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
 * @brief A Contract Operation Handle
 */
struct TALER_MERCHANT_ProposalOperation
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
  TALER_MERCHANT_ProposalCallback cb;

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
 * HTTP POST /proposal request.
 *
 * @param cls the `struct TALER_MERCHANT_ProposalOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not JSON
 */
static void
handle_proposal_finished (void *cls,
                          long response_code,
                          const void *response)
{
  struct TALER_MERCHANT_ProposalOperation *po = cls;
  const char *order_id = NULL;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("order_id",
                             &order_id),
    GNUNET_JSON_spec_end ()
  };

  po->job = NULL;
  switch (response_code)
  {
  case 0:
    hr.ec = TALER_EC_INVALID_RESPONSE;
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK !=
        GNUNET_JSON_parse (json,
                           spec,
                           NULL, NULL))
    {
      GNUNET_break_op (0);
      hr.http_status = 0;
      hr.ec = TALER_EC_PROPOSAL_REPLY_MALFORMED;
    }
    break;
  case MHD_HTTP_BAD_REQUEST:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    /* This should never happen, either us or
       the merchant is buggy (or API version conflict);
       just pass JSON reply to the application */
    break;
  case MHD_HTTP_CONFLICT:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_FORBIDDEN:
    /* Nothing really to verify, merchant says one
       of the signatures is invalid; as we checked them,
       this should never happen, we should pass the JSON
       reply to the application */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry,
       but this API leaves this to the application */
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
    GNUNET_break_op (0);
    break;
  }
  po->cb (po->cb_cls,
          &hr,
          order_id);
  if (MHD_HTTP_OK == response_code)
    GNUNET_JSON_parse_free (spec);
  TALER_MERCHANT_proposal_cancel (po);
}


/**
 * POST an order to the backend and receives the related proposal.
 *
 * @param ctx execution context
 * @param backend_url URL of the backend
 * @param order basic information about this purchase,
 *        to be extended by the backend
 * @param proposal_cb the callback to call when a reply
 *        for this request is available
 * @param proposal_cb_cls closure for @a proposal_cb
 * @return a handle for this request, NULL on error
 */
struct TALER_MERCHANT_ProposalOperation *
TALER_MERCHANT_order_put (struct GNUNET_CURL_Context *ctx,
                          const char *backend_url,
                          const json_t *order,
                          TALER_MERCHANT_ProposalCallback proposal_cb,
                          void *proposal_cb_cls)
{
  struct TALER_MERCHANT_ProposalOperation *po;
  json_t *req;
  CURL *eh;

  po = GNUNET_new (struct TALER_MERCHANT_ProposalOperation);
  po->ctx = ctx;
  po->cb = proposal_cb;
  po->cb_cls = proposal_cb_cls;
  po->url = TALER_url_join (backend_url, "order", NULL);
  req = json_pack ("{s:O}",
                   "order", (json_t *) order);
  eh = curl_easy_init ();
  if (GNUNET_OK != TALER_curl_easy_post (&po->post_ctx,
                                         eh,
                                         req))
  {
    GNUNET_break (0);
    json_decref (req);
    GNUNET_free (po);
    return NULL;
  }
  json_decref (req);

  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   po->url));
  po->job = GNUNET_CURL_job_add2 (ctx,
                                  eh,
                                  po->post_ctx.headers,
                                  &handle_proposal_finished,
                                  po);
  return po;
}


/**
 * Cancel a POST /proposal request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param po the proposal operation request handle
 */
void
TALER_MERCHANT_proposal_cancel (struct TALER_MERCHANT_ProposalOperation *po)
{
  if (NULL != po->job)
  {
    GNUNET_CURL_job_cancel (po->job);
    po->job = NULL;
  }
  GNUNET_free (po->url);
  TALER_curl_easy_post_finished (&po->post_ctx);
  GNUNET_free (po);
}


/* end of merchant_api_proposal.c */
