/*
  This file is part of TALER
  (C) 2018--2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_tip-reserve-helper.c
 * @brief helper functions to check the status of a tipping reserve
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_tip-reserve-helper.h"


/**
 * Head of active ctr context DLL.
 */
static struct TMH_CheckTipReserve *ctr_head;

/**
 * Tail of active ctr context DLL.
 */
static struct TMH_CheckTipReserve *ctr_tail;


/**
 * Resume connection underlying @a ctr.
 *
 * @param ctr what to resume
 */
static void
resume_ctr (struct TMH_CheckTipReserve *ctr)
{
  GNUNET_assert (GNUNET_YES == ctr->suspended);
  GNUNET_CONTAINER_DLL_remove (ctr_head,
                               ctr_tail,
                               ctr);
  MHD_resume_connection (ctr->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
}


/**
 * Resume the given context and send the given response.  Stores the response
 * in the @a ctr and signals MHD to resume the connection.  Also ensures MHD
 * runs immediately.
 *
 * @param ctr tip reserve query helper context
 * @param response_code response code to use
 * @param response response data to send back
 */
static void
resume_with_response (struct TMH_CheckTipReserve *ctr,
                      unsigned int response_code,
                      struct MHD_Response *response)
{
  ctr->response_code = response_code;
  ctr->response = response;
  resume_ctr (ctr);
  ctr->suspended = GNUNET_NO;
}


/**
 * Function called with the result of the /reserve/status request
 * for the tipping reserve.  Update our database balance with the
 * result.
 *
 * @param cls closure with a `struct TMH_CheckTipReserve *'
 * @param hr HTTP response details
 * @param balance current balance in the reserve, NULL on error
 * @param history_length number of entries in the transaction history, 0 on error
 * @param history detailed transaction history, NULL on error
 */
static void
handle_status (void *cls,
               const struct TALER_EXCHANGE_HttpResponse *hr,
               const struct TALER_Amount *balance,
               unsigned int history_length,
               const struct TALER_EXCHANGE_ReserveHistory *history)
{
  struct TMH_CheckTipReserve *ctr = cls;

  ctr->rsh = NULL;
  ctr->reserve_expiration = GNUNET_TIME_UNIT_ZERO_ABS;
  if (MHD_HTTP_NOT_FOUND == hr->http_status)
  {
    resume_with_response (
      ctr,
      MHD_HTTP_SERVICE_UNAVAILABLE,
      TALER_MHD_make_json_pack (
        "{s:I, s:I, s:s, s:I, s:O}",
        "code", (json_int_t) TALER_EC_TIP_QUERY_RESERVE_UNKNOWN_TO_EXCHANGE,
        "exchange_http_status", hr->http_status,
        "hint", "tipping reserve unknown at exchange",
        "exchange_code", hr->ec,
        "exchange_reply", hr->reply));
    return;
  }
  if (MHD_HTTP_OK != hr->http_status)
  {
    GNUNET_break_op (0);
    resume_with_response (
      ctr,
      MHD_HTTP_FAILED_DEPENDENCY,
      TALER_MHD_make_json_pack (
        "{s:I, s:I, s:s, s:I, s:O}",
        "code", (json_int_t) TALER_EC_TIP_QUERY_RESERVE_HISTORY_FAILED,
        "exchange_http_status", hr->http_status,
        "hint", "exchange failed to provide reserve history",
        "exchange_code", (json_int_t) hr->ec,
        "exchange_reply", hr->reply));
    return;
  }

  if (0 == history_length)
  {
    GNUNET_break_op (0);
    resume_with_response (ctr,
                          MHD_HTTP_FAILED_DEPENDENCY,
                          TALER_MHD_make_error (
                            TALER_EC_TIP_QUERY_RESERVE_HISTORY_FAILED_EMPTY,
                            "Exchange returned empty reserve history"));
    return;
  }

  {
    unsigned int found = UINT_MAX;

    for (unsigned int i = 0; i<history_length; i++)
      if (TALER_EXCHANGE_RTT_CREDIT == history[i].type)
        found = i;
    if (UINT_MAX == found)
    {
      GNUNET_break_op (0);
      resume_with_response (ctr,
                            MHD_HTTP_FAILED_DEPENDENCY,
                            TALER_MHD_make_error (
                              TALER_EC_TIP_QUERY_RESERVE_HISTORY_INVALID_NO_DEPOSIT,
                              "Exchange returned invalid reserve history"));
      return;
    }

    if (0 != strcasecmp (TMH_currency,
                         history[found].amount.currency))
    {
      GNUNET_break_op (0);
      resume_with_response (ctr,
                            MHD_HTTP_SERVICE_UNAVAILABLE,
                            TALER_MHD_make_error (
                              TALER_EC_TIP_QUERY_RESERVE_CURRENCY_MISMATCH,
                              "Exchange currency unexpected"));
      return;
    }
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (history[found].amount.currency,
                                          &ctr->amount_withdrawn));
  }

  if (GNUNET_YES == ctr->none_authorized)
    ctr->amount_authorized = ctr->amount_withdrawn; /* aka zero */
  ctr->amount_deposited = ctr->amount_withdrawn; /* aka zero */

  /* Update DB based on status! */
  for (unsigned int i = 0; i<history_length; i++)
  {
    const struct TALER_EXCHANGE_ReserveHistory *hi = &history[i];

    switch (hi->type)
    {
    case TALER_EXCHANGE_RTT_CREDIT:
      {
        enum GNUNET_DB_QueryStatus qs;
        struct GNUNET_HashCode uuid;
        struct GNUNET_TIME_Absolute deposit_expiration;

        if (0 >
            TALER_amount_add (&ctr->amount_deposited,
                              &ctr->amount_deposited,
                              &hi->amount))
        {
          GNUNET_break_op (0);
          resume_with_response (
            ctr,
            MHD_HTTP_FAILED_DEPENDENCY,
            TALER_MHD_make_error (
              TALER_EC_TIP_QUERY_RESERVE_HISTORY_ARITHMETIC_ISSUE_DEPOSIT,
              "Exchange returned invalid reserve history (amount overflow)"));
          return;
        }
        deposit_expiration = GNUNET_TIME_absolute_add (
          hi->details.in_details.timestamp,
          ctr->idle_reserve_expiration_time);
        /* We're interested in the latest DEPOSIT timestamp, since this determines the
         * reserve's expiration date. Note that the history isn't chronologically ordered. */
        ctr->reserve_expiration = GNUNET_TIME_absolute_max (
          ctr->reserve_expiration,
          deposit_expiration);
        GNUNET_CRYPTO_hash (hi->details.in_details.wire_reference,
                            hi->details.in_details.wire_reference_size,
                            &uuid);
        db->preflight (db->cls);
        qs = db->enable_tip_reserve_TR (db->cls,
                                        &ctr->reserve_priv,
                                        &uuid,
                                        &hi->amount,
                                        deposit_expiration);

        if (0 > qs)
        {
          /* This is not inherently fatal for the client's request, so we merely log it */
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Database error updating tipping reserve status: %d\n",
                      qs);
        }
      }
      break;
    case TALER_EXCHANGE_RTT_WITHDRAWAL:
      if (0 >
          TALER_amount_add (&ctr->amount_withdrawn,
                            &ctr->amount_withdrawn,
                            &hi->amount))
      {
        GNUNET_break_op (0);
        resume_with_response (
          ctr,
          MHD_HTTP_FAILED_DEPENDENCY,
          TALER_MHD_make_error (
            TALER_EC_TIP_QUERY_RESERVE_HISTORY_ARITHMETIC_ISSUE_WITHDRAW,
            "Exchange returned invalid reserve history (amount overflow)"));
        return;
      }
      break;
    case TALER_EXCHANGE_RTT_RECOUP:
      {
        enum GNUNET_DB_QueryStatus qs;
        struct GNUNET_HashContext *hc;
        struct GNUNET_HashCode uuid;
        struct GNUNET_TIME_Absolute deposit_expiration;
        struct GNUNET_TIME_AbsoluteNBO de;

        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Encountered unexpected recoup operation on tipping reserve\n");
        /* While unexpected, we can simply count these like deposits. */
        if (0 >
            TALER_amount_add (&ctr->amount_deposited,
                              &ctr->amount_deposited,
                              &hi->amount))
        {
          GNUNET_break_op (0);
          resume_with_response (
            ctr,
            MHD_HTTP_FAILED_DEPENDENCY,
            TALER_MHD_make_error (
              TALER_EC_TIP_QUERY_RESERVE_HISTORY_ARITHMETIC_ISSUE_RECOUP,
              "Exchange returned invalid reserve history (amount overflow)"));
          return;
        }
        deposit_expiration = GNUNET_TIME_absolute_add (
          hi->details.recoup_details.timestamp,
          ctr->idle_reserve_expiration_time);
        ctr->reserve_expiration = GNUNET_TIME_absolute_max (
          ctr->reserve_expiration,
          deposit_expiration);
        de = GNUNET_TIME_absolute_hton (deposit_expiration);
        hc = GNUNET_CRYPTO_hash_context_start ();
        GNUNET_CRYPTO_hash_context_read (
          hc,
          &hi->details.recoup_details.coin_pub,
          sizeof (struct TALER_CoinSpendPublicKeyP));
        GNUNET_CRYPTO_hash_context_read (hc,
                                         &de,
                                         sizeof (de));
        GNUNET_CRYPTO_hash_context_finish (hc,
                                           &uuid);
        db->preflight (db->cls);
        qs = db->enable_tip_reserve_TR (db->cls,
                                        &ctr->reserve_priv,
                                        &uuid,
                                        &hi->amount,
                                        deposit_expiration);
        if (0 > qs)
        {
          /* This is not inherently fatal for the client's request, so we merely log it */
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Database error updating tipping reserve status: %d\n",
                      qs);
        }
      }
      break;
    case TALER_EXCHANGE_RTT_CLOSE:
      /* We count 'closing' amounts just like withdrawals */
      if (0 >
          TALER_amount_add (&ctr->amount_withdrawn,
                            &ctr->amount_withdrawn,
                            &hi->amount))
      {
        GNUNET_break_op (0);
        resume_with_response (
          ctr,
          MHD_HTTP_FAILED_DEPENDENCY,
          TALER_MHD_make_error (
            TALER_EC_TIP_QUERY_RESERVE_HISTORY_ARITHMETIC_ISSUE_CLOSED,
            "Exchange returned invalid reserve history (amount overflow)"));
        return;
      }
      break;
    }
  }

  /* normal, non-error continuation */
  resume_with_response (ctr,
                        0,
                        NULL);
}


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.  Given the exchange handle, we will then interrogate
 * the exchange about the status of the tipping reserve.
 *
 * @param cls closure with a `struct TMH_CheckTipReserve *`
 * @param hr HTTP response details
 * @param eh handle to the exchange context
 * @param wire_fee current applicable wire fee for dealing with @a eh, NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
