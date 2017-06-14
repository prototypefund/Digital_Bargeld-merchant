/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2017 INRIA

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
 * @file backend/taler-merchant-httpd_refund.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_responses.h"


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


extern struct MerchantInstance *
get_instance (struct json_t *json);


/**
 * Handle request for increasing the refund associated with
 * a contract.
 *
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_refund_increase (struct TMH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            void **connection_cls,
                            const char *upload_data,
                            size_t *upload_data_size)
{
  int res;
  struct TMH_JsonParseContext *ctx;
  struct TALER_Amount refund;
  json_t *root;
  json_t *contract_terms;
  const char *order_id;
  const char *reason;
  struct MerchantInstance *mi;
  struct GNUNET_HashCode h_contract_terms;

  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("refund", &refund),
    GNUNET_JSON_spec_string ("order_id", &order_id),
    GNUNET_JSON_spec_string ("reason", &reason),
    GNUNET_JSON_spec_end
  }; 


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
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES;

  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  if (GNUNET_NO == res)
  {
    GNUNET_break_op (0);
    return MHD_YES;
  }

  if (GNUNET_SYSERR == res)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Hard error from JSON parser\n");
    return MHD_NO;
  }

  mi = get_instance (root);
  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No instance found\n");
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_REFUND_INSTANCE_UNKNOWN,
                                        "Unknown instance given");
  }
  
  /* Convert order id to h_contract_terms */
  if (GNUNET_OK != db->find_contract_terms (db->cls,
                                            &contract_terms,
                                            order_id,
                                            &mi->pubkey))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, 
                "Unknown order id given: %s\n",
                order_id);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_REFUND_ORDER_ID_UNKNOWN,
                                         "Order id not found in database");
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_JSON_parse_free (spec);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not hash contract terms\n");
    /**
     * Do we really need a error code for failing to hash something?
     * The HTTP 500 Internal server error sufficies for now.
     */
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_NONE,
                                              "Could not hash contract terms");
  }

  res = db->increase_refund_for_contract (db->cls,
                                          &h_contract_terms,
                                          &mi->pubkey,
                                          &refund,
                                          reason);
  if (GNUNET_NO == res)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Inconsistent refund amount: %s\n",
                TALER_amount_to_string (&refund));
    GNUNET_JSON_parse_free (spec);
    /**
     * FIXME: should the db function distinguish between a refund amount
     * lower than the previous one and a one which is too big to be paid back?
     */
    return TMH_RESPONSE_reply_external_error (connection,
                                              TALER_EC_REFUND_INCONSISTENT_AMOUNT,
                                              "Amount either lower than the previous"
                                              " or too big to be paid back");
  }

  /**
   * FIXME: return to the frontend.  The frontend will then return
   * a "402 Payment required" carrying a "X-Taler-Refund-Url: www"
   * where 'www' is the URL where the wallet can automatically fetch
   * the refund permission.
   *
   * Just a "200 OK" should be fine here, as the frontend has all
   * the information needed to generate the right response.
   */
  return MHD_YES;

  json_decref (contract_terms);
  json_decref (root);
  GNUNET_JSON_parse_free (spec);
  return res;
}


/**
 * Return refund situation about a contract.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_refund_lookup (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  const char *order_id;
  const char *instance;
  int res;
  struct MerchantInstance *mi;

  instance = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "instance");
  if (NULL == instance)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "instance");

  mi = TMH_lookup_instance (instance);
  GNUNET_assert (NULL != mi);

  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "order_id");
  /* FIXME: TBD */

  /* return res; */
}


/* end of taler-merchant-httpd_refund.c */
