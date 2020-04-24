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
 * @file lib/testing_api_cmd_lock_product.c
 * @brief command to test LOCK /product
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "POST /products/$ID" CMD.
 */
struct LockProductState
{

  /**
   * Handle for a "GET product" request.
   */
  struct TALER_MERCHANT_ProductLockHandle *iph;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * ID of the product to run GET for.
   */
  const char *product_id;

  /**
   * UUID that identifies the client holding the lock
   */
  struct GNUNET_Uuid uuid;

  /**
   * duration how long should the lock be held
   */
  struct GNUNET_TIME_Relative duration;

  /**
   * how much product should be locked
   */
  uint32_t quantity;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

};


/**
 * Callback for a POST /products/$ID/lock operation.
 *
 * @param cls closure for this function
 */
static void
lock_product_cb (void *cls,
                 const struct TALER_MERCHANT_HttpResponse *hr)
{
  struct LockProductState *pis = cls;

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
 * Run the "LOCK /products/$ID" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
lock_product_run (void *cls,
                  const struct TALER_TESTING_Command *cmd,
                  struct TALER_TESTING_Interpreter *is)
{
  struct LockProductState *pis = cls;

  pis->is = is;
  pis->iph = TALER_MERCHANT_product_lock (is->ctx,
                                          pis->merchant_url,
                                          pis->product_id,
                                          &pis->uuid,
                                          pis->duration,
                                          pis->quantity,
                                          &lock_product_cb,
                                          pis);
  GNUNET_assert (NULL != pis->iph);
}


/**
 * Free the state of a "GET product" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
lock_product_cleanup (void *cls,
                      const struct TALER_TESTING_Command *cmd)
{
  struct LockProductState *pis = cls;

  if (NULL != pis->iph)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "POST /product/$ID/lock operation did not complete\n");
    TALER_MERCHANT_product_lock_cancel (pis->iph);
  }
  GNUNET_free (pis);
}


/**
 * Define a "LOCK /products/$ID" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        LOCK /product request.
 * @param product_id the ID of the product to query
 * @param payto_uris_length length of the @a accounts array
 * @param payto_uris URIs of the bank accounts of the merchant product
 * @param name name of the merchant product
 * @param address physical address of the merchant product
 * @param jurisdiction jurisdiction of the merchant product
 * @param default_max_wire_fee default maximum wire fee merchant is willing to fully pay
 * @param default_wire_fee_amortization default amortization factor for excess wire fees
 * @param default_max_deposit_fee default maximum deposit fee merchant is willing to pay
 * @param default_wire_transfer_delay default wire transfer delay merchant will ask for
 * @param default_pay_delay default validity period for offers merchant makes
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_lock_product (
  const char *label,
  const char *merchant_url,
  const char *product_id,
  const struct GNUNET_Uuid *uuid,
  struct GNUNET_TIME_Relative duration,
  uint32_t quantity,
  unsigned int http_status)
{
  struct LockProductState *pis;

  pis = GNUNET_new (struct LockProductState);
  pis->merchant_url = merchant_url;
  pis->product_id = product_id;
  pis->http_status = http_status;
  pis->uuid = *uuid;
  pis->duration = duration;
  pis->quantity = quantity;

  {
    struct TALER_TESTING_Command cmd = {
      .cls = pis,
      .label = label,
      .run = &lock_product_run,
      .cleanup = &lock_product_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_lock_product.c */
