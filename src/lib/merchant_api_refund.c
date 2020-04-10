/*
  This file is part of TALER
  Copyright (C) 2014-2020 Taler Systems SA

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
 * @file lib/merchant_api_refund.c
 * @brief Implementation of the /refund POST and GET
 * @author Christian Grothoff
 * @author Marcello Stanisci
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
#include <taler/taler_curl_lib.h>


/**
 * Handle to the refund lookup operation.
 */
struct TALER_MERCHANT_RefundLookupOperation
{
  /**
   * URL of the request, includes parameters
   */
  char *url;

  /**
   * Handle of the request
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the response
   */
  TALER_MERCHANT_RefundLookupCallback cb;

  /**
   * Closure for cb
   */
  void *cb_cls;

  /**
   * Reference to the execution context
   */
  struct GNUNET_CURL_Context *ctx;

};


/**
 * Cancel a /refund lookup operation
 *
 * @param rlo operation to cancel
 */
void
TALER_MERCHANT_refund_lookup_cancel (
  struct TALER_MERCHANT_RefundLookupOperation *rlo)
{
  if (NULL != rlo->job)
  {
    GNUNET_CURL_job_cancel (rlo->job);
    rlo->job = NULL;
  }
  GNUNET_free (rlo->url);
  GNUNET_free (rlo);
}


/**
 * Check that the @a reply to the @a rlo is valid
 *
 * @param rlo lookup operation
 * @param reply JSON reply to verify
 * @return #TALER_EC_NONE if @a reply is well-formed
 */
static enum TALER_ErrorCode
check_refund_result (struct TALER_MERCHANT_RefundLookupOperation *rlo,
                     const json_t *reply)
{
  json_t *refunds;
  unsigned int num_refunds;
  struct GNUNET_HashCode h_contract_terms;
  struct TALER_MerchantPublicKeyP merchant_pub;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("refunds", &refunds),
    GNUNET_JSON_spec_fixed_auto ("h_contract_terms", &h_contract_terms),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &merchant_pub),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (reply,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return TALER_EC_REFUND_LOOKUP_INVALID_RESPONSE;
  }
  num_refunds = json_array_size (refunds);
  {
    struct TALER_MERCHANT_RefundDetail rds[GNUNET_NZL (num_refunds)];
    json_t *ercp[GNUNET_NZL (num_refunds)];

    memset (rds,
            0,
            sizeof (rds));
    memset (ercp,
            0,
            sizeof (ercp));
    for (unsigned int i = 0; i<num_refunds; i++)
    {
      struct TALER_MERCHANT_RefundDetail *rd = &rds[i];
      json_t *refund = json_array_get (refunds, i);
      uint32_t hs;
      struct GNUNET_JSON_Specification spec_detail[] = {
        GNUNET_JSON_spec_fixed_auto ("coin_pub",
                                     &rd->coin_pub),
        TALER_JSON_spec_amount ("refund_amount",
                                &rd->refund_amount),
        TALER_JSON_spec_amount ("refund_fee",
                                &rd->refund_fee),
        GNUNET_JSON_spec_uint32 ("exchange_http_status",
                                 &hs),
        GNUNET_JSON_spec_uint64 ("rtransaction_id",
                                 &rd->rtransaction_id),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (refund,
                             spec_detail,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
        GNUNET_JSON_parse_free (spec);
        return TALER_EC_REFUND_LOOKUP_INVALID_RESPONSE;
      }
      rd->hr.http_status = (unsigned int) hs;
    }

    for (unsigned int i = 0; i<num_refunds; i++)
    {
      struct TALER_MERCHANT_RefundDetail *rd = &rds[i];
      json_t *refund = json_array_get (refunds, i);

      if (MHD_HTTP_OK == rd->hr.http_status)
      {
        struct GNUNET_JSON_Specification spec_detail[] = {
          GNUNET_JSON_spec_fixed_auto ("exchange_pub",
                                       &rd->exchange_pub),
          GNUNET_JSON_spec_fixed_auto ("exchange_sig",
                                       &rd->exchange_sig),
          GNUNET_JSON_spec_end ()
        };

        if (GNUNET_OK !=
            GNUNET_JSON_parse (refund,
                               spec_detail,
                               NULL, NULL))
        {
          GNUNET_break_op (0);
          for (unsigned int j = 0; j<i; j++)
            if (NULL != ercp[j])
              json_decref (ercp[j]);
          GNUNET_JSON_parse_free (spec);
          return TALER_EC_REFUND_LOOKUP_INVALID_RESPONSE;
        }
        /* verify exchange sig (we should not trust the merchant) */
        {
          struct TALER_RefundConfirmationPS depconf = {
            .purpose.size = htonl (sizeof (depconf)),
            .purpose.purpose = htonl (TALER_SIGNATURE_EXCHANGE_CONFIRM_REFUND),
            .h_contract_terms = h_contract_terms,
            .coin_pub = rd->coin_pub,
            .merchant = merchant_pub,
            .rtransaction_id = GNUNET_htonll (rd->rtransaction_id)
          };

          TALER_amount_hton (&depconf.refund_amount,
                             &rd->refund_amount);
          TALER_amount_hton (&depconf.refund_fee,
                             &rd->refund_fee);
          if (GNUNET_OK !=
              GNUNET_CRYPTO_eddsa_verify (
                TALER_SIGNATURE_EXCHANGE_CONFIRM_REFUND,
                &depconf,
                &rd->exchange_sig.eddsa_signature,
                &rd->exchange_pub.eddsa_pub))
          {
            /* While the *exchange* signature is invalid, we do blame the
               merchant here, because the merchant should have checked and
               sent us an error code (with exchange HTTP status code 0) instead
               of claiming that the exchange yielded a good response. *///
            GNUNET_break_op (0);
            GNUNET_JSON_parse_free (spec);
            return TALER_EC_REFUND_LOOKUP_INVALID_RESPONSE;
          }
        }
      }
      else
      {
        uint32_t ec;
        struct GNUNET_JSON_Specification spec_detail[] = {
          GNUNET_JSON_spec_uint32 ("exchange_code",
                                   &ec),
          GNUNET_JSON_spec_end ()
        };

        if (GNUNET_OK !=
            GNUNET_JSON_parse (refund,
                               spec_detail,
                               NULL, NULL))
        {
          GNUNET_break_op (0);
          rd->hr.ec = TALER_EC_INVALID;
        }
        ercp[i] = json_incref (json_object_get (refund,
                                                "exchange_reply"));
        rd->hr.reply = ercp[i];
      }
    }
    {
      struct TALER_MERCHANT_HttpResponse hr = {
        .http_status = MHD_HTTP_OK,
        .reply = reply
      };
      rlo->cb (rlo->cb_cls,
               &hr,
               &h_contract_terms,
               &merchant_pub,
               num_refunds,
               rds);
    }
    for (unsigned int j = 0; j<num_refunds; j++)
      if (NULL != ercp[j])
        json_decref (ercp[j]);
  }
  GNUNET_JSON_parse_free (spec);
  return TALER_EC_NONE;
}


