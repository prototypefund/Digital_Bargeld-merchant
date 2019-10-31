/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file lib/testing_api_cmd_history.c
 * @brief command to test the /history API.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "history" CMD.
 */
struct HistoryState
{

  /**
   * Expected status code.
   */
  unsigned int http_status;

  /**
   * URL of the merchant backend serving the /history request.
   */
  const char *merchant_url;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Handle to the /history operation.
   */
  struct TALER_MERCHANT_HistoryOperation *ho;

  /**
   * Only history entries younger than this
   * value will be returned.
   */
  struct GNUNET_TIME_Absolute time;

  /**
   * First row index we want in the results.
   */
  unsigned long long start;


  /**
   * When this flag is GNUNET_YES, then the interpreter
   * will request /history *omitting* the 'start' URL argument.
   */
  int use_default_start;

  /**
   * How many rows we want the response to contain, at most.
   */
  long long nrows;

  /**
   * Expected number of history entries returned by the
   * backend.
   */
  unsigned int nresult;
};

/**
 * Parse given JSON object to absolute time.
 *
 * @param root the json object representing data
 * @param[out] ret where to write the data
 * @return #GNUNET_OK upon successful parsing;
 *         #GNUNET_SYSERR upon error
 */
static int
parse_abs_time (json_t *root,
                struct GNUNET_TIME_Absolute *ret)
{
  const char *val;
  unsigned long long int tval;

  val = json_string_value (root);
  if (NULL == val)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  if ( (0 == strcasecmp (val,
                         "/forever/")) ||
       (0 == strcasecmp (val,
                         "/end of time/")) ||
       (0 == strcasecmp (val,
                         "/never/")) )
  {
    *ret = GNUNET_TIME_UNIT_FOREVER_ABS;
    return GNUNET_OK;
  }
  if (1 != sscanf (val,
                   "/Date(%llu)/",
                   &tval))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  /* Time is in seconds in JSON, but in microseconds in
   * GNUNET_TIME_Absolute */
  ret->abs_value_us = tval * 1000LL * 1000LL;
  if ( (ret->abs_value_us) / 1000LL / 1000LL != tval)
  {
    /* Integer overflow */
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Callback for a /history request; checks that (1) HTTP status
 * is expected, the number of rows returned is expected, and that
 * the rows are sorted from the youngest to the oldest record.
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant
 *        backend
 * @param ec taler-specific error code
 * @param json actual body containing the history
 */
static void
history_cb (void *cls,
            unsigned int http_status,
            enum TALER_ErrorCode ec,
            const json_t *json)
{
  struct HistoryState *hs = cls;
  unsigned int nresult;
  struct GNUNET_TIME_Absolute last_timestamp;
  struct GNUNET_TIME_Absolute entry_timestamp;
  json_t *arr;

  hs->ho = NULL;

  if (hs->http_status != http_status)
    TALER_TESTING_FAIL (hs->is);

  if (0 == hs->http_status)
  {
    /* 0 was caused intentionally by the tests,
     * move on without further checking. */
    TALER_TESTING_interpreter_next (hs->is);
    return;
  }

  arr = json_object_get (json, "history");
  nresult = json_array_size (arr);
  if (hs->nresult != nresult)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected number of history entries."
                " Got %d, expected %d\n",
                nresult,
                hs->nresult);
    TALER_TESTING_FAIL (hs->is);
  }

  last_timestamp = GNUNET_TIME_absolute_get ();
  last_timestamp = GNUNET_TIME_absolute_add (last_timestamp,
                                             GNUNET_TIME_UNIT_DAYS);
  {
    json_t *entry;
    size_t index;
    json_array_foreach (arr, index, entry)
    {
      json_t *timestamp;

      timestamp = json_object_get (entry, "timestamp");
      if (GNUNET_OK != parse_abs_time (timestamp, &entry_timestamp))
        TALER_TESTING_FAIL (hs->is);

      if (last_timestamp.abs_value_us < entry_timestamp.abs_value_us)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "History entries are NOT"
                    " sorted from younger to older\n");
        TALER_TESTING_interpreter_fail (hs->is);
        return;
      }
      last_timestamp = entry_timestamp;
    }
  }
  TALER_TESTING_interpreter_next (hs->is);
}


/**
 * Free the state for a "history" CMD, and possibly cancel
 * any pending operation thereof.
 *
 * @param cls closure
 * @param cmd command being freed now.
 */
