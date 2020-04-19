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
 * @file lib/merchant_api_get_config.c
 * @brief Implementation of the /config request of the merchant's HTTP API
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
 * Which version of the Taler protocol is implemented
 * by this library?  Used to determine compatibility.
 */
#define MERCHANT_PROTOCOL_CURRENT 0

/**
 * How many configs are we backwards compatible with?
 */
#define MERCHANT_PROTOCOL_AGE 0


/**
 * @brief A handle for /config operations
 */
struct TALER_MERCHANT_ConfigGetHandle
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
  TALER_MERCHANT_ConfigCallback cb;

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
 * HTTP /config request.
 *
 * @param cls the `struct TALER_MERCHANT_ConfigGetHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_config_finished (void *cls,
                        long response_code,
                        const void *response)
{
  struct TALER_MERCHANT_ConfigGetHandle *vgh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /config response with status code %u\n",
              (unsigned int) response_code);

  vgh->job = NULL;
  switch (response_code)
  {
  case MHD_HTTP_OK:
    {
      struct TALER_MERCHANT_ConfigInformation vi;
      enum TALER_MERCHANT_VersionCompatibility vc =
        TALER_MERCHANT_VC_PROTOCOL_ERROR;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_string ("currency",
                                 &vi.currency),
        GNUNET_JSON_spec_string ("version",
                                 &vi.version),
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
        unsigned int age;
        unsigned int revision;
        unsigned int current;

        if (3 != sscanf (vi.version,
                         "%u:%u:%u",
                         &current,
                         &revision,
                         &age))
        {
          hr.http_status = 0;
          hr.ec = TALER_EC_INVALID_RESPONSE;
        }
        else
        {
          vc = TALER_MERCHANT_VC_MATCH;
          if (MERCHANT_PROTOCOL_CURRENT < current)
          {
            vc |= TALER_MERCHANT_VC_NEWER;
            if (MERCHANT_PROTOCOL_CURRENT < current - age)
              vc |= TALER_MERCHANT_VC_INCOMPATIBLE;
          }
          if (MERCHANT_PROTOCOL_CURRENT > current)
          {
            vc |= TALER_MERCHANT_VC_OLDER;
            if (MERCHANT_PROTOCOL_CURRENT - MERCHANT_PROTOCOL_AGE > current)
              vc |= TALER_MERCHANT_VC_INCOMPATIBLE;
          }
        }
      }
      vgh->cb (vgh->cb_cls,
               &hr,
               &vi,
               vc);
      TALER_MERCHANT_config_get_cancel (vgh);
      return;
    }
  default:
    /* unexpected response code */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    vgh->cb (vgh->cb_cls,
             &hr,
             NULL,
             TALER_MERCHANT_VC_PROTOCOL_ERROR);
    break;
  }
  TALER_MERCHANT_config_get_cancel (vgh);
}


/**
 * Get the config data of a merchant. Will connect to the merchant backend
 * and obtain information about the backend.  The respective information will
 * be passed to the @a config_cb once available.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param config_cb function to call with the
 *        backend's config information
 * @param config_cb_cls closure for @a config_cb
 * @return the config check handle; NULL upon error
 */
struct TALER_MERCHANT_ConfigGetHandle *
TALER_MERCHANT_config_get (struct GNUNET_CURL_Context *ctx,
                           const char *backend_url,
                           TALER_MERCHANT_ConfigCallback config_cb,
                           void *config_cb_cls)
{
  struct TALER_MERCHANT_ConfigGetHandle *vgh;
  CURL *eh;

  vgh = GNUNET_new (struct TALER_MERCHANT_ConfigGetHandle);
  vgh->ctx = ctx;
  vgh->cb = config_cb;
  vgh->cb_cls = config_cb_cls;
  vgh->url = TALER_url_join (backend_url,
                             "config",
                             NULL);
  if (NULL == vgh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (vgh);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              vgh->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   vgh->url));

  vgh->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_config_finished,
                                  vgh);
  return vgh;
}


/**
 * Cancel /config request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param vgh request to cancel.
 */
void
TALER_MERCHANT_config_get_cancel (struct TALER_MERCHANT_ConfigGetHandle *vgh)
{
  if (NULL != vgh->job)
  {
    GNUNET_CURL_job_cancel (vgh->job);
    vgh->job = NULL;
  }
  GNUNET_free (vgh->url);
  GNUNET_free (vgh);
}


/* end of merchant_api_config_get.c */
