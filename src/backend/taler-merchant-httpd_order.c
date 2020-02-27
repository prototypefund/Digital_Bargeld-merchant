/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2018 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_order.c
 * @brief HTTP serving layer mainly intended to communicate
 * with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
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

    if (NULL == json_object_get (value,
                                 "description"))
    {
      GNUNET_break (0);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Product description parsing failed at product #%u\n",
                  (unsigned int) index);
      return GNUNET_SYSERR;
    }
  }
  return GNUNET_OK;
}


/**
 * Information we keep for individual calls
 * to requests that parse JSON, but keep no other state.
 */
struct TMH_JsonParseContext
{

  /**
   * This field MUST be first.
   * FIXME: Explain why!
   */
  struct TM_HandlerContext hc;

  /**
   * Placeholder for #TALER_MHD_parse_post_json() to keep its internal state.
   */
  void *json_parse_context;
};


/**
 * Custom cleanup routine for a `struct TMH_JsonParseContext`.
 *
 * @param hc the `struct TMH_JsonParseContext` to clean up.
 */
static void
json_parse_cleanup (struct TM_HandlerContext *hc)
{
  struct TMH_JsonParseContext *jpc = (struct TMH_JsonParseContext *) hc;

  TALER_MHD_parse_post_cleanup_callback (jpc->json_parse_context);
  GNUNET_free (jpc);
}


/**
 * Generate the base URL for the given merchant instance.
 *
 * @param connection the MHD connection
 * @param instance_id the merchant instance ID
 * @returns the merchant instance's base URL
 */
static char *
make_merchant_base_url (struct MHD_Connection *connection, const
                        char *instance_id)
{
  const char *host;
  const char *forwarded_host;
  const char *uri_path;
  struct GNUNET_Buffer buf = { 0 };

  if (GNUNET_YES == TALER_mhd_is_https (connection))
    GNUNET_buffer_write_str (&buf, "https://");
  else
    GNUNET_buffer_write_str (&buf, "http://");


  host = MHD_lookup_connection_value (connection, MHD_HEADER_KIND, "Host");
  forwarded_host = MHD_lookup_connection_value (connection, MHD_HEADER_KIND,
                                                "X-Forwarded-Host");

  if (NULL != forwarded_host)
  {
    GNUNET_buffer_write_str (&buf, forwarded_host);
  }
  else
  {
    GNUNET_assert (NULL != host);
    GNUNET_buffer_write_str (&buf, host);
  }

  uri_path = MHD_lookup_connection_value (connection, MHD_HEADER_KIND,
                                          "X-Forwarded-Prefix");
  if (NULL != uri_path)
  {
    /* Currently the merchant backend is only supported at the root of the path,
       this might change in the future.  */
    GNUNET_assert (0);
  }

  GNUNET_buffer_write_path (&buf, "public");

  if (0 != strcmp (instance_id, "default"))
  {
    GNUNET_buffer_write_path (&buf, "/instances/");
    GNUNET_buffer_write_str (&buf, instance_id);
  }
  GNUNET_buffer_write_path (&buf, "");

  return GNUNET_buffer_reap_str (&buf);
}


/**
 * Transform an order into a proposal and store it in the
 * database. Write the resulting proposal or an error message
 * of a MHD connection.
 *
 * @param connection connection to write the result or error to
 * @param root root of the request
 * @param order[in] order to process (can be modified)
 * @return MHD result code
 */
