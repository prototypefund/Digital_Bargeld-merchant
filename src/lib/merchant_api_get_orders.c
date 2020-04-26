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
 * @file lib/merchant_api_get_orders.c
 * @brief Implementation of the GET /orders request of the merchant's HTTP API
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
 * Handle for a GET /orders operation.
 */
struct TALER_MERCHANT_OrdersGetHandle
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
  TALER_MERCHANT_OrdersGetCallback cb;

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
 * Parse order information from @a ia.
 *
 * @param ia JSON array (or NULL!) with order data
 * @param ogh operation handle
 * @return #GNUNET_OK on success
 */
static int
parse_orders (const json_t *ia,
              struct TALER_MERCHANT_OrdersGetHandle *ogh)
{
  unsigned int oes_len = json_array_size (ia);
  struct TALER_MERCHANT_OrderEntry oes[oes_len];
  size_t index;
  json_t *value;
  int ret;

  ret = GNUNET_OK;
  json_array_foreach (ia, index, value) {
    struct TALER_MERCHANT_OrderEntry *ie = &oes[index];
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("order_id",
                               &ie->order_id),
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
    if (GNUNET_SYSERR == ret)
      break;
  }
  if (GNUNET_OK == ret)
  {
    struct TALER_MERCHANT_HttpResponse hr = {
      .http_status = MHD_HTTP_OK
    };

    ogh->cb (ogh->cb_cls,
             &hr,
             oes_len,
             oes);
    ogh->cb = NULL; /* just to be sure */
  }
  return ret;
}


/**
 * Function called when we're done processing the
 * HTTP /orders request.
 *
 * @param cls the `struct TALER_MERCHANT_OrdersGetHandle`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_get_orders_finished (void *cls,
                            long response_code,
                            const void *response)
{
  struct TALER_MERCHANT_OrdersGetHandle *ogh = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  ogh->job = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Got /orders response with status code %u\n",
              (unsigned int) response_code);
  switch (response_code)
  {
  case MHD_HTTP_OK:
    {
      json_t *orders;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_json ("orders",
                               &orders),
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
        if ( (! json_is_array (orders)) ||
             (GNUNET_OK ==
              parse_orders (orders,
                            ogh)) )
        {
          GNUNET_JSON_parse_free (spec);
          TALER_MERCHANT_orders_get_cancel (ogh);
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
  ogh->cb (ogh->cb_cls,
           &hr,
           0,
           NULL);
  TALER_MERCHANT_orders_get_cancel (ogh);
}


/**
 * Make a GET /orders request.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param cb function to call with the backend's inventory information
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_OrdersGetHandle *
TALER_MERCHANT_orders_get (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  TALER_MERCHANT_OrdersGetCallback cb,
  void *cb_cls)
{
  return TALER_MERCHANT_orders_get2 (ctx,
                                     backend_url,
                                     TALER_MERCHANT_YNA_ALL,
                                     TALER_MERCHANT_YNA_ALL,
                                     TALER_MERCHANT_YNA_ALL,
                                     GNUNET_TIME_UNIT_FOREVER_ABS,
                                     UINT64_MAX,
                                     -20, /* default is most recent 20 entries */
                                     GNUNET_TIME_UNIT_ZERO,
                                     cb,
                                     cb_cls);
}


