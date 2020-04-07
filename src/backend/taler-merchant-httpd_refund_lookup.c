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
 * @file backend/taler-merchant-httpd_refund_lookup.c
 * @brief refund handling logic
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_refund.h"


/**
 * Return refund situation about a contract.
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
MH_handler_refund_lookup (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size,
                          struct MerchantInstance *mi)
{
  const char *order_id;
  struct GNUNET_HashCode h_contract_terms;
  json_t *contract_terms;
  enum GNUNET_DB_QueryStatus qs;

  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
  {
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MISSING,
                                       "order_id");
  }

  /* Convert order id to h_contract_terms */
  contract_terms = NULL;
  db->preflight (db->cls);
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
                                       "database error looking up order_id from merchant_contract_terms table");
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

  {
    json_t *response;
    enum TALER_ErrorCode ec;
    const char *errmsg;

    response = TM_get_refund_json (mi,
                                   &h_contract_terms,
                                   &ec,
                                   &errmsg);
    if (NULL == response)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         ec,
                                         errmsg);
    return TALER_MHD_reply_json_pack (connection, MHD_HTTP_OK,
                                      "{s:o, s:o, s:o}",
                                      "refund_permissions",
                                      response,
                                      "merchant_pub",
                                      GNUNET_JSON_from_data_auto (
                                        &mi->pubkey),
                                      "h_contract_terms",
                                      GNUNET_JSON_from_data_auto (
                                        &h_contract_terms));
  }
}


/* end of taler-merchant-httpd_refund_lookup.c */
