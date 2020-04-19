/*
  This file is part of TALER
  (C) 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation; either version 3,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not,
  see <http://www.gnu.org/licenses/>
*/

/**
 * @file backend/taler-merchant-httpd_private-post-products-ID-lock.c
 * @brief implementing POST /products/$ID/lock request handling
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-post-products-ID-lock.h"
#include <taler/taler_json_lib.h>


/**
 * Lock an existing product.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_post_products_ID_lock (const struct TMH_RequestHandler *rh,
                                   struct MHD_Connection *connection,
                                   struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  const char *product_id = hc->infix;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_Uuid uuid;
  uint32_t quantity;
  struct GNUNET_TIME_Relative duration;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("lock_uuid",
                                 &uuid),
    GNUNET_JSON_spec_uint32 ("quantity",
                             &quantity),
    GNUNET_JSON_spec_relative_time ("duration",
                                    &duration),
    GNUNET_JSON_spec_end ()
  };

  GNUNET_assert (NULL != mi);
  GNUNET_assert (NULL != product_id);
  {
    enum GNUNET_GenericReturnValue res;

    res = TALER_MHD_parse_json_data (connection,
                                     hc->request_body,
                                     spec);
    /* json is malformed */
    if (GNUNET_NO == res)
    {
      GNUNET_break_op (0);
      return MHD_YES;
    }
    /* other internal errors might have occurred */
    if (GNUNET_SYSERR == res)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                         "Impossible to parse the lock request");
  }

  qs = TMH_db->lock_product (TMH_db->cls,
                             mi->settings.id,
                             product_id,
                             &uuid,
                             quantity,
                             GNUNET_TIME_relative_to_absolute (duration));
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_PRODUCTS_PATCH_DB_COMMIT_HARD_ERROR,
                                       "Failed to execute DB transaction to lock product");
  case GNUNET_DB_STATUS_SOFT_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                       "Serialization error for single-statment request");
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    qs = TMH_db->lookup_product (TMH_db->cls,
                                 mi->settings.id,
                                 product_id,
                                 NULL);
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         TALER_EC_PRODUCTS_LOCK_UNKNOWN_PRODUCT,
                                         "The specified product is unknown");
    else
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_CONFLICT,
                                         TALER_EC_PRODUCTS_LOCK_INSUFFICIENT_STOCKS,
                                         "The specified product is out of stock");
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


/* end of taler-merchant-httpd_private-patch-products-ID-lock.c */
