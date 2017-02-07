/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

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
 * @file backend/taler-merchant-httpd_history.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_responses.h"

/**
 * Function called with information about a transaction.
 *
 * @param cls closure
 * @param merchant_pub merchant's public key
 * @param exchange_uri URI of the exchange
 * @param transaction_id proposal's transaction id
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */

static void
history_cb (void *cls,
	    const struct TALER_MerchantPublicKeyP *merchant_pub,
            const char *exchange_uri,
            const struct GNUNET_HashCode *h_proposal_data,
            const struct GNUNET_HashCode *h_wire,
            struct GNUNET_TIME_Absolute timestamp,
            struct GNUNET_TIME_Absolute refund,
            const struct TALER_Amount *total_amount)
{
  json_t *response = cls;
  json_t *entry;

  GNUNET_break (NULL !=
               (entry = json_pack ("{s:o, s:s, s:s, s:o}",
                                   "h_proposal_data", GNUNET_JSON_from_data_auto (h_proposal_data),
                                   "exchange", exchange_uri,
                                   "timestamp", GNUNET_JSON_from_time_abs (timestamp),
                                   "total_amount",
                                   TALER_JSON_from_amount (total_amount))));
  GNUNET_break (0 == json_array_append (response, entry));
}

/**
 * Manage a /history request. Query the db and returns transactions
 * younger than the date given as parameter
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_history (struct TMH_RequestHandler *rh,
                    struct MHD_Connection *connection,
                    void **connection_cls,
                    const char *upload_data,
                    size_t *upload_data_size)
{
  #define LOG_INFO(...) GNUNET_log (GNUNET_ERROR_TYPE_INFO, __VA_ARGS__)
  const char *str;
  struct GNUNET_TIME_Absolute date;
  json_t *response;
  unsigned int ret;
  unsigned long long seconds;
  
  response = json_array (); /*FIXME who decrefs this?*/
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "date");

  if (NULL == str)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "date");

  if (1 != sscanf (str, "%llu", &seconds))
    return TMH_RESPONSE_reply_arg_invalid (connection,
					   TALER_EC_PARAMETER_MALFORMED,
                                           "date");
  date.abs_value_us = seconds * 1000LL * 1000LL;
  if (date.abs_value_us / 1000LL / 1000LL != seconds)
    return TMH_RESPONSE_reply_bad_request (connection,
					   TALER_EC_HISTORY_TIMESTAMP_OVERFLOW,
                                           "Timestamp overflowed");
  ret = db->find_transactions_by_date (db->cls,
                                       date,
                                       history_cb,
                                       response);
  if (GNUNET_SYSERR == ret)
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_HISTORY_DB_FETCH_ERROR,
					      "db error to get history");
  return TMH_RESPONSE_reply_json (connection,
                                  response,
                                  MHD_HTTP_OK);
}

/* end of taler-merchant-httpd_history.c */