static int
proposal_put (struct MHD_Connection *connection,
              json_t *root,
              json_t *order,
              const struct MerchantInstance *mi)
{
  int res;
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
    TALER_JSON_spec_amount ("amount", &total),
    GNUNET_JSON_spec_string ("order_id", &order_id),
    GNUNET_JSON_spec_string ("summary", &summary),
    GNUNET_JSON_spec_string ("fulfillment_url",
                             &fulfillment_url),
    /**
     * The following entries we don't actually need,
     * except to check that the order is well-formed */
    GNUNET_JSON_spec_json ("products", &products),
    GNUNET_JSON_spec_json ("merchant", &merchant),
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
  struct WireMethod *wm;

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
    json_object_set_new (order,
                         "timestamp",
                         GNUNET_JSON_from_time_abs (now));
  }

  /* If no refund_deadline given, set one as zero.  */
  if (NULL == json_object_get (order,
                               "refund_deadline"))
  {
    struct GNUNET_TIME_Absolute zero = { 0 };

    json_object_set_new (order,
                         "refund_deadline",
                         GNUNET_JSON_from_time_abs (zero));
  }

  if (NULL == json_object_get (order,
                               "pay_deadline"))
  {
    struct GNUNET_TIME_Absolute t;

    t = GNUNET_TIME_relative_to_absolute (default_pay_deadline);
    (void) GNUNET_TIME_round_abs (&t);
    json_object_set_new (order,
                         "pay_deadline",
                         GNUNET_JSON_from_time_abs (t));
  }

  if (NULL == json_object_get (order,
                               "wire_transfer_deadline"))
  {
    struct GNUNET_TIME_Absolute t;
    t = GNUNET_TIME_relative_to_absolute (default_wire_transfer_delay);
    (void) GNUNET_TIME_round_abs (&t);
    json_object_set_new (order,
                         "wire_transfer_deadline",
                         GNUNET_JSON_from_time_abs (t));
  }

  if (NULL == json_object_get (order,
                               "max_wire_fee"))
  {
    json_object_set_new (order,
                         "max_wire_fee",
                         TALER_JSON_from_amount
                           (&default_max_wire_fee));
  }

  if (NULL == json_object_get (order,
                               "max_fee"))
  {
    json_object_set_new (order,
                         "max_fee",
                         TALER_JSON_from_amount
                           (&default_max_deposit_fee));
  }

  if (NULL == json_object_get (order,
                               "wire_fee_amortization"))
  {
    json_object_set_new
      (order,
      "wire_fee_amortization",
      json_integer
        ((json_int_t) default_wire_fee_amortization));
  }

  if (NULL == json_object_get (order,
                               "merchant_base_url"))
  {
    char *url;

    url = make_merchant_base_url (connection, mi->id);
    json_object_set_new (order,
                         "merchant_base_url",
                         json_string (url));
    GNUNET_free (url);
  }

  if (NULL == json_object_get (order,
                               "products"))
  {
    json_object_set_new (order,
                         "products",
                         json_array ());
  }

  /* Fill in merchant information if necessary */
  if (NULL == json_object_get (order, "merchant"))
  {
    const char *mj = NULL;
    const char *ma = NULL;
    json_t *locations;
    char *label;
    json_t *jmerchant;

    jmerchant = json_object ();
    json_object_set_new (jmerchant,
                         "name",
                         json_string (mi->name));
    json_object_set_new (jmerchant,
                         "instance",
                         json_string (mi->id));
    locations = json_object_get (order,
                                 "locations");
    if (NULL != locations)
    {
      json_t *loca;
      json_t *locj;

      /* Handle merchant address */
      GNUNET_assert (0 < GNUNET_asprintf (&label,
                                          "%s-address",
                                          mi->id));
      loca = json_object_get (default_locations,
                              label);
      if (NULL != loca)
      {
        loca = json_deep_copy (loca);
        ma = STANDARD_LABEL_MERCHANT_ADDRESS;
        json_object_set_new (locations,
                             ma,
                             loca);
        json_object_set_new (jmerchant,
                             "address",
                             json_string (ma));
      }
      GNUNET_free (label);

      /* Handle merchant jurisdiction */
      GNUNET_assert (0 < GNUNET_asprintf (&label,
                                          "%s-jurisdiction",
                                          mi->id));
      locj = json_object_get (default_locations,
                              label);
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
          json_object_set_new (locations,
                               mj,
                               locj);
        }
        json_object_set_new (merchant,
                             "jurisdiction",
                             json_string (mj));
      }
      GNUNET_free (label);
    } /* have locations */
    json_object_set_new (order,
                         "merchant",
                         jmerchant);
  } /* needed to synthesize merchant info */

  /* extract fields we need to sign separately */
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
    return TALER_MHD_reply_with_error
             (connection,
             MHD_HTTP_INTERNAL_SERVER_ERROR,
             TALER_EC_PROPOSAL_ORDER_PARSE_ERROR,
             "Impossible to parse the order");
  }
  if (0 !=
      strcasecmp (total.currency,
                  TMH_currency))
  {
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error
             (connection,
             MHD_HTTP_BAD_REQUEST,
             TALER_EC_PROPOSAL_ORDER_BAD_CURRENCY,
             "Total amount must be in currency supported by backend");
  }

  if (wire_transfer_deadline.abs_value_us <
      refund_deadline.abs_value_us)
  {
    GNUNET_JSON_parse_free (spec);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "invariant failed: wire_transfer_deadline >= refund_deadline\n");
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "wire_transfer_deadline: %s\n",
                GNUNET_STRINGS_absolute_time_to_string (
                  wire_transfer_deadline));
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "refund_deadline: %s\n",
                GNUNET_STRINGS_absolute_time_to_string (refund_deadline));
    return TALER_MHD_reply_with_error
             (connection,
             MHD_HTTP_BAD_REQUEST,
             TALER_EC_PARAMETER_MALFORMED,
             "order:wire_transfer_deadline;order:refund_deadline");
  }


  /* check contract is well-formed */
  if (GNUNET_OK != check_products (products))
  {
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error
             (connection,
             MHD_HTTP_BAD_REQUEST,
             TALER_EC_PARAMETER_MALFORMED,
             "order:products");
  }

  /* add fields to the contract that the backend should provide */
  json_object_set (order,
                   "exchanges",
                   trusted_exchanges);

  json_object_set (order,
                   "auditors",
                   j_auditors);
  /* TODO (#4939-12806): add proper mechanism for
     selection of wire method(s) by merchant! */
  wm = mi->wm_head;

  if (NULL == wm)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No wire method available for instance '%s'\n", mi->id);
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_PROPOSAL_INSTANCE_CONFIGURATION_LACKS_WIRE,
                                       "No wire method configured for instance");
  }
  json_object_set_new (order,
                       "h_wire",
                       GNUNET_JSON_from_data_auto (&wm->h_wire));
  json_object_set_new (order,
                       "wire_method",
                       json_string (wm->wire_method));
  json_object_set_new (order,
                       "merchant_pub",
                       GNUNET_JSON_from_data_auto (&mi->pubkey));

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Inserting order '%s' for instance '%s'\n",
              order_id,
              mi->id);

  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    db->preflight (db->cls);
    qs = db->insert_order (db->cls,
                           order_id,
                           &mi->pubkey,
                           timestamp,
                           order);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    /* Special report if retries insufficient */
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    {
      GNUNET_break (0);
      return TALER_MHD_reply_with_error
               (connection,
               MHD_HTTP_INTERNAL_SERVER_ERROR,
               TALER_EC_PROPOSAL_STORE_DB_ERROR_SOFT,
               "db error: could not check for existing order"
               " due to repeated soft transaction failure");
    }

    {
      /* Hard error could be constraint violation,
         check if order already exists */
      json_t *contract_terms = NULL;

      db->preflight (db->cls);
      qs = db->find_order (db->cls,
                           &contract_terms,
                           order_id,
                           &mi->pubkey);
      if (0 < qs)
      {
        /* Yep, indeed uniqueness constraint violation */
        int rv;
        char *msg;

        GNUNET_JSON_parse_free (spec);
        GNUNET_asprintf (&msg,
                         "order ID `%s' already exists",
                         order_id);
        {
          /* Log plenty of details for the admin */
          char *js;

          js = json_dumps (contract_terms,
                           JSON_COMPACT);
          GNUNET_log
            (GNUNET_ERROR_TYPE_ERROR,
            _ ("Order ID `%s' already exists with proposal `%s'\n"),
            order_id,
            js);
          free (js);
        }
        json_decref (contract_terms);

        /* contract_terms may be private, only expose
         * duplicate order_id to the network */
        rv = TALER_MHD_reply_with_error
               (connection,
               MHD_HTTP_BAD_REQUEST,  /* or conflict? */
               TALER_EC_PROPOSAL_STORE_DB_ERROR_ALREADY_EXISTS,
               msg);
        GNUNET_free (msg);
        return rv;
      }
    }

    /* Other hard transaction error (disk full, etc.) */
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error
             (connection,
             MHD_HTTP_INTERNAL_SERVER_ERROR,
             TALER_EC_PROPOSAL_STORE_DB_ERROR_HARD,
             "db error: could not store this proposal's data into db");
  }

  /* DB transaction succeeded, generate positive response */
  res = TALER_MHD_reply_json_pack (connection,
                                   MHD_HTTP_OK,
                                   "{s:s}",
                                   "order_id",
                                   order_id);
  GNUNET_JSON_parse_free (spec);
  return res;
}


