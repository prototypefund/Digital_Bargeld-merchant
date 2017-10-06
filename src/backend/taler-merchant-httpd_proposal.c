/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2017 INRIA

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
 * @file backend/taler-merchant-httpd_proposal.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


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
  int res;

  if (! json_is_array (products))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  json_array_foreach (products, index, value) {
    const char *description;
    const char *error_name;
    unsigned int error_line;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("description", &description),
      /* FIXME: there are other fields in the product specification
         that are currently not labeled as optional. Maybe check
         those as well, or make them truly optional. */
      GNUNET_JSON_spec_end()
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
                  "Product description parsing failed at #%u: %s:%u\n",
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
   * Placeholder for #TMH_PARSE_post_json() to keep its internal state.
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

  TMH_PARSE_post_cleanup_callback (jpc->json_parse_context);
  GNUNET_free (jpc);
}


/**
 * Transform an order into a proposal and store it in the database.
 * Write the resulting proposal or an error message ot a MHD connection
 *
 * @param connection connection to write the result or error to
 * @param order to process
 * @return MHD result code
 */
static int
proposal_put (struct MHD_Connection *connection,
              json_t *order)
{
  int res;
  struct MerchantInstance *mi;
  struct TALER_ProposalDataPS pdps;
  struct GNUNET_CRYPTO_EddsaSignature merchant_sig;
  struct TALER_Amount total;
  struct TALER_Amount max_fee;
  const char *order_id;
  json_t *products;
  json_t *merchant;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct GNUNET_TIME_Absolute pay_deadline;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("amount", &total),
    GNUNET_JSON_spec_string ("order_id", &order_id),
    TALER_JSON_spec_amount ("max_fee", &max_fee),
    /* The following entries we don't actually need, except to check that
       the order is well-formed */
    GNUNET_JSON_spec_json ("products", &products),
    GNUNET_JSON_spec_json ("merchant", &merchant),
    GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
    GNUNET_JSON_spec_absolute_time ("refund_deadline", &refund_deadline),
    GNUNET_JSON_spec_absolute_time ("pay_deadline", &pay_deadline),
    GNUNET_JSON_spec_end ()
  };
  enum GNUNET_DB_QueryStatus qs;

  /* Add order_id if it doesn't exist. */
  if (NULL ==
      json_string_value (json_object_get (order,
					  "order_id")))
  {
    char buf[256];
    time_t timer;
    struct tm* tm_info;
    size_t off;

    time (&timer);
    tm_info = localtime (&timer);

    off = strftime (buf,
                    sizeof (buf),
                    "%H:%M:%S",
                    tm_info);
    snprintf (buf + off,
	      sizeof (buf) - off,
              "-%llX",
              (long long unsigned) GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK,
                                                             UINT64_MAX));
    json_object_set_new (order,
                         "order_id",
                         json_string (buf));
  }

  if (NULL == json_object_get (order,
                               "timestamp"))
  {
    struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();

    (void) GNUNET_TIME_round_abs (&now);
    json_object_set_new (order,
                         "timestamp",
                         GNUNET_JSON_from_time_abs (now));
  }

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
                               "max_wire_fee"))
  {
    json_object_set_new (order,
                         "max_wire_fee",
                         TALER_JSON_from_amount (&default_max_wire_fee));
  }

  if (NULL == json_object_get (order,
                               "wire_fee_amortization"))
  {
    json_object_set_new (order,
                         "wire_fee_amortization",
                         json_integer ((json_int_t) default_wire_fee_amortization));
  }

  /* extract fields we need to sign separately */
  res = TMH_PARSE_json_data (connection,
                             order,
                             spec);
  if (GNUNET_NO == res)
  {
    return MHD_YES;
  }
  if (GNUNET_SYSERR == res)
  {
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_NONE,
					      "Impossible to parse the order");
  }

  /* check contract is well-formed */
  if (GNUNET_OK != check_products (products))
  {
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_arg_invalid (connection,
					   TALER_EC_PARAMETER_MALFORMED,
					   "order:products");
  }

  mi = TMH_lookup_instance_json (merchant);
  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Not able to find the specified instance\n");
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_CONTRACT_INSTANCE_UNKNOWN,
					 "Unknown instance given");
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Signing contract on behalf of instance '%s'\n",
              mi->id);
  /* add fields to the contract that the backend should provide */
  json_object_set (order,
                   "exchanges",
                   trusted_exchanges);
  json_object_set (order,
                   "auditors",
                   j_auditors);
  json_object_set_new (order,
                       "H_wire",
		       GNUNET_JSON_from_data_auto (&mi->h_wire));
  json_object_set_new (order,
                       "wire_method",
		       json_string (mi->wire_method));
  json_object_set_new (order,
                       "merchant_pub",
		       GNUNET_JSON_from_data_auto (&mi->pubkey));

  /* create proposal signature */
  pdps.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  pdps.purpose.size = htonl (sizeof (pdps));
  if (GNUNET_OK !=
      TALER_JSON_hash (order,
                       &pdps.hash))
  {
    GNUNET_break (0);
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_INTERNAL_LOGIC_ERROR,
                                              "Could not hash order");
  }

  GNUNET_CRYPTO_eddsa_sign (&mi->privkey.eddsa_priv,
                            &pdps.purpose,
                            &merchant_sig);

  for (unsigned int i=0;i<MAX_RETRIES;i++)
  {
    qs = db->insert_contract_terms (db->cls,
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
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PROPOSAL_STORE_DB_ERROR,
                                              "db error: could not store this proposal's data into db");
  }

  res = TMH_RESPONSE_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{s:O, s:o s:o}",
                                      "data", order,
                                      "sig", GNUNET_JSON_from_data_auto (&merchant_sig),
                                      "hash", GNUNET_JSON_from_data_auto (&pdps.hash));
  GNUNET_JSON_parse_free (spec);
  return res;
}


