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
 * @file lib/merchant_api_poll_payment.c
 * @brief Implementation of the /poll-payment GET request
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
 * @brief A poll payment operation handle
 */
struct TALER_MERCHANT_PollPaymentOperation
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
  TALER_MERCHANT_PollPaymentCallback cb;

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
 * Function called when we're done processing the GET /poll-payment request.
 *
 * @param cls the `struct TALER_MERCHANT_PollPaymentOperation`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, should be NULL
 */
static void
handle_poll_payment_finished (void *cls,
                              long response_code,
                              const void *response)
{
  struct TALER_MERCHANT_PollPaymentOperation *cpo = cls;
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
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Polling payment failed with HTTP status code %u\n",
                (unsigned int) response_code);
    GNUNET_break_op (0);
    cpo->cb (cpo->cb_cls,
             response_code,
             json,
             GNUNET_SYSERR,
             GNUNET_SYSERR,
             NULL,
             NULL);
    TALER_MERCHANT_poll_payment_cancel (cpo);
    return;
  }

  if (! json_boolean_value (json_object_get (json, "paid")))
  {
    const char *taler_pay_uri = json_string_value (json_object_get (json,
                                                                    "taler_pay_uri"));
    if (NULL == taler_pay_uri)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "no taler_pay_uri in unpaid poll-payment response\n");
      GNUNET_break_op (0);
      cpo->cb (cpo->cb_cls,
               0,
               json,
               GNUNET_SYSERR,
               GNUNET_SYSERR,
               NULL,
               NULL);
    }
    else
    {
      cpo->cb (cpo->cb_cls,
               response_code,
               json,
               GNUNET_NO,
               GNUNET_NO,
               NULL,
               taler_pay_uri);
    }
    TALER_MERCHANT_poll_payment_cancel (cpo);
    return;
  }

  if ( (NULL == (refunded = json_object_get (json,
                                             "refunded"))) ||
       ( (json_true () == refunded) &&
         (GNUNET_OK !=
          GNUNET_JSON_parse (json,
                             spec,
                             NULL, NULL)) ) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "poll payment failed to parse JSON\n");
    GNUNET_break_op (0);
    cpo->cb (cpo->cb_cls,
             0,
             json,
             GNUNET_SYSERR,
             GNUNET_SYSERR,
             NULL,
             NULL);
    TALER_MERCHANT_poll_payment_cancel (cpo);
    return;
  }

  cpo->cb (cpo->cb_cls,
           MHD_HTTP_OK,
           json,
           GNUNET_YES,
           (json_true () == refunded),
           (json_true () == refunded) ? &refund_amount : NULL,
           NULL);
  TALER_MERCHANT_poll_payment_cancel (cpo);
}


/**
 * Issue a /poll-payment request to the backend.  Polls the status
 * of a payment.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id to identify the payment
 * @param h_contract hash of the contract for @a order_id
 * @parem session_id sesion id for the payment (or NULL if the payment is not bound to a session)
 * @param timeout timeout to use in long polling (how long may the server wait to reply
 *        before generating an unpaid response). Note that this is just provided to
 *        the server, we as client will block until the response comes back or until
 *        #TALER_MERCHANT_poll_payment_cancel() is called.
 * @param poll_payment_cb callback which will work the response gotten from the backend
 * @param poll_payment_cb_cls closure to pass to @a poll_payment_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_PollPaymentOperation *
TALER_MERCHANT_poll_payment (struct GNUNET_CURL_Context *ctx,
                             const char *backend_url,
                             const char *order_id,
                             const struct GNUNET_HashCode *h_contract,
                             const char *session_id,
                             struct GNUNET_TIME_Relative timeout,
                             TALER_MERCHANT_PollPaymentCallback
                             poll_payment_cb,
                             void *poll_payment_cb_cls)
{
  struct TALER_MERCHANT_PollPaymentOperation *cpo;
  CURL *eh;
  char *h_contract_s;
  char *timeout_s;
  unsigned int ts;
  long tlong;

  GNUNET_assert (NULL != backend_url);
  GNUNET_assert (NULL != order_id);
  h_contract_s = GNUNET_STRINGS_data_to_string_alloc (h_contract,
                                                      sizeof (*h_contract));
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
  cpo = GNUNET_new (struct TALER_MERCHANT_PollPaymentOperation);
  cpo->ctx = ctx;
  cpo->cb = poll_payment_cb;
  cpo->cb_cls = poll_payment_cb_cls;
  cpo->url = TALER_url_join (backend_url,
                             "public/poll-payment",
                             "order_id", order_id,
                             "session_id", session_id,
                             "h_contract", h_contract_s,
                             (0 != ts) ? "timeout" : NULL,
                             timeout_s,
                             NULL);
  GNUNET_free (h_contract_s);
  GNUNET_free (timeout_s);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    cpo->url))
  {
    GNUNET_break (0);
    return NULL;
  }
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_TIMEOUT_MS,
                                    tlong))
  {
    GNUNET_break (0);
    return NULL;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "polling payment from %s\n",
              cpo->url);

  if (NULL == (cpo->job = GNUNET_CURL_job_add (ctx,
                                               eh,
                                               GNUNET_YES,
                                               &handle_poll_payment_finished,
                                               cpo)))
  {
    GNUNET_break (0);
    return NULL;
  }
  return cpo;
}


/**
 * Cancel a GET /poll-payment request.
 *
 * @param cph handle to the request to be canceled
 */
void
TALER_MERCHANT_poll_payment_cancel (struct
                                    TALER_MERCHANT_PollPaymentOperation *cph)
{
  if (NULL != cph->job)
  {
    GNUNET_CURL_job_cancel (cph->job);
    cph->job = NULL;
  }
  GNUNET_free (cph->url);
  GNUNET_free (cph);
}


/* end of merchant_api_poll_payment.c */
