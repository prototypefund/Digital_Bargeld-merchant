/*
  This file is part of TALER
  (C) 2017, 2019 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_check-payment.c
 * @brief implementation of /check-payment handler
 * @author Florian Dold
 * @author Christian Grothoff
 */
#include "platform.h"
#include <string.h>
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_check-payment.h"

/**
 * Maximum number of retries for database operations.
 */
#define MAX_RETRIES 5


/**
 * Data structure we keep for a check payment request.
 */
struct CheckPaymentRequestContext
{
  /**
   * Must be first for #handle_mhd_completion_callback.
   */
  struct TM_HandlerContext hc;

  /**
   * Entry in the #resume_timeout_heap for this check payment, if we are
   * suspended.
   */
  struct TMH_SuspendedConnection sc;

  /**
   * Which merchant instance is this for?
   */
  struct MerchantInstance *mi;

  /**
   * URL where the final contract can be found for this payment.
   */
  char *final_contract_url;

  /**
   * order ID for the payment
   */
  const char *order_id;

  /**
   * Where to get the contract
   */
  const char *contract_url;

  /**
   * session of the client
   */
  const char *session_id;

  /**
   * fulfillment URL of the contract (valid as long as
   * @e contract_terms is valid).
   */
  const char *fulfillment_url;

  /**
   * Contract terms of the payment we are checking. NULL when they
   * are not (yet) known.
   */
  json_t *contract_terms;

  /**
   * Hash of @e contract_terms, set only once @e contract_terms
   * is available.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * Total refunds granted for this payment. Only initialized
   * if @e refunded is set to #GNUNET_YES.
   */
  struct TALER_Amount refund_amount;

  /**
   * Set to #GNUNET_YES if this payment has been refunded and
   * @e refund_amount is initialized.
   */
  int refunded;

  /**
   * Initially #GNUNET_SYSERR. If we queued a response, set to the
   * result code (i.e. #MHD_YES or #MHD_NO).
   */
  int ret;

};


/**
 * Clean up the session state for a check payment request.
 *
 * @param hc must be a `struct CheckPaymentRequestContext *`
 */
static void
cprc_cleanup (struct TM_HandlerContext *hc)
{
  struct CheckPaymentRequestContext *cprc = (struct
                                             CheckPaymentRequestContext *) hc;

  if (NULL != cprc->contract_terms)
    json_decref (cprc->contract_terms);
  GNUNET_free_non_null (cprc->final_contract_url);
  GNUNET_free (cprc);
}


/**
 * Function called with information about a refund.
 * It is responsible for summing up the refund amount.
 *
 * @param cls closure
 * @param coin_pub public coin from which the refund comes from
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explaination of the refund
 * @param refund_amount refund amount which is being taken from coin_pub
 * @param refund_fee cost of this refund operation
 */
static void
process_refunds_cb (void *cls,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    uint64_t rtransaction_id,
                    const char *reason,
                    const struct TALER_Amount *refund_amount,
                    const struct TALER_Amount *refund_fee)
{
  struct CheckPaymentRequestContext *cprc = cls;

  if (cprc->refunded)
  {
    GNUNET_assert (GNUNET_SYSERR !=
                   TALER_amount_add (&cprc->refund_amount,
                                     &cprc->refund_amount,
                                     refund_amount));
    return;
  }
  cprc->refund_amount = *refund_amount;
  cprc->refunded = GNUNET_YES;
}


/**
 * The client did not yet pay, send it the payment request.
 *
 * @param cprc check pay request context
 * @return #MHD_YES on success
 */
static int
send_pay_request (struct CheckPaymentRequestContext *cprc)
{
  int ret;
  char *already_paid_order_id = NULL;
  char *taler_pay_uri;
  struct GNUNET_TIME_Relative remaining;

  remaining = GNUNET_TIME_absolute_get_remaining (cprc->sc.long_poll_timeout);
  if (0 != remaining.rel_value_us)
  {
    /* long polling: do not queue a response, suspend connection instead */
    TMH_compute_pay_key (cprc->order_id,
                         &cprc->mi->pubkey,
                         &cprc->sc.key);
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Suspending /check-payment on key %s\n",
                GNUNET_h2s (&cprc->sc.key));
    TMH_long_poll_suspend (&cprc->sc);
    return MHD_YES;
  }

  /* Check if resource_id has been paid for in the same session
   * with another order_id.
   */
  if ( (NULL != cprc->session_id) &&
       (NULL != cprc->fulfillment_url) )
  {
    enum GNUNET_DB_QueryStatus qs;

    qs = db->find_session_info (db->cls,
                                &already_paid_order_id,
                                cprc->session_id,
                                cprc->fulfillment_url,
                                &cprc->mi->pubkey);
    if (qs < 0)
    {
      /* single, read-only SQL statements should never cause
         serialization problems */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TMH_RESPONSE_reply_internal_error (cprc->sc.con,
                                                TALER_EC_CHECK_PAYMENT_DB_FETCH_ORDER_ERROR,
                                                "db error fetching pay session info");
    }
  }
  taler_pay_uri = TMH_make_taler_pay_uri (cprc->sc.con,
                                          cprc->order_id,
                                          cprc->session_id,
                                          cprc->mi->id);
  ret = TMH_RESPONSE_reply_json_pack (cprc->sc.con,
                                      MHD_HTTP_OK,
                                      "{s:s, s:s, s:b, s:s?}",
                                      "taler_pay_uri", taler_pay_uri,
                                      "contract_url", cprc->final_contract_url,
                                      "paid", 0,
                                      "already_paid_order_id",
                                      already_paid_order_id);
  GNUNET_free (taler_pay_uri);
  GNUNET_free_non_null (already_paid_order_id);
  return ret;
}


