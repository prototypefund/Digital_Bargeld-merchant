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
 * @file lib/testing_api_cmd_poll_payment.c
 * @brief command to test the /public/poll-payment feature.
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a /poll-payment conclude CMD.
 */
struct PollPaymentConcludeState;


/**
 * State for a /poll-payment start CMD.
 */
struct PollPaymentStartState
{

  /**
   * Operation handle.
   */
  struct TALER_MERCHANT_PollPaymentOperation *cpo;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Reference to a command that can provide a order id,
   * typically a /proposal test command.
   */
  const char *proposal_reference;

  /**
   * The merchant base URL.
   */
  const char *merchant_url;

  /**
   * Conclude state waiting for completion (if any).
   */
  struct PollPaymentConcludeState *cs;

  /**
   * How long is the long-polling allowed to take?
   */
  struct GNUNET_TIME_Relative timeout;

  /**
   * Set to the start time of the @e cpo plus the @e timeout.
   */
  struct GNUNET_TIME_Absolute deadline;

  /**
   * Amount refunded, set if @e refunded is #GNUNET_YES
   */
  struct TALER_Amount refund;

  /**
   * Final HTTP response status code.
   */
  unsigned int http_status;

  /**
   * #GNUNET_YES if the proposal was paid.
   */
  int paid;

  /**
   * #GNUNET_YES if the proposal was paid and then refunded
   */
  int refunded;

};


/**
 * State for a /poll-payment conclude CMD.
 */
struct PollPaymentConcludeState
{

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Reference to a command that can provide a poll payment start command.
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
 * Free a /poll-payment CMD, and possibly cancel a pending
 * operation thereof.
 *
 * @param cls closure
 * @param cmd the command currently getting freed.
 */
static void
poll_payment_start_cleanup (void *cls,
                            const struct TALER_TESTING_Command *cmd)
{
  struct PollPaymentStartState *cps = cls;

  if (NULL != cps->cpo)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Command `%s' was not terminated\n",
                TALER_TESTING_interpreter_get_current_label (
                  cps->is));
    TALER_MERCHANT_poll_payment_cancel (cps->cpo);
  }
  GNUNET_free (cps);
}


/**
 * Task called when either the timeout for the /poll-payment
 * command expired or we got a response.  Checks if the
 * result is what we expected.
 *
 * @param cls a `struct PollPaymentConcludeState`
 */
static void
conclude_task (void *cls)
{
  struct PollPaymentConcludeState *ppc = cls;
  const struct TALER_TESTING_Command *poll_cmd;
  struct PollPaymentStartState *cps;
  struct GNUNET_TIME_Absolute now;

  ppc->task = NULL;
  poll_cmd =
    TALER_TESTING_interpreter_lookup_command (ppc->is,
                                              ppc->start_reference);
  cps = poll_cmd->cls;
  if (NULL != cps->cpo)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected /poll/payment to have completed, but it did not!\n");
    TALER_TESTING_FAIL (ppc->is);
  }
  if (cps->http_status != ppc->expected_http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected HTTP status %u, got %u\n",
                ppc->expected_http_status,
                cps->http_status);
    TALER_TESTING_FAIL (ppc->is);
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
    TALER_TESTING_FAIL (ppc->is);
  }
  if (cps->paid != ppc->expected_paid)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Expected paid status %u, got %u\n",
                ppc->expected_paid,
                cps->paid);
    TALER_TESTING_FAIL (ppc->is);
  }
  TALER_TESTING_interpreter_next (ppc->is);
}


/**
 * Callback for a /poll-payment request.
 *
 * @param cls closure.
 * @param http_status HTTP status code we got.
 * @param json full response we got.
 * @param paid #GNUNET_YES (#GNUNET_NO) if the contract was (not) paid
 * @param refunded #GNUNET_YES (#GNUNET_NO) if the contract was
 *        (not) refunded.
 * @param refund_amount the amount that was refunded to this
 *        contract.
 * @param taler_pay_uri the URI that instructs the wallets to process
 *                      the payment
 */
static void
poll_payment_cb (void *cls,
                 unsigned int http_status,
                 const json_t *obj,
                 int paid,
                 int refunded,
                 struct TALER_Amount *refund_amount,
                 const char *taler_pay_uri)
{
  struct PollPaymentStartState *cps = cls;

  cps->cpo = NULL;
  if ( (MHD_HTTP_OK != http_status) &&
       (NULL != obj) )
  {
    char *log = json_dumps (obj,
                            JSON_COMPACT);

    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Poll payment returned %u: %s\n",
                http_status,
                log);
    free (log);
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Poll payment returned %u (%d/%d)\n",
                http_status,
                paid,
                refunded);
  }
  cps->paid = paid;
  cps->http_status = http_status;
  cps->refunded = refunded;
  if (GNUNET_YES == refunded)
    cps->refund = *refund_amount;
  if (NULL != cps->cs)
  {
    GNUNET_SCHEDULER_cancel (cps->cs->task);
    cps->cs->task = GNUNET_SCHEDULER_add_now (&conclude_task,
                                              cps->cs);
  }
}


/**
 * Run a /poll-payment CMD.
 *
 * @param cmd the command currenly being run.
 * @param cls closure.
 * @param is interpreter state.
 */
static void
poll_payment_start_run (void *cls,
                        const struct TALER_TESTING_Command *cmd,
                        struct TALER_TESTING_Interpreter *is)
{
  struct PollPaymentStartState *cps = cls;
  const struct TALER_TESTING_Command *proposal_cmd;
  const char *order_id;
  const struct GNUNET_HashCode *h_contract;

  cps->is = is;
  proposal_cmd = TALER_TESTING_interpreter_lookup_command (
    is, cps->proposal_reference);

  if (NULL == proposal_cmd)
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK !=
      TALER_TESTING_get_trait_order_id (proposal_cmd,
                                        0,
                                        &order_id))
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_h_contract_terms (proposal_cmd,
                                                0,
                                                &h_contract))
    TALER_TESTING_FAIL (is);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Polling for order id `%s'\n",
              order_id);
  /* add 1s grace time to timeout */
  cps->deadline
    = GNUNET_TIME_absolute_add (GNUNET_TIME_relative_to_absolute (cps->timeout),
                                GNUNET_TIME_UNIT_SECONDS);
  cps->cpo = TALER_MERCHANT_poll_payment (is->ctx,
                                          cps->merchant_url,
                                          order_id,
                                          h_contract,
                                          NULL, /* session id */
                                          cps->timeout,
                                          &poll_payment_cb,
                                          cps);
  GNUNET_assert (NULL != cps->cpo);
  /* We CONTINUE to run the interpreter while the long-polled command
     completes asynchronously! */
  TALER_TESTING_interpreter_next (cps->is);
}


