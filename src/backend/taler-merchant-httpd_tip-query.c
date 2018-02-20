/*
  This file is part of TALER
  (C) 2018 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_tip-query.c
 * @brief implement API for authorizing tips to be paid to visitors
 * @author Christian Grothoff
 * @author Florian Dold
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_tip-query.h"


/**
 * Maximum number of retries for database operations.
 */
#define MAX_RETRIES 5


struct TipQueryContext
{
  /**
   * This field MUST be first.
   * FIXME: Explain why!
   */
  struct TM_HandlerContext hc;

  /**
   * HTTP connection we are handling.
   */
  struct MHD_Connection *connection;

  /**
   * Merchant instance to use.
   */
  const char *instance;

  /**
   * Handle to pending /reserve/status request.
   */
  struct TALER_EXCHANGE_ReserveStatusHandle *rsh;

  /**
   * Handle for operation to obtain exchange handle.
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Reserve expiration time as provided by the exchange.
   * Set in #exchange_cont.
   */
  struct GNUNET_TIME_Relative idle_reserve_expiration_time;

  /**
   * Tip amount requested.
   */
  struct TALER_Amount amount_deposited;

  /**
   * Tip amount requested.
   */
  struct TALER_Amount amount_withdrawn;

  /**
   * Amount authorized.
   */
  struct TALER_Amount amount_authorized;

  /**
   * Private key used by this merchant for the tipping reserve.
   */
  struct TALER_ReservePrivateKeyP reserve_priv;

  /**
   * No tips were authorized yet.
   */
  int none_authorized;

  /**
   * Response to return, NULL if we don't have one yet.
   */
  struct MHD_Response *response;

  /**
   * HTTP status code to use for the reply, i.e 200 for "OK".
   * Special value UINT_MAX is used to indicate hard errors
   * (no reply, return #MHD_NO).
   */
  unsigned int response_code;

  /**
   * #GNUNET_NO if the @e connection was not suspended,
   * #GNUNET_YES if the @e connection was suspended,
   * #GNUNET_SYSERR if @e connection was resumed to as
   * part of #MH_force_pc_resume during shutdown.
   */
  int suspended;
};


/**
 * Custom cleanup routine for a `struct TipQueryContext`.
 *
 * @param hc the `struct TMH_JsonParseContext` to clean up.
 */
static void
cleanup_tqc (struct TM_HandlerContext *hc)
{
  struct TipQueryContext *tqc = (struct TipQueryContext *) hc;

  if (NULL != tqc->rsh)
  {
    TALER_EXCHANGE_reserve_status_cancel (tqc->rsh);
    tqc->rsh = NULL;
  }
  if (NULL != tqc->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (tqc->fo);
    tqc->fo = NULL;
  }
  GNUNET_free (tqc);
}


/**
 * Resume the given context and send the given response.  Stores the response
 * in the @a pc and signals MHD to resume the connection.  Also ensures MHD
 * runs immediately.
 *
 * @param pc payment context
 * @param response_code response code to use
 * @param response response data to send back
 */
static void
resume_with_response (struct TipQueryContext *tqc,
                      unsigned int response_code,
                      struct MHD_Response *response)
{
  tqc->response_code = response_code;
  tqc->response = response;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /tip-query response (%u)\n",
              response_code);
  GNUNET_assert (GNUNET_YES == tqc->suspended);
  tqc->suspended = GNUNET_NO;
  MHD_resume_connection (tqc->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
}


/**
 * Function called with the result of the /reserve/status request
 * for the tipping reserve.  Update our database balance with the
 * result.
 *
 * @param cls closure with a `struct TipAuthContext *'
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param[in] json original response in JSON format (useful only for diagnostics)
 * @param balance current balance in the reserve, NULL on error
 * @param history_length number of entries in the transaction history, 0 on error
 * @param history detailed transaction history, NULL on error
 */
