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
 * @file backend/taler-merchant-httpd_tip-enable.c
 * @brief implement API for authorizing tips to be paid to visitors
 * @author Christian Grothoff
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_tip-enable.h"


/**
 * Information we keep for individual calls
 * to requests that parse JSON, but keep no other state.
 */
struct TMH_JsonParseContext
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
};


/**
 * Custom cleanup routine for a `struct TMH_JsonParseContext`.
 *
 * @param hc the `struct TMH_JsonParseContext` to clean up.
 */
static void
json_parse_cleanup (struct TM_HandlerContext *hc)
{
  struct TMH_JsonParseContext *jpc = (struct TMH_JsonParseContext *) hc;

  TMH_PARSE_post_cleanup_callback (jpc->json_parse_context);
  GNUNET_free (jpc);
}


/**
 * Handle a "/tip-enable" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_tip_enable (struct TMH_RequestHandler *rh,
                       struct MHD_Connection *connection,
                       void **connection_cls,
                       const char *upload_data,
                       size_t *upload_data_size)
{
  enum GNUNET_DB_QueryStatus qs;
  int res;
  struct TALER_Amount credit;
  struct GNUNET_TIME_Absolute expiration;
  struct TALER_ReservePrivateKeyP reserve_priv;
  struct GNUNET_HashCode credit_uuid;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("credit", &credit),
    GNUNET_JSON_spec_absolute_time ("expiration", &expiration),
    GNUNET_JSON_spec_fixed_auto ("reserve_priv", &reserve_priv),
    GNUNET_JSON_spec_fixed_auto ("credit_uuid", &credit_uuid),
    GNUNET_JSON_spec_end()
  };
  struct TMH_JsonParseContext *ctx;
  json_t *root;

  if (NULL == *connection_cls)
  {
    ctx = GNUNET_new (struct TMH_JsonParseContext);
    ctx->hc.cc = &json_parse_cleanup;
    *connection_cls = ctx;
  }
  else
  {
    ctx = *connection_cls;
  }
  res = TMH_PARSE_post_json (connection,
                             &ctx->json_parse_context,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ( (GNUNET_NO == res) ||
       (NULL == root) )
    return MHD_YES;

  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  json_decref (root);
  if (GNUNET_YES != res)
  {
    GNUNET_break_op (0);
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  }

  qs = db->enable_tip_reserve (db->cls,
                               &reserve_priv,
                               &credit_uuid,
                               &credit,
                               expiration);
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_TIP_ENABLE_DB_TRANSACTION_ERROR,
                                              "Database error approving tip");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_OK,
                                         "{s:s}",
                                         "status", "duplicate submission");
  }
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:s}",
                                       "status", "ok");
}

/* end of taler-merchant-httpd_tip-enable.c */
