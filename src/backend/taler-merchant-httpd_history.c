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
 * Index to the first row to return in response to /history.
 */
static int start = -1;

/**
 * How many rows we are to return in response to /history.
 */
static unsigned int delta;

/**
 * Function called with information about a transaction.
 *
 * @param cls closure
 * @param order_id transaction's order ID.
 * @param row_id serial numer of the transaction in the table,
 * used as index by the frontend to skip previous results.
 */
static void
pd_cb (void *cls,
       const char *order_id,
       uint64_t row_id,
       const json_t *proposal_data)
{
  json_t *response = cls;
  json_t *entry;
  json_t *amount;
  json_t *timestamp;
  json_t *instance;

  GNUNET_assert (-1 != json_unpack ((json_t *) proposal_data,
                                    "{s:o, s:o, s:{s:o}}",
                                    "amount", &amount,
                                    "timestamp", &timestamp,
                                    "merchant", "instance", &instance));

  GNUNET_break (NULL != (entry = json_pack ("{s:I, s:s, s:O, s:O, s:O}",
                                            "row_id", row_id,
                                            "order_id", order_id,
                                            "amount", amount,
                                            "timestamp", timestamp,
                                            "instance", instance)));

  GNUNET_break (0 == json_array_append_new (response,
                                            entry));
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
  struct MerchantInstance *mi;

  response = json_array (); /*FIXME who decrefs this?*/
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "date");

  date = GNUNET_TIME_absolute_get ();

  if (NULL != str)
  {
    if (1 != sscanf (str, "%llu", &seconds))
    {
      json_decref (response);
      return TMH_RESPONSE_reply_arg_invalid (connection,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "date");
    }

  date.abs_value_us = seconds * 1000LL * 1000LL;

  if (date.abs_value_us / 1000LL / 1000LL != seconds)
  {
    json_decref (response);
    return TMH_RESPONSE_reply_bad_request (connection,
                                           TALER_EC_HISTORY_TIMESTAMP_OVERFLOW,
                                           "Timestamp overflowed");
  }


  }

  mi = TMH_lookup_instance ("default");
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "instance");
  if (NULL != str)
    mi = TMH_lookup_instance (str);

  if (NULL == mi)
  {
    json_decref (response);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_HISTORY_INSTANCE_UNKNOWN,
                                         "instance");
  }

  delta = 20;

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "start");
  if (NULL != str)
  {
    if ((1 != sscanf (str, "%d", &start)) ||
        start < 0)
    {
      json_decref (response);
      return TMH_RESPONSE_reply_arg_invalid (connection,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "start");
    }
  }

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "delta");

  if (NULL != str)
  {
    if ((1 != sscanf (str, "%d", &delta)) ||
        delta < 0)
      return TMH_RESPONSE_reply_arg_invalid (connection,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "delta");
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Querying history back to %s\n",
              GNUNET_STRINGS_absolute_time_to_string (date));

  if (0 > start)
    ret = db->find_proposal_data_by_date (db->cls,
                                          date,
                                          &mi->pubkey,
                                          delta,
                                          pd_cb,
                                          response);
  else
    ret = db->find_proposal_data_by_date_and_range (db->cls,
                                                    date,
                                                    &mi->pubkey,
                                                    start,
                                                    delta,
                                                    pd_cb,
                                                    response);
  if (GNUNET_SYSERR == ret)
  {
    json_decref (response);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_HISTORY_DB_FETCH_ERROR,
					      "db error to get history");
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "history data: %s\n",
              json_dumps (response, JSON_INDENT (1)));

  ret = TMH_RESPONSE_reply_json (connection,
                                 response,
                                 MHD_HTTP_OK);
  json_decref (response);
  return ret;
}

/* end of taler-merchant-httpd_history.c */