/**
 * Start a long-polled "poll-payment" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param timeout which timeout to use
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_poll_payment_start (const char *label,
                                      const char *merchant_url,
                                      const char *proposal_reference,
                                      struct GNUNET_TIME_Relative timeout)
{
  struct PollPaymentStartState *cps;

  cps = GNUNET_new (struct PollPaymentStartState);
  cps->proposal_reference = proposal_reference;
  cps->merchant_url = merchant_url;
  cps->timeout = timeout;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = cps,
      .label = label,
      .run = &poll_payment_start_run,
      .cleanup = &poll_payment_start_cleanup
    };

    return cmd;
  }
}


/**
 * Free a /poll-payment CMD, and possibly cancel a pending
 * operation thereof.
 *
 * @param cls closure
 * @param cmd the command currently getting freed.
 */
static void
poll_payment_conclude_cleanup (void *cls,
                               const struct TALER_TESTING_Command *cmd)
{
  struct PollPaymentConcludeState *cps = cls;

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
 * Run a /poll-payment CMD.
 *
 * @param cmd the command currenly being run.
 * @param cls closure.
 * @param is interpreter state.
 */
static void
poll_payment_conclude_run (void *cls,
                           const struct TALER_TESTING_Command *cmd,
                           struct TALER_TESTING_Interpreter *is)
{
  struct PollPaymentConcludeState *ppc = cls;
  const struct TALER_TESTING_Command *poll_cmd;
  struct PollPaymentStartState *cps;

  ppc->is = is;
  poll_cmd =
    TALER_TESTING_interpreter_lookup_command (is,
                                              ppc->start_reference);
  GNUNET_assert (poll_cmd->run == &poll_payment_start_run);
  cps = poll_cmd->cls;
  if (NULL == cps->cpo)
    ppc->task = GNUNET_SCHEDULER_add_now (&conclude_task,
                                          ppc);
  else
    ppc->task = GNUNET_SCHEDULER_add_at (cps->deadline,
                                         &conclude_task,
                                         ppc);
}


/**
 * Expect completion of a long-polled "poll payment" test command.
 *
 * @param label command label.
 * @param poll_start_reference payment start operation that should have
 *                   completed
 * @param http_status expected HTTP response code.
 * @param expect_paid #GNUNET_YES if we expect the proposal to be
 *        paid, #GNUNET_NO otherwise.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_poll_payment_conclude (const char *label,
                                         unsigned int http_status,
                                         const char *poll_start_reference,
                                         unsigned int expect_paid)
{
  struct PollPaymentConcludeState *cps;

  cps = GNUNET_new (struct PollPaymentConcludeState);
  cps->start_reference = poll_start_reference;
  cps->expected_paid = expect_paid;
  cps->expected_http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = cps,
      .label = label,
      .run = &poll_payment_conclude_run,
      .cleanup = &poll_payment_conclude_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_poll_payment.c */
