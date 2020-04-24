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
 * @file lib/testing_api_cmd_patch_product.c
 * @brief command to test PATCH /product
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "PATCH /product" CMD.
 */
struct PatchProductState
{

  /**
   * Handle for a "GET product" request.
   */
  struct TALER_MERCHANT_ProductPatchHandle *iph;

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
  int64_t total_stocked;

  /**
   * in @e units.
   */
  int64_t total_lost;

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
 * Callback for a PATCH /products/$ID operation.
 *
 * @param cls closure for this function
 */
static void
patch_product_cb (void *cls,
                  const struct TALER_MERCHANT_HttpResponse *hr)
{
  struct PatchProductState *pis = cls;

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
 * Run the "PATCH /products/$ID" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
patch_product_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct PatchProductState *pis = cls;

  pis->is = is;
  pis->iph = TALER_MERCHANT_product_patch (is->ctx,
                                           pis->merchant_url,
                                           pis->product_id,
                                           pis->description,
                                           pis->description_i18n,
                                           pis->unit,
                                           &pis->price,
                                           pis->image,
                                           pis->taxes,
                                           pis->total_stocked,
                                           pis->total_lost,
                                           pis->address,
                                           pis->next_restock,
                                           &patch_product_cb,
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
patch_product_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  struct PatchProductState *pis = cls;

  if (NULL != pis->iph)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "PATCH /products/$ID operation did not complete\n");
    TALER_MERCHANT_product_patch_cancel (pis->iph);
  }
  json_decref (pis->description_i18n);
  json_decref (pis->image);
  json_decref (pis->taxes);
  json_decref (pis->address);
  GNUNET_free (pis);
}


/**
 * Define a "PATCH /products/$ID" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        PATCH /product request.
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
 * @param total_stocked in @a units, -1 to indicate "infinite" (i.e. electronic books)
 * @param total_lost in @a units, must be larger than previous values, and may
 *               not exceed total_stocked minus total_sold; if it does, the transaction
 *               will fail with a #MHD_HTTP_CONFLICT HTTP status code
 * @param address where the product is in stock
 * @param next_restock when the next restocking is expected to happen, 0 for unknown,
 *                     #GNUNET_TIME_UNIT_FOREVER_ABS for 'never'.
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_patch_product (
  const char *label,
  const char *merchant_url,
  const char *product_id,
  const char *description,
  json_t *description_i18n,
  const char *unit,
  const struct TALER_Amount *price,
  json_t *image,
  json_t *taxes,
  int64_t total_stocked,
  uint64_t total_lost,
  json_t *address,
  struct GNUNET_TIME_Absolute next_restock,
  unsigned int http_status)
{
  struct PatchProductState *pis;

  pis = GNUNET_new (struct PatchProductState);
  pis->merchant_url = merchant_url;
  pis->product_id = product_id;
  pis->http_status = http_status;
  pis->description = description;
  pis->description_i18n = description_i18n; /* ownership taken */
  pis->unit = unit;
  pis->price = *price;
  pis->image = image; /* ownership taken */
  pis->taxes = taxes; /* ownership taken */
  pis->total_stocked = total_stocked;
  pis->total_lost = total_lost;
  pis->address = address; /* ownership taken */
  pis->next_restock = next_restock; {
    struct TALER_TESTING_Command cmd = {
      .cls = pis,
      .label = label,
      .run = &patch_product_run,
      .cleanup = &patch_product_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_patch_product.c */
