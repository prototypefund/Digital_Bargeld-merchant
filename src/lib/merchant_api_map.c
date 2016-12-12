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
 * @file lib/merchant_api_map.c
 * @brief Implementation of the /map/{in,out} request of the merchant's HTTP API
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


/**
 * This structure acts like a "handle" for both /map/in and
 * /map/out operations, as they only differ about the 'json_enc'
 * field (which is just left NULL when not needed).
 */
struct TALER_MERCHANT_MapOperation
{
  /**
   * Full URI, includes "/map/in".
   */
  char *url;

  /**
   * Request's body.  Left NULL in case of /map/out.
   */
  char *json_enc;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_MapOperationCallback cb;

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
 * Cancel a /map/{in,out} request.
 *
 * @param mio handle to the request to be canceled
 */
void
TALER_MERCHANT_map_cancel (struct TALER_MERCHANT_MapOperation *mo)
{
  if (NULL != mo->job)
  {
    GNUNET_CURL_job_cancel (mo->job);
    mo->job = NULL;
  }
  GNUNET_free (mo->url);
  GNUNET_free_non_null (mo->json_enc);
  GNUNET_free (mo);
}


/**
 * Function called when we're done processing the HTTP /map/{in,out} request.
 *
 * @param cls the `struct TALER_MERCHANT_MapInOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, should be NULL
 */
static void
handle_map_finished (void *cls,
                     long response_code,
                     const json_t *json)
{
  struct TALER_MERCHANT_MapOperation *mo = cls;

  /**
   * As no data is supposed to be extracted from this
   * call, we just invoke the provided callback from here.
   */
  mo->cb (mo->cb_cls,
          response_code,
          json);
}

/**
 * Issue a /map/out request to the backend.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param h_contract hashcode of `contract`
 * @param map_in_cb callback which will work the response gotten from the backend
 * @param map_in_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_MapOperation *
TALER_MERCHANT_map_out (struct GNUNET_CURL_Context *ctx,
                        const char *backend_uri,
                        const struct GNUNET_HashCode *h_contract,
                        TALER_MERCHANT_MapOperationCallback map_cb,
                        void *map_cb_cls)
{
  struct TALER_MERCHANT_MapOperation *mo;
  CURL *eh;
  char *hash_enc;

  mo = GNUNET_new (struct TALER_MERCHANT_MapOperation);
  mo->ctx = ctx;
  mo->cb = map_cb;
  mo->cb_cls = map_cb_cls;

  hash_enc = GNUNET_STRINGS_data_to_string_alloc (h_contract,
                                                  sizeof (struct GNUNET_HashCode));
  GNUNET_asprintf (&mo->url,
                   "%s/map/out?h_contract=%s",
                   backend_uri,
                   hash_enc);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    mo->url))
  {
    GNUNET_break (0);  
    return NULL;
  }

  if (NULL == (mo->job = GNUNET_CURL_job_add (ctx,
                                              eh,
                                              GNUNET_YES,
                                              &handle_map_finished,
                                              mo)))
  {
    GNUNET_break (0);
    return NULL;
  }
  return mo;
}

/**
 * Issue a /map/in request to the backend.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param contract contract to store
 * @param h_contract hashcode of `contract`
 * @param map_in_cb callback which will work the response gotten from the backend
 * @param map_in_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_MapOperation *
TALER_MERCHANT_map_in (struct GNUNET_CURL_Context *ctx,
                       const char *backend_uri,
                       const json_t *contract,
                       const struct GNUNET_HashCode *h_contract,
                       TALER_MERCHANT_MapOperationCallback map_cb,
                       void *map_cb_cls)
{
  struct TALER_MERCHANT_MapOperation *mo;
  CURL *eh;
  json_t *req;
  
  mo = GNUNET_new (struct TALER_MERCHANT_MapOperation);
  mo->ctx = ctx;
  mo->cb = map_cb;
  mo->cb_cls = map_cb_cls;

  GNUNET_asprintf (&mo->url,
                   "%s%s",
                   backend_uri,
                   "/map/in");

  // build final json
  req = json_pack ("{s:o, s:o}",
                   "contract", contract,
                   "h_contract", GNUNET_JSON_from_data_auto (h_contract));

  GNUNET_assert (NULL !=
                  (mo->json_enc = json_dumps (req, JSON_COMPACT))
                );

  json_decref (req);
  eh = curl_easy_init ();

  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   mo->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   mo->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (mo->json_enc)));
  mo->job = GNUNET_CURL_job_add (ctx,
                                 eh,
                                 GNUNET_YES,
                                 &handle_map_finished,
                                 mo);
  return mo;
}
