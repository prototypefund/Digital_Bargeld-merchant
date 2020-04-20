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
 * @file backend/taler-merchant-httpd_private-patch-products.c
 * @brief implementing PATCH /products/$ID request handling
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-patch-products-ID.h"
#include <taler/taler_json_lib.h>


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


/**
 * Determine the cause of the PATCH failure in more detail and report.
 *
 * @param connection connection to report on
 * @param instance_id instance we are processing
 * @param product_id ID of the product to patch
 * @param pd product details we failed to set
 */
static MHD_RESULT
determine_cause (struct MHD_Connection *connection,
                 const char *instance_id,
                 const char *product_id,
                 const struct TALER_MERCHANTDB_ProductDetails *pd)
{
  struct TALER_MERCHANTDB_ProductDetails pdx;
  enum GNUNET_DB_QueryStatus qs;

  qs = TMH_db->lookup_product (TMH_db->cls,
                               instance_id,
                               product_id,
                               &pdx);
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_PRODUCTS_PATCH_DB_COMMIT_HARD_ERROR,
                                       "Failed to get existing product");
  case GNUNET_DB_STATUS_SOFT_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                       "Serialization error for single-statment request");
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_PRODUCTS_PATCH_UNKNOWN_PRODUCT,
                                       "The specified product is unknown");
  case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
    break; /* do below */
  }

  {
    enum TALER_ErrorCode ec;
    const char *hint;

    ec = TALER_EC_INTERNAL_INVARIANT_FAILURE;
    hint = "transaction failed for causes unknown";
    if (pdx.total_lost > pd->total_lost)
    {
      ec = TALER_EC_PRODUCTS_PATCH_TOTAL_LOST_REDUCED;
      hint = "total lost cannot be lowered";
    }
    if (pdx.total_sold > pd->total_sold)
    {
      ec = TALER_EC_PRODUCTS_PATCH_TOTAL_SOLD_REDUCED;
      hint = "total sold cannot be lowered";
    }
    if (pdx.total_stocked > pd->total_stocked)
    {
      ec = TALER_EC_PRODUCTS_PATCH_TOTAL_STOCKED_REDUCED;
      hint = "total stocked cannot be lowered";
    }
    GNUNET_free (pdx.description);
    json_decref (pdx.description_i18n);
    GNUNET_free (pdx.unit);
    json_decref (pdx.taxes);
    json_decref (pdx.image);
    json_decref (pdx.address);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_CONFLICT,
                                       ec,
                                       hint);
  }
}


/**
 * PATCH configuration of an existing instance, given its configuration.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_patch_products_ID (const struct TMH_RequestHandler *rh,
                               struct MHD_Connection *connection,
                               struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  const char *product_id = hc->infix;
  struct TALER_MERCHANTDB_ProductDetails pd;
  int64_t total_stocked;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("description",
                             (const char **) &pd.description),
    GNUNET_JSON_spec_json ("description_i18n",
                           &pd.description_i18n),
    GNUNET_JSON_spec_string ("unit",
                             (const char **) &pd.unit),
    TALER_JSON_spec_amount ("price",
                            &pd.price),
    GNUNET_JSON_spec_json ("image",
                           &pd.image),
    GNUNET_JSON_spec_json ("taxes",
                           &pd.taxes),
    GNUNET_JSON_spec_json ("address",
                           &pd.address),
    GNUNET_JSON_spec_int64 ("total_stocked",
                            &total_stocked),
    GNUNET_JSON_spec_absolute_time ("next_restock",
                                    &pd.next_restock),
    GNUNET_JSON_spec_end ()
  };

  pd.total_sold = 0; /* will be ignored anyway */
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
                                         "Impossible to parse the product description");
  }
  if (-1 == total_stocked)
    pd.total_stocked = UINT64_MAX;
  else
    pd.total_stocked = (uint64_t) total_stocked;
  if (NULL != json_object_get (hc->request_body,
                               "next_restock"))
  {
    enum GNUNET_GenericReturnValue res;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_absolute_time ("next_restock",
                                      &pd.next_restock),
      GNUNET_JSON_spec_end ()
    };

    res = TALER_MHD_parse_json_data (connection,
                                     hc->request_body,
                                     spec);
    /* json is malformed */
    if (GNUNET_NO == res)
    {
      GNUNET_break_op (0);
      GNUNET_JSON_parse_free (spec);
      return MHD_YES;
    }
    /* other internal errors might have occurred */
    if (GNUNET_SYSERR == res)
    {
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                         "Impossible to parse the restock time");
    }
  }
  else
  {
    pd.next_restock.abs_value_us = 0;
  }

  qs = TMH_db->update_product (TMH_db->cls,
                               mi->settings.id,
                               product_id,
                               &pd);
  {
    MHD_RESULT ret;
    switch (qs)
    {
    case GNUNET_DB_STATUS_HARD_ERROR:
      ret = TALER_MHD_reply_with_error (connection,
                                        MHD_HTTP_INTERNAL_SERVER_ERROR,
                                        TALER_EC_PRODUCTS_PATCH_DB_COMMIT_HARD_ERROR,
                                        "Failed to commit change");
      break;
    case GNUNET_DB_STATUS_SOFT_ERROR:
      GNUNET_break (0);
      ret = TALER_MHD_reply_with_error (connection,
                                        MHD_HTTP_INTERNAL_SERVER_ERROR,
                                        TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                        "Serialization error for single-statment request");
      break;
    case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
      ret = determine_cause (connection,
                             mi->settings.id,
                             product_id,
                             &pd);
      break;
    case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
      ret = TALER_MHD_reply_static (connection,
                                    MHD_HTTP_NO_CONTENT,
                                    NULL,
                                    NULL,
                                    0);
      break;
    }
    GNUNET_JSON_parse_free (spec);
    return ret;
  }
}


/* end of taler-merchant-httpd_private-patch-products-ID.c */
