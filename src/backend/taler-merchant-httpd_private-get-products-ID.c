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
 * @file backend/taler-merchant-httpd_private-get-products-ID.c
 * @brief implement GET /products/$ID
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-get-products-ID.h"
#include <taler/taler_json_lib.h>


/**
 * Handle a GET "/products/$ID" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_get_products_ID (const struct TMH_RequestHandler *rh,
                             struct MHD_Connection *connection,
                             struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  struct TALER_MERCHANTDB_ProductDetails pd;
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_assert (NULL != mi);
  qs = TMH_db->lookup_product (TMH_db->cls,
                               mi->settings.id,
                               hc->infix,
                               &pd);
  if (0 > qs)
  {
    GNUNET_break (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_GET_PRODUCTS_DB_LOOKUP_ERROR,
                                       "failed to lookup products in database");
  }
  {
    json_t *reply;

    reply = json_pack (
      "{s:s, s:o, s:s, s:o, s:o,"
      " s:I, s:I, s:I, s:o, s:o,"
      " s:o}",
      "description",
      pd.description,
      "unit",
      pd.unit,
      "price",
      TALER_JSON_from_amount (&pd.price),
      "taxes",
      pd.taxes,
      /* end of first group of 5 */
      "total_stocked",
      (UINT64_MAX == pd.total_stocked)
      ? (json_int_t) -1
      : (json_int_t) pd.total_stocked,
      "total_sold",
      (json_int_t) pd.total_sold,
      "total_lost",
      (json_int_t) pd.total_lost,
      "description_i18n",
      pd.description_i18n,
      "location",
      pd.location,
      "image",
      pd.image);
    GNUNET_free (pd.description);
    GNUNET_free (pd.unit);
    if (0 != pd.next_restock.abs_value_us)
      json_object_set_new (reply,
                           "next_restock",
                           GNUNET_JSON_from_time_abs (pd.next_restock));
    return TALER_MHD_reply_json (connection,
                                 reply,
                                 MHD_HTTP_OK);
  }
}


/* end of taler-merchant-httpd_private-get-products-ID.c */
