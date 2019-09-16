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
 * Closure for #pd_cb.
 */
struct ProcessContractClosure
{

  /**
   * Updated by #pd_cb to build the response.
   */
  json_t *response;

  /**
   * Set to #GNUNET_SYSERR if the database returned a contract
   * that was not well-formed.
   */
  int failure;

};


/**
 * Function called with information about a transaction.
 *
 * @param cls closure of type `struct ProcessContractClosure`
 * @param order_id transaction's order ID.
 * @param row_id serial numer of the transaction in the table,
 * used as index by the frontend to skip previous results.
 */
static void
pd_cb (void *cls,
       const char *order_id,
       uint64_t row_id,
       const json_t *contract_terms)
{
  struct ProcessContractClosure *pcc = cls;
  json_t *entry;
  json_t *amount;
  json_t *timestamp;
  json_t *instance;
  json_t *summary;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "/history's row_id: %llu\n",
              (unsigned long long) row_id);
  summary = NULL;
  if (-1 == json_unpack ((json_t *) contract_terms,
                         "{s:o, s:o, s:{s:o}, s?:o}",
                         "amount", &amount,
                         "timestamp", &timestamp,
                         "merchant", "instance", &instance,
                         "summary", &summary))
  {
    GNUNET_break (0);
    pcc->failure = GNUNET_SYSERR;
    return;
  }

  /* summary is optional, but we need something, so we use
     the order ID if it is not given. */
  if (NULL == summary)
    summary = json_string (order_id);

  if (NULL == (entry =
                 json_pack ("{s:I, s:s, s:O, s:O, s:O, s:O}",
                            "row_id", row_id,
                            "order_id", order_id,
                            "amount", amount,
                            "timestamp", timestamp,
                            "instance", instance,
                            "summary", summary)))
  {
    GNUNET_break (0);
    pcc->failure = GNUNET_SYSERR;
    return;
  }
  if (0 !=
      json_array_append_new (pcc->response,
                             entry))
  {
    GNUNET_break (0);
    pcc->failure = GNUNET_SYSERR;
    json_decref (entry);
    return;
  }
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
  int ret;
  unsigned int ascending = GNUNET_NO;
  unsigned long long seconds;
  struct MerchantInstance *mi;
  unsigned long long start = INT64_MAX;
  long long delta = -20;
  enum GNUNET_DB_QueryStatus qs;
  struct ProcessContractClosure pcc;

  LOG_INFO ("Serving /history\n");
  response = json_array ();
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "date");
  date = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&date);
  if (NULL != str)
  {
    if (1 != sscanf (str,
                     "%llu",
                     &seconds))
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

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "instance");
  mi = TMH_lookup_instance (NULL != str ? str : "default");

  if (NULL == mi)
  {
    json_decref (response);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_HISTORY_INSTANCE_UNKNOWN,
                                         "instance");
  }

  /* Sanity check that we don't have some odd stale transaction running */
  db->preflight (db->cls);

  /* Here goes the cherry-picking logic */
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "order_id");
  if (NULL != str)
  {
    pcc.response = response;
    pcc.failure = GNUNET_NO;
    qs = db->find_contract_terms_history (db->cls,
                                          str,
                                          &mi->pubkey,
                                          &pd_cb,
                                          &pcc);
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    if ( (0 > qs) ||
         (GNUNET_SYSERR == pcc.failure) )
    {
      json_decref (response);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_HISTORY_DB_FETCH_ERROR,
                                                "db error to get history");
    }
    ret = TMH_RESPONSE_reply_json (connection,
                                   response,
                                   MHD_HTTP_OK);
    json_decref (response);
    return ret;
  }

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "start");
  if (NULL != str)
  {
    TALER_LOG_DEBUG ("'start' argument given ('%s')\n",
                     str);
    if (1 != sscanf (str,
                     "%llu",
                     &start))
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
    if (1 != sscanf (str,
                     "%lld",
                     &delta))
      return TMH_RESPONSE_reply_arg_invalid (connection,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "delta");
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Querying history back to %s, start: %llu, delta: %lld\n",
              GNUNET_STRINGS_absolute_time_to_string (date),
              start,
              delta);

  pcc.response = response;
  pcc.failure = GNUNET_NO;

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "ordering");
  if ( (NULL != str) &&
       (0 == strcmp ("ascending",
                     str)) )
    ascending = GNUNET_YES;

  qs = db->find_contract_terms_by_date_and_range (db->cls,
                                                  date,
                                                  &mi->pubkey,
                                                  start,
                                                  llabs (delta),
                                                  (delta < 0) ? GNUNET_YES :
                                                  GNUNET_NO,
                                                  ascending,
                                                  &pd_cb,
                                                  &pcc);
  if ( (0 > qs) ||
       (GNUNET_SYSERR == pcc.failure) )
  {
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    json_decref (response);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_HISTORY_DB_FETCH_ERROR,
                                              "db error to get history");
  }
  ret = TMH_RESPONSE_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{ s:o }",
                                      "history",
                                      response /* consumes 'response' */);
  LOG_INFO ("/history, http code: %d\n",
            MHD_HTTP_OK);
  return ret;
}

/* end of taler-merchant-httpd_history.c */