static void
handle_status (void *cls,
               unsigned int http_status,
               enum TALER_ErrorCode ec,
               const json_t *json,
               const struct TALER_Amount *balance,
               unsigned int history_length,
               const struct TALER_EXCHANGE_ReserveHistory *history)
{
  struct TipQueryContext *tqc = cls;
  struct GNUNET_TIME_Absolute expiration;

  tqc->rsh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break_op (0);
    resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                          TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                   "Unable to obtain reserve status from exchange"));
    return;
  }

  if (0 == history_length)
  {
    GNUNET_break_op (0);
    resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                          TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                   "Exchange returned empty reserve history"));
    return;
  }

  if (TALER_EXCHANGE_RTT_DEPOSIT != history[0].type)
  {
    GNUNET_break_op (0);
    resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                          TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                   "Exchange returned invalid reserve history"));
    return;
  }

  if (GNUNET_OK != TALER_amount_get_zero (history[0].amount.currency, &tqc->amount_withdrawn))
  {
    GNUNET_break_op (0);
    resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                          TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                   "Exchange returned invalid reserve history"));
    return;
  }

  if (GNUNET_YES == tqc->none_authorized)
    memcpy (&tqc->amount_authorized, &tqc->amount_withdrawn, sizeof (struct TALER_Amount));
  memcpy (&tqc->amount_deposited, &tqc->amount_withdrawn, sizeof (struct TALER_Amount));

  /* Update DB based on status! */
  for (unsigned int i=0;i<history_length;i++)
  {
    switch (history[i].type)
    {
    case TALER_EXCHANGE_RTT_DEPOSIT:
      {
        enum GNUNET_DB_QueryStatus qs;
        struct GNUNET_HashCode uuid;

        expiration = GNUNET_TIME_absolute_add (history[i].details.in_details.timestamp,
                                               tqc->idle_reserve_expiration_time);
        GNUNET_CRYPTO_hash (history[i].details.in_details.wire_reference,
                            history[i].details.in_details.wire_reference_size,
                            &uuid);
        qs = db->enable_tip_reserve (db->cls,
                                     &tqc->reserve_priv,
                                     &uuid,
                                     &history[i].amount,
                                     expiration);
        if (GNUNET_OK != TALER_amount_add (&tqc->amount_deposited,
                                           &tqc->amount_deposited,
                                           &history[i].amount))
        {
          GNUNET_break_op (0);
          resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                                TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                         "Exchange returned invalid reserve history (amount overflow)"));
          return;
        }

        if (0 > qs)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      _("Database error updating tipping reserve status: %d\n"),
                      qs);
        }
      }
      break;
    case TALER_EXCHANGE_RTT_WITHDRAWAL:
      if (GNUNET_OK != TALER_amount_add (&tqc->amount_withdrawn,
                                         &tqc->amount_withdrawn,
                                         &history[i].amount))
      {
        GNUNET_break_op (0);
        resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                              TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                       "Exchange returned invalid reserve history (amount overflow)"));
        return;
      }
      break;
    case TALER_EXCHANGE_RTT_PAYBACK:
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  _("Encountered unsupported /payback operation on tipping reserve\n"));
      break;
    case TALER_EXCHANGE_RTT_CLOSE:
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  _("Exchange closed reserve (due to expiration), balance calulation is likely wrong. Please create a fresh reserve.\n"));
      break;
    }
  }

  {
    struct GNUNET_CRYPTO_EddsaPublicKey reserve_pub;
    struct TALER_Amount amount_available;
    GNUNET_CRYPTO_eddsa_key_get_public (&tqc->reserve_priv.eddsa_priv,
                                        &reserve_pub);
    if (GNUNET_SYSERR == TALER_amount_subtract (&amount_available, &tqc->amount_deposited, &tqc->amount_withdrawn))
    {
        GNUNET_break_op (0);
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "amount overflow, deposited %s but withdrawn %s\n",
                    TALER_amount_to_string (&tqc->amount_deposited),
                    TALER_amount_to_string (&tqc->amount_withdrawn));

        resume_with_response (tqc, MHD_HTTP_SERVICE_UNAVAILABLE,
                              TMH_RESPONSE_make_error (TALER_EC_NONE /* FIXME */,
                                                       "Exchange returned invalid reserve history (amount overflow)"));
    }
    resume_with_response (tqc, MHD_HTTP_OK,
                          TMH_RESPONSE_make_json_pack ("{s:o, s:o, s:o, s:o, s:o}",
                                                       "reserve_pub",
                                                       GNUNET_JSON_from_data_auto (&reserve_pub),
                                                       "reserve_expiration",
                                                       GNUNET_JSON_from_time_abs (expiration),
                                                       "amount_authorized",
                                                       TALER_JSON_from_amount (&tqc->amount_authorized),
                                                       "amount_picked_up",
                                                       TALER_JSON_from_amount (&tqc->amount_withdrawn),
                                                       "amount_available",
                                                       TALER_JSON_from_amount (&amount_available)));
  }
}


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.
 *
 * @param cls closure with a `struct TipQueryContext *'
 * @param eh handle to the exchange context
 * @param wire_fee current applicable wire fee for dealing with @a eh, NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
exchange_cont (void *cls,
               struct TALER_EXCHANGE_Handle *eh,
               const struct TALER_Amount *wire_fee,
               int exchange_trusted)
{
  struct TipQueryContext *tqc = cls;
  struct TALER_ReservePublicKeyP reserve_pub;
  const struct TALER_EXCHANGE_Keys *keys;

  tqc->fo = NULL;
  if (NULL == eh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("Failed to contact exchange configured for tipping!\n"));
    MHD_resume_connection (tqc->connection);
    TMH_trigger_daemon ();
    return;
  }
  keys = TALER_EXCHANGE_get_keys (eh);
  GNUNET_assert (NULL != keys);
  tqc->idle_reserve_expiration_time
    = keys->reserve_closing_delay;
  GNUNET_CRYPTO_eddsa_key_get_public (&tqc->reserve_priv.eddsa_priv,
                                      &reserve_pub.eddsa_pub);
  tqc->rsh = TALER_EXCHANGE_reserve_status (eh,
                                            &reserve_pub,
                                            &handle_status,
                                            tqc);
}


/**
 * Handle a "/tip-query" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_tip_query (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  struct TipQueryContext *tqc;
  int res;
  struct MerchantInstance *mi;

  if (NULL == *connection_cls)
  {
    tqc = GNUNET_new (struct TipQueryContext);
    tqc->hc.cc = &cleanup_tqc;
    tqc->connection = connection;
    *connection_cls = tqc;
  }
  else
  {
    tqc = *connection_cls;
  }

  if (0 != tqc->response_code)
  {
    /* We are *done* processing the request, just queue the response (!) */
    if (UINT_MAX == tqc->response_code)
    {
      GNUNET_break (0);
      return MHD_NO; /* hard error */
    }
    res = MHD_queue_response (connection,
			      tqc->response_code,
			      tqc->response);
    MHD_destroy_response (tqc->response);
    tqc->response = NULL;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"Queueing response (%u) for /tip-query (%s).\n",
		(unsigned int) tqc->response_code,
		res ? "OK" : "FAILED");
    return res;
  }

  tqc->instance = MHD_lookup_connection_value (connection,
                                               MHD_GET_ARGUMENT_KIND,
                                               "instance");
  if (NULL == tqc->instance)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "instance");

  mi = TMH_lookup_instance (tqc->instance);
  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured\n",
                tqc->instance);
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TIP_AUTHORIZE_INSTANCE_UNKNOWN,
					 "unknown instance");
  }
  if (NULL == mi->tip_exchange)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured for tipping\n",
                tqc->instance);
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TIP_AUTHORIZE_INSTANCE_DOES_NOT_TIP,
					 "exchange for tipping not configured for the instance");
  }
  tqc->reserve_priv = mi->tip_reserve;

  {
    int qs;
    for (unsigned int i=0;i<MAX_RETRIES;i++)
    {
      qs = db->get_authorized_tip_amount (db->cls,
                                          &tqc->reserve_priv,
                                          &tqc->amount_authorized);
      if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
        break;
    }
    if (0 > qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Database hard error on get_authorized_tip_amount\n");
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_NONE /* FIXME */,
                                                "Merchant database error");
    }
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    {
      // We'll set amount_authorized to zero later once
      // we know the currency
      tqc->none_authorized = GNUNET_YES;
    }
  }


  MHD_suspend_connection (connection);
  tqc->suspended = GNUNET_YES;

  tqc->fo = TMH_EXCHANGES_find_exchange (mi->tip_exchange,
                                         NULL,
                                         &exchange_cont,
                                         tqc);
  return MHD_YES;
}

/* end of taler-merchant-httpd_tip-query.c */
