/*
  This file is part of TALER
  (C) 2019, 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_private-get-products.c
 * @brief implement GET /products
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-get-products.h"


/**
 * Add product details to our JSON array.
 *
 * @param cls a `json_t *` JSON array to build
 * @param key unused
 * @param product_id ID of the product
 * @param in_stock how many are currently in stock (possibly locked), -1 for infinite
 * @param unit in which unit is the stock measured in
 */
static void
add_product (void *cls,
             const struct GNUNET_HashCode *key,
             const char *product_id,
             long long in_stock,
             const char *unit)
{
  json_t *pa = cls;

  (void) key;
  GNUNET_assert (0 ==
                 json_array_append_new (
                   pa,
                   json_pack (
                     "{s:s, s:I, s:s}",
                     "product_id",
                     product_id,
                     "stock",
                     (json_int_t) in_stock,
                     "unit",
                     unit)));
}


/**
 * Handle a GET "/products" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_get_products (const struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          struct TMH_HandlerContext *hc)
{
  json_t *pa;
  enum GNUNET_DB_QueryStatus qs;

  pa = json_array ();
  GNUNET_assert (NULL != pa);
  qs = TMH_db->lookup_products (TMH_db->cls,
                                hc->instance->settings.id,
                                &add_product,
                                pa);
  if (0 > qs)
  {
    GNUNET_break (0);
    json_decref (pa);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_GET_PRODUCTS_DB_LOOKUP_ERROR,
                                       "failed to lookup products in database");
  }
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:o}",
                                    "products", pa);
}


/* end of taler-merchant-httpd_private-get-products.c */
