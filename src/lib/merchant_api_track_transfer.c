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
 * exchange_api_transfers_get.c::check_transfers_get_response_ok.
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
check_transfers_get_response_ok (
  struct TALER_MERCHANT_TrackTransferHandle *wdh,
  const json_t *json)
{
  json_t *deposits;
  struct GNUNET_HashCode h_wire;
  struct TALER_Amount total_amount;
  struct TALER_MerchantPublicKeyP merchant_pub;
  unsigned int num_details;
  struct TALER_ExchangePublicKeyP exchange_pub;
  struct GNUNET_JSON_Specification inner_spec[] = {
    TALER_JSON_spec_amount ("total", &total_amount),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &merchant_pub),
    GNUNET_JSON_spec_fixed_auto ("h_wire", &h_wire),
    GNUNET_JSON_spec_json ("deposits_sums", &deposits),
    GNUNET_JSON_spec_fixed_auto ("exchange_pub", &exchange_pub),
    GNUNET_JSON_spec_end ()
  };
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = MHD_HTTP_OK,
    .reply = json
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         inner_spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  num_details = json_array_size (deposits);
  {
    struct TALER_MERCHANT_TrackTransferDetails details[num_details];

    for (unsigned int i = 0; i<num_details; i++)
    {
      struct TALER_MERCHANT_TrackTransferDetails *detail = &details[i];
      json_t *deposit = json_array_get (deposits, i);
      struct GNUNET_JSON_Specification spec_detail[] = {
        GNUNET_JSON_spec_string ("order_id", &detail->order_id),
        TALER_JSON_spec_amount ("deposit_value", &detail->deposit_value),
        TALER_JSON_spec_amount ("deposit_fee", &detail->deposit_fee),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (deposit,
                             spec_detail,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        GNUNET_JSON_parse_free (inner_spec);
        return GNUNET_SYSERR;
      }
    }
    wdh->cb (wdh->cb_cls,
             &hr,
             &exchange_pub,
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
handle_transfers_get_finished (void *cls,
                               long response_code,
                               const void *response)
{
  struct TALER_MERCHANT_TrackTransferHandle *tdo = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  tdo->job = NULL;
  switch (response_code)
  {
  case 0:
    hr.ec = TALER_EC_INVALID_RESPONSE;
    break;
  case MHD_HTTP_OK:
    if (GNUNET_OK ==
        check_transfers_get_response_ok (tdo,
                                         json))
    {
      TALER_MERCHANT_track_transfer_cancel (tdo);
      return;
    }
    GNUNET_break_op (0);
    hr.http_status = 0;
    hr.ec = TALER_EC_INVALID_RESPONSE; // TODO: use more specific code!
    break;
  case MHD_HTTP_FAILED_DEPENDENCY:
    /* Not a reason to break execution.  */
    TALER_MERCHANT_parse_error_details_ (json,
                                         response_code,
                                         &hr);
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
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
    response_code = 0;
    break;
  }
  tdo->cb (tdo->cb_cls,
           &hr,
           NULL,
           NULL,
           NULL,
           0,
           NULL);
  TALER_MERCHANT_track_transfer_cancel (tdo);
}


/**
 * Request backend to return transfers associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_url base URL of the backend
 * @param wire_method wire method used for the wire transfer
 * @param wtid base32 string indicating a wtid
 * @param exchange_url base URL of the exchange in charge of returning the wanted information
 * @param track_transfer_cb the callback to call when a reply for this request is available
 * @param track_transfer_cb_cls closure for @a contract_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransferHandle *
TALER_MERCHANT_track_transfer (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *wire_method,
  const struct TALER_WireTransferIdentifierRawP *wtid,
  const char *exchange_url,
  TALER_MERCHANT_TrackTransferCallback track_transfer_cb,
  void *track_transfer_cb_cls)
{
  struct TALER_MERCHANT_TrackTransferHandle *tdo;
  CURL *eh;
  char *wtid_str;

  wtid_str = GNUNET_STRINGS_data_to_string_alloc (
    wtid,
    sizeof (struct TALER_WireTransferIdentifierRawP));
  tdo = GNUNET_new (struct TALER_MERCHANT_TrackTransferHandle);
  tdo->ctx = ctx;
  tdo->cb = track_transfer_cb; // very last to be called
  tdo->cb_cls = track_transfer_cb_cls;
  tdo->url = TALER_url_join (backend_url, "track/transfer",
                             "wtid", wtid_str,
                             "exchange", exchange_url,
                             "wire_method", wire_method,
                             NULL);
  GNUNET_free (wtid_str);
  if (NULL == tdo->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (tdo);
    return NULL;
  }
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   tdo->url));
  tdo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_transfers_get_finished,
                                  tdo);
  return tdo;
}


/**
 * Cancel a /track/transfer request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param tdo handle to the tracking operation being cancelled
 */
void
TALER_MERCHANT_track_transfer_cancel (
  struct TALER_MERCHANT_TrackTransferHandle *tdo)
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
