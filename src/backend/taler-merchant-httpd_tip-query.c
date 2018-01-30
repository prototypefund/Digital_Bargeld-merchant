/*
  This file is part of TALER
  (C) 2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_tip-query.c
 * @brief implementation of /tip-query handler
 * @author Christian Grothoff
 * @author Florian Dold
 */
#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_tip-query.h"


/**
 * Manages a /tip-query call, checking if a tip authorization
 * exists and, if so, returning its details.
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
  const char *tip_id_str;
  struct GNUNET_HashCode tip_id;

  tip_id_str = MHD_lookup_connection_value (connection,
                                            MHD_GET_ARGUMENT_KIND,
                                            "tip_id");
  if (NULL == tip_id_str)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "tip_id");

  if (GNUNET_OK != GNUNET_STRINGS_string_to_data (tip_id_str,
                                                  strlen (tip_id_str), &tip_id,
                                                  sizeof (struct GNUNET_HashCode)))
    return TMH_RESPONSE_reply_arg_invalid (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "tip_id");

  enum GNUNET_DB_QueryStatus qs;
  struct TALER_Amount tip_amount;
  struct GNUNET_TIME_Absolute tip_timestamp;
  char *tip_exchange_url;

  qs = db->lookup_tip_by_id (db->cls,
                             &tip_id,
                             &tip_exchange_url,
                             &tip_amount,
                             &tip_timestamp);

  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    return TMH_RESPONSE_reply_rc (connection,
                                  MHD_HTTP_NOT_FOUND,
                                  TALER_EC_TIP_QUERY_TIP_ID_UNKNOWN,
                                  "tip id not found");
  }
  else if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
      return TMH_RESPONSE_reply_json_pack (connection,
                                           MHD_HTTP_OK,
                                           "{s:s, s:s}",
                                           "exchange", tip_exchange_url,
                                           "timestamp", GNUNET_JSON_from_time_abs (tip_timestamp),
                                           "amount", TALER_JSON_from_amount (&tip_amount));
  }
  return TMH_RESPONSE_reply_rc (connection,
                                MHD_HTTP_INTERNAL_SERVER_ERROR,
                                TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                "tip lookup failure");
}
