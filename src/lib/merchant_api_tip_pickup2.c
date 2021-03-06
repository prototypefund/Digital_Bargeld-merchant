/*
  This file is part of TALER
  Copyright (C) 2014-2017, 2020 Taler Systems SA

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
 * @file lib/merchant_api_tip_pickup2.c
 * @brief Implementation of the /tip-pickup request of the merchant's HTTP API
 * @author Marcello Stanisci
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
#include <taler/taler_curl_lib.h>


/**
 * @brief A handle for tracking transactions.
 */
struct TALER_MERCHANT_TipPickup2Operation
{

  /**
   * The url for this request.
   */
  char *url;

  /**
   * Minor context that holds body and headers.
   */
  struct TALER_CURL_PostContext post_ctx;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_TipPickup2Callback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Expected number of planchets.
   */
  unsigned int num_planchets;
};


/**
 * We got a 200 response back from the exchange (or the merchant).
 * Now we need to parse the response and if it is well-formed,
 * call the callback (and set it to NULL afterwards).
 *
 * @param tpo handle of the original authorization operation
 * @param json cryptographic proof returned by the exchange/merchant
 * @return #GNUNET_OK if response is valid
 */
static int
check_ok (struct TALER_MERCHANT_TipPickup2Operation *tpo,
          const json_t *json)
{
  json_t *ja;
  unsigned int ja_len;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("blind_sigs", &ja),
    GNUNET_JSON_spec_end ()
  };
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = MHD_HTTP_OK,
    .reply = json
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  ja_len = json_array_size (ja);
  if (ja_len != tpo->num_planchets)
  {
    GNUNET_break_op (0);
    GNUNET_JSON_parse_free (spec);
    return GNUNET_SYSERR;
  }
  {
    struct TALER_MERCHANT_BlindSignature mblind_sigs[ja_len];
    struct GNUNET_CRYPTO_RsaSignature *blind_sigs[ja_len];

    for (unsigned int i = 0; i<ja_len; i++)
    {
      json_t *pj = json_array_get (ja, i);
      struct GNUNET_JSON_Specification ispec[] = {
        GNUNET_JSON_spec_rsa_signature ("blind_sig", &blind_sigs[i]),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (pj,
                             ispec,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        GNUNET_JSON_parse_free (spec);
        return GNUNET_SYSERR;
      }
      mblind_sigs[i].blind_sig = blind_sigs[i];
    }
    tpo->cb (tpo->cb_cls,
             &hr,
             ja_len,
             mblind_sigs);
    for (unsigned int i = 0; i<ja_len; i++)
      GNUNET_CRYPTO_rsa_signature_free (blind_sigs[i]);
    tpo->cb = NULL; /* do not call twice */
  }
  GNUNET_JSON_parse_free (spec);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /track/transaction request.
 *
 * @param cls the `struct TALER_MERCHANT_TipPickupOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_tip_pickup_finished (void *cls,
                            long response_code,
                            const void *response)
{
  struct TALER_MERCHANT_TipPickup2Operation *tpo = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  tpo->job = NULL;
  switch (response_code)
  {
  case MHD_HTTP_OK:
    if (GNUNET_OK != check_ok (tpo,
                               json))
    {
      GNUNET_break_op (0);
      hr.http_status = 0;
      hr.ec = TALER_EC_INVALID_RESPONSE;
    }
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_CONFLICT:
    /* legal, can happen if we pickup a tip twice... */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_NOT_FOUND:
    /* legal, can happen if tip ID is unknown */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  default:
    /* unexpected response code */
    GNUNET_break_op (0);
    TALER_MERCHANT_parse_error_details_ (json,
                                         response_code,
                                         &hr);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    break;
  }
  if (NULL != tpo->cb)
  {
    tpo->cb (tpo->cb_cls,
             &hr,
             0,
             NULL);
    tpo->cb = NULL;
  }
  TALER_MERCHANT_tip_pickup2_cancel (tpo);
}


/**
 * Issue a /tip-pickup request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param tip_id unique identifier for the tip
 * @param num_planches number of planchets provided in @a planchets
 * @param planchets array of planchets to be signed into existence for the tip
 * @param pickup_cb callback which will work the response gotten from the backend
 * @param pickup_cb_cls closure to pass to @a pickup_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipPickup2Operation *
TALER_MERCHANT_tip_pickup2 (struct GNUNET_CURL_Context *ctx,
                            const char *backend_url,
                            const struct GNUNET_HashCode *tip_id,
                            unsigned int num_planchets,
                            struct TALER_PlanchetDetail *planchets,
                            TALER_MERCHANT_TipPickup2Callback pickup_cb,
                            void *pickup_cb_cls)
{
  struct TALER_MERCHANT_TipPickup2Operation *tpo;
  CURL *eh;
  json_t *pa;
  json_t *tp_obj;

  if (0 == num_planchets)
  {
    GNUNET_break (0);
    return NULL;
  }
  pa = json_array ();
  for (unsigned int i = 0; i<num_planchets; i++)
  {
    const struct TALER_PlanchetDetail *planchet = &planchets[i];
    json_t *p;

    p = json_pack ("{"
                   " s:o," /* denom_pub_hash */
                   " s:o," /* coin_ev */
                   "}",
                   "denom_pub_hash", GNUNET_JSON_from_data_auto (
                     &planchet->denom_pub_hash),
                   "coin_ev", GNUNET_JSON_from_data (planchet->coin_ev,
                                                     planchet->coin_ev_size));
    if (NULL == p)
    {
      GNUNET_break (0);
      json_decref (pa);
      return NULL;
    }
    if (0 !=
        json_array_append_new (pa,
                               p))
    {
      GNUNET_break (0);
      json_decref (pa);
      return NULL;
    }
  }
  tp_obj = json_pack ("{"
                      " s:o," /* tip_id */
                      " s:o," /* planchets */
                      "}",
                      "tip_id", GNUNET_JSON_from_data_auto (tip_id),
                      "planchets", pa);
  if (NULL == tp_obj)
  {
    GNUNET_break (0);
    return NULL;
  }
  tpo = GNUNET_new (struct TALER_MERCHANT_TipPickup2Operation);
  tpo->num_planchets = num_planchets;
  tpo->ctx = ctx;
  tpo->cb = pickup_cb;
  tpo->cb_cls = pickup_cb_cls;

  tpo->url = TALER_url_join (backend_url,
                             "tip-pickup",
                             NULL);
  if (NULL == tpo->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    json_decref (tp_obj);
    GNUNET_free (tpo);
    return NULL;
  }
  eh = curl_easy_init ();
  if (GNUNET_OK != TALER_curl_easy_post (&tpo->post_ctx,
                                         eh,
                                         tp_obj))
  {
    GNUNET_break (0);
    json_decref (tp_obj);
    GNUNET_free (tpo);
    return NULL;
  }
  json_decref (tp_obj);

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              tpo->url);

  GNUNET_assert (CURLE_OK == curl_easy_setopt (eh,
                                               CURLOPT_URL,
                                               tpo->url));
  tpo->job = GNUNET_CURL_job_add2 (ctx,
                                   eh,
                                   tpo->post_ctx.headers,
                                   &handle_tip_pickup_finished,
                                   tpo);
  return tpo;
}


/**
 * Cancel a /track/transaction request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param tpo handle to the tracking operation being cancelled
 */
void
TALER_MERCHANT_tip_pickup2_cancel (
  struct TALER_MERCHANT_TipPickup2Operation *tpo)
{
  if (NULL != tpo->job)
  {
    GNUNET_CURL_job_cancel (tpo->job);
    tpo->job = NULL;
  }
  TALER_curl_easy_post_finished (&tpo->post_ctx);
  GNUNET_free (tpo->url);
  GNUNET_free (tpo);
}


/* end of merchant_api_tip_pickup2.c */
