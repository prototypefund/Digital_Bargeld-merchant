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
 * @file lib/testing_api_cmd_tip_query.c
 * @brief command to test the tipping.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a /tip-query CMD.
 */
struct TipQueryState
{

  /**
   * The merchant base URL.
   */
  const char *merchant_url;

  /**
   * Expected HTTP response code for this CMD.
   */
  unsigned int http_status;

  /**
   * The handle to the current /tip-query request.
   */
  struct TALER_MERCHANT_TipQueryOperation *tqo;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected amount to be picked up.
   */
  const char *expected_amount_picked_up;

  /**
   * Expected amount to be tip-authorized.
   */
  const char *expected_amount_authorized;

  /**
   * Amount that is expected to be still available
   * from the tip reserve.
   */
  const char *expected_amount_available;
};


/**
 * Callback to process a GET /tip-query request, it mainly
 * checks that what the backend returned matches the command's
 * expectations.
 *
 * @param cls closure
 * @param hr HTTP response
 * @param reserve_expiration when the tip reserve will expire
 * @param reserve_pub tip reserve public key
 * @param amount_authorized total amount authorized on tip reserve
 * @param amount_available total amount still available on
 *        tip reserve
 * @param amount_picked_up total amount picked up from tip reserve
 */
static void
tip_query_cb (void *cls,
              const struct TALER_MERCHANT_HttpResponse *hr,
              struct GNUNET_TIME_Absolute reserve_expiration,
              struct TALER_ReservePublicKeyP *reserve_pub,
              struct TALER_Amount *amount_authorized,
              struct TALER_Amount *amount_available,
              struct TALER_Amount *amount_picked_up)
{
  struct TipQueryState *tqs = cls;
  struct TALER_Amount a;

  tqs->tqo = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Tip query callback at command `%s'\n",
              TALER_TESTING_interpreter_get_current_label (tqs->is));
  GNUNET_assert (NULL != reserve_pub);
  GNUNET_assert (NULL != amount_authorized);
  GNUNET_assert (NULL != amount_available);
  GNUNET_assert (NULL != amount_picked_up);

  if (tqs->expected_amount_available)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (tqs->expected_amount_available,
                                           &a));
    {
      char *str;

      str = TALER_amount_to_string (amount_available);
      TALER_LOG_INFO ("expected available %s, actual %s\n",
                      TALER_amount2s (&a),
                      str);
      GNUNET_free (str);
    }
    if (0 !=
        TALER_amount_cmp (amount_available,
                          &a))
      TALER_TESTING_FAIL (tqs->is);
  }

  if (tqs->expected_amount_authorized)
  {
    char *str;

    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (tqs->expected_amount_authorized,
                                           &a));
    str = TALER_amount_to_string (amount_authorized);
    TALER_LOG_INFO ("expected authorized %s, actual %s\n",
                    TALER_amount2s (&a),
                    str);
    GNUNET_free (str);
    if (0 !=
        TALER_amount_cmp (amount_authorized,
                          &a))
      TALER_TESTING_FAIL (tqs->is);
  }

  if (tqs->expected_amount_picked_up)
  {
    char *str;

    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (tqs->expected_amount_picked_up,
                                           &a));
    str = TALER_amount_to_string (amount_picked_up);
    TALER_LOG_INFO ("expected picked_up %s, actual %s\n",
                    TALER_amount2s (&a),
                    str);
    GNUNET_free (str);
    if (0 !=
        TALER_amount_cmp (amount_picked_up,
                          &a))
      TALER_TESTING_FAIL (tqs->is);
  }

  if (tqs->http_status != hr->http_status)
    TALER_TESTING_FAIL (tqs->is);
  TALER_TESTING_interpreter_next (tqs->is);
}


/**
 * Free the state from a /tip-query CMD, and possibly cancel
 * a pending /tip-query request.
 *
 * @param cls closure.
 * @param cmd the /tip-query CMD to free.
 */
static void
tip_query_cleanup (void *cls,
                   const struct TALER_TESTING_Command *cmd)
{
  struct TipQueryState *tqs = cls;

  if (NULL != tqs->tqo)
  {
    TALER_LOG_WARNING ("Tip-query operation"
                       " did not complete\n");
    TALER_MERCHANT_tip_query_cancel (tqs->tqo);
  }
  GNUNET_free (tqs);
}


/**
 * Run a /tip-query CMD.
 *
 * @param cls closure.
 * @param cmd the current /tip-query CMD.
 * @param is the interpreter state.
 */
static void
tip_query_run (void *cls,
               const struct TALER_TESTING_Command *cmd,
               struct TALER_TESTING_Interpreter *is)
{
  struct TipQueryState *tqs = cls;

  tqs->is = is;
  tqs->tqo = TALER_MERCHANT_tip_query (is->ctx,
                                       tqs->merchant_url,
                                       &tip_query_cb,
                                       tqs);
  GNUNET_assert (NULL != tqs->tqo);
}


/**
 * Define a /tip-query CMD equipped with a expected amount.
 *
 * @param label the command label
 * @param merchant_url base URL of the merchant which will
 *        server the /tip-query request.
 * @param http_status expected HTTP response code for the
 *        /tip-query request.
 * @param expected_amount_picked_up expected amount already
 *        picked up.
 * @param expected_amount_authorized expected amount that was
 *        authorized in the first place.
 * @param expected_amount_available expected amount which is
 *        still available from the tip reserve
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query_with_amounts (const char *label,
                                          const char *merchant_url,
                                          unsigned int http_status,
                                          const char *expected_amount_picked_up,
                                          const char *expected_amount_authorized,
                                          const char *expected_amount_available)
{
  struct TipQueryState *tqs;

  tqs = GNUNET_new (struct TipQueryState);
  tqs->merchant_url = merchant_url;
  tqs->http_status = http_status;
  tqs->expected_amount_picked_up = expected_amount_picked_up;
  tqs->expected_amount_authorized = expected_amount_authorized;
  tqs->expected_amount_available = expected_amount_available;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = tqs,
      .label = label,
      .run = &tip_query_run,
      .cleanup = &tip_query_cleanup
    };

    return cmd;
  }
}


/**
 * Define a /tip-query CMD.
 *
 * @param label the command label
 * @param merchant_url base URL of the merchant which will
 *        server the /tip-query request.
 * @param http_status expected HTTP response code for the
 *        /tip-query request.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query (const char *label,
                             const char *merchant_url,
                             unsigned int http_status)
{
  struct TipQueryState *tqs;

  tqs = GNUNET_new (struct TipQueryState);
  tqs->merchant_url = merchant_url;
  tqs->http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = tqs,
      .label = label,
      .run = &tip_query_run,
      .cleanup = &tip_query_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_tip_query.c */
