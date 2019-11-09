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
 * @file lib/testing_api_cmd_check_payment.c
 * @brief command to test the /check-payment feature.
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a /check-payment CMD.
 */
struct CheckPaymentState
{

  /**
   * Operation handle.
   */
  struct TALER_MERCHANT_CheckPaymentOperation *cpo;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected HTTP response status code.
   */
  unsigned int http_status;

  /**
   * Reference to a command that can provide a order id,
   * typically a /proposal test command.
   */
  const char *proposal_reference;

  /**
   * GNUNET_YES if we expect the proposal was paid.
   */
  unsigned int expect_paid;

  /**
   * The merchant base URL.
   */
  const char *merchant_url;
};


/**
 * Free a /check-payment CMD, and possibly cancel a pending
 * operation thereof.
 *
 * @param cls closure
 * @param cmd the command currently getting freed.
 */
static void
check_payment_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  struct CheckPaymentState *cps = cls;

  if (NULL != cps->cpo)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command `%s' was not terminated\n",
                TALER_TESTING_interpreter_get_current_label (
                  cps->is));
    TALER_MERCHANT_check_payment_cancel (cps->cpo);
  }
  GNUNET_free (cps);
}


/**
 * Callback for a /check-payment request.
 *
 * @param cls closure.
 * @param http_status HTTP status code we got.
 * @param json full response we got.
 * @param paid #GNUNET_YES (GNUNET_NO) if the contract was paid
 *        (not paid).
 * @param refunded #GNUNET_YES (GNUNET_NO) if the contract was
 *        refunded (not refunded).
 * @param refund_amount the amount that was refunded to this
 *        contract.
 * @param taler_pay_uri the URI that instructs the wallets to process
 *                      the payment
 */
static void
check_payment_cb (void *cls,
                  unsigned int http_status,
                  const json_t *obj,
                  int paid,
                  int refunded,
                  struct TALER_Amount *refund_amount,
                  const char *taler_pay_uri)
{
  struct CheckPaymentState *cps = cls;

  cps->cpo = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "check payment (%s): expected paid: %d, paid: %d, url: %s\n",
              TALER_TESTING_interpreter_get_current_label (cps->is),
              cps->expect_paid,
              paid,
              taler_pay_uri);
  if (paid != cps->expect_paid)
    TALER_TESTING_FAIL (cps->is);
  if (cps->http_status != http_status)
    TALER_TESTING_FAIL (cps->is);
  TALER_TESTING_interpreter_next (cps->is);
}


/**
 * Run a /check-payment CMD.
 *
 * @param cmd the command currenly being run.
 * @param cls closure.
 * @param is interpreter state.
 */
static void
check_payment_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct CheckPaymentState *cps = cls;
  const struct TALER_TESTING_Command *proposal_cmd;
  const char *order_id;

  cps->is = is;
  proposal_cmd = TALER_TESTING_interpreter_lookup_command (
    is, cps->proposal_reference);
  if (NULL == proposal_cmd)
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK != TALER_TESTING_get_trait_order_id (
        proposal_cmd, 0, &order_id))
    TALER_TESTING_FAIL (is);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Checking for order id `%s'\n",
              order_id);
  cps->cpo = TALER_MERCHANT_check_payment (is->ctx,
                                           cps->merchant_url,
                                           order_id,
                                           NULL,
                                           GNUNET_TIME_UNIT_ZERO,
                                           &check_payment_cb,
                                           cps);
  GNUNET_assert (NULL != cps->cpo);
}


/**
 * Make a "check payment" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param http_status expected HTTP response code.
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param expect_paid #GNUNET_YES if we expect the proposal to be
 *        paid, #GNUNET_NO otherwise.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment (const char *label,
                                 const char *merchant_url,
                                 unsigned int http_status,
                                 const char *proposal_reference,
                                 unsigned int expect_paid)
{
  struct CheckPaymentState *cps;

  cps = GNUNET_new (struct CheckPaymentState);
  cps->http_status = http_status;
  cps->proposal_reference = proposal_reference;
  cps->expect_paid = expect_paid;
  cps->merchant_url = merchant_url;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = cps,
      .label = label,
      .run = &check_payment_run,
      .cleanup = &check_payment_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_check_payment.c */
