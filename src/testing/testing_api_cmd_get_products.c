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
 * @file lib/testing_api_cmd_get_products.c
 * @brief command to test GET /products
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "GET products" CMD.
 */
struct GetProductsState
{

  /**
   * Handle for a "GET product" request.
   */
  struct TALER_MERCHANT_ProductsGetHandle *igh;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

};


/**
 * Callback for a GET /products operation.
 *
 * @param cls closure for this function
 * @param hr HTTP response details
 * @param products_length length of the @a products array
 * @param products array of products the requested instance offers
 */
static void
get_products_cb (void *cls,
                 const struct TALER_MERCHANT_HttpResponse *hr,
                 unsigned int products_length,
                 const struct TALER_MERCHANT_InventoryEntry products[])
{
  /* FIXME, deeper checks should be implemented here. */
  struct GetProductsState *gis = cls;

  gis->igh = NULL;
  if (gis->http_status != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                (int) hr->ec,
                TALER_TESTING_interpreter_get_current_label (gis->is));
    TALER_TESTING_interpreter_fail (gis->is);
    return;
  }
  switch (hr->http_status)
  {
  case MHD_HTTP_OK:
    // FIXME: use gis->product_reference here to
    // check if the data returned matches that from the POST / PATCH
    break;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Unhandled HTTP status.\n");
  }
  TALER_TESTING_interpreter_next (gis->is);
}


/**
 * Run the "GET /products" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
get_products_run (void *cls,
                  const struct TALER_TESTING_Command *cmd,
                  struct TALER_TESTING_Interpreter *is)
{
  struct GetProductsState *gis = cls;

  gis->is = is;
  gis->igh = TALER_MERCHANT_products_get (is->ctx,
                                          gis->merchant_url,
                                          &get_products_cb,
                                          gis);
  GNUNET_assert (NULL != gis->igh);
}


/**
 * Free the state of a "GET product" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
get_products_cleanup (void *cls,
                      const struct TALER_TESTING_Command *cmd)
{
  struct GetProductsState *gis = cls;

  if (NULL != gis->igh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "GET /products/$ID operation did not complete\n");
    TALER_MERCHANT_products_get_cancel (gis->igh);
  }
  GNUNET_free (gis);
}


/**
 * Define a "GET /products" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        GET /products request.
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_get_products (const char *label,
                                         const char *merchant_url,
                                         unsigned int http_status)
{
  struct GetProductsState *gis;

  gis = GNUNET_new (struct GetProductsState);
  gis->merchant_url = merchant_url;
  gis->http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = gis,
      .label = label,
      .run = &get_products_run,
      .cleanup = &get_products_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_get_products.c */
