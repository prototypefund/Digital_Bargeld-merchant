/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
 * @file merchant/backend/taler-merchant-httpd.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <curl/curl.h>
#include <taler/taler_amount_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_mint_service.h>
#include "taler-mint-httpd.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "merchant_db.h"
#include "merchant.h"
#include "taler_merchant_lib.h"

extern struct MERCHANT_Mint *mints;
extern struct GNUNET_CRYPTO_EddsaPrivateKey privkey;
extern const struct MERCHANT_WIREFORMAT_Sepa *wire;
extern unsigned int nmints;
extern PGconn *db_conn;
extern long long salt;

/**
 * Manage a contract request. In practical terms, it adds the fields 'mints',
 * 'merchant_pub', and 'H_wire' to the contract 'proposition' gotten from the
 * frontend. Finally, it adds (outside of the contract) a signature of the
 * (hashed stringification) of this contract and the hashed stringification
 * of this contract to the final bundle sent back to the frontend.
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
  json_t *trusted_mints;
  json_t *mint;
  json_t *j_wire;
  const struct TALER_MINT_Keys *keys;
  int res;
  int cnt;
  struct GNUNET_HashCode h_wire;
  struct GNUNET_CRYPTO_EddsaPublicKey pubkey;
  struct MERCHANT_Contract contract;
  char *contract_str;

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

  /* Generate preferred mint(s) array. */
  
  trusted_mints = json_array ();
  for (cnt = 0; cnt < nmints; cnt++)
  {
    if (!mints[cnt].pending)
    {
      keys = TALER_MINT_get_keys (mints[cnt].conn);
      mint = json_pack ("{s:s, s:o}",
                        "url", mints[cnt].hostname,
			"master_pub",
			TALER_json_from_data
			(&keys->master_pub.eddsa_pub,
		        sizeof (keys->master_pub.eddsa_pub)));
      json_array_append_new (trusted_mints, mint);
    }
  }

  /**
   * Return badly if no mints are trusted (or no call to /keys has still
   * returned the expected data). WARNING: it
   * may be possible that a mint trusted by the wallet is good, but
   * still pending; that case must be handled with some "polling-style"
   * routine, simply ignored, or ended with an invitation to the wallet
   * to just retry later
   */
  if (!json_array_size (trusted_mints))
    return MHD_NO;

  json_object_set_new (root, "mints", trusted_mints);

  if (NULL == (j_wire = MERCHANT_get_wire_json (wire,
                                                salt)))                       
    return MHD_NO;

  /* hash wire objcet */
  if (GNUNET_SYSERR ==
      TALER_hash_json (j_wire, &h_wire))
    return MHD_NO;

  json_object_set_new (root,
                       "H_wire",
		       TALER_json_from_data (&h_wire, sizeof (h_wire)));

  GNUNET_CRYPTO_eddsa_key_get_public (&privkey, &pubkey);
  json_object_set_new (root,
                       "merchant_pub",
		       TALER_json_from_data (&pubkey, sizeof (pubkey)));

  /* Sign */
  
  return TMH_RESPONSE_reply_json (connection, root, MHD_HTTP_OK);
  
}
