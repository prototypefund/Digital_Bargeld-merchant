/*
  This file is part of TALER
  Copyright (C) 2014-2018, 2020 Taler Systems SA

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
 * @file lib/merchant_api_get_instance.c
 * @brief Implementation of the GET /instance/$ID request of the merchant's HTTP API
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
 * Handle for a GET /instances/$ID operation.
 */
struct TALER_MERCHANT_InstanceGetHandle
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
  TALER_MERCHANT_InstanceGetCallback cb;

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
 * HTTP GET /instances/$ID request.
 *
 * @param cls the `struct TALER_MERCHANT_InstanceGetHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_get_instance_finished (void *cls,
                              long response_code,
                              const void *response)
{
  struct TALER_MERCHANT_InstanceGetHandle *igh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  igh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /instances/$ID response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_OK:
    {
      json_t *accounts;
      const char *name;
      struct TALER_MerchantPublicKeyP merchant_pub;
      json_t *address;
      json_t *jurisdiction;
      struct TALER_Amount default_max_wire_fee;
      uint32_t default_wire_fee_amortization;
      struct TALER_Amount default_max_deposit_fee;
      struct GNUNET_TIME_Relative default_wire_transfer_delay;
      struct GNUNET_TIME_Relative default_pay_delay;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_json ("accounts",
                               &accounts),
        GNUNET_JSON_spec_string ("name",
                                 &name),
        GNUNET_JSON_spec_fixed_auto ("merchant_pub",
                                     &merchant_pub),
        GNUNET_JSON_spec_json ("address",
                               &address),
        GNUNET_JSON_spec_json ("jurisdiction",
                               &jurisdiction),
        TALER_JSON_spec_amount ("default_max_wire_fee",
                                &default_max_wire_fee),
        GNUNET_JSON_spec_uint32 ("default_wire_fee_amortization",
                                 &default_wire_fee_amortization),
        TALER_JSON_spec_amount ("default_max_deposit_fee",
                                &default_max_deposit_fee),
        GNUNET_JSON_spec_relative_time ("default_wire_transfer_delay",
                                        &default_wire_transfer_delay),
        GNUNET_JSON_spec_relative_time ("default_pay_delay",
                                        &default_pay_delay),
        GNUNET_JSON_spec_end ()
      };

      if ( (GNUNET_OK ==
            GNUNET_JSON_parse (json,
                               spec,
                               NULL, NULL)) &&
           (json_is_array (accounts)) )
      {
        unsigned int accounts_length = json_array_size (accounts);
        struct TALER_MERCHANT_Account aa[accounts_length];
        const char *payto_uris[accounts_length];
        size_t index;
        json_t *value;
        int ret = GNUNET_OK;

        memset (payto_uris, 0, sizeof (payto_uris));
        json_array_foreach (accounts, index, value)
        {
          struct GNUNET_JSON_Specification spec[] = {
            GNUNET_JSON_spec_fixed_auto ("salt",
                                         &aa[index].salt),
            GNUNET_JSON_spec_string ("payto_uri",
                                     &payto_uris[index]),
            GNUNET_JSON_spec_fixed_auto ("h_wire",
                                         &aa[index].h_wire),
            GNUNET_JSON_spec_bool ("active",
                                   &aa[index].active),
            GNUNET_JSON_spec_end ()
          };

          if (GNUNET_OK !=
              GNUNET_JSON_parse (value,
                                 spec,
                                 NULL, NULL))
          {
            GNUNET_break_op (0);
            ret = GNUNET_SYSERR;
            break;
          }
          aa[index].payto_uri = payto_uris[index];
        }

        if (GNUNET_OK == ret)
        {
          struct TALER_MERCHANT_InstanceDetails details = {
            .name = name,
            .merchant_pub = &merchant_pub,
            .address = address,
            .jurisdiction = jurisdiction,
            .default_max_wire_fee = &default_max_wire_fee,
            .default_wire_fee_amortization = default_wire_fee_amortization,
            .default_max_deposit_fee = &default_max_deposit_fee,
            .default_wire_transfer_delay = default_wire_transfer_delay,
            .default_pay_delay = default_pay_delay
          };

          igh->cb (igh->cb_cls,
                   &hr,
                   accounts_length,
                   aa,
                   &details);
          GNUNET_JSON_parse_free (spec);
          TALER_MERCHANT_instance_get_cancel (igh);
          return;
        }
      }
      GNUNET_break_op (0);
      hr.http_status = 0;
      hr.ec = TALER_EC_INVALID_RESPONSE;
      GNUNET_JSON_parse_free (spec);
      break;
    }
  default:
    /* unexpected response code */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    break;
  }
  igh->cb (igh->cb_cls,
           &hr,
           0,
           NULL,
           NULL);
  TALER_MERCHANT_instance_get_cancel (igh);
}


/**
 * Get the instance data of a backend. Will connect to the merchant backend
 * and obtain information about the instances.  The respective information will
 * be passed to the @a cb once available.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id identity of the instance to get information about
 * @param cb function to call with the
 *        backend's instances information
 * @param cb_cls closure for @a cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstanceGetHandle *
TALER_MERCHANT_instance_get (struct GNUNET_CURL_Context *ctx,
                             const char *backend_url,
                             const char *instance_id,
                             TALER_MERCHANT_InstanceGetCallback cb,
                             void *cb_cls)
{
  struct TALER_MERCHANT_InstanceGetHandle *igh;
  CURL *eh;

  igh = GNUNET_new (struct TALER_MERCHANT_InstanceGetHandle);
  igh->ctx = ctx;
  igh->cb = cb;
  igh->cb_cls = cb_cls;
  {
    char *path;

    GNUNET_asprintf (&path,
                     "private/instances/%s",
                     instance_id);
    igh->url = TALER_url_join (backend_url,
                               path,
                               NULL);
    GNUNET_free (path);
  }
  if (NULL == igh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (igh);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              igh->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   igh->url));
  igh->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_get_instance_finished,
                                  igh);
  return igh;
}


/**
 * Cancel GET /instance/$ID request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param igh request to cancel.
 */
void
TALER_MERCHANT_instance_get_cancel (
  struct TALER_MERCHANT_InstanceGetHandle *igh)
{
  if (NULL != igh->job)
    GNUNET_CURL_job_cancel (igh->job);
  GNUNET_free (igh->url);
  GNUNET_free (igh);
}
