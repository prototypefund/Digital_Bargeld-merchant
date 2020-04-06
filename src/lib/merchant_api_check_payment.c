/*
  This file is part of TALER
  Copyright (C) 2018, 2020 Taler Systems SA

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
 * @file lib/merchant_api_check_payment.c
 * @brief Implementation of the /check-payment GET request
 * @author Christian Grothoff
 * @author Marcello Stanisci
 * @author Florian Dold
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
 * @brief A check payment operation handle
 */
struct TALER_MERCHANT_CheckPaymentOperation
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
  TALER_MERCHANT_CheckPaymentCallback cb;

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
 * Function called when we're done processing the GET /check-payment request.
 *
 * @param cls the `struct TALER_MERCHANT_CheckPaymentOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, should be NULL
 */
static void
handle_check_payment_finished (void *cls,
                               long response_code,
                               const void *response)
{
  struct TALER_MERCHANT_CheckPaymentOperation *cpo = cls;
  struct TALER_Amount refund_amount = { 0 };
  const json_t *json = response;
  const json_t *refunded;

  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("refund_amount",
                            &refund_amount),
    GNUNET_JSON_spec_end ()
  };

  cpo->job = NULL;

  if (MHD_HTTP_OK != response_code)
  {
    struct TALER_MERCHANT_HttpResponse hr;

    TALER_MERCHANT_parse_error_details_ (response,
                                         response_code,
                                         &hr);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Checking payment failed with HTTP status code %u/%d\n",
                (unsigned int) response_code,
                (int) hr.ec);
    GNUNET_break_op (0);
    cpo->cb (cpo->cb_cls,
             &hr,
             GNUNET_SYSERR,
             GNUNET_SYSERR,
             NULL,
             NULL);
    TALER_MERCHANT_check_payment_cancel (cpo);
    return;
  }

  if (! json_boolean_value (json_object_get (json, "paid")))
  {
    const char *taler_pay_uri = json_string_value (json_object_get (json,
                                                                    "taler_pay_uri"));
    if (NULL == taler_pay_uri)
    {
      struct TALER_MERCHANT_HttpResponse hr = {
        .ec = TALER_EC_CHECK_PAYMENT_RESPONSE_MALFORMED,
        .reply = json
      };
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "no taler_pay_uri in unpaid check-payment response\n");
      GNUNET_break_op (0);
      cpo->cb (cpo->cb_cls,
               &hr,
               GNUNET_SYSERR,
               GNUNET_SYSERR,
               NULL,
               NULL);
    }
    else
    {
      struct TALER_MERCHANT_HttpResponse hr = {
        .reply = json,
        .http_status = MHD_HTTP_OK
      };
      cpo->cb (cpo->cb_cls,
               &hr,
               GNUNET_NO,
               GNUNET_NO,
               NULL,
               taler_pay_uri);
    }
    TALER_MERCHANT_check_payment_cancel (cpo);
    return;
  }

  if ( (NULL == (refunded = json_object_get (json, "refunded"))) ||
       ( (json_true () == refunded) &&
         (GNUNET_OK !=
          GNUNET_JSON_parse (json,
                             spec,
                             NULL, NULL)) ) )
  {
    struct TALER_MERCHANT_HttpResponse hr = {
      .ec = TALER_EC_CHECK_PAYMENT_RESPONSE_MALFORMED,
      .reply = json
    };
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "check payment failed to parse JSON\n");
    GNUNET_break_op (0);
    cpo->cb (cpo->cb_cls,
             &hr,
             GNUNET_SYSERR,
             GNUNET_SYSERR,
             NULL,
             NULL);
    TALER_MERCHANT_check_payment_cancel (cpo);
    return;
  }
  {
    struct TALER_MERCHANT_HttpResponse hr = {
      .reply = json,
      .http_status = MHD_HTTP_OK
    };

    cpo->cb (cpo->cb_cls,
             &hr,
             GNUNET_YES,
             (json_true () == refunded),
             (json_true () == refunded) ? &refund_amount : NULL,
             NULL);
  }
  TALER_MERCHANT_check_payment_cancel (cpo);
}


/**
 * Issue a /check-payment request to the backend.  Checks the status
 * of a payment.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id to identify the payment
 * @param session_id sesion id for the payment (or NULL if the payment is not bound to a session)
 * @param timeout timeout to use in long polling (how long may the server wait to reply
 *        before generating an unpaid response). Note that this is just provided to
 *        the server, we as client will block until the response comes back or until
 *        #TALER_MERCHANT_poll_payment_cancel() is called.
 * @param check_payment_cb callback which will work the response gotten from the backend
 * @param check_payment_cb_cls closure to pass to @a check_payment_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_CheckPaymentOperation *
TALER_MERCHANT_check_payment (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *order_id,
  const char *session_id,
  struct GNUNET_TIME_Relative timeout,
  TALER_MERCHANT_CheckPaymentCallback check_payment_cb,
  void *check_payment_cb_cls)
{
  struct TALER_MERCHANT_CheckPaymentOperation *cpo;
  CURL *eh;
  char *timeout_s;
  unsigned int ts;
  long tlong;

  GNUNET_assert (NULL != backend_url);
  GNUNET_assert (NULL != order_id);
  ts = (unsigned int) (timeout.rel_value_us
                       / GNUNET_TIME_UNIT_SECONDS.rel_value_us);
  /* set curl timeout to *our* long poll timeout plus one minute
     (for network latency and processing delays) */
  tlong = (long) (GNUNET_TIME_relative_add (timeout,
                                            GNUNET_TIME_UNIT_MINUTES).
                  rel_value_us
                  / GNUNET_TIME_UNIT_MILLISECONDS.rel_value_us);
  GNUNET_asprintf (&timeout_s,
                   "%u",
                   ts);
  cpo = GNUNET_new (struct TALER_MERCHANT_CheckPaymentOperation);
  cpo->ctx = ctx;
  cpo->cb = check_payment_cb;
  cpo->cb_cls = check_payment_cb_cls;
  cpo->url = TALER_url_join (backend_url, "check-payment",
                             "order_id", order_id,
                             "session_id", session_id,
                             (0 != ts) ? "timeout" : NULL,
                             timeout_s,
                             NULL);
  if (NULL == cpo->url)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not construct request URL.\n");
    GNUNET_free (cpo);
    return NULL;
  }
  GNUNET_free (timeout_s);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    cpo->url))
  {
    GNUNET_break (0);
    curl_easy_cleanup (eh);
    GNUNET_free (cpo->url);
    GNUNET_free (cpo);
    return NULL;
  }
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_TIMEOUT_MS,
                                    tlong))
  {
    GNUNET_break (0);
    curl_easy_cleanup (eh);
    GNUNET_free (cpo->url);
    GNUNET_free (cpo);
    return NULL;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Checking payment from %s\n",
              cpo->url);
  if (NULL == (cpo->job = GNUNET_CURL_job_add (ctx,
                                               eh,
                                               GNUNET_YES,
                                               &handle_check_payment_finished,
                                               cpo)))
  {
    GNUNET_break (0);
    GNUNET_free (cpo->url);
    GNUNET_free (cpo);
    return NULL;
  }
  return cpo;
}


/**
 * Cancel a GET /check-payment request.
 *
 * @param cph handle to the request to be canceled
 */
void
TALER_MERCHANT_check_payment_cancel (
  struct TALER_MERCHANT_CheckPaymentOperation *cph)
{
  if (NULL != cph->job)
  {
    GNUNET_CURL_job_cancel (cph->job);
    cph->job = NULL;
  }
  GNUNET_free (cph->url);
  GNUNET_free (cph);
}


/* end of merchant_api_check_payment.c */
