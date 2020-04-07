/*
  This file is part of TALER
  (C) 2014-2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_refund_increase.c
 * @brief Handle request to increase the refund for an order
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_refund.h"

/**
 * How often do we retry the non-trivial refund INSERT database
 * transaction?
 */
#define MAX_RETRIES 5


/**
 * Information we keep for individual calls
 * to requests that parse JSON, but keep no other state.
 */
struct TMH_JsonParseContext
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
};


/**
 * Make a taler://refund URI
 *
 * @param connection MHD connection to take host and path from
 * @param instance_id merchant's instance ID, must not be NULL
 * @param order_id order ID to show a refund for, must not be NULL
 * @returns the URI, must be freed with #GNUNET_free
 */
static char *
make_taler_refund_uri (struct MHD_Connection *connection,
                       const char *instance_id,
                       const char *order_id)
{
  const char *host;
  const char *forwarded_host;
  const char *uri_path;
  const char *uri_instance_id;
  const char *query;
  char *result;

  GNUNET_assert (NULL != instance_id);
  GNUNET_assert (NULL != order_id);
  host = MHD_lookup_connection_value (connection,
                                      MHD_HEADER_KIND,
                                      MHD_HTTP_HEADER_HOST);
  forwarded_host = MHD_lookup_connection_value (connection,
                                                MHD_HEADER_KIND,
                                                "X-Forwarded-Host");
  if (NULL != forwarded_host)
    host = forwarded_host;
  if (NULL == host)
  {
    /* Should never happen, at least the host header should be defined */
    GNUNET_break (0);
    return NULL;
  }
  uri_path = MHD_lookup_connection_value (connection,
                                          MHD_HEADER_KIND,
                                          "X-Forwarded-Prefix");
  if (NULL == uri_path)
    uri_path = "-";
  if (0 == strcmp (instance_id,
                   "default"))
    uri_instance_id = "-";
  else
    uri_instance_id = instance_id;
  if (GNUNET_YES == TALER_mhd_is_https (connection))
    query = "";
  else
    query = "?insecure=1";
  GNUNET_assert (0 < GNUNET_asprintf (&result,
                                      "taler://refund/%s/%s/%s/%s%s",
                                      host,
                                      uri_path,
                                      uri_instance_id,
                                      order_id,
                                      query));
  return result;
}


/**
 * Custom cleanup routine for a `struct TMH_JsonParseContext`.
 *
 * @param hc the `struct TMH_JsonParseContext` to clean up.
 */
static void
json_parse_cleanup (struct TM_HandlerContext *hc)
{
  struct TMH_JsonParseContext *jpc = (struct TMH_JsonParseContext *) hc;

  TALER_MHD_parse_post_cleanup_callback (jpc->json_parse_context);
  GNUNET_free (jpc);
}


/**
 * Process a refund request.
 *
 * @param connection HTTP client connection
 * @param mi merchant instance doing the processing
 * @param refund amount to be refunded
 * @param order_id for which order is the refund
 * @param reason reason for the refund
 * @return MHD result code
 */
static int
process_refund (struct MHD_Connection *connection,
                struct MerchantInstance *mi,
                const struct TALER_Amount *refund,
                const char *order_id,
                const char *reason)
{
  json_t *contract_terms;
  enum GNUNET_DB_QueryStatus qs;
  enum GNUNET_DB_QueryStatus qsx;
  struct GNUNET_HashCode h_contract_terms;

  db->preflight (db->cls);
  /* Convert order id to h_contract_terms */
  qs = db->find_contract_terms (db->cls,
                                &contract_terms,
                                order_id,
                                &mi->pubkey);
  if (0 > qs)
  {
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_REFUND_LOOKUP_DB_ERROR,
                                       "An error occurred while retrieving payment data from db");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Unknown order id given: `%s'\n",
                order_id);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_REFUND_ORDER_ID_UNKNOWN,
                                       "order_id not found in database");
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_break (0);
    json_decref (contract_terms);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_LOGIC_ERROR,
                                       "Could not hash contract terms");
  }
  json_decref (contract_terms);
  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    if (GNUNET_OK !=
        db->start (db->cls,
                   "increase refund"))
    {
      GNUNET_break (0);
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    qs = db->increase_refund_for_contract_NT (db->cls,
                                              &h_contract_terms,
                                              &mi->pubkey,
                                              refund,
                                              reason);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "increase refund returned %d\n",
                qs);
    if (GNUNET_DB_STATUS_HARD_ERROR == qs)
    {
      GNUNET_break (0);
      db->rollback (db->cls);
      break;
    }
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      db->rollback (db->cls);
      continue;
    }
    /* Got one or more deposits */
    if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
    {
      db->rollback (db->cls);
      break;
    }
    qsx = db->commit (db->cls);
    if (GNUNET_DB_STATUS_HARD_ERROR == qsx)
    {
      GNUNET_break (0);
      qs = qsx;
      break;
    }
    if (GNUNET_DB_STATUS_SOFT_ERROR != qsx)
      break;
  }
  if (0 > qs)
  {
    /* Special report if retries insufficient */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_REFUND_MERCHANT_DB_COMMIT_ERROR,
                                       "Internal database error or refund amount too big");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Refusing refund amount %s that is larger than original payment\n",
                TALER_amount2s (refund));
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_CONFLICT,
                                       TALER_EC_REFUND_INCONSISTENT_AMOUNT,
                                       "Amount above payment");
  }

  {
    int ret;
    char *taler_refund_uri;

    taler_refund_uri = make_taler_refund_uri (connection,
                                              mi->id,
                                              order_id);
    ret = TALER_MHD_reply_json_pack (
      connection,
      MHD_HTTP_OK,
      "{s:o, s:s}",
      "h_contract_terms",
      GNUNET_JSON_from_data_auto (&h_contract_terms),
      "taler_refund_url",
      taler_refund_uri);
    GNUNET_free (taler_refund_uri);
    return ret;
  }
}


/**
 * Handle request for increasing the refund associated with
 * a contract.
 *
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_refund_increase (struct TMH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            void **connection_cls,
                            const char *upload_data,
                            size_t *upload_data_size,
                            struct MerchantInstance *mi)
{
  int res;
  struct TMH_JsonParseContext *ctx;
  struct TALER_Amount refund;
  const char *order_id;
  const char *reason;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("refund", &refund),
    GNUNET_JSON_spec_string ("order_id", &order_id),
    GNUNET_JSON_spec_string ("reason", &reason),
    GNUNET_JSON_spec_end ()
  };
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

  res = TALER_MHD_parse_post_json (connection,
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

  res = TALER_MHD_parse_json_data (connection,
                                   root,
                                   spec);
  if (GNUNET_NO == res)
  {
    GNUNET_break_op (0);
    json_decref (root);
    return MHD_YES;
  }
  if (GNUNET_SYSERR == res)
  {
    GNUNET_break_op (0);
    json_decref (root);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_JSON_INVALID,
                                       "Request body does not match specification");
  }
  res = process_refund (connection,
                        mi,
                        &refund,
                        order_id,
                        reason);
  GNUNET_JSON_parse_free (spec);
  json_decref (root);
  return res;
}


/* end of taler-merchant-httpd_refund_increase.c */
