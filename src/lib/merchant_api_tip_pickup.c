/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

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
 * @file lib/merchant_api_tip_pickup.c
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
#include "merchant_api_common.h"


/**
 * @brief A handle for tracking transactions.
 */
struct TALER_MERCHANT_TipPickupOperation
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
  TALER_MERCHANT_TipPickupCallback cb;

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
check_ok (struct TALER_MERCHANT_TipPickupOperation *tpo,
          const json_t *json)
{
  struct TALER_ReservePublicKeyP reserve_pub;
  json_t *ja;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("reserve_pub", &reserve_pub),
    GNUNET_JSON_spec_json ("reserve_sigs", &ja),
    GNUNET_JSON_spec_end()
  };
  unsigned int ja_len;

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
    return GNUNET_SYSERR;
  }
  {
    struct TALER_ReserveSignatureP reserve_sigs[ja_len];

    for (unsigned int i=0;i<ja_len;i++)
    {
      json_t *pj = json_array_get (ja, i);

      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_fixed_auto ("reserve_sig", &reserve_sigs[i]),
        GNUNET_JSON_spec_end()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (pj,
                             spec,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        return GNUNET_SYSERR;
      }
    }
    tpo->cb (tpo->cb_cls,
             MHD_HTTP_OK,
             TALER_JSON_get_error_code (json),
             &reserve_pub,
             ja_len,
             reserve_sigs,
             json);
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
                            const json_t *json)
{
  struct TALER_MERCHANT_TipPickupOperation *tpo = cls;

  tpo->job = NULL;
  switch (response_code)
  {
  case MHD_HTTP_OK:
    if (GNUNET_OK != check_ok (tpo,
                               json))
    {
      GNUNET_break_op (0);
      response_code = 0;
    }
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
  if (NULL != tpo->cb)
    tpo->cb (tpo->cb_cls,
             response_code,
             TALER_JSON_get_error_code (json),
             NULL,
             0,
             NULL,
             json);
  TALER_MERCHANT_tip_pickup_cancel (tpo);
}


/**
 * Issue a /tip-pickup request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param tip_id unique identifier for the tip
 * @param num_planches number of planchets provided in @a planchets
 * @param planchets array of planchets to be signed into existence for the tip
 * @param pickup_cb callback which will work the response gotten from the backend
 * @param pickup_cb_cls closure to pass to @a pickup_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipPickupOperation *
TALER_MERCHANT_tip_pickup (struct GNUNET_CURL_Context *ctx,
                           const char *backend_uri,
                           const struct GNUNET_HashCode *tip_id,
                           unsigned int num_planchets,
                           struct TALER_PlanchetDetail *planchets,
                           TALER_MERCHANT_TipPickupCallback pickup_cb,
                           void *pickup_cb_cls)
{
  struct TALER_MERCHANT_TipPickupOperation *tpo;
  CURL *eh;
  json_t *pa;
  json_t *tp_obj;

  pa = json_array ();
  for (unsigned int i=0;i<num_planchets;i++)
  {
    const struct TALER_PlanchetDetail *planchet = &planchets[i];
    json_t *p;

    p = json_pack ("{"
                   " s:o," /* denom_pub_hash */
                   " s:o," /* coin_ev */
                   "}",
                   "denom_pub_hash", GNUNET_JSON_from_data_auto (&planchet->denom_pub_hash),
                   "coin_ev", GNUNET_JSON_from_data (planchet->coin_ev,
                                                     planchet->coin_ev_size));
    if (NULL == p)
    {
      GNUNET_break (0);
      json_decref (pa);
      return NULL;
    }
    json_array_append_new (pa,
                           p);
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
  tpo = GNUNET_new (struct TALER_MERCHANT_TipPickupOperation);
  tpo->num_planchets = num_planchets;
  tpo->ctx = ctx;
  tpo->cb = pickup_cb;
  tpo->cb_cls = pickup_cb_cls;
  tpo->url = MAH_path_to_url_ (backend_uri,
                               "/tip-pickup");
  if (NULL == (tpo->json_enc =
               json_dumps (tp_obj,
                           JSON_COMPACT)))
  {
    GNUNET_break (0);
    json_decref (tp_obj);
    GNUNET_free (tpo->url);
    GNUNET_free (tpo);
    return NULL;
  }
  json_decref (tp_obj);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URI '%s'\n",
              tpo->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tpo->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   tpo->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (tpo->json_enc)));
  tpo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
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
TALER_MERCHANT_tip_pickup_cancel (struct TALER_MERCHANT_TipPickupOperation *tpo)
{
  if (NULL != tpo->job)
  {
    GNUNET_CURL_job_cancel (tpo->job);
    tpo->job = NULL;
  }
  GNUNET_free_non_null (tpo->json_enc);
  GNUNET_free (tpo->url);
  GNUNET_free (tpo);
}

/* end of merchant_api_tip_pickup.c */