/**
 * Parse the "contract_terms" in @a cprc and set the
 * "fulfillment_url" and the "h_contract_terms" in @a cprc
 * accordingly.
 *
 * On errors, the response is being queued and the status
 * code set in @cprc "ret".
 *
 * @param cprc[in,out] context to process
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on failure
 */
static int
parse_contract_terms (struct CheckPaymentRequestContext *cprc)
{
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("fulfillment_url",
                             &cprc->fulfillment_url),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (cprc->contract_terms,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break (0);
    cprc->ret
      = TMH_RESPONSE_reply_internal_error (cprc->sc.con,
                                           TALER_EC_CHECK_PAYMENT_DB_FETCH_CONTRACT_TERMS_ERROR,
                                           "Merchant database error (contract terms corrupted)");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_JSON_hash (cprc->contract_terms,
                       &cprc->h_contract_terms))
  {
    GNUNET_break (0);
    cprc->ret
      = TMH_RESPONSE_reply_internal_error (cprc->sc.con,
                                           TALER_EC_CHECK_PAYMENT_FAILED_COMPUTE_PROPOSAL_HASH,
                                           "Failed to hash proposal");
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Check that we are aware of @a order_id and if so request the payment,
 * otherwise generate an error response.
 *
 * @param cprc session state
 * @return status code to return to MHD for @a connection
 */
static int
check_order_and_request_payment (struct CheckPaymentRequestContext *cprc)
{
  enum GNUNET_DB_QueryStatus qs;

  if (NULL != cprc->contract_terms)
  {
    /* This should never happen. */
    GNUNET_break (0);
    json_decref (cprc->contract_terms);
    cprc->contract_terms = NULL;
  }
  qs = db->find_order (db->cls,
                       &cprc->contract_terms,
                       cprc->order_id,
                       &cprc->mi->pubkey);
  if (0 > qs)
  {
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return TMH_RESPONSE_reply_internal_error (cprc->sc.con,
                                              TALER_EC_CHECK_PAYMENT_DB_FETCH_ORDER_ERROR,
                                              "db error fetching order");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    return TMH_RESPONSE_reply_not_found (cprc->sc.con,
                                         TALER_EC_CHECK_PAYMENT_ORDER_ID_UNKNOWN,
                                         "unknown order_id");
  }

  if (GNUNET_OK !=
      parse_contract_terms (cprc))
    return cprc->ret;
  /* Offer was not picked up yet, but we ensured that it exists */
  return send_pay_request (cprc);
}


/**
 * Manages a /check-payment call, checking the status
 * of a payment and, if necessary, constructing the URL
 * for a payment redirect URL.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_check_payment (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size,
                          struct MerchantInstance *mi)
{
  struct CheckPaymentRequestContext *cprc = *connection_cls;
  enum GNUNET_DB_QueryStatus qs;
  int ret;

  if (NULL == cprc)
  {
    const char *long_poll_timeout_s;

    /* First time here, parse request and check order is known */
    cprc = GNUNET_new (struct CheckPaymentRequestContext);
    cprc->hc.cc = &cprc_cleanup;
    cprc->ret = GNUNET_SYSERR;
    cprc->sc.con = connection;
    cprc->mi = mi;
    *connection_cls = cprc;

    cprc->order_id = MHD_lookup_connection_value (connection,
                                                  MHD_GET_ARGUMENT_KIND,
                                                  "order_id");
    if (NULL == cprc->order_id)
    {
      /* order_id is required but missing */
      GNUNET_break_op (0);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MISSING,
                                             "order_id required");
    }
    cprc->contract_url = MHD_lookup_connection_value (connection,
                                                      MHD_GET_ARGUMENT_KIND,
                                                      "contract_url");
    if (NULL == cprc->contract_url)
    {
      cprc->final_contract_url = TALER_url_absolute_mhd (connection,
                                                         "/public/proposal",
                                                         "instance", mi->id,
                                                         "order_id",
                                                         cprc->order_id,
                                                         NULL);
      GNUNET_assert (NULL != cprc->final_contract_url);
    }
    else
    {
      cprc->final_contract_url = GNUNET_strdup (cprc->contract_url);
    }
    cprc->session_id = MHD_lookup_connection_value (connection,
                                                    MHD_GET_ARGUMENT_KIND,
                                                    "session_id");
    long_poll_timeout_s = MHD_lookup_connection_value (connection,
                                                       MHD_GET_ARGUMENT_KIND,
                                                       "timeout");
    if (NULL != long_poll_timeout_s)
    {
      unsigned int timeout;

      if (1 != sscanf (long_poll_timeout_s,
                       "%u",
                       &timeout))
      {
        GNUNET_break_op (0);
        return TMH_RESPONSE_reply_bad_request (connection,
                                               TALER_EC_PARAMETER_MALFORMED,
                                               "timeout must be non-negative number");
      }
      cprc->sc.long_poll_timeout
        = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_relative_multiply (
                                              GNUNET_TIME_UNIT_SECONDS,
                                              timeout));
    }
    else
    {
      cprc->sc.long_poll_timeout = GNUNET_TIME_UNIT_ZERO_ABS;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Starting /check-payment processing with timeout %s\n",
                GNUNET_STRINGS_absolute_time_to_string (
                  cprc->sc.long_poll_timeout));
    db->preflight (db->cls);
    qs = db->find_contract_terms (db->cls,
                                  &cprc->contract_terms,
                                  cprc->order_id,
                                  &mi->pubkey);
    if (0 > qs)
    {
      /* single, read-only SQL statements should never cause
         serialization problems */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_CHECK_PAYMENT_DB_FETCH_CONTRACT_TERMS_ERROR,
                                                "db error fetching contract terms");
    }

    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    {
      /* Check that we're at least aware of the order */
      return check_order_and_request_payment (cprc);
    }
    GNUNET_assert (NULL != cprc->contract_terms);

    if (GNUNET_OK !=
        parse_contract_terms (cprc))
      return cprc->ret;
  }

  GNUNET_assert (NULL != cprc->contract_terms);

  /* Check if the order has been paid for. */
  if (NULL != cprc->session_id)
  {
    /* Check if paid within a session. */
    char *already_paid_order_id = NULL;

    qs = db->find_session_info (db->cls,
                                &already_paid_order_id,
                                cprc->session_id,
                                cprc->fulfillment_url,
                                &mi->pubkey);
    if (qs < 0)
    {
      /* single, read-only SQL statements should never cause
         serialization problems */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_CHECK_PAYMENT_DB_FETCH_ORDER_ERROR,
                                                "db error fetching pay session info");
    }
    else if (0 == qs)
    {
      ret = send_pay_request (cprc);
      GNUNET_free_non_null (already_paid_order_id);
      return ret;
    }
    GNUNET_break (1 == qs);
    GNUNET_break (0 == strcmp (cprc->order_id,
                               already_paid_order_id));
    GNUNET_free_non_null (already_paid_order_id);
  }
  else
  {
    /* Check if paid regardless of session. */
    json_t *xcontract_terms = NULL;

    qs = db->find_paid_contract_terms_from_hash (db->cls,
                                                 &xcontract_terms,
                                                 &cprc->h_contract_terms,
                                                 &mi->pubkey);
    if (0 > qs)
    {
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                                "Merchant database error");
    }
    if (0 == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "not paid yet\n");
      return send_pay_request (cprc);
    }
    GNUNET_break (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs);
    GNUNET_assert (NULL != xcontract_terms);
    json_decref (xcontract_terms);
  }

  /* Accumulate refunds, if any. */
  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    qs = db->get_refunds_from_contract_terms_hash (db->cls,
                                                   &mi->pubkey,
                                                   &cprc->h_contract_terms,
                                                   &process_refunds_cb,
                                                   cprc);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Database hard error on refunds_from_contract_terms_hash lookup: %s\n",
                GNUNET_h2s (&cprc->h_contract_terms));
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                              "Merchant database error");
  }
  if (cprc->refunded)
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_OK,
                                         "{s:o, s:b, s:b, s:o}",
                                         "contract_terms", cprc->contract_terms,
                                         "paid", 1,
                                         "refunded", cprc->refunded,
                                         "refund_amount",
                                         TALER_JSON_from_amount (
                                           &cprc->refund_amount));
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:o, s:b, s:b }",
                                       "contract_terms", cprc->contract_terms,
                                       "paid", 1,
                                       "refunded", 0);
}
