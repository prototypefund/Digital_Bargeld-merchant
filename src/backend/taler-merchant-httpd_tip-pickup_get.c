/*
  This file is part of TALER
  (C) 2017-2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_tip-pickup.c
 * @brief implementation of /tip-pickup handler
 * @author Christian Grothoff
 */
#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_tip-pickup.h"


/**
 * Manages a GET /tip-pickup call, checking that the tip is authorized,
 * and if so, returning the withdrawal permissions.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
MHD_RESULT
MH_handler_tip_pickup_get (struct TMH_RequestHandler *rh,
                           struct MHD_Connection *connection,
                           void **connection_cls,
                           const char *upload_data,
                           size_t *upload_data_size,
                           struct MerchantInstance *mi)
{
  const char *tip_id_str;
  char *exchange_url;
  json_t *extra;
  struct GNUNET_HashCode tip_id;
  struct TALER_Amount tip_amount;
  struct TALER_Amount tip_amount_left;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute timestamp_expire;
  MHD_RESULT ret;
  enum GNUNET_DB_QueryStatus qs;

  tip_id_str = MHD_lookup_connection_value (connection,
                                            MHD_GET_ARGUMENT_KIND,
                                            "tip_id");

  if (NULL == tip_id_str)
  {
    /* tip_id is required but missing */
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MISSING,
                                       "tip_id required");
  }

  if (GNUNET_OK !=
      GNUNET_CRYPTO_hash_from_string (tip_id_str,
                                      &tip_id))
  {
    /* tip_id has wrong encoding */
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "tip_id malformed");
  }

  db->preflight (db->cls);
  qs = db->lookup_tip_by_id (db->cls,
                             &tip_id,
                             &exchange_url,
                             &extra,
                             &tip_amount,
                             &tip_amount_left,
                             &timestamp);

  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    unsigned int response_code;
    enum TALER_ErrorCode ec;

    switch (qs)
    {
    case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
      ec = TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN;
      response_code = MHD_HTTP_NOT_FOUND;
      break;
    case GNUNET_DB_STATUS_SOFT_ERROR:
      ec = TALER_EC_TIP_PICKUP_DB_ERROR_SOFT;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    case GNUNET_DB_STATUS_HARD_ERROR:
      ec = TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    default:
      GNUNET_break (0);
      ec = TALER_EC_INTERNAL_LOGIC_ERROR;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    }
    return TALER_MHD_reply_with_error (connection,
                                       response_code,
                                       ec,
                                       "Could not determine exchange URL for the given tip id");
  }

  timestamp_expire = GNUNET_TIME_absolute_add (timestamp,
                                               GNUNET_TIME_UNIT_DAYS);

  ret = TALER_MHD_reply_json_pack (
    connection,
    MHD_HTTP_OK,
    "{s:s, s:o, s:o, s:o, s:o, s:o}",
    "exchange_url", exchange_url,
    "amount", TALER_JSON_from_amount (&tip_amount),
    "amount_left", TALER_JSON_from_amount (&tip_amount_left),
    "stamp_created", GNUNET_JSON_from_time_abs (timestamp),
    "stamp_expire", GNUNET_JSON_from_time_abs (timestamp_expire),
    "extra", extra);

  GNUNET_free (exchange_url);
  json_decref (extra);
  return ret;
}
