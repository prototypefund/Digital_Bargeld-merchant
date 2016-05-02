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
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("amount", &total),
    TALER_JSON_spec_amount ("max_fee", &max_fee),
    GNUNET_JSON_spec_uint64 ("transaction_id", &transaction_id),
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
    return TMH_RESPONSE_reply_external_error (connection,
                                              "contract request malformed");
  }
  /* extract fields we need to sign separately */
  res = TMH_PARSE_json_data (connection,
                             jcontract,
                             spec);
  if (GNUNET_NO == res)
    return MHD_YES;
  if (GNUNET_SYSERR == res)
    return TMH_RESPONSE_reply_external_error (connection,
                                              "contract request malformed");
  /* add fields to the contract that the backend should provide */
  json_object_set (jcontract,
                   "exchanges",
                   trusted_exchanges);
  json_object_set (jcontract,
                   "auditors",
                   j_auditors);
  json_object_set_new (jcontract,
                       "H_wire",
		       GNUNET_JSON_from_data (&h_wire,
                                             sizeof (h_wire)));
  json_object_set_new (jcontract,
                       "merchant_pub",
		       GNUNET_JSON_from_data (&pubkey,
                                             sizeof (pubkey)));

  /* create contract signature */
  GNUNET_assert (GNUNET_OK ==
                 TALER_JSON_hash (jcontract,
                                  &contract.h_contract));
  contract.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract.purpose.size = htonl (sizeof (contract));
  contract.transaction_id = GNUNET_htonll (transaction_id);
  TALER_amount_hton (&contract.total_amount,
                     &total);
  TALER_amount_hton (&contract.max_fee,
                     &max_fee);
  GNUNET_CRYPTO_eddsa_sign (privkey,
                            &contract.purpose,
                            &contract_sig);

  /* return final response */
  res = TMH_RESPONSE_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{s:O, s:O, s:O}",
                                      "contract", jcontract,
                                      "merchant_sig", GNUNET_JSON_from_data (&contract_sig,
                                                                   sizeof (contract_sig)),
                                      "H_contract", GNUNET_JSON_from_data (&contract.h_contract,
                                                                          sizeof (contract.h_contract)));
  json_decref (root);
  return res;
}

/* end of taler-merchant-httpd_contract.c */
