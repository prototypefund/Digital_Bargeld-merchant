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
 * @file lib/testing_api_cmd_post_products.c
 * @brief command to test POST /products
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "POST /products" CMD.
 */
struct PostProductsState
{

  /**
   * Handle for a "GET product" request.
   */
  struct TALER_MERCHANT_ProductsPostHandle *iph;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * ID of the product to run POST for.
   */
  const char *product_id;

  /**
   * description of the product
   */
  const char *description;

  /**
   * Map from IETF BCP 47 language tags to localized descriptions
   */
  json_t *description_i18n;

  /**
   * unit in which the product is measured (liters, kilograms, packages, etc.)
   */
  const char *unit;

  /**
   * the price for one @a unit of the product
   */
  struct TALER_Amount price;

  /**
   * base64-encoded product image
   */
  json_t *image;

  /**
   * list of taxes paid by the merchant
   */
  json_t *taxes;

  /**
   * in @e units, -1 to indicate "infinite" (i.e. electronic books)
   */
  int64_t total_stock;

  /**
   * where the product is in stock
   */
  json_t *address;

  /**
   * when the next restocking is expected to happen, 0 for unknown,
   */
  struct GNUNET_TIME_Absolute next_restock;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

};


/**
 * Callback for a POST /products operation.
 *
 * @param cls closure for this function
 */
static void
post_products_cb (void *cls,
                  const struct TALER_MERCHANT_HttpResponse *hr)
{
  struct PostProductsState *pis = cls;

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
  case MHD_HTTP_NO_CONTENT:
    break;
  case MHD_HTTP_CONFLICT:
    break;
  // FIXME: add other legitimate states here...
  default:
    GNUNET_break (0);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Unhandled HTTP status.\n");
  }
  TALER_TESTING_interpreter_next (pis->is);
}


/**
 * Run the "POST /products" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
post_products_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct PostProductsState *pis = cls;

  pis->is = is;
  pis->iph = TALER_MERCHANT_products_post (is->ctx,
                                           pis->merchant_url,
                                           pis->product_id,
                                           pis->description,
                                           pis->description_i18n,
                                           pis->unit,
                                           &pis->price,
                                           pis->image,
                                           pis->taxes,
                                           pis->total_stock,
                                           pis->address,
                                           pis->next_restock,
                                           &post_products_cb,
                                           pis);
  GNUNET_assert (NULL != pis->iph);
}


/**
 * Free the state of a "POST product" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
post_products_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  struct PostProductsState *pis = cls;

  if (NULL != pis->iph)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "POST /products operation did not complete\n");
    TALER_MERCHANT_products_post_cancel (pis->iph);
  }
  json_decref (pis->description_i18n);
  json_decref (pis->image);
  json_decref (pis->taxes);
  json_decref (pis->address);
  GNUNET_free (pis);
}


/**
 * Define a "POST /products" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        POST /products request.
 * @param product_id the ID of the product to query
 * @param description description of the product
 * @param description_i18n Map from IETF BCP 47 language tags to localized descriptions
 * @param unit unit in which the product is measured (liters, kilograms, packages, etc.)
 * @param price the price for one @a unit of the product, zero is used to imply that
 *              this product is not sold separately or that the price is not fixed and
 *              must be supplied by the front-end.  If non-zero, price must include
 *              applicable taxes.
 * @param image base64-encoded product image
 * @param taxes list of taxes paid by the merchant
 * @param total_stock in @a units, -1 to indicate "infinite" (i.e. electronic books)
 * @param address where the product is in stock
 * @param next_restock when the next restocking is expected to happen, 0 for unknown,
 *                     #GNUNET_TIME_UNIT_FOREVER_ABS for 'never'.
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_post_products2 (
  const char *label,
  const char *merchant_url,
  const char *product_id,
  const char *description,
  json_t *description_i18n,
  const char *unit,
  const char *price,
  json_t *image,
  json_t *taxes,
  int64_t total_stock,
  json_t *address,
  struct GNUNET_TIME_Absolute next_restock,
  unsigned int http_status)
{
  struct PostProductsState *pis;

  pis = GNUNET_new (struct PostProductsState);
  pis->merchant_url = merchant_url;
  pis->product_id = product_id;
  pis->http_status = http_status;
  pis->description = description;
  pis->description_i18n = description_i18n; /* ownership taken */
  pis->unit = unit;
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (price,
                                         &pis->price));
  pis->image = image; /* ownership taken */
  pis->taxes = taxes; /* ownership taken */
  pis->total_stock = total_stock;
  pis->address = address; /* ownership taken */
  pis->next_restock = next_restock;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pis,
      .label = label,
      .run = &post_products_run,
      .cleanup = &post_products_cleanup
    };

    return cmd;
  }
}


/**
 * Define a "POST /products" CMD, simple version
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        POST /products request.
 * @param product_id the ID of the product to create
 * @param description name of the product
 * @param price price of the product
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_post_products (const char *label,
                                          const char *merchant_url,
                                          const char *product_id,
                                          const char *description,
                                          const char *price,
                                          unsigned int http_status)
{
  return TALER_TESTING_cmd_merchant_post_products2 (
    label,
    merchant_url,
    product_id,
    description,
    json_pack ("{s:s}", "en", description),
    "test-unit",
    price,
    json_object (),
    json_object (),
    4,
    json_pack ("{s:s}", "street", "my street"),
    GNUNET_TIME_UNIT_ZERO_ABS,
    http_status);
}


/* end of testing_api_cmd_post_products.c */
