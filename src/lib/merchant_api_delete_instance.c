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
 * @file lib/merchant_api_delete_instance.c
 * @brief Implementation of the DELETE /instance/$ID request of the merchant's HTTP API
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
 * Handle for a DELETE /instances/$ID operation.
 */
struct TALER_MERCHANT_InstanceDeleteHandle
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
  TALER_MERCHANT_InstanceDeleteCallback cb;

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
 * @param cls the `struct TALER_MERCHANT_InstanceDeleteHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_delete_instance_finished (void *cls,
                                 long response_code,
                                 const void *response)
{
  struct TALER_MERCHANT_InstanceDeleteHandle *idh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  idh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /instances/$ID response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_NO_CONTENT:
    break;
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
  idh->cb (idh->cb_cls,
           &hr);
  TALER_MERCHANT_instance_delete_cancel (idh);
}


/**
 * Delete the private key of an instance of a backend, thereby disabling the
 * instance for future requests.  Will preserve the other instance data
 * (i.e. for taxation).
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id which instance should be deleted
 * @param purge purge instead of just deleting
 * @param instances_cb function to call with the
 *        backend's return
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
static struct TALER_MERCHANT_InstanceDeleteHandle *
instance_delete (struct GNUNET_CURL_Context *ctx,
                 const char *backend_url,
                 const char *instance_id,
                 bool purge,
                 TALER_MERCHANT_InstanceDeleteCallback cb,
                 void *cb_cls)
{
  struct TALER_MERCHANT_InstanceDeleteHandle *idh;

  idh = GNUNET_new (struct TALER_MERCHANT_InstanceDeleteHandle);
  idh->ctx = ctx;
  idh->cb = cb;
  idh->cb_cls = cb_cls;
  {
    char *path;

    GNUNET_asprintf (&path,
                     "private/instances/%s",
                     instance_id);
    if (purge)
      idh->url = TALER_url_join (backend_url,
                                 path,
                                 "purge",
                                 "yes",
                                 NULL);
    else
      idh->url = TALER_url_join (backend_url,
                                 path,
                                 NULL);
    GNUNET_free (path);
  }
  if (NULL == idh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (idh);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              idh->url);
  {
    CURL *eh;

    eh = curl_easy_init ();
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_URL,
                                     idh->url));
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_CUSTOMREQUEST,
                                     MHD_HTTP_METHOD_DELETE));
    idh->job = GNUNET_CURL_job_add (ctx,
                                    eh,
                                    GNUNET_YES,
                                    &handle_delete_instance_finished,
                                    idh);
  }
  return idh;
}


/**
 * Delete the private key of an instance of a backend, thereby disabling the
 * instance for future requests.  Will preserve the other instance data
 * (i.e. for taxation).
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id which instance should be deleted
 * @param instances_cb function to call with the
 *        backend's return
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstanceDeleteHandle *
TALER_MERCHANT_instance_delete (struct GNUNET_CURL_Context *ctx,
                                const char *backend_url,
                                const char *instance_id,
                                TALER_MERCHANT_InstanceDeleteCallback cb,
                                void *cb_cls)
{
  return instance_delete (ctx,
                          backend_url,
                          instance_id,
                          false,
                          cb,
                          cb_cls);
}


/**
 * Purge all data associated with an instance. Use with
 * extreme caution.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id which instance should be deleted
 * @param instances_cb function to call with the
 *        backend's return
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstanceDeleteHandle *
TALER_MERCHANT_instance_purge (struct GNUNET_CURL_Context *ctx,
                               const char *backend_url,
                               const char *instance_id,
                               TALER_MERCHANT_InstanceDeleteCallback cb,
                               void *cb_cls)
{
  return instance_delete (ctx,
                          backend_url,
                          instance_id,
                          true,
                          cb,
                          cb_cls);
}


/**
 * Cancel DELETE /instance/$ID request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param idh request to cancel.
 */
void
TALER_MERCHANT_instance_delete_cancel (
  struct TALER_MERCHANT_InstanceDeleteHandle *idh)
{
  if (NULL != idh->job)
    GNUNET_CURL_job_cancel (idh->job);
  GNUNET_free (idh->url);
  GNUNET_free (idh);
}
