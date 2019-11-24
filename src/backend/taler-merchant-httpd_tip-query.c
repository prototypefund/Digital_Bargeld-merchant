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
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_tip-query.h"
#include "taler-merchant-httpd_tip-reserve-helper.h"


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
   * Merchant instance to use.
   */
  const char *instance;

  /**
   * GNUNET_YES if the tip query has already been processed
   * and we can queue the response.
   */
  int processed;

  /**
   * Context for checking the tipping reserve's status.
   */
  struct CheckTipReserve ctr;

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

  TMH_check_tip_reserve_cleanup (&tqc->ctr);
  GNUNET_free (tqc);
}


/**
 * We've been resumed after processing the reserve data from the
 * exchange without error. Generate the final response.
 *
 * @param tqc context for which to generate the response.
 */
static int
generate_final_response (struct TipQueryContext *tqc)
{
  struct GNUNET_CRYPTO_EddsaPublicKey reserve_pub;
  struct TALER_Amount amount_available;

  GNUNET_CRYPTO_eddsa_key_get_public (&tqc->ctr.reserve_priv.eddsa_priv,
                                      &reserve_pub);
  if (GNUNET_SYSERR ==
      TALER_amount_subtract (&amount_available,
                             &tqc->ctr.amount_deposited,
                             &tqc->ctr.amount_withdrawn))
  {
    GNUNET_break_op (0);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "amount overflow, deposited %s but withdrawn %s\n",
                TALER_amount_to_string (&tqc->ctr.amount_deposited),
                TALER_amount_to_string (&tqc->ctr.amount_withdrawn));
    return TALER_MHD_reply_with_error (tqc->ctr.connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_TIP_QUERY_RESERVE_HISTORY_ARITHMETIC_ISSUE_INCONSISTENT,
                                       "Exchange returned invalid reserve history (amount overflow)");
  }
  return TALER_MHD_reply_json_pack (tqc->ctr.connection,
                                    MHD_HTTP_OK,
                                    "{s:o, s:o, s:o, s:o, s:o}",
                                    "reserve_pub",
                                    GNUNET_JSON_from_data_auto (
                                      &reserve_pub),
                                    "reserve_expiration",
                                    GNUNET_JSON_from_time_abs (
                                      tqc->ctr.reserve_expiration),
                                    "amount_authorized",
                                    TALER_JSON_from_amount (
                                      &tqc->ctr.amount_authorized),
                                    "amount_picked_up",
                                    TALER_JSON_from_amount (
                                      &tqc->ctr.amount_withdrawn),
                                    "amount_available",
                                    TALER_JSON_from_amount (
                                      &amount_available));
}


/**
 * Handle a "/tip-query" request.
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
MH_handler_tip_query (struct TMH_RequestHandler *rh,
                      struct MHD_Connection *connection,
                      void **connection_cls,
                      const char *upload_data,
                      size_t *upload_data_size,
                      struct MerchantInstance *mi)
{
  struct TipQueryContext *tqc;

  if (NULL == *connection_cls)
  {
    tqc = GNUNET_new (struct TipQueryContext);
    tqc->hc.cc = &cleanup_tqc;
    tqc->ctr.connection = connection;
    *connection_cls = tqc;
  }
  else
  {
    tqc = *connection_cls;
  }

  if (0 != tqc->ctr.response_code)
  {
    int res;

    /* We are *done* processing the request, just queue the response (!) */
    if (UINT_MAX == tqc->ctr.response_code)
    {
      GNUNET_break (0);
      return MHD_NO; /* hard error */
    }
    res = MHD_queue_response (connection,
                              tqc->ctr.response_code,
                              tqc->ctr.response);
    MHD_destroy_response (tqc->ctr.response);
    tqc->ctr.response = NULL;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Queueing response (%u) for /tip-query (%s).\n",
                (unsigned int) tqc->ctr.response_code,
                res ? "OK" : "FAILED");
    return res;
  }

  if (GNUNET_YES == tqc->processed)
  {
    /* We've been here before, so TMH_check_tip_reserve() must have
       finished and left the result for us. Finish processing. */
    return generate_final_response (tqc);
  }

  if (NULL == mi->tip_exchange)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured for tipping\n",
                mi->id);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_TIP_AUTHORIZE_INSTANCE_DOES_NOT_TIP,
                                       "exchange for tipping not configured for the instance");
  }
  tqc->ctr.reserve_priv = mi->tip_reserve;

  {
    int qs;
    for (unsigned int i = 0; i<MAX_RETRIES; i++)
    {
      db->preflight (db->cls);
      qs = db->get_authorized_tip_amount (db->cls,
                                          &tqc->ctr.reserve_priv,
                                          &tqc->ctr.amount_authorized);
      if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
        break;
    }
    if (0 > qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Database hard error on get_authorized_tip_amount\n");
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_TIP_QUERY_DB_ERROR,
                                         "Merchant database error");
    }
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    {
      /* we'll set amount_authorized to zero later once
         we know the currency */
      tqc->ctr.none_authorized = GNUNET_YES;
    }
  }

  tqc->processed = GNUNET_YES;
  TMH_check_tip_reserve (&tqc->ctr,
                         mi->tip_exchange);
  return MHD_YES;
}


/* end of taler-merchant-httpd_tip-query.c */
