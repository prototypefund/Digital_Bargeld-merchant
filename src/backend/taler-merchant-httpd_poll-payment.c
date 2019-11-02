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
 * @file backend/taler-merchant-httpd_poll-payment.c
 * @brief implementation of /public/poll-payment handler
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
#include "taler-merchant-httpd_poll-payment.h"

/**
 * Maximum number of retries for database operations.
 */
#define MAX_RETRIES 5


/**
 * Data structure we keep for a check payment request.
 */
struct PollPaymentRequestContext
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
   * fulfillment URL of the contract (valid as long as
   * @e contract_terms is valid).
   */
  const char *fulfillment_url;

  /**
   * session of the client
   */
  const char *session_id;

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
 * @param hc must be a `struct PollPaymentRequestContext *`
 */
static void
pprc_cleanup (struct TM_HandlerContext *hc)
{
  struct PollPaymentRequestContext *pprc = (struct
                                            PollPaymentRequestContext *) hc;

  if (NULL != pprc->contract_terms)
    json_decref (pprc->contract_terms);
  GNUNET_free_non_null (pprc->final_contract_url);
  GNUNET_free (pprc);
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
  struct PollPaymentRequestContext *pprc = cls;

  if (pprc->refunded)
  {
    GNUNET_assert (GNUNET_SYSERR !=
                   TALER_amount_add (&pprc->refund_amount,
                                     &pprc->refund_amount,
                                     refund_amount));
    return;
  }
  pprc->refund_amount = *refund_amount;
  pprc->refunded = GNUNET_YES;
}


/**
 * Resume processing all suspended connections past timeout.
 *
 * @param cls unused
 */
static void
do_resume (void *cls)
{
  struct TMH_SuspendedConnection *sc;

  (void) cls;
  resume_timeout_task = NULL;
  while (1)
  {
    sc = GNUNET_CONTAINER_heap_peek (resume_timeout_heap);
    if (NULL == sc)
      return;
    if  (0 !=
         GNUNET_TIME_absolute_get_remaining (
           sc->long_poll_timeout).rel_value_us)
      break;
    GNUNET_assert (sc ==
                   GNUNET_CONTAINER_heap_remove_root (resume_timeout_heap));
    sc->hn = NULL;
    GNUNET_assert (GNUNET_YES ==
                   GNUNET_CONTAINER_multihashmap_remove (payment_trigger_map,
                                                         &sc->key,
                                                         sc));
    MHD_resume_connection (sc->con);
  }
  resume_timeout_task = GNUNET_SCHEDULER_add_at (sc->long_poll_timeout,
                                                 &do_resume,
                                                 NULL);
}


/**
 * The client did not yet pay, send it the payment request.
 *
 * @param pprc check pay request context
 * @return #MHD_YES on success
 */
static int
send_pay_request (struct PollPaymentRequestContext *pprc)
{
  int ret;
  char *already_paid_order_id = NULL;
  char *taler_pay_uri;

  if (0 !=
      GNUNET_TIME_absolute_get_remaining (
        pprc->sc.long_poll_timeout).rel_value_us)
  {
    struct TMH_SuspendedConnection *sc;

    /* long polling: do not queue a response, suspend connection instead */
    TMH_compute_pay_key (pprc->order_id,
                         &pprc->mi->pubkey,
                         &pprc->sc.key);
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CONTAINER_multihashmap_put (payment_trigger_map,
                                                      &pprc->sc.key,
                                                      &pprc->sc,
                                                      GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE));
    pprc->sc.hn = GNUNET_CONTAINER_heap_insert (resume_timeout_heap,
                                                &pprc->sc,
                                                pprc->sc.long_poll_timeout.
                                                abs_value_us);
    MHD_suspend_connection (pprc->sc.con);
    if (NULL != resume_timeout_task)
    {
      GNUNET_SCHEDULER_cancel (resume_timeout_task);
      resume_timeout_task = NULL;
    }
    sc = GNUNET_CONTAINER_heap_peek (resume_timeout_heap);
    resume_timeout_task = GNUNET_SCHEDULER_add_at (sc->long_poll_timeout,
                                                   &do_resume,
                                                   NULL);
    return MHD_YES;
  }

  /* Check if resource_id has been paid for in the same session
   * with another order_id.
   */
  if ( (NULL != pprc->session_id) &&
       (NULL != pprc->fulfillment_url) )
  {
    enum GNUNET_DB_QueryStatus qs;

    qs = db->find_session_info (db->cls,
                                &already_paid_order_id,
                                pprc->session_id,
                                pprc->fulfillment_url,
                                &pprc->mi->pubkey);
    if (qs < 0)
    {
      /* single, read-only SQL statements should never cause
         serialization problems */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TMH_RESPONSE_reply_internal_error (pprc->sc.con,
                                                TALER_EC_CHECK_PAYMENT_DB_FETCH_ORDER_ERROR,
                                                "db error fetching pay session info");
    }
  }
  taler_pay_uri = TMH_make_taler_pay_uri (pprc->sc.con,
                                          pprc->order_id,
                                          pprc->session_id,
                                          pprc->mi->id);
  ret = TMH_RESPONSE_reply_json_pack (pprc->sc.con,
                                      MHD_HTTP_OK,
                                      "{s:s, s:s, s:b, s:s?}",
                                      "taler_pay_uri", taler_pay_uri,
                                      "contract_url", pprc->final_contract_url,
                                      "paid", 0,
                                      "already_paid_order_id",
                                      already_paid_order_id);
  GNUNET_free (taler_pay_uri);
  GNUNET_free_non_null (already_paid_order_id);
  return ret;
}


