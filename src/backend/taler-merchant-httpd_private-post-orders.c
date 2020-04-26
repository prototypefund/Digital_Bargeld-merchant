/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2018, 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_private-post-orders.c
 * @brief the POST /orders handler
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd_private-post-orders.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3

/**
 * What is the label under which we find/place the merchant's
 * jurisdiction in the locations list by default?
 */
#define STANDARD_LABEL_MERCHANT_JURISDICTION "_mj"

/**
 * What is the label under which we find/place the merchant's
 * address in the locations list by default?
 */
#define STANDARD_LABEL_MERCHANT_ADDRESS "_ma"


/**
 * Check that the given JSON array of products is well-formed.
 *
 * @param products JSON array to check
 * @return #GNUNET_OK if all is fine
 */
static int
check_products (json_t *products)
{
  size_t index;
  json_t *value;

  if (! json_is_array (products))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  json_array_foreach (products, index, value) {
    const char *description;
    const char *error_name;
    unsigned int error_line;
    int res;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("description", &description),
      GNUNET_JSON_spec_end ()
    };

    /* extract fields we need to sign separately */
    res = GNUNET_JSON_parse (value,
                             spec,
                             &error_name,
                             &error_line);
    if (GNUNET_OK != res)
    {
      GNUNET_break (0);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Product parsing failed at #%u: %s:%u\n",
                  (unsigned int) index,
                  error_name,
                  error_line);
      return GNUNET_SYSERR;
    }
    GNUNET_JSON_parse_free (spec);
  }
  return GNUNET_OK;
}


/**
 * Generate the base URL for the given merchant instance.
 *
 * @param connection the MHD connection
 * @param instance_id the merchant instance ID
 * @returns the merchant instance's base URL
 */
static char *
make_merchant_base_url (struct MHD_Connection *connection,
                        const char *instance_id)
{
  const char *host;
  const char *forwarded_host;
  const char *uri_path;
  struct GNUNET_Buffer buf = { 0 };

  if (GNUNET_YES == TALER_mhd_is_https (connection))
    GNUNET_buffer_write_str (&buf, "https://");
  else
    GNUNET_buffer_write_str (&buf, "http://");
  host = MHD_lookup_connection_value (connection,
                                      MHD_HEADER_KIND,
                                      "Host");
  forwarded_host = MHD_lookup_connection_value (connection,
                                                MHD_HEADER_KIND,
                                                "X-Forwarded-Host");
  if (NULL != forwarded_host)
  {
    GNUNET_buffer_write_str (&buf,
                             forwarded_host);
  }
  else
  {
    GNUNET_assert (NULL != host);
    GNUNET_buffer_write_str (&buf,
                             host);
  }
  uri_path = MHD_lookup_connection_value (connection,
                                          MHD_HEADER_KIND,
                                          "X-Forwarded-Prefix");
  if (NULL != uri_path)
  {
    /* Currently the merchant backend is only supported at the root of the path,
       this might change in the future.  */
    GNUNET_assert (0);
  }
  if (0 != strcmp (instance_id,
                   "default"))
  {
    GNUNET_buffer_write_path (&buf,
                              "/instances/");
    GNUNET_buffer_write_str (&buf,
                             instance_id);
  }
  GNUNET_buffer_write_path (&buf,
                            "");
  return GNUNET_buffer_reap_str (&buf);
}


/**
 * Information about a product we are supposed to add to the order
 * based on what we know it from our inventory.
 */
struct InventoryProduct
{
  /**
   * Identifier of the product in the inventory.
   */
  const char *product_id;

  /**
   * Number of units of the product to add to the order.
   */
  uint32_t quantity;
};


/**
 * Execute the database transaction to setup the order.
 *
 * @param hc handler context for the request
 * @param[in] order order to process (not modified)
 * @param inventory_products_length length of the @a inventory_products array
 * @param inventory_products array of products to add to @a order from our inventory
 * @param uuids_length length of the @a uuids array
 * @param uuids array of UUIDs used to reserve products from @a inventory_products
 * @return transaction status, 0 if @a uuids were insufficient to reserve required inventory
 */
