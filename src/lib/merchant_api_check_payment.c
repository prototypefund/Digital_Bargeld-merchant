/*
  This file is part of TALER
  Copyright (C) 2018 GNUnet e.V. and INRIA

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
  int refunded;
  const json_t *json = response;
  
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_boolean ("refunded",
                              &refunded),
    TALER_JSON_spec_amount ("refund_amount",
                            &refund_amount),
    GNUNET_JSON_spec_end()
  };

  cpo->job = NULL;

  if (MHD_HTTP_OK != response_code)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Checking payment failed with HTTP status code %u\n",
                (unsigned int) response_code);
    GNUNET_break_op (0);
    cpo->cb (cpo->cb_cls,
             response_code,
             json,
             GNUNET_SYSERR,
             GNUNET_SYSERR,
             &refund_amount,
             NULL);
    TALER_MERCHANT_check_payment_cancel (cpo);
    return;
  }

  if (! json_boolean_value (json_object_get (json, "paid")))
  {
    const char *taler_pay_uri = json_string_value (json_object_get (json, "taler_pay_uri"));
    if (NULL == taler_pay_uri)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "no taler_pay_uri in unpaid check-payment response\n");
      GNUNET_break_op (0);
      cpo->cb (cpo->cb_cls,
               0,
               json,
               GNUNET_SYSERR,
               GNUNET_SYSERR,
               &refund_amount,
               NULL);
    } else {
      cpo->cb (cpo->cb_cls,
               response_code,
               json,
               GNUNET_NO,
               GNUNET_NO,
               &refund_amount,
               taler_pay_uri);
    }
    TALER_MERCHANT_check_payment_cancel (cpo);
    return;
  }

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "check payment failed to parse JSON\n");
    GNUNET_break_op (0);
    cpo->cb (cpo->cb_cls,
             0,
             json,
             GNUNET_SYSERR,
             GNUNET_SYSERR,
             &refund_amount,
             NULL);
    TALER_MERCHANT_check_payment_cancel (cpo);
    return;
  }

  cpo->cb (cpo->cb_cls,
           MHD_HTTP_OK,
           json,
           GNUNET_YES,
           refunded,
           &refund_amount,
           NULL);
  GNUNET_JSON_parse_free (spec);
  TALER_MERCHANT_check_payment_cancel (cpo);
}


/**
 * Issue a /check-payment request to the backend.  Checks the status
 * of a payment.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param instance instance used for the transaction
 * @param order_id order id to identify the payment
 * @parem resource_url resource URL to identify duplicate payments (can be NULL)
 * @parem session_id sesion id for the payment (or NULL if the payment is not bound to a session) 
 * @param check_payment_cb callback which will work the response gotten from the backend
 * @param check_payment_cb_cls closure to pass to @a check_payment_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_CheckPaymentOperation *
TALER_MERCHANT_check_payment (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              const char *instance,
                              const char *order_id,
                              const char *resource_url,
                              const char *session_id,
                              TALER_MERCHANT_CheckPaymentCallback check_payment_cb,
                              void *check_payment_cb_cls)
{
  struct TALER_MERCHANT_CheckPaymentOperation *cpo;
  CURL *eh;

  GNUNET_assert (NULL != backend_url);
  GNUNET_assert (NULL != instance);
  GNUNET_assert (NULL != order_id);

  cpo = GNUNET_new (struct TALER_MERCHANT_CheckPaymentOperation);
  cpo->ctx = ctx;
  cpo->cb = check_payment_cb;
  cpo->cb_cls = check_payment_cb_cls;
  cpo->url = TALER_url_join (backend_url, "/check-payment",
                             "instance", instance,
                             "order_id", order_id,
                             "resource_url", resource_url,
                             "session_id", session_id,
                             NULL);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    cpo->url))
  {
    GNUNET_break (0);
    return NULL;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "checking payment from %s\n",
              cpo->url);

  if (NULL == (cpo->job = GNUNET_CURL_job_add (ctx,
                                               eh,
                                               GNUNET_YES,
                                               &handle_check_payment_finished,
                                               cpo)))
  {
    GNUNET_break (0);
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
TALER_MERCHANT_check_payment_cancel (struct TALER_MERCHANT_CheckPaymentOperation *cph)
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
