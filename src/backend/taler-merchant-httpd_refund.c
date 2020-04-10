/*
  This file is part of TALER
  (C) 2014-2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_refund.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_refund.h"

/**
 * How often do we retry the non-trivial refund INSERT database
 * transaction?
 */
#define MAX_RETRIES 5


/**
 * Closure for #process_refunds_cb.
 */
struct ProcessRefundData
{
  /**
   * The array containing all the refund permissions.
   */
  json_t *response;

  /**
   * Hashed version of contract terms; needed by the callback
   * to pack the response.
   */
  const struct GNUNET_HashCode *h_contract_terms;

  /**
   * Both public and private key are needed by the callback
   */
  const struct MerchantInstance *merchant;

  /**
   * Return code: #TALER_EC_NONE if successful.
   */
  enum TALER_ErrorCode ec;
};


/**
 * Function called with information about a refund.
 * It is responsible for packing up the data to return.
 *
 * @param cls closure
 * @param coin_pub public coin from which the refund comes from
 * @param exchange_url URL of the exchange that issued @a coin_pub
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explanation of the refund
 * @param refund_amount refund amount which is being taken from @a coin_pub
 * @param refund_fee cost of this refund operation
 */
static void
process_refunds_cb (void *cls,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const char *exchange_url,
                    uint64_t rtransaction_id,
                    const char *reason,
                    const struct TALER_Amount *refund_amount,
                    const struct TALER_Amount *refund_fee)
{
  struct ProcessRefundData *prd = cls;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  json_t *element;

  (void) exchange_url;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Found refund of %s for coin %s with reason `%s' in database\n",
              TALER_B2S (coin_pub),
              TALER_amount2s (refund_amount),
              reason);
  {
    struct TALER_RefundRequestPS rr = {
      .purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND),
      .purpose.size = htonl (sizeof (rr)),
      .h_contract_terms = *prd->h_contract_terms,
      .coin_pub = *coin_pub,
      .merchant = prd->merchant->pubkey,
      .rtransaction_id = GNUNET_htonll (rtransaction_id)
    };

    TALER_amount_hton (&rr.refund_amount,
                       refund_amount);
    TALER_amount_hton (&rr.refund_fee,
                       refund_fee);
    GNUNET_CRYPTO_eddsa_sign (&prd->merchant->privkey.eddsa_priv,
                              &rr,
                              &sig);
  }

  element = json_pack ("{s:o, s:o, s:o, s:I, s:o}",
                       "refund_amount", TALER_JSON_from_amount (refund_amount),
                       "refund_fee", TALER_JSON_from_amount (refund_fee),
                       "coin_pub", GNUNET_JSON_from_data_auto (coin_pub),
                       "rtransaction_id", (json_int_t) rtransaction_id,
                       "merchant_sig", GNUNET_JSON_from_data_auto (&sig));
  if (NULL == element)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not pack refund response element\n");
    prd->ec = TALER_EC_PARSER_OUT_OF_MEMORY;
    return;
  }
  if (-1 == json_array_append_new (prd->response,
                                   element))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not append a response's element\n");
    prd->ec = TALER_EC_PARSER_OUT_OF_MEMORY;
    return;
  }
}


/**
 * Get the JSON representation of a refund.
 *
 * @param merchant_pub the merchant's public key
 * @param mi merchant instance
 * @param ret_ec where to store error code
 * @param ret_errmsg where to store error message
 * @return NULL on error, JSON array with refunds on success
 */
json_t *
TM_get_refund_json (const struct MerchantInstance *mi,
                    const struct GNUNET_HashCode *h_contract_terms,
                    enum TALER_ErrorCode *ret_ec,
                    const char **ret_errmsg)
{
  enum GNUNET_DB_QueryStatus qs;
  struct ProcessRefundData prd;

  prd.response = json_array ();
  if (NULL == prd.response)
  {
    GNUNET_break (0);
    return NULL;
  }
  prd.h_contract_terms = h_contract_terms;
  prd.merchant = mi;
  prd.ec = TALER_EC_NONE;
  db->preflight (db->cls);
  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    qs = db->get_refunds_from_contract_terms_hash (db->cls,
                                                   &mi->pubkey,
                                                   h_contract_terms,
                                                   &process_refunds_cb,
                                                   &prd);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Database hard error on refunds_from_contract_terms_hash lookup: %s\n",
                GNUNET_h2s (h_contract_terms));
    json_decref (prd.response);
    *ret_ec = TALER_EC_REFUND_LOOKUP_DB_ERROR;
    *ret_errmsg = "Failed to lookup refunds for contract";
  }
  if (TALER_EC_NONE != prd.ec)
  {
    json_decref (prd.response);
    /* NOTE: error already logged by the callback */
    *ret_ec = prd.ec;
    *ret_errmsg = "Could not generate a response";
  }
  return prd.response;
}


/* end of taler-merchant-httpd_refund.c */