/**
 * Generate a proposal, given its order. In practical terms, it adds the
 * fields  'exchanges', 'merchant_pub', and 'H_wire' to the order gotten
 * from the frontend. Finally, it signs this data, and returns it to the
 * frontend.
 *
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_proposal_put (struct TMH_RequestHandler *rh,
                         struct MHD_Connection *connection,
                         void **connection_cls,
                         const char *upload_data,
                         size_t *upload_data_size)
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

  res = TMH_PARSE_post_json (connection,
                             &ctx->json_parse_context,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ( (GNUNET_NO == res) ||
       (NULL == root) )
    return MHD_YES;

  order = json_object_get (root,
                           "order");
  if (NULL == order)
  {
    res = TMH_RESPONSE_reply_arg_missing (connection,
                                          TALER_EC_PARAMETER_MISSING,
				          "order");
  }
  else
  {
    res = proposal_put (connection,
                        order);
  }
  json_decref (root);
  return res;
}


/**
 * Manage a GET /proposal request. Query the db and returns the
 * proposal's data related to the transaction id given as the URL's
 * parameter.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_proposal_lookup (struct TMH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            void **connection_cls,
                            const char *upload_data,
                            size_t *upload_data_size)
{
  const char *order_id;
  const char *instance;
  int res;
  enum GNUNET_DB_QueryStatus qs;
  json_t *contract_terms;
  struct MerchantInstance *mi;

  instance = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "instance");
  if (NULL == instance)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "instance");

  mi = TMH_lookup_instance (instance);
  if (NULL == mi)
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_CONTRACT_INSTANCE_UNKNOWN,
                                         "instance");
  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "order_id");

  qs = db->find_contract_terms (db->cls,
				&contract_terms,
				order_id,
				&mi->pubkey);
  if (0 > qs)
  {
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PROPOSAL_LOOKUP_DB_ERROR,
                                              "An error occurred while retrieving proposal data from db");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_PROPOSAL_LOOKUP_NOT_FOUND,
                                         "unknown transaction id");

  res = TMH_RESPONSE_reply_json (connection,
                                 contract_terms,
                                 MHD_HTTP_OK);
  json_decref (contract_terms);
  return res;
}


/* end of taler-merchant-httpd_proposal.c */
