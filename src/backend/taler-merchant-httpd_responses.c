/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-merchant-httpd_responses.c
 * @brief API for generating the various replies of the exchange; these
 *        functions are called TMH_RESPONSE_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_responses.h"
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include <gnunet/gnunet_util_lib.h>


/**
 * Make JSON response object.
 *
 * @param json the json object
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_json (const json_t *json)
{
  struct MHD_Response *resp;
  char *json_str;

  json_str = json_dumps (json, JSON_INDENT(2));
  GNUNET_assert (NULL != json_str);
  resp = MHD_create_response_from_buffer (strlen (json_str), json_str,
                                          MHD_RESPMEM_MUST_FREE);
  if (NULL == resp)
  {
    free (json_str);
    GNUNET_break (0);
    return NULL;
  }
  (void) MHD_add_response_header (resp,
                                  MHD_HTTP_HEADER_CONTENT_TYPE,
                                  "application/json");
  return resp;
}


/**
 * Send JSON object as response.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json (struct MHD_Connection *connection,
                         const json_t *json,
                         unsigned int response_code)
{
  struct MHD_Response *resp;
  int ret;

  resp = TMH_RESPONSE_make_json (json);
  if (NULL == resp)
    return MHD_NO;
  ret = MHD_queue_response (connection,
                            response_code,
                            resp);
  MHD_destroy_response (resp);
  return ret;
}


/**
 * Make JSON response object.
 *
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_json_pack (const char *fmt,
                             ...)
{
  json_t *json;
  va_list argp;
  struct MHD_Response *ret;
  json_error_t jerror;

  va_start (argp, fmt);
  json = json_vpack_ex (&jerror, 0, fmt, argp);
  va_end (argp);
  if (NULL == json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to pack JSON with format `%s': %s\n",
                fmt,
                jerror.text);
    GNUNET_break (0);
    return MHD_NO;
  }
  ret = TMH_RESPONSE_make_json (json);
  json_decref (json);
  return ret;
}


/**
 * Function to call to handle the request by building a JSON
 * reply from a format string and varargs.
 *
 * @param connection the MHD connection to handle
 * @param response_code HTTP response code to use
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json_pack (struct MHD_Connection *connection,
                              unsigned int response_code,
                              const char *fmt,
                              ...)
{
  json_t *json;
  va_list argp;
  int ret;
  json_error_t jerror;

  va_start (argp, fmt);
  json = json_vpack_ex (&jerror, 0, fmt, argp);
  va_end (argp);
  if (NULL == json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to pack JSON with format `%s': %s\n",
                fmt,
                jerror.text);
    GNUNET_break (0);
    return MHD_NO;
  }
  ret = TMH_RESPONSE_reply_json (connection,
                                 json,
                                 response_code);
  json_decref (json);
  return ret;
}


/**
 * Create a response indicating an internal error.
 *
 * @param hint hint about the internal error's nature
 * @return a MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_internal_error (const char *hint)
{
  return TMH_RESPONSE_make_json_pack ("{s:s, s:s}",
                                      "error", "internal error",
                                      "hint", hint);
}


/**
 * Send a response indicating an internal error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the internal error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_internal_error (struct MHD_Connection *connection,
                                   const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       "{s:s, s:s}",
                                       "error", "internal error",
                                       "hint", hint);
}


/**
 * Send a response indicating that the request was too big.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_request_too_large (struct MHD_Connection *connection)
{
  struct MHD_Response *resp;
  int ret;

  resp = MHD_create_response_from_buffer (0,
                                          NULL,
                                          MHD_RESPMEM_PERSISTENT);
  if (NULL == resp)
    return MHD_NO;
  ret = MHD_queue_response (connection,
                            MHD_HTTP_REQUEST_ENTITY_TOO_LARGE,
                            resp);
  MHD_destroy_response (resp);
  return ret;
}


/**
 * Send a response indicating that the JSON was malformed.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_invalid_json (struct MHD_Connection *connection)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s}",
                                       "error",
                                       "invalid json");
}


/**
 * Send a response indicating that we did not find the @a object
 * needed for the reply.
 *
 * @param connection the MHD connection to use
 * @param object name of the object we did not find
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_not_found (struct MHD_Connection *connection,
                              const char *object)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       "{s:s}",
                                       "error",
                                       object);
}


/**
 * Send a response indicating that the request was malformed.
 *
 * @param connection the MHD connection to use
 * @param issue description of what was wrong with the request
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_bad_request (struct MHD_Connection *connection,
                                const char *issue)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s}",
                                       "error",
                                       issue);
}


/**
 * Add headers we want to return in every response.
 * Useful for testing, like if we want to always close
 * connections.
 *
 * @param response response to modify
 */
void
TMH_RESPONSE_add_global_headers (struct MHD_Response *response)
{
  if (TMH_merchant_connection_close)
    (void) MHD_add_response_header (response,
                                    MHD_HTTP_HEADER_CONNECTION,
                                    "close");
}


/**
 * Send a response indicating an external error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_external_error (struct MHD_Connection *connection,
                                   const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s, s:s}",
                                       "error", "client error",
                                       "hint", hint);
}


/**
 * Create a response indicating an external error.
 *
 * @param hint hint about the internal error's nature
 * @return a MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_external_error (const char *hint)
{
  return TMH_RESPONSE_make_json_pack ("{s:s, s:s}",
                                      "error", "client error",
                                      "hint", hint);
}

/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_missing (struct MHD_Connection *connection,
                                const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{ s:s, s:s}",
                                       "error", "missing parameter",
                                       "parameter", param_name);
}


/**
 * Send a response indicating an invalid argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_invalid (struct MHD_Connection *connection,
                                const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s, s:s}",
                                       "error", "invalid parameter",
                                       "parameter", param_name);
}


/**
 * Generate /track/transaction response.
 *
 * @param num_transfers how many wire transfers make up the transaction
 * @param transfers data on each wire transfer
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_track_transaction_ok (unsigned int num_transfers,
                                        const struct TALER_MERCHANT_TransactionWireTransfer *transfers)
{
  struct MHD_Response *ret;
  unsigned int i;
  json_t *j_transfers;

  j_transfers = json_array ();
  for (i=0;i<num_transfers;i++)
  {
    const struct TALER_MERCHANT_TransactionWireTransfer *transfer = &transfers[i];
    json_t *j_coins;
    unsigned int j;

    j_coins = json_array ();
    for (j=0;j<transfer->num_coins;j++)
    {
      const struct TALER_MERCHANT_CoinWireTransfer *coin = &transfer->coins[j];

      GNUNET_assert (0 ==
                     json_array_append_new (j_coins,
                                            json_pack ("{s:o, s:o, s:o}",
                                                       "coin_pub", GNUNET_JSON_from_data_auto (&coin->coin_pub),
                                                       "amount_with_fee", TALER_JSON_from_amount (&coin->amount_with_fee),
                                                       "deposit_fee", TALER_JSON_from_amount (&coin->deposit_fee))));
    }
    GNUNET_assert (0 ==
                   json_array_append_new (j_transfers,
                                          json_pack ("{s:o, s:o}",
                                                     "wtid", GNUNET_JSON_from_data_auto (&transfer->wtid),
                                                     "execution_time", GNUNET_JSON_from_time_abs (transfer->execution_time),
                                                     "coins", j_coins)));
  }
  ret = TMH_RESPONSE_make_json (j_transfers);
  json_decref (j_transfers);
  return ret;
}


/* end of taler-exchange-httpd_responses.c */