/**
 * Manages a /public/poll-payment call, checking the status
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
MH_handler_poll_payment (struct TMH_RequestHandler *rh,
                         struct MHD_Connection *connection,
                         void **connection_cls,
                         const char *upload_data,
                         size_t *upload_data_size,
                         struct MerchantInstance *mi)
{
  struct PollPaymentRequestContext *pprc = *connection_cls;
  enum GNUNET_DB_QueryStatus qs;
  int ret;

  if (NULL == pprc)
  {
    /* First time here, parse request and check order is known */
    const char *long_poll_timeout_s;
    const char *cts;

    pprc = GNUNET_new (struct PollPaymentRequestContext);
    pprc->hc.cc = &pprc_cleanup;
    pprc->ret = GNUNET_SYSERR;
    pprc->sc.con = connection;
    pprc->mi = mi;
    *connection_cls = pprc;

    pprc->order_id = MHD_lookup_connection_value (connection,
                                                  MHD_GET_ARGUMENT_KIND,
                                                  "order_id");
    if (NULL == pprc->order_id)
    {
      /* order_id is required but missing */
      GNUNET_break_op (0);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MISSING,
                                             "order_id required");
    }
    cts = MHD_lookup_connection_value (connection,
                                       MHD_GET_ARGUMENT_KIND,
                                       "h_contract");
    if (NULL == cts)
    {
      /* h_contract required but missing */
      GNUNET_break_op (0);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MISSING,
                                             "h_contract required");
    }
    if (GNUNET_OK !=
        GNUNET_CRYPTO_hash_from_string (cts,
                                        &pprc->h_contract_terms))
    {
      /* cts has wrong encoding */
      GNUNET_break_op (0);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "h_contract malformed");
    }
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
      pprc->sc.long_poll_timeout
        = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_relative_multiply (
                                              GNUNET_TIME_UNIT_SECONDS,
                                              timeout));
    }
    else
    {
      pprc->sc.long_poll_timeout = GNUNET_TIME_UNIT_ZERO_ABS;
    }
    pprc->contract_url = MHD_lookup_connection_value (connection,
                                                      MHD_GET_ARGUMENT_KIND,
                                                      "contract_url");
    if (NULL == pprc->contract_url)
    {
      pprc->final_contract_url = TALER_url_absolute_mhd (connection,
                                                         "/public/proposal",
                                                         "instance", mi->id,
                                                         "order_id",
                                                         pprc->order_id,
                                                         NULL);
      GNUNET_assert (NULL != pprc->final_contract_url);
    }
    else
    {
      pprc->final_contract_url = GNUNET_strdup (pprc->contract_url);
    }
    pprc->session_id = MHD_lookup_connection_value (connection,
                                                    MHD_GET_ARGUMENT_KIND,
                                                    "session_id");

    /* obtain contract terms, indirectly checking that the client's contract
       terms hash is actually valid and known. */
    db->preflight (db->cls);
    qs = db->find_contract_terms_from_hash (db->cls,
                                            &pprc->contract_terms,
                                            &pprc->h_contract_terms,
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
                  "Contract unknown\n");
      return TMH_RESPONSE_reply_not_found (connection,
                                           TALER_EC_POLL_PAYMENT_CONTRACT_NOT_FOUND,
                                           "Given order_id doesn't map to any proposal");
    }
    GNUNET_break (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs);

    {
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_string ("fulfillment_url",
                                 &pprc->fulfillment_url),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (pprc->contract_terms,
                             spec,
                             NULL, NULL))
      {
        GNUNET_break (0);
        return TMH_RESPONSE_reply_internal_error (connection,
                                                  TALER_EC_CHECK_PAYMENT_DB_FETCH_CONTRACT_TERMS_ERROR,
                                                  "Merchant database error (contract terms corrupted)");
      }
    }

  } /* end of first-time initialization / sanity checks */


  db->preflight (db->cls);

  /* Check if the order has been paid for. */
  if (NULL != pprc->session_id)
  {
    /* Check if paid within a session. */
    char *already_paid_order_id = NULL;

    qs = db->find_session_info (db->cls,
                                &already_paid_order_id,
                                pprc->session_id,
                                pprc->fulfillment_url,
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
      ret = send_pay_request (pprc);
      GNUNET_free_non_null (already_paid_order_id);
      return ret;
    }
    GNUNET_break (1 == qs);
    GNUNET_break (0 == strcmp (pprc->order_id,
                               already_paid_order_id));
    GNUNET_free_non_null (already_paid_order_id);
  }
  else
  {
    /* Check if paid regardless of session. */
    json_t *xcontract_terms = NULL;

    qs = db->find_paid_contract_terms_from_hash (db->cls,
                                                 &xcontract_terms,
                                                 &pprc->h_contract_terms,
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
      return send_pay_request (pprc);
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
                                                   &pprc->h_contract_terms,
                                                   &process_refunds_cb,
                                                   pprc);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Database hard error on refunds_from_contract_terms_hash lookup: %s\n",
                GNUNET_h2s (&pprc->h_contract_terms));
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                              "Merchant database error");
  }
  if (pprc->refunded)
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_OK,
                                         "{s:b, s:b, s:o}",
                                         "paid", 1,
                                         "refunded", pprc->refunded,
                                         "refund_amount",
                                         TALER_JSON_from_amount (
                                           &pprc->refund_amount));
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:b, s:b }",
                                       "paid", 1,
                                       "refunded", 0);
}
