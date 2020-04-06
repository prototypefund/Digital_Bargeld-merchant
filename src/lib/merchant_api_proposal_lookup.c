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
 * @file lib/merchant_api_proposal_lookup.c
 * @brief Implementation of the /proposal GET
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
 * Structure representing a GET /proposal operation.
 */
struct TALER_MERCHANT_ProposalLookupOperation
{
  /**
   * Full URL, includes "/proposal".
   */
  char *url;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_ProposalLookupOperationCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Should we send the lookup operation with a nonce?
   */
  int has_nonce;

  /**
   * Nonce, only initialized if has_nonce is GNUNET_YES.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey nonce;

};


/**
 * Function called when we're done processing the GET /proposal request.
 *
 * @param cls the `struct TALER_MERCHANT_ProposalLookupOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, should be NULL
 */
static void
handle_proposal_lookup_finished (void *cls,
                                 long response_code,
                                 const void *response)
{
  struct TALER_MERCHANT_ProposalLookupOperation *plo = cls;
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

  plo->job = NULL;
  if (MHD_HTTP_OK != response_code)
  {
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Proposal lookup failed with HTTP status code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    plo->cb (plo->cb_cls,
             &hr,
             NULL,
             NULL,
             NULL);
    TALER_MERCHANT_proposal_lookup_cancel (plo);
    return;
  }

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "proposal lookup failed to parse JSON\n");
    GNUNET_break_op (0);
    hr.ec = TALER_EC_INVALID_RESPONSE;
    hr.http_status = 0;
    plo->cb (plo->cb_cls,
             &hr,
             NULL,
             NULL,
             NULL);
    TALER_MERCHANT_proposal_lookup_cancel (plo);
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
    plo->cb (plo->cb_cls,
             &hr,
             NULL,
             NULL,
             NULL);
    TALER_MERCHANT_proposal_lookup_cancel (plo);
    return;
  }

  plo->job = NULL;
  /**
   * As no data is supposed to be extracted from this
   * call, we just invoke the provided callback.
   */
  plo->cb (plo->cb_cls,
           &hr,
           contract_terms,
           &sig,
           &hash);
  GNUNET_JSON_parse_free (spec);
  TALER_MERCHANT_proposal_lookup_cancel (plo);
}


/**
 * Calls the GET /proposal API at the backend.  That is,
 * retrieve a proposal data by providing its transaction id.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param nonce nonce used to perform the lookup
 * @param plo_cb callback which will work the response gotten from the backend
 * @param plo_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_ProposalLookupOperation *
TALER_MERCHANT_proposal_lookup (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *order_id,
  const struct GNUNET_CRYPTO_EddsaPublicKey *nonce,
  TALER_MERCHANT_ProposalLookupOperationCallback plo_cb,
  void *plo_cb_cls)
{
  struct TALER_MERCHANT_ProposalLookupOperation *plo;
  CURL *eh;
  char *nonce_str = NULL;

  plo = GNUNET_new (struct TALER_MERCHANT_ProposalLookupOperation);
  plo->ctx = ctx;
  plo->cb = plo_cb;
  plo->cb_cls = plo_cb_cls;
  if (NULL != nonce)
  {
    plo->has_nonce = GNUNET_YES;
    plo->nonce = *nonce;
    nonce_str = GNUNET_STRINGS_data_to_string_alloc (
      nonce,
      sizeof (struct GNUNET_CRYPTO_EddsaPublicKey));
  }
  plo->url = TALER_url_join (backend_url,
                             "proposal",
                             "order_id",
                             order_id,
                             "nonce",
                             nonce_str,
                             NULL);
  GNUNET_free_non_null (nonce_str);
  if (NULL == plo->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (plo);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "looking up proposal from %s\n",
              plo->url);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    plo->url))
  {
    GNUNET_break (0);
    curl_easy_cleanup (eh);
    GNUNET_free (plo->url);
    GNUNET_free (plo);
    return NULL;
  }

  if (NULL == (plo->job = GNUNET_CURL_job_add (ctx,
                                               eh,
                                               GNUNET_YES,
                                               &handle_proposal_lookup_finished,
                                               plo)))
  {
    GNUNET_break (0);
    GNUNET_free (plo->url);
    GNUNET_free (plo);
    return NULL;
  }
  return plo;
}


/**
 * Cancel a GET /proposal request.
 *
 * @param plo handle to the request to be canceled
 */
void
TALER_MERCHANT_proposal_lookup_cancel (
  struct TALER_MERCHANT_ProposalLookupOperation *plo)
{
  if (NULL != plo->job)
  {
    GNUNET_CURL_job_cancel (plo->job);
    plo->job = NULL;
  }
  GNUNET_free (plo->url);
  GNUNET_free (plo);
}


/* end of merchant_api_proposal_lookup.c */