/**
 * Make a GET /orders request with filters.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param paid filter on payment status
 * @param refunded filter on refund status
 * @param wired filter on wire transfer status
 * @param date range limit by date
 * @param start_row range limit by order table row
 * @param delta range from which @a date and @a start_row apply, positive
 *              to return delta items after the given limit(s), negative to
 *              return delta items before the given limit(s)
 * @param timeout how long to wait (long polling) of zero results match the query
 * @param cb function to call with the backend's inventory information
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_OrdersGetHandle *
TALER_MERCHANT_orders_get2 (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  enum TALER_MERCHANT_YesNoAll paid,
  enum TALER_MERCHANT_YesNoAll refunded,
  enum TALER_MERCHANT_YesNoAll wired,
  struct GNUNET_TIME_Absolute date,
  uint64_t start_row,
  int64_t delta,
  struct GNUNET_TIME_Relative timeout,
  TALER_MERCHANT_OrdersGetCallback cb,
  void *cb_cls)
{
  struct TALER_MERCHANT_OrdersGetHandle *ogh;
  CURL *eh;
  unsigned int timeout_ms = timeout.rel_value_us
                            / GNUNET_TIME_UNIT_MILLISECONDS.rel_value_us;

  GNUNET_assert (NULL != backend_url);
  if (0 == delta)
  {
    GNUNET_break (0);
    return NULL;
  }
  ogh = GNUNET_new (struct TALER_MERCHANT_OrdersGetHandle);
  ogh->ctx = ctx;
  ogh->cb = cb;
  ogh->cb_cls = cb_cls;

  /* build ogh->url with the various optional arguments */
  {
    struct GNUNET_Buffer buf = { 0 };
    bool first = true;
    /**
     * Macro to append @a a and @a b to @a buf, using
     * the right separators between key (@a a) and
     * value (@a b). Uses "first" to decide between
     * using "?" and "&" as the separator.
     *
     * @param a a key
     * @param b a value
     */
#define APPEND(a,b)                             \
  do { \
    if (first) \
      GNUNET_buffer_write_str (&buf, \
                               "?");            \
    else \
      GNUNET_buffer_write_str (&buf, \
                               "&"); \
    first = false; \
    GNUNET_buffer_write_str (&buf, (a)); \
    GNUNET_buffer_write_str (&buf, "="); \
    GNUNET_buffer_write_str (&buf, (b)); \
  } while (0)

    {
      char *url;

      url = TALER_url_join (backend_url,
                            "private/orders",
                            NULL);
      if (NULL == url)
        goto finished;
      GNUNET_buffer_write_str (&buf,
                               url);
      GNUNET_free (url);
    }
    if (TALER_MERCHANT_YNA_ALL != paid)
      APPEND ("paid",
              (TALER_MERCHANT_YNA_YES == paid) ? "yes" : "no");
    if (TALER_MERCHANT_YNA_ALL != refunded)
      APPEND ("refunded",
              (TALER_MERCHANT_YNA_YES == refunded) ? "yes" : "no");
    if (TALER_MERCHANT_YNA_ALL != wired)
      APPEND ("wired",
              (TALER_MERCHANT_YNA_YES == wired) ? "yes" : "no");
    if (delta > 0)
    {
      if (0 != date.abs_value_us)
      {
        const char *str;

        str = GNUNET_STRINGS_absolute_time_to_string (date);
        APPEND ("date",
                str);
      }
      if (0 != start_row)
      {
        char cbuf[30];

        GNUNET_snprintf (cbuf,
                         sizeof (cbuf),
                         "%llu",
                         (unsigned long long) start_row);
        APPEND ("start",
                cbuf);
      }
    }
    else
    {
      if (GNUNET_TIME_UNIT_FOREVER_ABS.abs_value_us != date.abs_value_us)
      {
        const char *str;

        str = GNUNET_STRINGS_absolute_time_to_string (date);
        APPEND ("date",
                str);
      }
      if (UINT64_MAX != start_row)
      {
        char cbuf[30];

        GNUNET_snprintf (cbuf,
                         sizeof (cbuf),
                         "%llu",
                         (unsigned long long) start_row);
        APPEND ("start",
                cbuf);
      }
    }
    if (-20 != delta)
    {
      char cbuf[30];

      GNUNET_snprintf (cbuf,
                       sizeof (cbuf),
                       "%lld",
                       (long long) delta);
      APPEND ("delta",
              cbuf);
    }
    if (0 != timeout_ms)
    {
      char cbuf[30];

      GNUNET_snprintf (cbuf,
                       sizeof (cbuf),
                       "%llu",
                       (unsigned long long) timeout_ms);
      APPEND ("timeout_ms",
              cbuf);
    }
    ogh->url = GNUNET_buffer_reap_str (&buf);
#undef APPEND
  }

finished:
  if (NULL == ogh->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (ogh);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Requesting URL '%s'\n",
              ogh->url);
  eh = curl_easy_init ();
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   ogh->url));
  ogh->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_YES,
                                  &handle_get_orders_finished,
                                  ogh);
  return ogh;
}


/**
 * Cancel /orders request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param ogh request to cancel.
 */
void
TALER_MERCHANT_orders_get_cancel (
  struct TALER_MERCHANT_OrdersGetHandle *ogh)
{
  if (NULL != ogh->job)
    GNUNET_CURL_job_cancel (ogh->job);
  GNUNET_free (ogh->url);
  GNUNET_free (ogh);
}
