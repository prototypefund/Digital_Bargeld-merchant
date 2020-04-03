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
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_tip-authorize.h"
#include "taler-merchant-httpd_tip-reserve-helper.h"


struct TipAuthContext
{
  /**
   * This field MUST be first for handle_mhd_completion_callback() to work
   * when it treats this struct as a `struct TM_HandlerContext`.
   */
  struct TM_HandlerContext hc;

  /**
   * Placeholder for #TALER_MHD_parse_post_json() to keep its internal state.
   */
  void *json_parse_context;

  /**
   * Justification to use.
   */
  const char *justification;

  /**
   * JSON request received.
   */
  json_t *root;

  /**
   * Context for checking the tipping reserve's status.
   */
  struct CheckTipReserve ctr;

  /**
   * Tip amount requested.
   */
  struct TALER_Amount amount;

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
  TMH_check_tip_reserve_cleanup (&tac->ctr);
  TALER_MHD_parse_post_cleanup_callback (tac->json_parse_context);
  GNUNET_free (tac);
}


/**
 * Handle a "/tip-authorize" request.
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
MH_handler_tip_authorize (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size,
                          struct MerchantInstance *mi)
{
  struct TipAuthContext *tac;
  int res;
  enum TALER_ErrorCode ec;
  struct GNUNET_TIME_Absolute expiration;
  struct GNUNET_HashCode tip_id;
  json_t *extra;

  if (NULL == *connection_cls)
  {
    tac = GNUNET_new (struct TipAuthContext);
    tac->hc.cc = &cleanup_tac;
    tac->ctr.connection = connection;
    *connection_cls = tac;
  }
  else
  {
    tac = *connection_cls;
  }
  if (NULL != tac->ctr.response)
  {
    res = MHD_queue_response (connection,
                              tac->ctr.response_code,
                              tac->ctr.response);
    MHD_destroy_response (tac->ctr.response);
    tac->ctr.response = NULL;
    return res;
  }
  if (GNUNET_NO == tac->parsed_json)
  {
    struct GNUNET_JSON_Specification spec[] = {
      TALER_JSON_spec_amount ("amount", &tac->amount),
      GNUNET_JSON_spec_string ("justification", &tac->justification),
      GNUNET_JSON_spec_end ()
    };

    res = TALER_MHD_parse_post_json (connection,
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

    res = TALER_MHD_parse_json_data (connection,
                                     tac->root,
                                     spec);
    if (GNUNET_YES != res)
    {
      GNUNET_break_op (0);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
    tac->parsed_json = GNUNET_YES;
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
  tac->ctr.reserve_priv = mi->tip_reserve;
  extra = json_object_get (tac->root, "extra");
  if (NULL == extra)
    extra = json_object ();
  else
    json_incref (extra);


  db->preflight (db->cls);
  ec = db->authorize_tip_TR (db->cls,
                             tac->justification,
                             extra,
                             &tac->amount,
                             &mi->tip_reserve,
                             mi->tip_exchange,
                             &expiration,
                             &tip_id);
  json_decref (extra);
  /* If we have insufficient funds according to OUR database,
     check with exchange to see if the reserve has been topped up
     in the meantime (or if tips were not withdrawn yet). */
  if ( (TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS == ec) &&
       (GNUNET_NO == tac->checked_status) )
  {
    tac->checked_status = GNUNET_YES;
    tac->ctr.none_authorized = GNUNET_YES;
    TMH_check_tip_reserve (&tac->ctr,
                           mi->tip_exchange);
    return MHD_YES;
  }

  /* handle irrecoverable errors */
  if (TALER_EC_NONE != ec)
  {
    unsigned int rc;
    const char *msg;

    switch (ec)
    {
    case TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS:
      rc = MHD_HTTP_PRECONDITION_FAILED;
      msg = "Failed to approve tip: merchant has insufficient tipping funds";
      break;
    case TALER_EC_TIP_AUTHORIZE_RESERVE_EXPIRED:
      msg = "Failed to approve tip: merchant's tipping reserve expired";
      rc = MHD_HTTP_PRECONDITION_FAILED;
      break;
    case TALER_EC_TIP_AUTHORIZE_RESERVE_UNKNOWN:
      msg = "Failed to approve tip: merchant's tipping reserve does not exist";
      rc = MHD_HTTP_NOT_FOUND;
      break;
    default:
      rc = MHD_HTTP_INTERNAL_SERVER_ERROR;
      msg = "Failed to approve tip: internal server error";
      break;
    }

    return TALER_MHD_reply_with_error (connection,
                                       rc,
                                       ec,
                                       msg);
  }

  /* generate success response */
  {
    char *taler_tip_uri;
    const char *host;
    const char *forwarded_host;
    const char *uri_path;
    const char *uri_instance_id;
    struct GNUNET_CRYPTO_HashAsciiEncoded hash_enc;

    host = MHD_lookup_connection_value (connection, MHD_HEADER_KIND, "Host");
    forwarded_host = MHD_lookup_connection_value (connection, MHD_HEADER_KIND,
                                                  "X-Forwarded-Host");

    uri_path = MHD_lookup_connection_value (connection, MHD_HEADER_KIND,
                                            "X-Forwarded-Prefix");
    if (NULL == uri_path)
      uri_path = "-";

    if (NULL != forwarded_host)
      host = forwarded_host;

    if (NULL == host)
    {
      /* Should never happen, at last the host header should be defined */
      GNUNET_break (0);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                         "unable to identify backend host");
    }

    if (0 == strcmp (mi->id, "default"))
      uri_instance_id = "-";
    else
      uri_instance_id = mi->id;

    GNUNET_CRYPTO_hash_to_enc (&tip_id, &hash_enc);

    GNUNET_assert (0 < GNUNET_asprintf (&taler_tip_uri,
                                        "taler://tip/%s/%s/%s/%s",
                                        host,
                                        uri_path,
                                        uri_instance_id,
                                        hash_enc.encoding));


    res = TALER_MHD_reply_json_pack (connection,
                                     MHD_HTTP_OK,
                                     "{s:s, s:s}",
                                     "taler_tip_uri", taler_tip_uri,
                                     "tip_id", hash_enc.encoding);
    return res;
  }
}


/* end of taler-merchant-httpd_tip-authorize.c */
