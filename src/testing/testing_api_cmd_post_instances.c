/*
  This file is part of TALER
  Copyright (C) 2020 Taler Systems SA

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
 * @file lib/testing_api_cmd_post_instances.c
 * @brief command to test POST /instances
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "POST /instances" CMD.
 */
struct PostInstancesState
{

  /**
   * Handle for a "GET instance" request.
   */
  struct TALER_MERCHANT_InstancesPostHandle *iph;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * ID of the instance to run GET for.
   */
  const char *instance_id;

  /**
   * Length of the @payto_uris array
   */
  unsigned int payto_uris_length;

  /**
   * Array of payto URIs.
   */
  const char **payto_uris;

  /**
   * Name of the instance.
   */
  const char *name;

  /**
   * Address to use.
   */
  json_t *address;

  /**
   * Jurisdiction to use.
   */
  json_t *jurisdiction;

  /**
   * Wire fee to use.
   */
  struct TALER_Amount default_max_wire_fee;

  /**
   * Amortization to use.
   */
  uint32_t default_wire_fee_amortization;

  /**
   * Deposit fee ceiling to use.
   */
  struct TALER_Amount default_max_deposit_fee;

  /**
   * Wire transfer delay to use.
   */
  struct GNUNET_TIME_Relative default_wire_transfer_delay;

  /**
   * Order validity default duration to use.
   */
  struct GNUNET_TIME_Relative default_pay_delay;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

};


/**
 * Callback for a POST /instances operation.
 *
 * @param cls closure for this function
 */
static void
post_instances_cb (void *cls,
                   const struct TALER_MERCHANT_HttpResponse *hr)
{
  struct PostInstancesState *pis = cls;

  pis->iph = NULL;
  if (pis->http_status != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                (int) hr->ec,
                TALER_TESTING_interpreter_get_current_label (pis->is));
    TALER_TESTING_interpreter_fail (pis->is);
    return;
  }
  switch (hr->http_status)
  {
  case MHD_HTTP_OK:
    break;
  // FIXME: add other legitimate states here...
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Unhandled HTTP status.\n");
  }
  TALER_TESTING_interpreter_next (pis->is);
}


/**
 * Run the "POST /instances" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
post_instances_run (void *cls,
                    const struct TALER_TESTING_Command *cmd,
                    struct TALER_TESTING_Interpreter *is)
{
  struct PostInstancesState *pis = cls;

  pis->is = is;
  pis->iph = TALER_MERCHANT_instances_post (is->ctx,
                                            pis->merchant_url,
                                            pis->instance_id,
                                            pis->payto_uris_length,
                                            pis->payto_uris,
                                            pis->name,
                                            pis->address,
                                            pis->jurisdiction,
                                            &pis->default_max_wire_fee,
                                            pis->default_wire_fee_amortization,
                                            &pis->default_max_deposit_fee,
                                            pis->default_wire_transfer_delay,
                                            pis->default_pay_delay,
                                            &post_instances_cb,
                                            pis);
  GNUNET_assert (NULL != pis->iph);
}


/**
 * Free the state of a "GET instance" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
post_instances_cleanup (void *cls,
                        const struct TALER_TESTING_Command *cmd)
{
  struct PostInstancesState *pis = cls;

  if (NULL != pis->iph)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "GET /instances/$ID operation did not complete\n");
    TALER_MERCHANT_instances_post_cancel (pis->iph);
  }
  json_decref (pis->address);
  json_decref (pis->jurisdiction);
  GNUNET_free (pis);
}


/**
 * Define a "POST /instances" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        POST /instances request.
 * @param instance_id the ID of the instance to query
 * @param payto_uris_length length of the @a accounts array
 * @param payto_uris URIs of the bank accounts of the merchant instance
 * @param name name of the merchant instance
 * @param address physical address of the merchant instance
 * @param jurisdiction jurisdiction of the merchant instance
 * @param default_max_wire_fee default maximum wire fee merchant is willing to fully pay
 * @param default_wire_fee_amortization default amortization factor for excess wire fees
 * @param default_max_deposit_fee default maximum deposit fee merchant is willing to pay
 * @param default_wire_transfer_delay default wire transfer delay merchant will ask for
 * @param default_pay_delay default validity period for offers merchant makes
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_post_instances2 (
  const char *label,
  const char *merchant_url,
  const char *instance_id,
  unsigned int payto_uris_length,
  const char *payto_uris[],
  const char *name,
  json_t *address,
  json_t *jurisdiction,
  const struct TALER_Amount *default_max_wire_fee,
  uint32_t default_wire_fee_amortization,
  const struct TALER_Amount *default_max_deposit_fee,
  struct GNUNET_TIME_Relative default_wire_transfer_delay,
  struct GNUNET_TIME_Relative default_pay_delay,
  unsigned int http_status)
{
  struct PostInstancesState *pis;

  pis = GNUNET_new (struct PostInstancesState);
  pis->merchant_url = merchant_url;
  pis->instance_id = instance_id;
  pis->http_status = http_status;
  pis->payto_uris_length = payto_uris_length;
  pis->payto_uris = payto_uris;
  pis->name = name;
  pis->address = address; /* ownership transfer! */
  pis->jurisdiction = jurisdiction; /* ownership transfer! */
  pis->default_max_wire_fee = *default_max_wire_fee;
  pis->default_wire_fee_amortization = default_wire_fee_amortization;
  pis->default_max_deposit_fee = *default_max_deposit_fee;
  pis->default_wire_transfer_delay = default_wire_transfer_delay;
  pis->default_pay_delay = default_pay_delay;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pis,
      .label = label,
      .run = &post_instances_run,
      .cleanup = &post_instances_cleanup
    };

    return cmd;
  }
}


/**
 * Define a "POST /instances" CMD, simple version
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        POST /instances request.
 * @param instance_id the ID of the instance to create
 * @param payto_uri payment URI to use
 * @param currency currency to use for default fees
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_post_instances (const char *label,
                                           const char *merchant_url,
                                           const char *instance_id,
                                           const char *payto_uri,
                                           const char *currency,
                                           unsigned int http_status)
{
  const char *payto_uris[] = {
    payto_uri
  };
  struct TALER_Amount default_max_wire_fee;
  struct TALER_Amount default_max_deposit_fee;

  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &default_max_wire_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (currency,
                                        &default_max_deposit_fee));
  default_max_wire_fee.value = 1;
  default_max_deposit_fee.value = 1;

  return TALER_TESTING_cmd_merchant_post_instances2 (
    label,
    merchant_url,
    instance_id,
    1,
    payto_uris,
    instance_id,
    json_pack ("{s:s}", "city", "shopcity"),
    json_pack ("{s:s}", "city", "lawyercity"),
    &default_max_wire_fee,
    10,
    &default_max_deposit_fee,
    GNUNET_TIME_UNIT_MINUTES,
    GNUNET_TIME_UNIT_MINUTES,
    http_status);
}


/* end of testing_api_cmd_post_instance.c */
