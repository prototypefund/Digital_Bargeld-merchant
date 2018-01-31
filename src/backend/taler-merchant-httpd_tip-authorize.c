/*
  This file is part of TALER
  (C) 2014-2017 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_tip-authorize.c
 * @brief implement API for authorizing tips to be paid to visitors
 * @author Christian Grothoff
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
#include "taler-merchant-httpd_tip-authorize.h"


struct TipAuthContext
{
  /**
   * This field MUST be first.
   * FIXME: Explain why!
   */
  struct TM_HandlerContext hc;

  /**
   * Placeholder for #TMH_PARSE_post_json() to keep its internal state.
   */
  void *json_parse_context;

  /**
   * HTTP connection we are handling.
   */
  struct MHD_Connection *connection;

  /**
   * Merchant instance to use.
   */
  const char *instance;

  /**
   * Justification to use.
   */
  const char *justification;

  /**
   * Pickup URL to use.
   */
  const char *pickup_url;

  /**
   * URL to navigate to after tip.
   */
  const char *next_url;

  /**
   * JSON request received.
   */
  json_t *root;

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
  struct TALER_Amount amount;

  /**
   * Private key used by this merchant for the tipping reserve.
   */
  struct TALER_ReservePrivateKeyP reserve_priv;

  /**
   * Flag set to #GNUNET_YES when we have tried /reserve/status of the
   * tipping reserve already.
   */
  int checked_status;

  /**
   * Flag set to #GNUNET_YES when we have parsed the incoming JSON already.
   */
  int parsed_json;
};


/**
 * Custom cleanup routine for a `struct TipAuthContext`.
 *
 * @param hc the `struct TMH_JsonParseContext` to clean up.
 */
