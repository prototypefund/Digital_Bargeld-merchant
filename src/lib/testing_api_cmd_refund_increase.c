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
 * @file lib/testing_api_cmd_refund_increase.c
 * @brief command to test refunds.
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "refund increase" CMD.
 */
struct RefundIncreaseState
{
  /**
   * Operation handle for a POST /refund request.
   */
  struct TALER_MERCHANT_RefundIncreaseOperation *rio;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * Order id of the contract to refund.
   */
  const char *order_id;

  /**
   * The amount to refund.
   */
  const char *refund_amount;

  /**
   * Refund fee.
   */
  const char *refund_fee;

  /**
   * Human-readable justification for the refund.
   */
  const char *reason;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_code;
};


/**
 * Free the state of a "refund increase" CMD, and
 * possibly cancel a pending "refund increase" operation.
 *
 * @param cls closure
 * @param cmd command currently being freed.
 */
static void
refund_increase_cleanup (void *cls,
                         const struct TALER_TESTING_Command *cmd)
{
  struct RefundIncreaseState *ris = cls;

  if (NULL != ris->rio)
  {
    TALER_LOG_WARNING ("Refund-increase operation"
                       " did not complete\n");
    TALER_MERCHANT_refund_increase_cancel (ris->rio);
  }
  GNUNET_free (ris);
}


/**
 * Process POST /refund (increase) response; just checking
 * if the HTTP response code is the one expected.
 *
 * @param cls closure
 * @param hr HTTP response
 */
static void
refund_increase_cb (void *cls,
                    const struct TALER_MERCHANT_HttpResponse *hr)
{
  struct RefundIncreaseState *ris = cls;

  ris->rio = NULL;
  if (ris->http_code != hr->http_status)
    TALER_TESTING_FAIL (ris->is);
  TALER_TESTING_interpreter_next (ris->is);
}


/**
 * Run the "refund increase" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being run.
 * @param is the interpreter state.
 */
static void
refund_increase_run (void *cls,
                     const struct TALER_TESTING_Command *cmd,
                     struct TALER_TESTING_Interpreter *is)
{
  struct RefundIncreaseState *ris = cls;
  struct TALER_Amount refund_amount;

  ris->is = is;
  if (GNUNET_OK != TALER_string_to_amount (ris->refund_amount,
                                           &refund_amount))
    TALER_TESTING_FAIL (is);
  ris->rio = TALER_MERCHANT_refund_increase (is->ctx,
                                             ris->merchant_url,
                                             ris->order_id,
                                             &refund_amount,
                                             ris->reason,
                                             &refund_increase_cb,
                                             ris);
  if (NULL == ris->rio)
    TALER_TESTING_FAIL (is);
}


/**
 * Offer internal data from the "refund increase" CMD
 * state to other commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
refund_increase_traits (void *cls,
                        const void **ret,
                        const char *trait,
                        unsigned int index)
{
  struct RefundIncreaseState *ris = cls;
  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_string (0,
                                     ris->refund_amount),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


/**
 * Define a "refund increase" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the backend serving the
 *        "refund increase" request.
 * @param reason refund justification, human-readable.
 * @param order_id order id of the contract to refund.
 * @param refund_amount amount to be refund-increased.
 * @param refund_fee refund fee.
 * @param http_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_increase (const char *label,
                                   const char *merchant_url,
                                   const char *reason,
                                   const char *order_id,
                                   const char *refund_amount,
                                   const char *refund_fee,
                                   unsigned int http_code)
{
  struct RefundIncreaseState *ris;

  ris = GNUNET_new (struct RefundIncreaseState);
  ris->merchant_url = merchant_url;
  ris->order_id = order_id;
  ris->refund_amount = refund_amount;
  ris->refund_fee = refund_fee;
  ris->reason = reason;
  ris->http_code = http_code;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = ris,
      .label = label,
      .run = &refund_increase_run,
      .cleanup = &refund_increase_cleanup,
      .traits = &refund_increase_traits
    };

    return cmd;
  }
}


/* end of testing_api_cmd_refund_increase.c */