/**
 * Generate a proposal, given its order. In practical terms,
 * it adds the fields  'exchanges', 'merchant_pub', and 'h_wire'
 * to the order gotten from the frontend. Finally, it signs this
 * data, and returns it to the frontend.
 *
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure
 *                (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in
 *                @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_order_post (struct TMH_RequestHandler *rh,
                       struct MHD_Connection *connection,
                       void **connection_cls,
                       const char *upload_data,
                       size_t *upload_data_size,
                       struct MerchantInstance *mi)
{
  int res;
  struct TMH_JsonParseContext *ctx;
  json_t *root;
  json_t *order;

  if (NULL == *connection_cls)
  {
    ctx = GNUNET_new (struct TMH_JsonParseContext);
    ctx->hc.cc = &json_parse_cleanup;
    *connection_cls = ctx;
  }
  else
  {
    ctx = *connection_cls;
  }

  res = TALER_MHD_parse_post_json (connection,
                                   &ctx->json_parse_context,
                                   upload_data,
                                   upload_data_size,
                                   &root);

  if (GNUNET_SYSERR == res)
    return MHD_NO;

  /* A error response was already generated */
  if ( (GNUNET_NO == res) ||
       /* or, need more data to accomplish parsing */
       (NULL == root) )
    return MHD_YES;
  order = json_object_get (root,
                           "order");
  if (NULL == order)
  {
    res = TALER_MHD_reply_with_error
            (connection,
            MHD_HTTP_BAD_REQUEST,
            TALER_EC_PARAMETER_MISSING,
            "order");
  }
  else
    res = proposal_put (connection, root, order, mi);
  json_decref (root);
  return res;
}


/* end of taler-merchant-httpd_order.c */
