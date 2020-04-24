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
 * @file lib/merchant_api_get_instances.c
 * @brief Implementation of the GET /instances request of the merchant's HTTP API
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
 * Handle for a GET /instances operation.
 */
struct TALER_MERCHANT_InstancesGetHandle
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
  TALER_MERCHANT_InstancesGetCallback cb;

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
 * Parse instance information from @a ia.
 *
 * @param ia JSON array (or NULL!) with instance data
 * @param igh operation handle
 * @return #GNUNET_OK on success
 */
static int
parse_instances (const json_t *ia,
                 struct TALER_MERCHANT_InstancesGetHandle *igh)
{
  unsigned int iis_len = json_array_size (ia);
  struct TALER_MERCHANT_InstanceInformation iis[iis_len];
  size_t index;
  json_t *value;
  int ret;

  ret = GNUNET_OK;
  json_array_foreach (ia, index, value) {
    struct TALER_MERCHANT_InstanceInformation *ii = &iis[index];
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("name",
                               &ii->name),
      GNUNET_JSON_spec_string ("id",
                               &ii->id),
      GNUNET_JSON_spec_fixed_auto ("merchant_pub",
                                   &ii->merchant_pub),
      GNUNET_JSON_spec_json ("payment_targets",
                             &ii->payment_targets),
      GNUNET_JSON_spec_end ()
    };

    if (GNUNET_OK !=
        GNUNET_JSON_parse (value,
                           spec,
                           NULL, NULL))
    {
      GNUNET_break_op (0);
      ret = GNUNET_SYSERR;
      continue;
    }
    if (! json_is_array (ii->payment_targets))
    {
      GNUNET_break_op (0);
      ret = GNUNET_SYSERR;
      break;
    }
    for (unsigned int i = 0; i<json_array_size (ii->payment_targets); i++)
    {
      if  (! json_is_string (json_array_get (ii->payment_targets,
                                             i)))
      {
        GNUNET_break_op (0);
        ret = GNUNET_SYSERR;
        break;
      }
    }
    if (GNUNET_SYSERR == ret)
      break;
  }
  if (GNUNET_OK == ret)
  {
    struct TALER_MERCHANT_HttpResponse hr = {
      .http_status = MHD_HTTP_OK
    };

    igh->cb (igh->cb_cls,
             &hr,
             iis_len,
             iis);
    igh->cb = NULL; /* just to be sure */
  }
  for (unsigned int i = 0; i<iis_len; i++)
    if (NULL != iis[i].payment_targets)
      json_decref (iis[i].payment_targets);
  return ret;
}


/**
 * Function called when we're done processing the
 * HTTP /instances request.
 *
 * @param cls the `struct TALER_MERCHANT_InstancesGetHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_instances_finished (void *cls,
                           long response_code,
                           const void *response)
{
  struct TALER_MERCHANT_InstancesGetHandle *igh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  igh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /instances response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_OK:
    {
      json_t *instances;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_json ("instances",
                               &instances),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (json,
                             spec,
                             NULL, NULL))
      {
        hr.http_status = 0;
        hr.ec = TALER_EC_INVALID_RESPONSE;
      }
      else
      {
        if ( (! json_is_array (instances)) ||
             (GNUNET_OK ==
              parse_instances (instances,
                               igh)) )
        {
          GNUNET_JSON_parse_free (spec);
          TALER_MERCHANT_instances_get_cancel (igh);
          return;
        }
        else
        {
          hr.http_status = 0;
          hr.ec = TALER_EC_INVALID_RESPONSE;
        }
      }
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
           NULL);
  TALER_MERCHANT_instances_get_cancel (igh);
}


/**
 * Get the instance data of a backend. Will connect to the merchant backend
 * and obtain information about the instances.  The respective information will
 * be passed to the @a instances_cb once available.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instances_cb function to call with the
 *        backend's instances information
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstancesGetHandle *
TALER_MERCHANT_instances_get (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              TALER_MERCHANT_InstancesGetCallback instances_cb,
                              void *instances_cb_cls)
{
  struct TALER_MERCHANT_InstancesGetHandle *igh;
  CURL *eh;

  igh = GNUNET_new (struct TALER_MERCHANT_InstancesGetHandle);
  igh->ctx = ctx;
  igh->cb = instances_cb;
  igh->cb_cls = instances_cb_cls;
  igh->url = TALER_url_join (backend_url,
                             "private/instances",
                             NULL);
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
                                  &handle_instances_finished,
                                  igh);
  return igh;
}


/**
 * Cancel /instances request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param igh request to cancel.
 */
void
TALER_MERCHANT_instances_get_cancel (
  struct TALER_MERCHANT_InstancesGetHandle *igh)
{
  if (NULL != igh->job)
    GNUNET_CURL_job_cancel (igh->job);
  GNUNET_free (igh->url);
  GNUNET_free (igh);
}