static void
cleanup_tac (struct TM_HandlerContext *hc)
{
  struct TipAuthContext *tac = (struct TipAuthContext *) hc;

  if (NULL != tac->root)
  {
    json_decref (tac->root);
    tac->root = NULL;
  }
  if (NULL != tac->rsh)
  {
    TALER_EXCHANGE_reserve_status_cancel (tac->rsh);
    tac->rsh = NULL;
  }
  if (NULL != tac->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (tac->fo);
    tac->fo = NULL;
  }
  TMH_PARSE_post_cleanup_callback (tac->json_parse_context);
  GNUNET_free (tac);
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
  struct TipAuthContext *tac = cls;

  tac->rsh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("Failed to obtain tipping reserve status from exchange (%u/%d)\n"),
                http_status,
                ec);
    MHD_resume_connection (tac->connection);
    TMH_trigger_daemon ();
    return;
  }

  /* Update DB based on status! */
  for (unsigned int i=0;i<history_length;i++)
  {
    switch (history[i].type)
    {
    case TALER_EXCHANGE_RTT_DEPOSIT:
      {
        enum GNUNET_DB_QueryStatus qs;
        struct GNUNET_HashCode uuid;
        struct GNUNET_TIME_Absolute expiration;

        expiration = GNUNET_TIME_absolute_add (history[i].details.in_details.timestamp,
                                               tac->idle_reserve_expiration_time);
        GNUNET_CRYPTO_hash (history[i].details.in_details.wire_reference,
                            history[i].details.in_details.wire_reference_size,
                            &uuid);
        qs = db->enable_tip_reserve (db->cls,
                                     &tac->reserve_priv,
                                     &uuid,
                                     &history[i].amount,
                                     expiration);
        if (0 > qs)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      _("Database error updating tipping reserve status: %d\n"),
                      qs);
        }
      }
    case TALER_EXCHANGE_RTT_WITHDRAWAL:
      /* expected */
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
  /* Finally, resume processing */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming HTTP processing\n");
  MHD_resume_connection (tac->connection);
  TMH_trigger_daemon ();
}


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.
 *
 * @param cls closure with a `struct TipAuthContext *'
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
  struct TipAuthContext *tac = cls;
  struct TALER_ReservePublicKeyP reserve_pub;
  const struct TALER_EXCHANGE_Keys *keys;

  tac->fo = NULL;
  if (NULL == eh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                _("Failed to contact exchange configured for tipping!\n"));
    MHD_resume_connection (tac->connection);
    TMH_trigger_daemon ();
    return;
  }
  keys = TALER_EXCHANGE_get_keys (eh);
  GNUNET_assert (NULL != keys);
  tac->idle_reserve_expiration_time
    = keys->reserve_closing_delay;
  GNUNET_CRYPTO_eddsa_key_get_public (&tac->reserve_priv.eddsa_priv,
                                      &reserve_pub.eddsa_pub);
  tac->rsh = TALER_EXCHANGE_reserve_status (eh,
                                            &reserve_pub,
                                            &handle_status,
                                            tac);
}


/**
 * Handle a "/tip-authorize" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_tip_authorize (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  struct TipAuthContext *tac;
  int res;
  struct MerchantInstance *mi;
  enum TALER_ErrorCode ec;
  struct GNUNET_TIME_Absolute expiration;
  struct GNUNET_HashCode tip_id;

  if (NULL == *connection_cls)
  {
    tac = GNUNET_new (struct TipAuthContext);
    tac->hc.cc = &cleanup_tac;
    tac->connection = connection;
    *connection_cls = tac;
  }
  else
  {
    tac = *connection_cls;
  }

  if (GNUNET_NO == tac->parsed_json)
  {
    struct GNUNET_JSON_Specification spec[] = {
      TALER_JSON_spec_amount ("amount", &tac->amount),
      GNUNET_JSON_spec_string ("instance", &tac->instance),
      GNUNET_JSON_spec_string ("justification", &tac->justification),
      GNUNET_JSON_spec_string ("pickup_url", &tac->pickup_url),
      GNUNET_JSON_spec_string ("next_url", &tac->next_url),
      GNUNET_JSON_spec_end()
    };

    res = TMH_PARSE_post_json (connection,
                               &tac->json_parse_context,
                               upload_data,
                               upload_data_size,
                               &tac->root);
    if (GNUNET_SYSERR == res)
      return MHD_NO;
    /* the POST's body has to be further fetched */
    if ( (GNUNET_NO == res) ||
         (NULL == tac->root) )
      return MHD_YES;

    if (NULL == json_object_get (tac->root, "pickup_url"))
    {
      char *pickup_url = TALER_url_absolute_mhd (connection,
                                                 "tip-pickup",
                                                 NULL);
      GNUNET_assert (NULL != pickup_url);
      json_object_set_new (tac->root, "pickup_url", json_string (pickup_url));
      GNUNET_free (pickup_url);
    }

    res = TMH_PARSE_json_data (connection,
                               tac->root,
                               spec);
    if (GNUNET_YES != res)
    {
      GNUNET_break_op (0);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
    tac->parsed_json = GNUNET_YES;
  }

  mi = TMH_lookup_instance (tac->instance);
  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured\n",
                tac->instance);
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TIP_AUTHORIZE_INSTANCE_UNKNOWN,
					 "unknown instance");
  }
  if (NULL == mi->tip_exchange)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured for tipping\n",
                tac->instance);
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TIP_AUTHORIZE_INSTANCE_DOES_NOT_TIP,
					 "exchange for tipping not configured for the instance");
  }
  tac->reserve_priv = mi->tip_reserve;
  ec = db->authorize_tip (db->cls,
                          tac->justification,
                          &tac->amount,
                          &mi->tip_reserve,
			  mi->tip_exchange,
                          &expiration,
                          &tip_id);
  /* If we have insufficient funds according to OUR database,
     check with exchange to see if the reserve has been topped up
     in the meantime (or if tips were not withdrawn yet). */
  if ( (TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS == ec) &&
       (GNUNET_NO == tac->checked_status) )
  {
    MHD_suspend_connection (connection);
    tac->checked_status = GNUNET_YES;
    tac->fo = TMH_EXCHANGES_find_exchange (mi->tip_exchange,
                                           NULL,
                                           &exchange_cont,
                                           tac);
    return MHD_YES;
  }

  /* handle irrecoverable errors */
  if (TALER_EC_NONE != ec)
  {
    unsigned int rc;

    switch (ec)
    {
    case TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS:
      rc = MHD_HTTP_PRECONDITION_FAILED;
      break;
    case TALER_EC_TIP_AUTHORIZE_RESERVE_EXPIRED:
      rc = MHD_HTTP_PRECONDITION_FAILED;
      break;
    case TALER_EC_TIP_AUTHORIZE_RESERVE_UNKNOWN:
      rc = MHD_HTTP_NOT_FOUND;
      break;
    default:
      rc = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    }
    return TMH_RESPONSE_reply_rc (connection,
                                  rc,
                                  ec,
                                  "Database error approving tip");
  }

  /* generate success response */
  {
    json_t *tip_token;
    char *tip_token_str;
    char *tip_redirect_url;

    tip_token = json_pack ("{s:o, s:o, s:o, s:s, s:s, s:s}",
                           "tip_id", GNUNET_JSON_from_data_auto (&tip_id),
                           "expiration", GNUNET_JSON_from_time_abs (expiration),
                           "amount", TALER_JSON_from_amount (&tac->amount),
                           "exchange_url", mi->tip_exchange,
                           "next_url", tac->next_url,
                           "pickup_url", tac->pickup_url);
    tip_token_str = json_dumps (tip_token, JSON_COMPACT);
    GNUNET_assert (NULL != tip_token_str);
    tip_redirect_url = TALER_url_absolute_mhd (connection, "public/trigger-pay",
                                               "tip_token", tip_token_str,
                                               NULL);
    GNUNET_assert (NULL != tip_redirect_url);
    /* FIXME:  This is pretty redundant, but we want to support some older
     * merchant implementations.  Newer ones should only get the
     * tip_redirect_url. */
    res = TMH_RESPONSE_reply_json_pack (connection,
                                        MHD_HTTP_OK,
                                        "{s:o, s:o, s:s, s:o, s:s}",
                                        "tip_id", GNUNET_JSON_from_data_auto (&tip_id),
                                        "expiration", GNUNET_JSON_from_time_abs (expiration),
                                        "exchange_url", mi->tip_exchange,
                                        "tip_token", tip_token,
                                        "tip_redirect_url", tip_redirect_url);
    GNUNET_free (tip_token_str);
    GNUNET_free (tip_redirect_url);
    return res;
  }
}

/* end of taler-merchant-httpd_tip-authorize.c */
