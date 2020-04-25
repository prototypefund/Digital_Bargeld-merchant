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
 * @file backend/taler-merchant-httpd_private-post-products.c
 * @brief implementing POST /products request handling
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-post-products.h"
#include <taler/taler_json_lib.h>


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


/**
 * Check if the two products are identical.
 *
 * @param p1 product to compare
 * @param p2 other product to compare
 * @return true if they are 'equal', false if not or of payto_uris is not an array
 */
static bool
products_equal (const struct TALER_MERCHANTDB_ProductDetails *p1,
                const struct TALER_MERCHANTDB_ProductDetails *p2)
{
  return ( (0 == strcmp (p1->description,
                         p2->description)) &&
           (1 == json_equal (p1->description_i18n,
                             p2->description_i18n)) &&
           (0 == strcmp (p1->unit,
                         p2->unit)) &&
           (0 == TALER_amount_cmp_currency (&p1->price,
                                            &p2->price)) &&
           (0 == TALER_amount_cmp (&p1->price,
                                   &p2->price)) &&
           (1 == json_equal (p1->taxes,
                             p2->taxes)) &&
           (p1->total_stock == p2->total_stock) &&
           (p1->total_sold == p2->total_sold) &&
           (p1->total_lost == p2->total_lost) &&
           (1 == json_equal (p1->image,
                             p2->image)) &&
           (1 == json_equal (p1->address,
                             p2->address)) &&
           (p1->next_restock.abs_value_us ==
            p2->next_restock.abs_value_us) );
}


/**
 * Generate an instance, given its configuration.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_post_products (const struct TMH_RequestHandler *rh,
                           struct MHD_Connection *connection,
                           struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  struct TALER_MERCHANTDB_ProductDetails pd;
  const char *product_id;
  int64_t total_stock;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("product_id",
                             &product_id),
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
    GNUNET_JSON_spec_int64 ("total_stock",
                            &total_stock),
    GNUNET_JSON_spec_absolute_time ("next_restock",
                                    &pd.next_restock),
    GNUNET_JSON_spec_end ()
  };

  GNUNET_assert (NULL != mi);
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
                                         "Impossible to parse the order");
  }

  if (-1 == total_stock)
    pd.total_stock = UINT64_MAX;
  else
    pd.total_stock = (uint64_t) total_stock;
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

  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    /* Test if an product of this id is known */
    struct TALER_MERCHANTDB_ProductDetails epd;

    if (GNUNET_OK !=
        TMH_db->start (TMH_db->cls,
                       "/post products"))
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PRODUCTS_POST_DB_START_ERROR,
                                         "Failed to start transaction");
    }
    qs = TMH_db->lookup_product (TMH_db->cls,
                                 mi->settings.id,
                                 product_id,
                                 &epd);
    switch (qs)
    {
    case GNUNET_DB_STATUS_HARD_ERROR:
      /* Clean up and fail hard */
      break;
    case GNUNET_DB_STATUS_SOFT_ERROR:
      /* restart transaction */
      goto retry;
    case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
      /* Good, we can proceed! */
      break;
    case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
      /* idempotency check: is epd == pd? */
      if (products_equal (&pd,
                          &epd))
      {
        TMH_db->rollback (TMH_db->cls);
        GNUNET_JSON_parse_free (spec);
        return TALER_MHD_reply_static (connection,
                                       MHD_HTTP_NO_CONTENT,
                                       NULL,
                                       NULL,
                                       0);
      }
      else
      {
        TMH_db->rollback (TMH_db->cls);
        GNUNET_JSON_parse_free (spec);
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_CONFLICT,
                                           TALER_EC_PRODUCTS_POST_CONFLICT_PRODUCT_EXISTS,
                                           "different product exists under this product ID");
      }
    }

    qs = TMH_db->insert_product (TMH_db->cls,
                                 mi->settings.id,
                                 product_id,
                                 &pd);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      goto retry;
    qs = TMH_db->commit (TMH_db->cls);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
retry:
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      TMH_db->rollback (TMH_db->cls);
      continue;
    }
  }
  GNUNET_JSON_parse_free (spec);
  if (qs < 0)
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       (GNUNET_DB_STATUS_SOFT_ERROR == qs)
                                       ?
                                       TALER_EC_PRODUCTS_POST_DB_COMMIT_SOFT_ERROR
                                       :
                                       TALER_EC_PRODUCTS_POST_DB_COMMIT_HARD_ERROR,
                                       "Failed to commit transaction");
  return TALER_MHD_reply_static (connection,
                                 MHD_HTTP_NO_CONTENT,
                                 NULL,
                                 NULL,
                                 0);
}


/* end of taler-merchant-httpd_private-post-products.c */
