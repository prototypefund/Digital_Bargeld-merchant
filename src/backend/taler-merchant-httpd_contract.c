/*
  This file is part of TALER
  (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
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
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_mints.h"
#include "taler-merchant-httpd_responses.h"


/**
 * Manage a contract request. In practical terms, it adds the fields
 * 'mints', 'merchant_pub', and 'H_wire' to the contract 'proposition'
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
  int res;
  struct GNUNET_HashCode h_wire;
  struct TALER_ContractPS contract;
  struct GNUNET_CRYPTO_EddsaSignature contract_sig;

  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ((GNUNET_NO == res) || (NULL == root))
    return MHD_YES;

  /* add fields to the "root" that the backend should provide */
  json_object_set_new (root,
                       "mints",
                       trusted_mints);
  json_object_set_new (root,
                       "auditors",
                       j_auditors);
  json_object_set_new (root,
                       "H_wire",
		       TALER_json_from_data (&h_wire,
                                             sizeof (h_wire)));
  json_object_set_new (root,
                       "merchant_pub",
		       TALER_json_from_data (&pubkey,
                                             sizeof (pubkey)));
  /* create contract signature */
  GNUNET_assert (GNUNET_OK ==
                 TALER_hash_json (root,
                                  &contract.h_contract));
  contract.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract.purpose.size = htonl (sizeof (contract));
  GNUNET_CRYPTO_eddsa_sign (privkey,
                            &contract.purpose,
                            &contract_sig);
  /* return final response */
  return TMH_RESPONSE_reply_json_pack (connection,
				       MHD_HTTP_OK,
				       "{s:o, s:o, s:o}",
				       "contract", root,
				       "sig", TALER_json_from_data (&contract_sig,
                                                                    sizeof (contract_sig)),
				       "H_contract", TALER_json_from_data (&contract.h_contract,
                                                                           sizeof (contract.h_contract)));
}

/* end of taler-merchant-httpd_contract.c */
