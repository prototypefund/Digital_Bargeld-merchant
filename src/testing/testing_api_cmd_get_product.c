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
 * @file lib/testing_api_cmd_get_product.c
 * @brief command to test GET /product/$ID
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "GET product" CMD.
 */
struct GetProductState
{

  /**
   * Handle for a "GET product" request.
   */
  struct TALER_MERCHANT_ProductGetHandle *igh;

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
   * Reference for a POST or PATCH /products CMD (optional).
   */
  const char *product_reference;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

};


/**
 * Callback for a /get/product/$ID operation.
 *
 * @param cls closure for this function
 * @param hr HTTP response details
 * @param description description of the product
 * @param description_i18n Map from IETF BCP 47 language tags to localized descriptions
 * @param unit unit in which the product is measured (liters, kilograms, packages, etc.)
 * @param price the price for one @a unit of the product, zero is used to imply that
 *              this product is not sold separately or that the price is not fixed and
 *              must be supplied by the front-end.  If non-zero, price must include
 *              applicable taxes.
 * @param image base64-encoded product image
 * @param taxes list of taxes paid by the merchant
 * @param total_stocked in @a units, -1 to indicate "infinite" (i.e. electronic books),
 *                does NOT indicate remaining stocks, to get remaining stocks,
 *                subtract @a total_sold and @a total_lost. Note that this still
 *                does not then say how many of the remaining inventory are locked.
 * @param total_sold in @a units, total number of @a unit of product sold
 * @param total_lost in @a units, total number of @a unit of product lost from inventory
 * @param location where the product is in stock
 * @param next_restock when the next restocking is expected to happen, 0 for unknown,
 *                     #GNUNET_TIME_UNIT_FOREVER_ABS for 'never'.
 */
static void
get_product_cb (void *cls,
                const struct TALER_MERCHANT_HttpResponse *hr,
                const char *description,
                const json_t *description_i18n,
                const char *unit,
                const struct TALER_Amount *price,
                const json_t *image,
                const json_t *taxes,
                int64_t total_stocked,
                uint64_t total_sold,
                uint64_t total_lost,
                const json_t *location,
                struct GNUNET_TIME_Absolute next_restock)
{
  /* FIXME, deeper checks should be implemented here. */
  struct GetProductState *gis = cls;

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
 * Run the "GET product" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
get_product_run (void *cls,
                 const struct TALER_TESTING_Command *cmd,
                 struct TALER_TESTING_Interpreter *is)
{
  struct GetProductState *gis = cls;

  gis->is = is;
  gis->igh = TALER_MERCHANT_product_get (is->ctx,
                                         gis->merchant_url,
                                         gis->product_id,
                                         &get_product_cb,
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
get_product_cleanup (void *cls,
                     const struct TALER_TESTING_Command *cmd)
{
  struct GetProductState *gis = cls;

  if (NULL != gis->igh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "GET /products/$ID operation did not complete\n");
    TALER_MERCHANT_product_get_cancel (gis->igh);
  }
  GNUNET_free (gis);
}


/**
 * Define a "GET product" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        GET /products/$ID request.
 * @param product_id the ID of the product to query
 * @param http_status expected HTTP response code.
 * @param product_reference reference to a "POST /products" or "PATCH /products/$ID" CMD
 *        that will provide what we expect the backend to return to us
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_get_product (const char *label,
                                        const char *merchant_url,
                                        const char *product_id,
                                        unsigned int http_status,
                                        const char *product_reference)
{
  struct GetProductState *gis;

  gis = GNUNET_new (struct GetProductState);
  gis->merchant_url = merchant_url;
  gis->product_id = product_id;
  gis->http_status = http_status;
  gis->product_reference = product_reference;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = gis,
      .label = label,
      .run = &get_product_run,
      .cleanup = &get_product_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_get_product.c */