/**
 * Process GET /refund response
 *
 * @param cls a `struct TALER_MERCHANT_RefundLookupOperation *`
 * @param response_code HTTP status, 0 for HTTP failure
 * @param response a `const json_t *` with the JSON of the HTTP body
 */
static void
handle_refund_lookup_finished (void *cls,
                               long response_code,
                               const void *response)
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo = cls;
  const json_t *json = response;
  struct TALER_MERCHANT_HttpResponse hr = {
    .http_status = (unsigned int) response_code,
    .reply = json
  };

  rlo->job = NULL;
  switch (response_code)
  {
  case 0:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Backend didn't even return from GET /refund\n");
    hr.ec = TALER_EC_INVALID_RESPONSE;
    break;
  case MHD_HTTP_OK:
    if (TALER_EC_NONE ==
        (hr.ec = check_refund_result (rlo,
                                      json)))
    {
      TALER_MERCHANT_refund_lookup_cancel (rlo);
      return;
    }
    /* failure, report! */
    hr.http_status = 0;
    break;
  case MHD_HTTP_NOT_FOUND:
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  default:
    GNUNET_break_op (0); /* unexpected status code */
    hr.ec = TALER_JSON_get_error_code (json);
    hr.hint = TALER_JSON_get_error_hint (json);
    break;
  }
  rlo->cb (rlo->cb_cls,
           &hr,
           NULL,
           NULL,
           0,
           NULL);
  TALER_MERCHANT_refund_lookup_cancel (rlo);
}


/**
 * Does a GET /refund.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param cb callback which will work the response gotten from the backend
 * @param cb_cls closure to pass to the callback
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_RefundLookupOperation *
TALER_MERCHANT_refund_lookup (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              const char *order_id,
                              TALER_MERCHANT_RefundLookupCallback cb,
                              void *cb_cls)
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo;
  CURL *eh;

  rlo = GNUNET_new (struct TALER_MERCHANT_RefundLookupOperation);
  rlo->ctx = ctx;
  rlo->cb = cb;
  rlo->cb_cls = cb_cls;
  rlo->url = TALER_url_join (backend_url,
                             "refund",
                             "order_id",
                             order_id,
                             NULL);
  eh = curl_easy_init ();
  if (CURLE_OK != curl_easy_setopt (eh,
                                    CURLOPT_URL,
                                    rlo->url))
  {
    GNUNET_break (0);
    GNUNET_free (rlo->url);
    GNUNET_free (rlo);
    return NULL;
  }
  rlo->job = GNUNET_CURL_job_add (ctx,
                                  eh,
                                  GNUNET_NO,
                                  handle_refund_lookup_finished,
                                  rlo);
  if (NULL == rlo->job)
  {
    GNUNET_free (rlo->url);
    GNUNET_free (rlo);
    GNUNET_break (0);
    return NULL;
  }
  return rlo;
}