exchange_cont (void *cls,
               const struct TALER_EXCHANGE_HttpResponse *hr,
               struct TALER_EXCHANGE_Handle *eh,
               const struct TALER_Amount *wire_fee,
               int exchange_trusted)
{
  struct TMH_CheckTipReserve *ctr = cls;
  struct TALER_ReservePublicKeyP reserve_pub;
  const struct TALER_EXCHANGE_Keys *keys;

  ctr->fo = NULL;
  if (NULL == eh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to contact exchange configured for tipping!\n");
    resume_with_response (ctr,
                          MHD_HTTP_FAILED_DEPENDENCY,
                          TALER_MHD_make_error (
                            TALER_EC_TIP_QUERY_RESERVE_STATUS_FAILED_EXCHANGE_DOWN,
                            "Unable to obtain /keys from exchange"));
    return;
  }
  keys = TALER_EXCHANGE_get_keys (eh);
  GNUNET_assert (NULL != keys);
  ctr->idle_reserve_expiration_time
    = keys->reserve_closing_delay;
  GNUNET_CRYPTO_eddsa_key_get_public (&ctr->reserve_priv.eddsa_priv,
                                      &reserve_pub.eddsa_pub);
  ctr->rsh = TALER_EXCHANGE_reserves_get (eh,
                                          &reserve_pub,
                                          &handle_status,
                                          ctr);
}


