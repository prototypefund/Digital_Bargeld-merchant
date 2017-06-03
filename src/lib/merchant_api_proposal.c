/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

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
 * @brief Implementation of the /proposal PUT and GET
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
   * JSON encoding of the request to POST.
   */
  char *json_enc;

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
};

/**
 * Structure representing a GET /proposal operation.
 */
struct TALER_MERCHANT_ProposalLookupOperation
{
  /**
   * Full URI, includes "/proposal".
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

};


/**
 * Function called when we're done processing the
 * HTTP PUT /proposal request.
 *
 * @param cls the `struct TALER_MERCHANT_ProposalOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_proposal_finished (void *cls,
                          long response_code,
                          const json_t *json)
{
  struct TALER_MERCHANT_ProposalOperation *po = cls;
  json_t *contract_terms;
  const struct TALER_MerchantSignatureP *sigp;
  const struct GNUNET_HashCode *hashp;
  struct TALER_MerchantSignatureP sig;
  struct GNUNET_HashCode hash;

  po->job = NULL;
  contract_terms = NULL;
  sigp = NULL;
  hashp = NULL;
  switch (response_code)
  {
    case 0:
      break;
    case MHD_HTTP_OK:
    {
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_json ("data", &contract_terms),
        GNUNET_JSON_spec_fixed_auto ("sig", &sig),
        GNUNET_JSON_spec_fixed_auto ("hash", &hash),
        GNUNET_JSON_spec_end()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (json,
                             spec,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        response_code = 0;
        break;
      }
      hashp = &hash;
      sigp = &sig;
    }
      break;
    case MHD_HTTP_BAD_REQUEST:
      /* This should never happen, either us or the merchant is buggy
         (or API version conflict); just pass JSON reply to the application */
      break;
    case MHD_HTTP_FORBIDDEN:
      break;
    case MHD_HTTP_UNAUTHORIZED:
      /* Nothing really to verify, merchant says one of the signatures is
         invalid; as we checked them, this should never happen, we
         should pass the JSON reply to the application */
      break;
    case MHD_HTTP_NOT_FOUND:
      /* Nothing really to verify, this should never
         happen, we should pass the JSON reply to the application */
      break;
    case MHD_HTTP_INTERNAL_SERVER_ERROR:
      /* Server had an internal issue; we should retry, but this API
         leaves this to the application */
      break;
    default:
      /* unexpected response code */
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unexpected response code %u\n",
                  (unsigned int) response_code);
      GNUNET_break (0);
      response_code = 0;
  }
  po->cb (po->cb_cls,
          response_code,
	  TALER_JSON_get_error_code (json),
          json,
          contract_terms,
          sigp,
          hashp);
  if (NULL != contract_terms)
    json_decref (contract_terms);
}


/**
 * PUT an order to the backend and receives the related proposal.
 *
 * @param ctx execution context
 * @param backend_uri URI of the backend
 * @param order basic information about this purchase, to be extended by the
 * backend
 * @param proposal_cb the callback to call when a reply for this request is
 * available
 * @param proposal_cb_cls closure for @a proposal_cb
 * @return a handle for this request, NULL on error
 */
struct TALER_MERCHANT_ProposalOperation *
TALER_MERCHANT_order_put (struct GNUNET_CURL_Context *ctx,
                          const char *backend_uri,
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
  GNUNET_asprintf (&po->url,
                   "%s%s",
                   backend_uri,
                   "/proposal");

  req = json_pack ("{s:O}",
                   "order", (json_t *) order);
  eh = curl_easy_init ();
  po->json_enc = json_dumps (req,
                             JSON_COMPACT);
  json_decref (req);
  if (NULL == po->json_enc)
  {
    GNUNET_break (0);
    GNUNET_free (po);
    return NULL;
  }
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   po->url));
  /* FIXME: as for the specs, POST becomes PUT */
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   po->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (po->json_enc)));
  po->job = GNUNET_CURL_job_add (ctx,
                                 eh,
                                 GNUNET_YES,
                                 &handle_proposal_finished,
                                 po);
  return po;
}

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
                                 const json_t *json)
{
  struct TALER_MERCHANT_ProposalLookupOperation *plo = cls;

  /**
   * As no data is supposed to be extracted from this
   * call, we just invoke the provided callback.
   */
  plo->cb (plo->cb_cls,
           response_code,
           json);
}

/**
 * Calls the GET /proposal API at the backend.  That is,
 * retrieve a proposal data by providing its transaction id.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param transaction_id transaction id used to perform the lookup
 * @param plo_cb callback which will work the response gotten from the backend
 * @param plo_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_ProposalLookupOperation *
TALER_MERCHANT_proposal_lookup (struct GNUNET_CURL_Context *ctx,
                                const char *backend_uri,
                                const char *order_id,
                                const char *instance,
                                TALER_MERCHANT_ProposalLookupOperationCallback plo_cb,
                                void *plo_cb_cls)
{
  struct TALER_MERCHANT_ProposalLookupOperation *plo;
  CURL *eh;

  plo = GNUNET_new (struct TALER_MERCHANT_ProposalLookupOperation);
  plo->ctx = ctx;
  plo->cb = plo_cb;
  plo->cb_cls = plo_cb_cls;

  GNUNET_asprintf (&plo->url,
                   "%s/proposal?order_id=%s&instance=%s",
                   backend_uri,
                   order_id,
                   instance);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    plo->url))
  {
    GNUNET_break (0);
    return NULL;
  }

  if (NULL == (plo->job = GNUNET_CURL_job_add (ctx,
                                               eh,
                                               GNUNET_YES,
                                               &handle_proposal_lookup_finished,
                                               plo)))
  {
    GNUNET_break (0);
    return NULL;
  }
  return plo;

}

/**
 * Cancel a PUT /proposal request.  This function cannot be used
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
  GNUNET_free (po->json_enc);
  GNUNET_free (po);
}

/**
 * Cancel a GET /proposal request.
 *
 * @param plo handle to the request to be canceled
 */
void
TALER_MERCHANT_proposal_lookup_cancel (struct TALER_MERCHANT_ProposalLookupOperation *plo)
{
  if (NULL != plo->job)
  {
    GNUNET_CURL_job_cancel (plo->job);
    plo->job = NULL;
  }
  GNUNET_free (plo->url);
  GNUNET_free (plo);
}

/* end of merchant_api_contract.c */