static void
history_cleanup (void *cls,
                 const struct TALER_TESTING_Command *cmd)
{
  struct HistoryState *hs = cls;

  if (NULL != hs->ho)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "/history operation did not complete\n");
    TALER_MERCHANT_history_cancel (hs->ho);
  }
  GNUNET_free (hs);
}


/**
 * Run a "history" CMD.
 *
 * @param cls closure.
 * @param cmd current command.
 * @param is interpreter state.
 */
static void
history_run (void *cls,
             const struct TALER_TESTING_Command *cmd,
             struct TALER_TESTING_Interpreter *is)
{
  struct HistoryState *hs = cls;

  hs->is = is;
  if (0 == hs->time.abs_value_us)
  {
    hs->time = GNUNET_TIME_absolute_add
                 (GNUNET_TIME_absolute_get (),
                 GNUNET_TIME_UNIT_HOURS);
    GNUNET_TIME_round_abs (&hs->time);
  }

  switch (hs->use_default_start)
  {
  case GNUNET_YES:
    hs->ho = TALER_MERCHANT_history_default_start
               (is->ctx,
               hs->merchant_url,
               hs->nrows,
               hs->time,
               &history_cb,
               hs);
    break;

  case GNUNET_NO:
    hs->ho = TALER_MERCHANT_history (is->ctx,
                                     hs->merchant_url,
                                     hs->start,
                                     hs->nrows,
                                     hs->time,
                                     &history_cb,
                                     hs);
    break;
  default:
    TALER_LOG_ERROR ("Bad value for 'use_default_start'\n");
    TALER_TESTING_FAIL (is);
  }

  if (NULL == hs->ho)
    TALER_TESTING_FAIL (is);
}


/**
 * Make a "history" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        request.
 * @param ctx CURL context.
 * @param http_status expected HTTP response code
 * @param time limit towards the past for the history
 *        records we want returned.
 * @param nresult how many results are expected
 * @param start first row id we want in the result.
 * @param use_default_start if GNUNET_YES, then it will
 *        use the API call that requests /history omitting
 *        the 'start' argument.  This makes easier to test
 *        the server default behaviour.
 * @param nrows how many row we want to receive, at most.
 */
static struct TALER_TESTING_Command
cmd_history2 (const char *label,
              const char *merchant_url,
              unsigned int http_status,
              struct GNUNET_TIME_Absolute time,
              unsigned int nresult,
              unsigned long long start,
              int use_default_start,
              long long nrows)
{
  struct HistoryState *hs;

  hs = GNUNET_new (struct HistoryState);
  hs->http_status = http_status;
  hs->time = time;
  hs->nresult = nresult;
  hs->start = start;
  hs->nrows = nrows;
  hs->merchant_url = merchant_url;
  hs->use_default_start = use_default_start;

  struct TALER_TESTING_Command cmd = {
    .cls = hs,
    .label = label,
    .run = &history_run,
    .cleanup = &history_cleanup
  };

  return cmd;
}


/**
 * Make a "history" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        request.
 * @param ctx CURL context.
 * @param http_status expected HTTP response code
 * @param time limit towards the past for the history
 *        records we want returned.
 * @param nresult how many results are expected
 * @param nrows how many row we want to receive, at most.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_history_default_start
  (const char *label,
  const char *merchant_url,
  unsigned int http_status,
  struct GNUNET_TIME_Absolute time,
  unsigned int nresult,
  long long nrows)
{
  return cmd_history2 (label,
                       merchant_url,
                       http_status,
                       time,
                       nresult,
                       -1, /* ignored */
                       GNUNET_YES,
                       nrows);
}


/**
 * Make a "history" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        request.
 * @param ctx CURL context.
 * @param http_status expected HTTP response code
 * @param time limit towards the past for the history
 *        records we want returned.
 * @param nresult how many results are expected
 * @param start first row id we want in the result.
 * @param nrows how many row we want to receive, at most.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_history (const char *label,
                           const char *merchant_url,
                           unsigned int http_status,
                           struct GNUNET_TIME_Absolute time,
                           unsigned int nresult,
                           unsigned long long start,
                           long long nrows)
{
  return cmd_history2 (label,
                       merchant_url,
                       http_status,
                       time,
                       nresult,
                       start,
                       GNUNET_NO,
                       nrows);
}


/* end of testing_api_cmd_history.c */
