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
  TALER; see the file COPYING.LGPL.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_contract.c
 * @brief Implementation of the /contract request of the merchant's HTTP API
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
 * @brief A Contract Operation Handle
 */
struct TALER_MERCHANT_ContractOperation
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
  TALER_MERCHANT_ContractCallback cb;

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
 * HTTP /contract request.
 *
 * @param cls the `struct TALER_MERCHANT_Pay`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_contract_finished (void *cls,
                          long response_code,
                          const json_t *json)
{
  struct TALER_MERCHANT_ContractOperation *co = cls;
  json_t *contract;
  const struct TALER_MerchantSignatureP *sigp;
  const struct GNUNET_HashCode *h_contractp;
  struct TALER_MerchantSignatureP sig;
  struct GNUNET_HashCode h_contract;

  co->job = NULL;
  contract = NULL;
  sigp = NULL;
  h_contractp = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    {
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_json ("contract", &contract),
        GNUNET_JSON_spec_fixed_auto ("merchant_sig", &sig),
        GNUNET_JSON_spec_fixed_auto ("H_contract", &h_contract),
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
      h_contractp = &h_contract;
      sigp = &sig;
    }
    break;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the merchant is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_FORBIDDEN:
    /* Duplicate transaction ID, frontend is buggy! */
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
    break;
  }
  co->cb (co->cb_cls,
          response_code,
          json,
          contract,
          sigp,
          h_contractp);
  if (NULL != contract)
    json_decref (contract);
  TALER_MERCHANT_contract_sign_cancel (co);
}


/**
 * Request backend to sign a contract (and add fields like wire transfer
 * details).
 *
 * @param ctx execution context
 * @param backend_uri URI of the backend
 * @param contract prototype of the contract
 * @param contract_cb the callback to call when a reply for this request is available
 * @param contract_cb_cls closure for @a contract_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_ContractOperation *
TALER_MERCHANT_contract_sign (struct GNUNET_CURL_Context *ctx,
                              const char *backend_uri,
                              const json_t *contract,
                              TALER_MERCHANT_ContractCallback contract_cb,
                              void *contract_cb_cls)
{
  struct TALER_MERCHANT_ContractOperation *co;
  json_t *req;
  CURL *eh;

  co = GNUNET_new (struct TALER_MERCHANT_ContractOperation);
  co->ctx = ctx;
  co->cb = contract_cb;
  co->cb_cls = contract_cb_cls;
  co->url = GNUNET_strdup (backend_uri);

  req = json_pack ("{s:O}",
                   "contract", (json_t *) contract);
  eh = curl_easy_init ();
  GNUNET_assert (NULL != (co->json_enc =
                          json_dumps (req,
                                      JSON_COMPACT)));
  json_decref (req);
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   co->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   co->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (co->json_enc)));
  co->job = GNUNET_CURL_job_add (ctx,
                                 eh,
                                 GNUNET_YES,
                                 &handle_contract_finished,
                                 co);
  return co;
}


/**
 * Cancel a /contract request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param co the contract operation request handle
 */
void
TALER_MERCHANT_contract_sign_cancel (struct TALER_MERCHANT_ContractOperation *co)
{
  if (NULL != co->job)
  {
    GNUNET_CURL_job_cancel (co->job);
    co->job = NULL;
  }
  GNUNET_free (co->url);
  GNUNET_free (co->json_enc);
  GNUNET_free (co);
}


/* end of merchant_api_contract.c */
