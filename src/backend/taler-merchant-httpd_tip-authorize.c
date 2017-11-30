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
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_tip-authorize.h"


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
  struct MerchantInstance *mi;
  int res;
  struct TALER_Amount amount;
  const char *instance;
  const char *justification;
  const char *pickup_url;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("amount", &amount),
    GNUNET_JSON_spec_string ("instance", &instance),
    GNUNET_JSON_spec_string ("justification", &justification),
    GNUNET_JSON_spec_string ("pickup_url", &pickup_url),
    GNUNET_JSON_spec_end()
  };
  json_t *root;
  struct GNUNET_TIME_Absolute expiration;
  struct GNUNET_HashCode tip_id;
  struct TMH_JsonParseContext *ctx;
  enum TALER_ErrorCode ec;

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
  if (GNUNET_YES != res)
  {
    GNUNET_break_op (0);
    json_decref (root);
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  }

  mi = TMH_lookup_instance (instance);
  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured\n",
                instance);
    json_decref (root);  
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TIP_AUTHORIZE_INSTANCE_UNKNOWN,
					 "unknown instance");
  }
  if (NULL == mi->tip_exchange)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Instance `%s' not configured for tipping\n",
                instance);
    json_decref (root);
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TIP_AUTHORIZE_INSTANCE_DOES_NOT_TIP,
					 "exchange for tipping not configured for the instance");
  }
  ec = db->authorize_tip (db->cls,
                          justification,
                          &amount,
                          &mi->tip_reserve,
			  mi->tip_exchange,
                          &expiration,
                          &tip_id);
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
    case TALER_EC_TIP_AUTHORIZE_RESERVE_NOT_ENABLED:
      rc = MHD_HTTP_NOT_FOUND;
      break;
    default:
      rc = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    }
    json_decref (root);      
    return TMH_RESPONSE_reply_rc (connection,
				  rc,
				  ec,
				  "Database error approving tip");
  }
  if (0)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Insufficient funds to authorize tip over `%s' at instance `%s'\n",
                TALER_amount2s (&amount),
                instance);
    json_decref (root);
    return TMH_RESPONSE_reply_rc (connection,
                                  MHD_HTTP_PRECONDITION_FAILED,
                                  TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS,
                                  "Insufficient funds for tip");
  }
  json_t *tip_token = json_pack ("{s:o, s:o, s:s, s:s}"
                                 "tip_id", GNUNET_JSON_from_data_auto (&tip_id),
                                 "expiration", GNUNET_JSON_from_time_abs (expiration),
                                 "exchange_url", mi->tip_exchange,
                                 "pickup_url", pickup_url);
  char *tip_token_str = json_dumps (tip_token,  JSON_ENSURE_ASCII | JSON_COMPACT);
  json_decref (tip_token);
  json_decref (root);
  int ret = TMH_RESPONSE_reply_json_pack (connection,
                                          MHD_HTTP_OK,
                                          "{s:o, s:o, s:s, s:s}",
                                          "tip_id", GNUNET_JSON_from_data_auto (&tip_id),
                                          "expiration", GNUNET_JSON_from_time_abs (expiration),
                                          "exchange_url", mi->tip_exchange,
                                          "tip_token", tip_token_str);
  GNUNET_free (tip_token_str);
  return ret;
}

/* end of taler-merchant-httpd_tip-authorize.c */