static enum GNUNET_DB_QueryStatus
execute_transaction (struct TMH_HandlerContext *hc,
                     const char *order_id,
                     struct GNUNET_TIME_Absolute pay_deadline,
                     json_t *order,
                     unsigned int inventory_products_length,
                     const struct InventoryProduct inventory_products[],
                     unsigned int uuids_length,
                     const struct GNUNET_Uuid uuids[])
{
  enum GNUNET_DB_QueryStatus qs;

  if (GNUNET_OK !=
      TMH_db->start (TMH_db->cls,
                     "insert_order"))
  {
    GNUNET_break (0);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  /* Setup order */
  qs = TMH_db->insert_order (TMH_db->cls,
                             hc->instance->settings.id,
                             order_id,
                             pay_deadline,
                             order);
  if (qs < 0)
  {
    TMH_db->rollback (TMH_db->cls);
    return qs;
  }
  GNUNET_assert (qs > 0);
  /* Migrate locks from UUIDs to new order: first release old locks */
  for (unsigned int i = 0; i<uuids_length; i++)
  {
    qs = TMH_db->unlock_inventory (TMH_db->cls,
                                   &uuids[i]);
    if (qs < 0)
    {
      TMH_db->rollback (TMH_db->cls);
      return qs;
    }
    /* qs == 0 is OK here, that just means we did not HAVE any lock under this
       UUID */
  }
  /* Migrate locks from UUIDs to new order: acquire new locks
     (note: this can basically ONLY fail on serializability OR
     because the UUID locks were insufficient for the desired
     quantities). */
  for (unsigned int i = 0; i<inventory_products_length; i++)
  {
    qs = TMH_db->insert_order_lock (TMH_db->cls,
                                    hc->instance->settings.id,
                                    order_id,
                                    inventory_products[i].product_id,
                                    inventory_products[i].quantity);
    if (qs <= 0)
    {
      /* qs == 0: lock acquisition failed due to insufficient stocks */
      TMH_db->rollback (TMH_db->cls);
      return qs;
    }
  }
  /* finally, commit transaction (note: if it fails, we ALSO re-acquire
     the UUID locks, which is exactly what we want) */
  qs = TMH_db->commit (TMH_db->cls);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT; /* 1 == success! */
  return qs;
}


/**
 * Transform an order into a proposal and store it in the
 * database. Write the resulting proposal or an error message
 * of a MHD connection.
 *
 * @param connection connection to write the result or error to
 * @param hc handler context for the request
 * @param[in,out] order order to process (can be modified)
 * @param inventory_products_length length of the @a inventory_products array
 * @param inventory_products array of products to add to @a order from our inventory
 * @param uuids_length length of the @a uuids array
 * @param uuids array of UUIDs used to reserve products from @a inventory_products
 * @return MHD result code
 */
static MHD_RESULT
execute_order (struct MHD_Connection *connection,
               struct TMH_HandlerContext *hc,
               json_t *order,
               unsigned int inventory_products_length,
               const struct InventoryProduct inventory_products[],
               unsigned int uuids_length,
               const struct GNUNET_Uuid uuids[])
{
  const struct TALER_MERCHANTDB_InstanceSettings *settings =
    &hc->instance->settings;
  struct TALER_Amount total;
  const char *order_id;
  const char *summary;
  const char *fulfillment_url;
  json_t *products;
  json_t *merchant;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct GNUNET_TIME_Absolute wire_transfer_deadline;
  struct GNUNET_TIME_Absolute pay_deadline;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("amount",
                            &total),
    GNUNET_JSON_spec_string ("order_id",
                             &order_id),
    GNUNET_JSON_spec_string ("summary",
                             &summary),
    GNUNET_JSON_spec_string ("fulfillment_url",
                             &fulfillment_url),
    /**
     * The following entries we don't actually need,
     * except to check that the order is well-formed */
    GNUNET_JSON_spec_json ("products",
                           &products),
    GNUNET_JSON_spec_json ("merchant",
                           &merchant),
    GNUNET_JSON_spec_absolute_time ("timestamp",
                                    &timestamp),
    GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                    &refund_deadline),
    GNUNET_JSON_spec_absolute_time ("pay_deadline",
                                    &pay_deadline),
    GNUNET_JSON_spec_absolute_time ("wire_transfer_deadline",
                                    &wire_transfer_deadline),
    GNUNET_JSON_spec_end ()
  };
  enum GNUNET_DB_QueryStatus qs;

  /* extract fields we need to sign separately */
  {
    enum GNUNET_GenericReturnValue res;

    res = TALER_MHD_parse_json_data (connection,
                                     order,
                                     spec);
    /* json is malformed */
    if (GNUNET_NO == res)
    {
      return MHD_YES;
    }
    /* other internal errors might have occurred */
    if (GNUNET_SYSERR == res)
    {
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PROPOSAL_ORDER_PARSE_ERROR,
                                         "Impossible to parse the order");
    }
  }
  if (0 !=
      strcasecmp (total.currency,
                  TMH_currency))
  {
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (
      connection,
      MHD_HTTP_BAD_REQUEST,
      TALER_EC_PROPOSAL_ORDER_BAD_CURRENCY,
      "Total amount must be in currency supported by backend");
  }

  if (wire_transfer_deadline.abs_value_us <
      refund_deadline.abs_value_us)
  {
    GNUNET_JSON_parse_free (spec);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "invariant failed: wire_transfer_deadline >= refund_deadline\n");
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "wire_transfer_deadline: %s\n",
                GNUNET_STRINGS_absolute_time_to_string (
                  wire_transfer_deadline));
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "refund_deadline: %s\n",
                GNUNET_STRINGS_absolute_time_to_string (refund_deadline));
    return TALER_MHD_reply_with_error (
      connection,
      MHD_HTTP_BAD_REQUEST,
      TALER_EC_PARAMETER_MALFORMED,
      "order:wire_transfer_deadline;order:refund_deadline");
  }


  /* check contract is well-formed */
  if (GNUNET_OK != check_products (products))
  {
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "order:products");
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Executing database transaction to create order '%s' for instance '%s'\n",
              order_id,
              settings->id);
  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    TMH_db->preflight (TMH_db->cls);
    qs = execute_transaction (hc,
                              order_id,
                              pay_deadline,
                              order,
                              inventory_products_length,
                              inventory_products,
                              uuids_length,
                              uuids);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    /* Special report if retries insufficient */
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      GNUNET_break (0);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PROPOSAL_STORE_DB_ERROR_SOFT,
                                         "serialization error, maybe try again?");
    }

    {
      /* Hard error could be constraint violation,
         check if order already exists */
      TMH_db->preflight (TMH_db->cls);
      qs = TMH_db->lookup_order (TMH_db->cls,
                                 settings->id,
                                 order_id,
                                 NULL);
      if (0 < qs)
      {
        /* Yep, indeed uniqueness constraint violation */
        int rv;
        char *msg;

        GNUNET_JSON_parse_free (spec);
        GNUNET_asprintf (&msg,
                         "order ID `%s' already exists",
                         order_id);
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Order `%s' already exists\n",
                    order_id);
        /* contract_terms may be private, only expose
         * duplicate order_id to the network */
        rv = TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_BAD_REQUEST, /* or conflict? */
                                         TALER_EC_PROPOSAL_STORE_DB_ERROR_ALREADY_EXISTS,
                                         msg);
        GNUNET_free (msg);
        return rv;
      }
    }

    /* Other hard transaction error (disk full, etc.) */
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (
      connection,
      MHD_HTTP_INTERNAL_SERVER_ERROR,
      TALER_EC_PROPOSAL_STORE_DB_ERROR_HARD,
      "Failed to store the order in the DB");
  }
  /* DB transaction succeeded, generate positive response */
  {
    MHD_RESULT ret;

    ret = TALER_MHD_reply_json_pack (connection,
                                     MHD_HTTP_OK,
                                     "{s:s}",
                                     "order_id",
                                     order_id);
    GNUNET_JSON_parse_free (spec);
    return ret;
  }
}


/**
 * Add missing fields to the order.  Upon success, continue
 * processing with execute_order().
 *
 * @param connection connection to write the result or error to
 * @param hc handler context for the request
 * @param[in,out] order order to process (can be modified)
 * @param inventory_products_length length of the @a inventory_products array
 * @param inventory_products array of products to add to @a order from our inventory
 * @param uuids_length length of the @a uuids array
 * @param uuids array of UUIDs used to reserve products from @a inventory_products
 * @return MHD result code
 */
static MHD_RESULT
patch_order (struct MHD_Connection *connection,
             struct TMH_HandlerContext *hc,
             json_t *order,
             unsigned int inventory_products_length,
             const struct InventoryProduct inventory_products[],
             unsigned int uuids_length,
             const struct GNUNET_Uuid uuids[])
{
  const struct TALER_MERCHANTDB_InstanceSettings *settings =
    &hc->instance->settings;

  /* Add order_id if it doesn't exist. */
  if (NULL ==
      json_string_value (json_object_get (order,
                                          "order_id")))
  {
    char buf[256];
    time_t timer;
    struct tm*tm_info;
    size_t off;
    uint64_t rand;
    char *last;

    time (&timer);
    tm_info = localtime (&timer);
    if (NULL == tm_info)
    {
      return TALER_MHD_reply_with_error
               (connection,
               MHD_HTTP_INTERNAL_SERVER_ERROR,
               TALER_EC_PROPOSAL_NO_LOCALTIME,
               "failed to determine local time");
    }
    off = strftime (buf,
                    sizeof (buf),
                    "%Y.%j",
                    tm_info);
    buf[off++] = '-';
    rand = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                     UINT64_MAX);
    last = GNUNET_STRINGS_data_to_string (&rand,
                                          sizeof (uint64_t),
                                          &buf[off],
                                          sizeof (buf) - off);
    *last = '\0';
    json_object_set_new (order,
                         "order_id",
                         json_string (buf));
  }

  /* Add timestamp if it doesn't exist */
  if (NULL == json_object_get (order,
                               "timestamp"))
  {
    struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();

    (void) GNUNET_TIME_round_abs (&now);
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "timestamp",
                                        GNUNET_JSON_from_time_abs (now)));
  }

  /* If no refund_deadline given, set one as zero.  */
  if (NULL == json_object_get (order,
                               "refund_deadline"))
  {
    struct GNUNET_TIME_Absolute zero = { 0 };

    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "refund_deadline",
                                        GNUNET_JSON_from_time_abs (zero)));
  }

  if (NULL == json_object_get (order,
                               "pay_deadline"))
  {
    struct GNUNET_TIME_Absolute t;

    t = GNUNET_TIME_relative_to_absolute (settings->default_pay_delay);
    (void) GNUNET_TIME_round_abs (&t);
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "pay_deadline",
                                        GNUNET_JSON_from_time_abs (t)));
  }

  if (NULL == json_object_get (order,
                               "wire_transfer_deadline"))
  {
    struct GNUNET_TIME_Absolute t;
    t = GNUNET_TIME_relative_to_absolute (
      settings->default_wire_transfer_delay);
    (void) GNUNET_TIME_round_abs (&t);
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "wire_transfer_deadline",
                                        GNUNET_JSON_from_time_abs (t)));
  }

  if (NULL == json_object_get (order,
                               "max_wire_fee"))
  {
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "max_wire_fee",
                                        TALER_JSON_from_amount
                                          (&settings->default_max_wire_fee)));
  }

  if (NULL == json_object_get (order,
                               "max_fee"))
  {
    GNUNET_assert (0 ==
                   json_object_set_new (
                     order,
                     "max_fee",
                     TALER_JSON_from_amount
                       (&settings->default_max_deposit_fee)));
  }

  if (NULL == json_object_get (order,
                               "wire_fee_amortization"))
  {
    GNUNET_assert (0 ==
                   json_object_set_new (
                     order,
                     "wire_fee_amortization",
                     json_integer
                       ((json_int_t) settings->default_wire_fee_amortization)));
  }

  if (NULL == json_object_get (order,
                               "merchant_base_url"))
  {
    char *url;

    url = make_merchant_base_url (connection,
                                  settings->id);
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "merchant_base_url",
                                        json_string (url)));
    GNUNET_free (url);
  }


  /* Fill in merchant information if necessary */
  if (NULL == json_object_get (order,
                               "merchant"))
  {
    const char *mj = NULL;
    const char *ma = NULL;
    json_t *locations;
    json_t *jmerchant;

    jmerchant = json_object ();
    GNUNET_assert (NULL != jmerchant);
    GNUNET_assert (0 ==
                   json_object_set_new (jmerchant,
                                        "name",
                                        json_string (settings->name)));
    GNUNET_assert (0 ==
                   json_object_set_new (jmerchant,
                                        "instance",
                                        json_string (settings->id)));
    locations = json_object_get (order,
                                 "locations");
    if (NULL != locations)
    {
      json_t *loca;
      json_t *locj;

      /* Handle merchant address */
      loca = settings->address;
      if (NULL != loca)
      {
        loca = json_deep_copy (loca);
        ma = STANDARD_LABEL_MERCHANT_ADDRESS;
        GNUNET_assert (0 ==
                       json_object_set_new (locations,
                                            ma,
                                            loca));
        GNUNET_assert (0 ==
                       json_object_set_new (jmerchant,
                                            "address",
                                            json_string (ma)));
      }

      /* Handle merchant jurisdiction */
      locj = settings->jurisdiction;
      if (NULL != locj)
      {
        if ( (NULL != loca) &&
             (1 == json_equal (locj,
                               loca)) )
        {
          /* addresses equal, re-use */
          mj = ma;
        }
        else
        {
          locj = json_deep_copy (locj);
          mj = STANDARD_LABEL_MERCHANT_JURISDICTION;
          GNUNET_assert (0 ==
                         json_object_set_new (locations,
                                              mj,
                                              locj));
        }
        GNUNET_assert (0 ==
                       json_object_set_new (jmerchant,
                                            "jurisdiction",
                                            json_string (mj)));
      }
    } /* have locations */
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "merchant",
                                        jmerchant));
  } /* needed to synthesize merchant info */

  /* add fields to the contract that the backend should provide */
  GNUNET_assert (0 ==
                 json_object_set (order,
                                  "exchanges",
                                  TMH_trusted_exchanges));
  GNUNET_assert (0 ==
                 json_object_set (order,
                                  "auditors",
                                  j_auditors));
  GNUNET_assert (0 ==
                 json_object_set_new (order,
                                      "merchant_pub",
                                      GNUNET_JSON_from_data_auto (
                                        &hc->instance->merchant_pub)));
  return execute_order (connection,
                        hc,
                        order,
                        inventory_products_length,
                        inventory_products,
                        uuids_length,
                        uuids);
}


/**
 * Process the @a payment_target and add the details of how the
 * order could be paid to @a order. On success, continue
 * processing with patch_order().
 *
 * @param connection connection to write the result or error to
 * @param hc handler context for the request
 * @param[in,out] order order to process (can be modified)
 * @param payment_target desired wire method, NULL for no preference
 * @param inventory_products_length length of the @a inventory_products array
 * @param inventory_products array of products to add to @a order from our inventory
 * @param uuids_length length of the @a uuids array
 * @param uuids array of UUIDs used to reserve products from @a inventory_products
 * @return MHD result code
 */
static MHD_RESULT
add_payment_details (struct MHD_Connection *connection,
                     struct TMH_HandlerContext *hc,
                     json_t *order,
                     const char *payment_target,
                     unsigned int inventory_products_length,
                     const struct InventoryProduct inventory_products[],
                     unsigned int uuids_length,
                     const struct GNUNET_Uuid uuids[])
{
  struct TMH_WireMethod *wm;

  wm = hc->instance->wm_head;
  if (NULL != payment_target)
  {
    while ( (NULL != wm) &&
            (GNUNET_YES == wm->active) &&
            (0 != strcasecmp (payment_target,
                              wm->wire_method) ) )
      wm = wm->next;
  }
  if (GNUNET_YES != wm->active)
    wm = NULL;
  if (NULL == wm)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No wire method available for instance '%s'\n",
                hc->instance->settings.id);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_PROPOSAL_INSTANCE_CONFIGURATION_LACKS_WIRE,
                                       "No wire method configured for instance");
  }
  GNUNET_assert (0 ==
                 json_object_set_new (order,
                                      "h_wire",
                                      GNUNET_JSON_from_data_auto (
                                        &wm->h_wire)));
  GNUNET_assert (0 ==
                 json_object_set_new (order,
                                      "wire_method",
                                      json_string (wm->wire_method)));
  return patch_order (connection,
                      hc,
                      order,
                      inventory_products_length,
                      inventory_products,
                      uuids_length,
                      uuids);
}


/**
 * Merge the inventory products into @a order, querying the
 * database about the details of those products. Upon success,
 * continue processing by calling add_payment_details().
 *
 * @param connection connection to write the result or error to
 * @param hc handler context for the request
 * @param[in,out] order order to process (can be modified)
 * @param inventory_products_length length of the @a inventory_products array
 * @param inventory_products array of products to add to @a order from our inventory
 * @param uuids_length length of the @a uuids array
 * @param uuids array of UUIDs used to reserve products from @a inventory_products
 * @return MHD result code
 */
static MHD_RESULT
merge_inventory (struct MHD_Connection *connection,
                 struct TMH_HandlerContext *hc,
                 json_t *order,
                 const char *payment_target,
                 unsigned int inventory_products_length,
                 const struct InventoryProduct inventory_products[],
                 unsigned int uuids_length,
                 const struct GNUNET_Uuid uuids[])
{
  if (NULL == json_object_get (order,
                               "products"))
  {
    GNUNET_assert (0 ==
                   json_object_set_new (order,
                                        "products",
                                        json_array ()));
  }

  {
    json_t *np = json_array ();

    for (unsigned int i = 0; i<inventory_products_length; i++)
    {
      struct TALER_MERCHANTDB_ProductDetails pd;
      enum GNUNET_DB_QueryStatus qs;

      qs = TMH_db->lookup_product (TMH_db->cls,
                                   hc->instance->settings.id,
                                   inventory_products[i].product_id,
                                   &pd);
      if (qs <= 0)
      {
        enum TALER_ErrorCode ec;
        unsigned int http_status;

        switch (qs)
        {
        case GNUNET_DB_STATUS_HARD_ERROR:
          http_status = MHD_HTTP_INTERNAL_SERVER_ERROR;
          ec = TALER_EC_ORDERS_LOOKUP_PRODUCT_DB_HARD_FAILURE;
          break;
        case GNUNET_DB_STATUS_SOFT_ERROR:
          GNUNET_break (0);
          http_status = MHD_HTTP_INTERNAL_SERVER_ERROR;
          ec = TALER_EC_ORDERS_LOOKUP_PRODUCT_DB_SOFT_FAILURE;
          break;
        case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
          http_status = MHD_HTTP_NOT_FOUND;
          ec = TALER_EC_ORDERS_LOOKUP_PRODUCT_NOT_FOUND;
          break;
        case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
          /* case listed to make compilers happy */
          GNUNET_assert (0);
        }
        json_decref (np);
        return TALER_MHD_reply_with_error (connection,
                                           http_status,
                                           ec,
                                           inventory_products[i].product_id);
      }
      {
        json_t *p;

        p = json_pack ("{s:s, s:o, s:s, s:o, s:o, s:o}",
                       "description",
                       pd.description,
                       "description_i18n",
                       pd.description_i18n,
                       "unit",
                       pd.unit,
                       "price",
                       TALER_JSON_from_amount (&pd.price),
                       "taxes",
                       pd.taxes,
                       "image",
                       pd.image);
        GNUNET_assert (NULL != p);
        GNUNET_assert (0 ==
                       json_array_append_new (np,
                                              p));
      }
      GNUNET_free (pd.description);
      GNUNET_free (pd.unit);
      json_decref (pd.address);
    }
    /* merge into existing products list */
    {
      json_t *xp;

      xp = json_object_get (order,
                            "products");
      GNUNET_assert (NULL != xp);
      json_array_extend (xp, np);
      json_decref (np);
    }
  }
  return add_payment_details (connection,
                              hc,
                              order,
                              payment_target,
                              inventory_products_length,
                              inventory_products,
                              uuids_length,
                              uuids);


}


/**
 * Generate an order.  We add the fields 'exchanges', 'merchant_pub', and
 * 'H_wire' to the order gotten from the frontend, as well as possibly other
 * fields if the frontend did not provide them. Returns the order_id.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_post_orders (const struct TMH_RequestHandler *rh,
                         struct MHD_Connection *connection,
                         struct TMH_HandlerContext *hc)
{
  json_t *order;
  const char *payment_target;
  unsigned int ips_len = 0;
  struct InventoryProduct *ips = NULL;
  unsigned int uuids_len = 0;
  struct GNUNET_Uuid *uuids = NULL;

  order = json_object_get (hc->request_body,
                           "order");
  if (NULL == order)
  {
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MISSING,
                                       "order");
  }

  /* parse the payment_target (optionally given) */
  {
    const json_t *pt;

    pt = json_object_get (hc->request_body,
                          "payment_target");
    if (NULL != pt)
    {
      if (! json_is_string (pt))
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "payment_target");
      payment_target = json_string_value (pt);
    }
  }
  /* parse the inventory_products (optionally given) */
  {
    const json_t *ip;

    ip = json_object_get (hc->request_body,
                          "inventory_products");
    if (NULL != ip)
    {
      if (! json_is_array (ip))
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "inventory_products");
      GNUNET_array_grow (ips,
                         ips_len,
                         json_array_size (ip));
      for (unsigned int i = 0; i<ips_len; i++)
      {
        const char *error_name;
        unsigned int error_line;
        int res;
        struct GNUNET_JSON_Specification spec[] = {
          GNUNET_JSON_spec_string ("product_id",
                                   &ips[i].product_id),
          GNUNET_JSON_spec_uint32 ("quantity",
                                   &ips[i].quantity),
          GNUNET_JSON_spec_end ()
        };

        res = GNUNET_JSON_parse (json_array_get (ip,
                                                 i),
                                 spec,
                                 &error_name,
                                 &error_line);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          GNUNET_array_grow (ips,
                             ips_len,
                             0);
          GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                      "Product parsing failed at #%u: %s:%u\n",
                      i,
                      error_name,
                      error_line);
          return TALER_MHD_reply_with_error (connection,
                                             MHD_HTTP_BAD_REQUEST,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "inventory_products");
        }
      }
    }
  }
  /* parse the lock_uuids (optionally given) */
  {
    const json_t *uuid;

    uuid = json_object_get (hc->request_body,
                            "lock_uuids");
    if (NULL != uuid)
    {
      if (! json_is_array (uuid))
      {
        GNUNET_array_grow (ips,
                           ips_len,
                           0);
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "lock_uuids");
      }
      GNUNET_array_grow (uuids,
                         uuids_len,
                         json_array_size (uuid));
      for (unsigned int i = 0; i<uuids_len; i++)
      {
        const char *error_name;
        unsigned int error_line;
        int res;
        struct GNUNET_JSON_Specification spec[] = {
          GNUNET_JSON_spec_fixed_auto ("uuid",
                                       &uuids[i]),
          GNUNET_JSON_spec_end ()
        };

        res = GNUNET_JSON_parse (json_array_get (uuid,
                                                 i),
                                 spec,
                                 &error_name,
                                 &error_line);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          GNUNET_array_grow (ips,
                             ips_len,
                             0);
          GNUNET_array_grow (uuids,
                             uuids_len,
                             0);
          GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                      "UUID parsing failed at #%u: %s:%u\n",
                      i,
                      error_name,
                      error_line);
          return TALER_MHD_reply_with_error (connection,
                                             MHD_HTTP_BAD_REQUEST,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "lock_uuids");
        }
      }
    }
  }
  /* Finally, start by completing the order */
  {
    MHD_RESULT res;

    res = merge_inventory (connection,
                           hc,
                           order,
                           payment_target,
                           ips_len,
                           ips,
                           uuids_len,
                           uuids);
    GNUNET_array_grow (ips,
                       ips_len,
                       0);
    GNUNET_array_grow (uuids,
                       uuids_len,
                       0);
    return res;
  }
}


/* end of taler-merchant-httpd_private-post-orders.c */
