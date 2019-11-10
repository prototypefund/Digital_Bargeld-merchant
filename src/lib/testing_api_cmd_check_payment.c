/*
  This file is part of TALER
  Copyright (C) 2014-2019 Taler Systems SA

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
 * State for a /check-payment conclude CMD.
 */
struct CheckPaymentConcludeState;

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
   * The merchant base URL.
   */
  const char *merchant_url;

  /**
   * Reference to a command that can provide a order id,
   * typically a /proposal test command.
   */
  const char *proposal_reference;

  /**
   * State for a /check-payment conclude CMD.
 */
  struct CheckPaymentConcludeState *cs;

  /**
   * 0 if long-polling is not desired.
   */
  struct GNUNET_TIME_Relative timeout;

  /**
   * Set to the start time of the @e cpo plus the @e timeout.
   */
  struct GNUNET_TIME_Absolute deadline;

  /**
   * #GNUNET_YES if we expect the proposal was paid, synchronous variant.
   */
  int expect_paid;

  /**
   * #GNUNET_YES if the proposal was paid.
   */
  int paid;

  /**
   * #GNUNET_YES if the proposal was paid and then refunded
   */
  int refunded;

  /**
   * Observed HTTP response status code.
   */
  unsigned int http_status;

  /**
   * Expected HTTP response status code, synchronous variant.
   */
  unsigned int expected_http_status;

};


/**
 * State for a /check-payment conclude CMD.
 */
struct CheckPaymentConcludeState
{

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Reference to a command that can provide a check payment start command.
   */
  const char *start_reference;

  /**
   * Task to wait for the deadline.
   */
  struct GNUNET_SCHEDULER_Task *task;

  /**
   * Expected HTTP response status code.
   */
  unsigned int expected_http_status;

  /**
   * #GNUNET_YES if the proposal was expected to be paid.
   */
  int expected_paid;

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
 * Task called when either the timeout for the /check-payment
 * command expired or we got a response.  Checks if the
 * result is what we expected.
 *
 * @param cls a `struct CheckPaymentConcludeState`
 */
static void
conclude_task (void *cls)
{
  struct CheckPaymentConcludeState *cpc = cls;
  const struct TALER_TESTING_Command *check_cmd;
  struct CheckPaymentState *cps;
  struct GNUNET_TIME_Absolute now;

  cpc->task = NULL;
  check_cmd =
    TALER_TESTING_interpreter_lookup_command (cpc->is,
                                              cpc->start_reference);
  cps = check_cmd->cls;
  if (NULL != cps->cpo)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected /poll/payment to have completed, but it did not!\n");
    TALER_TESTING_FAIL (cpc->is);
  }
  if (cps->http_status != cpc->expected_http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected HTTP status %u, got %u\n",
                cpc->expected_http_status,
                cps->http_status);
    TALER_TESTING_FAIL (cps->is);
  }
  now = GNUNET_TIME_absolute_get ();
  if ( (GNUNET_NO == cps->paid) &&
       (GNUNET_TIME_absolute_add (cps->deadline,
                                  GNUNET_TIME_UNIT_SECONDS).abs_value_us <
        now.abs_value_us) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected answer to be delayed until %llu, but got response at %llu\n",
                (unsigned long long) cps->deadline.abs_value_us,
                (unsigned long long) now.abs_value_us);
    TALER_TESTING_FAIL (cps->is);
  }
  if (cps->paid != cpc->expected_paid)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected paid status %u, got %u\n",
                cpc->expected_paid,
                cps->paid);
    TALER_TESTING_FAIL (cps->is);
  }
  TALER_TESTING_interpreter_next (cps->is);
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
  cps->paid = paid;
  cps->http_status = http_status;
  cps->refunded = refunded;
  if (0 == cps->timeout.rel_value_us)
  {
    /* synchronous variant */
    if (paid != cps->expect_paid)
      TALER_TESTING_FAIL (cps->is);
    if (cps->http_status != http_status)
      TALER_TESTING_FAIL (cps->is);
    TALER_TESTING_interpreter_next (cps->is);
  }
  else
  {
    /* asynchronous variant */
    if (NULL != cps->cs)
    {
      GNUNET_SCHEDULER_cancel (cps->cs->task);
      cps->cs->task = GNUNET_SCHEDULER_add_now (&conclude_task,
                                                cps->cs);
    }
  }
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
                                           cps->timeout,
                                           &check_payment_cb,
                                           cps);
  GNUNET_assert (NULL != cps->cpo);
  if (0 != cps->timeout.rel_value_us)
    TALER_TESTING_interpreter_next (cps->is);
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
  cps->expected_http_status = http_status;
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


/**
 * Make a "check payment" test command with long polling support.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param timeout how long to wait during long polling for the reply
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment_start (const char *label,
                                       const char *merchant_url,
                                       const char *proposal_reference,
                                       struct GNUNET_TIME_Relative timeout)
{
  struct CheckPaymentState *cps;

  if (0 == timeout.rel_value_us)
    timeout.rel_value_us = 1; /* 0 reserved for blocking version */
  cps = GNUNET_new (struct CheckPaymentState);
  cps->timeout = timeout;
  cps->proposal_reference = proposal_reference;
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


/**
 * Free a /check-payment conclusion CMD, and possibly cancel a pending
 * operation thereof.
 *
 * @param cls closure
 * @param cmd the command currently getting freed.
 */
static void
check_payment_conclude_cleanup (void *cls,
                                const struct TALER_TESTING_Command *cmd)
{
  struct CheckPaymentConcludeState *cps = cls;

  if (NULL != cps->task)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Command `%s' was not terminated\n",
                TALER_TESTING_interpreter_get_current_label (
                  cps->is));
    GNUNET_SCHEDULER_cancel (cps->task);
    cps->task = NULL;
  }
}


/**
 * Run a /check-payment conclusion CMD.
 *
 * @param cmd the command currenly being run.
 * @param cls closure.
 * @param is interpreter state.
 */
static void
check_payment_conclude_run (void *cls,
                            const struct TALER_TESTING_Command *cmd,
                            struct TALER_TESTING_Interpreter *is)
{
  struct CheckPaymentConcludeState *cpc = cls;
  const struct TALER_TESTING_Command *check_cmd;
  struct CheckPaymentState *cps;

  cpc->is = is;
  check_cmd =
    TALER_TESTING_interpreter_lookup_command (is,
                                              cpc->start_reference);
  GNUNET_assert (check_cmd->run == &check_payment_run);
  cps = check_cmd->cls;
  if (NULL == cps->cpo)
    cpc->task = GNUNET_SCHEDULER_add_now (&conclude_task,
                                          cpc);
  else
    cpc->task = GNUNET_SCHEDULER_add_at (cps->deadline,
                                         &conclude_task,
                                         cpc);
}


/**
 * Expect completion of a long-polled "check payment" test command.
 *
 * @param label command label.
 * @param check_start_reference payment start operation that should have
 *                   completed
 * @param http_status expected HTTP response code.
 * @param expect_paid #GNUNET_YES if we expect the proposal to be
 *        paid, #GNUNET_NO otherwise.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment_conclude (const char *label,
                                          unsigned int http_status,
                                          const char *poll_start_reference,
                                          unsigned int expect_paid)
{
  struct CheckPaymentConcludeState *cps;

  cps = GNUNET_new (struct CheckPaymentConcludeState);
  cps->start_reference = poll_start_reference;
  cps->expected_paid = expect_paid;
  cps->expected_http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = cps,
      .label = label,
      .run = &check_payment_conclude_run,
      .cleanup = &check_payment_conclude_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_check_payment.c */
