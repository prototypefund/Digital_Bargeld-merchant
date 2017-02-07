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
 * @file lib/merchant_api_track_transfer.c
 * @brief Implementation of the /track/transfer request of the
 * merchant's HTTP API
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


/**
 * @brief A Handle for tracking wire transfers.
 */
struct TALER_MERCHANT_TrackTransferHandle
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
  TALER_MERCHANT_TrackTransferCallback cb;

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
 * We got a #MHD_HTTP_OK response for the /track/transfer request.
 * Check that the response is well-formed and if it is, call the
 * callback.  If not, return an error code.
 *
 * This code is very similar to
 * exchange_api_track_transfer.c::check_track_transfer_response_ok.
 * (Except we do not check the signature, as that was done by the
 * backend which we trust already.)
 * Any changes should likely be reflected there as well.
 *
 * @param wdh handle to the operation
 * @param json response we got
 * @return #GNUNET_OK if we are done and all is well,
 *         #GNUNET_SYSERR if the response was bogus
 */
static int
check_track_transfer_response_ok (struct TALER_MERCHANT_TrackTransferHandle *wdh,
                                  const json_t *json)
{
  json_t *details_j;
  struct GNUNET_HashCode h_wire;
  struct TALER_Amount total_amount;
  struct TALER_MerchantPublicKeyP merchant_pub;
  unsigned int num_details;
  struct TALER_ExchangePublicKeyP exchange_pub;
  struct GNUNET_JSON_Specification inner_spec[] = {
    TALER_JSON_spec_amount ("total", &total_amount),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &merchant_pub),
    GNUNET_JSON_spec_fixed_auto ("H_wire", &h_wire),
    GNUNET_JSON_spec_json ("deposits", &details_j),
    GNUNET_JSON_spec_fixed_auto ("exchange_pub", &exchange_pub),
    GNUNET_JSON_spec_end()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         inner_spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  num_details = json_array_size (details_j);
  {
    struct TALER_TrackTransferDetails details[num_details];
    unsigned int i;

    for (i=0;i<num_details;i++)
    {
      struct TALER_TrackTransferDetails *detail = &details[i];
      struct json_t *detail_j = json_array_get (details_j, i);
      struct GNUNET_JSON_Specification spec_detail[] = {
        GNUNET_JSON_spec_fixed_auto ("h_proposal_data", &detail->h_proposal_data),
        GNUNET_JSON_spec_fixed_auto ("coin_pub", &detail->coin_pub),
        TALER_JSON_spec_amount ("deposit_value", &detail->coin_value),
        TALER_JSON_spec_amount ("deposit_fee", &detail->coin_fee),
        GNUNET_JSON_spec_end()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (detail_j,
                             spec_detail,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        GNUNET_JSON_parse_free (inner_spec);
        return GNUNET_SYSERR;
      }
    }
    wdh->cb (wdh->cb_cls,
             MHD_HTTP_OK,
	     TALER_EC_NONE,
             &exchange_pub,
             json,
             &h_wire,
             &total_amount,
             num_details,
             details);
  }
  GNUNET_JSON_parse_free (inner_spec);
  return GNUNET_OK;
}


/**
 * Function called when we're done processing the
 * HTTP /track/transfer request.
 *
 * @param cls the `struct TALER_MERCHANT_TrackTransferHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_track_transfer_finished (void *cls,
                                long response_code,
                                const json_t *json)
{
  struct TALER_MERCHANT_TrackTransferHandle *tdo = cls;

  tdo->job = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK ==
        check_track_transfer_response_ok (tdo,
                                          json))
      return;
    GNUNET_break_op (0);
    response_code = 0;
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
  tdo->cb (tdo->cb_cls,
           response_code,
	   TALER_JSON_get_error_code (json),
           NULL,
           json,
           NULL,
           NULL,
           0,
           NULL);
}


/**
 * Request backend to return transfers associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_uri base URI of the backend
 * @param instance which merchant instance is going to be tracked
 * @param wtid base32 string indicating a wtid
 * @param exchange base URL of the exchange in charge of returning the wanted information
 * @param track_transfer_cb the callback to call when a reply for this request is available
 * @param track_transfer_cb_cls closure for @a contract_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransferHandle *
TALER_MERCHANT_track_transfer (struct GNUNET_CURL_Context *ctx,
                               const char *backend_uri,
                               const char *instance,
                               const struct TALER_WireTransferIdentifierRawP *wtid,
                               const char *exchange_uri,
                               TALER_MERCHANT_TrackTransferCallback track_transfer_cb,
                               void *track_transfer_cb_cls)
{
  struct TALER_MERCHANT_TrackTransferHandle *tdo;
  CURL *eh;
  char *wtid_str;

  wtid_str = GNUNET_STRINGS_data_to_string_alloc (wtid,
                                                  sizeof (struct TALER_WireTransferIdentifierRawP));
  tdo = GNUNET_new (struct TALER_MERCHANT_TrackTransferHandle);
  tdo->ctx = ctx;
  tdo->cb = track_transfer_cb; // very last to be called
  tdo->cb_cls = track_transfer_cb_cls;
  /* TODO: do we need to escape 'exchange_uri' here? */
  GNUNET_asprintf (&tdo->url,
                   "%s/track/transfer?wtid=%s&exchange=%s&instance=%s",
                   backend_uri,
                   wtid_str,
                   exchange_uri,
		   instance);
  GNUNET_free (wtid_str);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tdo->url));
  tdo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_track_transfer_finished,
                                  tdo);
  return tdo;
}


/**
 * Cancel a /track/transfer request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param co the transfer's tracking handle
 */
void
TALER_MERCHANT_track_transfer_cancel (struct TALER_MERCHANT_TrackTransferHandle *tdo)
{
  if (NULL != tdo->job)
  {
    GNUNET_CURL_job_cancel (tdo->job);
    tdo->job = NULL;
  }
  GNUNET_free (tdo->url);
  GNUNET_free (tdo);
}

/* end of merchant_api_track_transfer.c */
