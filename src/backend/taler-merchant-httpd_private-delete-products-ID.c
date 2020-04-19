/*
  This file is part of TALER
  (C) 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_private-delete-products-ID.c
 * @brief implement DELETE /products/$ID
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-delete-products-ID.h"
#include <taler/taler_json_lib.h>


/**
 * Handle a DELETE "/products/$ID" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_delete_products_ID (const struct TMH_RequestHandler *rh,
                                struct MHD_Connection *connection,
                                struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_assert (NULL != mi);
  qs = TMH_db->delete_product (TMH_db->cls,
                               mi->settings.id,
                               hc->infix);
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_DELETE_PRODUCTS_ID_DB_HARD_FAILURE,
                                       "Transaction failed");
  case GNUNET_DB_STATUS_SOFT_ERROR:
    GNUNET_break (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                       "Serialization error for single SQL statement");
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    qs = TMH_db->lookup_product (TMH_db->cls,
                                 mi->settings.id,
                                 hc->infix,
                                 NULL);
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         TALER_EC_DELETE_PRODUCTS_NO_SUCH_PRODUCT,
                                         "Product unknown");
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_CONFLICT,
                                       TALER_EC_DELETE_PRODUCTS_CONFLICTING_LOCK,
                                       "Product deletion impossible, product is locked");
  case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
    return TALER_MHD_reply_static (connection,
                                   MHD_HTTP_NO_CONTENT,
                                   NULL,
                                   NULL,
                                   0);
  }
  GNUNET_assert (0);
  return MHD_NO;
}


/* end of taler-merchant-httpd_private-delete-products-ID.c */