/**
 * Check the status of the given reserve at the given exchange.
 * Suspends the MHD connection while this is happening and resumes
 * processing once we know the reserve status (or once an error
 * code has been determined).
 *
 * @param[in,out] ctr context for checking the reserve status
 * @param tip_exchange the URL of the exchange to query
 */
void
TMH_check_tip_reserve (struct TMH_CheckTipReserve *ctr,
                       const char *tip_exchange)
{
  MHD_suspend_connection (ctr->connection);
  ctr->suspended = GNUNET_YES;
  GNUNET_CONTAINER_DLL_insert (ctr_head,
                               ctr_tail,
                               ctr);
  db->preflight (db->cls);
  ctr->fo = TMH_EXCHANGES_find_exchange (tip_exchange,
                                         NULL,
                                         GNUNET_NO,
                                         &exchange_cont,
                                         ctr);
  if (NULL == ctr->fo)
  {
    GNUNET_break (0);
    resume_with_response (ctr,
                          MHD_HTTP_INTERNAL_SERVER_ERROR,
                          TALER_MHD_make_error (
                            TALER_EC_INTERNAL_INVARIANT_FAILURE,
                            "Unable to find exchange handle"));
  }
}


/**
 * Clean up any state that might be left in @a ctr.
 *
 * @param[in] context to clean up
 */
void
TMH_check_tip_reserve_cleanup (struct TMH_CheckTipReserve *ctr)
{
  if (NULL != ctr->rsh)
  {
    TALER_EXCHANGE_reserves_get_cancel (ctr->rsh);
    ctr->rsh = NULL;
  }
  if (NULL != ctr->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (ctr->fo);
    ctr->fo = NULL;
  }
  if (NULL != ctr->response)
  {
    MHD_destroy_response (ctr->response);
    ctr->response = NULL;
  }
  if (MHD_YES == ctr->suspended)
  {
    resume_ctr (ctr);
    ctr->suspended = GNUNET_NO;
  }
}


/**
 * Force all tip reserve helper contexts to be resumed as we are about to shut
 * down MHD.
 */
void
MH_force_trh_resume ()
{
  struct TMH_CheckTipReserve *n;

  for (struct TMH_CheckTipReserve *ctr = ctr_head;
       NULL != ctr;
       ctr = n)
  {
    n = ctr->next;
    resume_ctr (ctr);
    ctr->suspended = GNUNET_SYSERR;
  }
}


/* end of taler-merchant-httpd_tip-reserve-helper.c */
