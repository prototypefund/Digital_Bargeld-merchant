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
 * @file lib/testing_api_cmd_pay_abort_refund.c
 * @brief command to test the pay-abort-refund feature.
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "pay abort refund" CMD.  This command
 * takes the refund permissions from a "pay abort" CMD,
 * and redeems those at the exchange.
 */
struct PayAbortRefundState
{

  /**
   * "abort" CMD that will provide with refund permissions.
   */
  const char *abort_reference;

  /**
   * Expected number of coins that were refunded.
   * Only used to counter-check, not to perform any
   * operation.
   */
  unsigned int num_coins;

  /**
   * The amount to be "withdrawn" from the refund session.
   */
  const char *refund_amount;

  /**
   * The refund fee (charged to the merchant).
   */
  const char *refund_fee;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Handle to the refund operation.
   */
  struct TALER_EXCHANGE_RefundHandle *rh;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;
};


/**
 * Callback used to work out the response from the exchange
 * to a refund operation.  Currently only checks if the response
 * code is as expected.
 *
 * @param cls closure
 * @param hr HTTP response code details
 * @param sign_key exchange key used to sign @a obj, or NULL
 */
static void
abort_refund_cb (void *cls,
                 const struct TALER_EXCHANGE_HttpResponse *hr,
                 const struct TALER_ExchangePublicKeyP *sign_key)
{
  struct PayAbortRefundState *pars = cls;

  pars->rh = NULL;
  if (pars->http_status != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                hr->ec,
                TALER_TESTING_interpreter_get_current_label (pars->is));
    TALER_TESTING_interpreter_fail (pars->is);
    return;
  }
  TALER_TESTING_interpreter_next (pars->is);
}


/**
 * Free the state of a "pay abort refund" CMD, and possibly
 * cancel a pending operation.
 *
 * @param cls closure.
 * @param cmd the command currently being freed.
 */
static void
pay_abort_refund_cleanup (void *cls,
                          const struct TALER_TESTING_Command *cmd)
{
  struct PayAbortRefundState *pars = cls;

  if (NULL != pars->rh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command `%s' did not complete.\n",
                TALER_TESTING_interpreter_get_current_label (
                  pars->is));
    TALER_EXCHANGE_refund_cancel (pars->rh);
  }
  GNUNET_free (pars);
}


/**
 * Run a "pay abort refund" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being run.
 * @param is interpreter state.
 */
static void
pay_abort_refund_run (void *cls,
                      const struct TALER_TESTING_Command *cmd,
                      struct TALER_TESTING_Interpreter *is)
{
  struct PayAbortRefundState *pars = cls;
  struct TALER_Amount refund_fee;
  struct TALER_Amount refund_amount;
  const struct TALER_MERCHANT_RefundEntry *refund_entry;
  const unsigned int *num_refunds;
  const struct TALER_TESTING_Command *abort_cmd;
  const struct TALER_MerchantPublicKeyP *merchant_pub;
  const struct GNUNET_HashCode *h_contract_terms;

  pars->is = is;
  if (NULL ==
      (abort_cmd =
         TALER_TESTING_interpreter_lookup_command (is,
                                                   pars->abort_reference)) )
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_uint (abort_cmd,
                                    0,
                                    &num_refunds))
    TALER_TESTING_FAIL (is);
  if (pars->num_coins >= *num_refunds)
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_h_contract_terms (abort_cmd,
                                                0,
                                                &h_contract_terms))
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_merchant_pub (abort_cmd,
                                            0,
                                            &merchant_pub))
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_refund_entry (abort_cmd,
                                            0,
                                            &refund_entry))
    TALER_TESTING_FAIL (is);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (pars->refund_amount,
                                         &refund_amount));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (pars->refund_fee,
                                         &refund_fee));
  pars->rh = TALER_EXCHANGE_refund2
               (is->exchange,
               &refund_amount,
               &refund_fee,
               h_contract_terms,
               &refund_entry->coin_pub,
               refund_entry->rtransaction_id,
               merchant_pub,
               &refund_entry->merchant_sig,
               &abort_refund_cb,
               pars);
  GNUNET_assert (NULL != pars->rh);
}


/**
 * Make a "pay abort refund" CMD.  This command uses the
 * refund permission from a "pay abort" CMD, and redeems it
 * at the exchange.
 *
 * @param label command label.
 * @param abort_reference reference to the "pay abort" CMD that
 *        will offer the refund permission.
 * @param num_coins how many coins are expected to be refunded.
 * @param refund_amount the amount we are going to redeem as
 *        refund.
 * @param refund_fee the refund fee (merchant pays it)
 * @param http_status expected HTTP response code.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay_abort_refund
  (const char *label,
  const char *abort_reference,
  unsigned int num_coins,
  const char *refund_amount,
  const char *refund_fee,
  unsigned int http_status)
{
  struct PayAbortRefundState *pars;

  pars = GNUNET_new (struct PayAbortRefundState);
  pars->abort_reference = abort_reference;
  pars->num_coins = num_coins;
  pars->refund_amount = refund_amount;
  pars->refund_fee = refund_fee;
  pars->http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pars,
      .label = label,
      .run = &pay_abort_refund_run,
      .cleanup = &pay_abort_refund_cleanup
    };

    return cmd;
  }
}
