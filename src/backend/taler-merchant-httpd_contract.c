/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_contract.c
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


extern char *TMH_merchant_currency_string;


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
         that rre currently not labeled as optional. Maybe check
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
 * Manage a contract request. In practical terms, it adds the fields
 * 'exchanges', 'merchant_pub', and 'H_wire' to the contract 'proposition'
 * gotten from the frontend. Finally, it adds (outside of the
 * contract) a signature of the (hashed stringification) of the
 * contract (and the hashed stringification of this contract as well
 * to aid diagnostics) to the final bundle, which is then send back to
 * the frontend.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_contract (struct TMH_RequestHandler *rh,
                     struct MHD_Connection *connection,
                     void **connection_cls,
                     const char *upload_data,
                     size_t *upload_data_size)
{

  json_t *root;
  json_t *jcontract;
  int res;
  struct TMH_JsonParseContext *ctx;
  struct TALER_ContractPS contract;
  struct GNUNET_CRYPTO_EddsaSignature contract_sig;
  struct TALER_Amount total;
  struct TALER_Amount max_fee;
  uint64_t transaction_id;
  json_t *products;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct GNUNET_TIME_Absolute expiry;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("amount", &total),
    TALER_JSON_spec_amount ("max_fee", &max_fee),
    GNUNET_JSON_spec_uint64 ("transaction_id", &transaction_id),
    /* The following entries we don't actually need, except to check that
       the contract is well-formed */
    GNUNET_JSON_spec_json ("products", &products),
    GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
    GNUNET_JSON_spec_absolute_time ("refund_deadline", &refund_deadline),
    GNUNET_JSON_spec_absolute_time ("expiry", &expiry),
    GNUNET_JSON_spec_end()
  };

  if (NULL == *connection_cls)
  {
    ctx = GNUNET_new (struct TMH_JsonParseContext);
    ctx->hc.cc = &TMH_json_parse_cleanup;
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
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES;

  jcontract = json_object_get (root, "contract");

  if (NULL == jcontract)
  {
    json_decref (root);
    return TMH_RESPONSE_reply_external_error (connection,
                                              "contract request malformed");
  }
  /* extract fields we need to sign separately */
  res = TMH_PARSE_json_data (connection,
                             jcontract,
                             spec);
  if (GNUNET_NO == res)
  {
    json_decref (root);
    return MHD_YES;
  }
  if (GNUNET_SYSERR == res)
  {
    json_decref (root);
    return TMH_RESPONSE_reply_external_error (connection,
                                              "contract request malformed");
  }
  /* check contract is well-formed */
  if (GNUNET_OK != check_products (products))
  {
    GNUNET_JSON_parse_free (spec);
    json_decref (root);
    return TMH_RESPONSE_reply_external_error (connection,
                                              "products in contract request malformed");
  }

  /* Check if this transaction ID erroneously corresponds to a
     contract that already paid, in which case we should refuse
     to sign it again (frontend buggy, it should use a fresh
     transaction ID each time)! */
  if (GNUNET_OK ==
      db->check_payment (db->cls,
                         transaction_id))
  {
    struct MHD_Response *resp;
    int ret;

    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Transaction %llu already paid in the past, refusing to sign!\n",
                (unsigned long long) transaction_id);
    resp = MHD_create_response_from_buffer (strlen ("Duplicate transaction ID!"),
                                            "Duplicate transaction ID!",
                                            MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response (connection,
                              MHD_HTTP_FORBIDDEN,
                              resp);
    MHD_destroy_response (resp);
    return ret;
  }

  /* add fields to the contract that the backend should provide */
  json_object_set (jcontract,
                   "exchanges",
                   trusted_exchanges);
  json_object_set (jcontract,
                   "auditors",
                   j_auditors);
  json_object_set_new (jcontract,
                       "H_wire",
		       GNUNET_JSON_from_data_auto (&h_wire));
  json_object_set_new (jcontract,
                       "merchant_pub",
		       GNUNET_JSON_from_data_auto (&pubkey));

  /* create contract signature */
  contract.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract.purpose.size = htonl (sizeof (contract));
  contract.transaction_id = GNUNET_htonll (transaction_id);
  TALER_amount_hton (&contract.total_amount,
                     &total);
  TALER_amount_hton (&contract.max_fee,
                     &max_fee);
  GNUNET_assert (GNUNET_OK ==
                 TALER_JSON_hash (jcontract,
                                  &contract.h_contract));
  GNUNET_CRYPTO_eddsa_sign (privkey,
                            &contract.purpose,
                            &contract_sig);

  /* return final response */
  res = TMH_RESPONSE_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{s:O, s:O, s:O}",
                                      "contract", jcontract,
                                      "merchant_sig", GNUNET_JSON_from_data_auto (&contract_sig),
                                      "H_contract", GNUNET_JSON_from_data_auto (&contract.h_contract));
  GNUNET_JSON_parse_free (spec);
  json_decref (root);
  return res;
}

/* end of taler-merchant-httpd_contract.c */
