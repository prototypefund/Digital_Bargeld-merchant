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
 * @file backend/taler-merchant-httpd_refund.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_refund.h"

/**
 * How often do we retry the non-trivial refund INSERT database
 * transaction?
 */
#define MAX_RETRIES 5

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
 * Handle request for increasing the refund associated with
 * a contract.
 *
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_refund_increase (struct TMH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            void **connection_cls,
                            const char *upload_data,
                            size_t *upload_data_size)
{
  int res;
  struct TMH_JsonParseContext *ctx;
  struct TALER_Amount refund;
  json_t *root;
  json_t *contract_terms;
  const char *order_id;
  const char *reason;
  const char *merchant;
  struct MerchantInstance *mi;
  struct GNUNET_HashCode h_contract_terms;
  struct TALER_MerchantRefundConfirmationPS confirmation;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  struct GNUNET_JSON_Specification spec[] = {
    TALER_JSON_spec_amount ("refund", &refund),
    GNUNET_JSON_spec_string ("order_id", &order_id),
    GNUNET_JSON_spec_string ("reason", &reason),
    GNUNET_JSON_spec_string ("instance", &merchant),
    GNUNET_JSON_spec_end ()
  };
  enum GNUNET_DB_QueryStatus qs;

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

  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  if (GNUNET_NO == res)
  {
    GNUNET_break_op (0);
    return MHD_YES;
  }

  if (GNUNET_SYSERR == res)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Hard error from JSON parser\n");
    return MHD_NO;
  }

  mi = TMH_lookup_instance (merchant);
  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No instance found\n");
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_REFUND_INSTANCE_UNKNOWN,
					 "Unknown instance given");
  }

  /* Convert order id to h_contract_terms */
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
                                              TALER_EC_REFUND_LOOKUP_DB_ERROR,
                                              "An error occurred while retrieving payment data from db");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown order id given: %s\n",
                order_id);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_REFUND_ORDER_ID_UNKNOWN,
                                         "Order id not found in database");
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_break (0);
    GNUNET_JSON_parse_free (spec);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not hash contract terms\n");
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_INTERNAL_LOGIC_ERROR,
                                              "Could not hash contract terms");
  }
  for (unsigned int i=0;i<MAX_RETRIES;i++)
  {
    qs = db->increase_refund_for_contract (db->cls,
					   &h_contract_terms,
					   &mi->pubkey,
					   &refund,
					   reason);
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
                                              TALER_EC_REFUND_MERCHANT_DB_COMMIT_ERROR,
                                              "Internal database error or refund amount too big");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Refunded amount lower or equal to previous refund: %s\n",
                TALER_amount2s (&refund));
    GNUNET_JSON_parse_free (spec);
    return TMH_RESPONSE_reply_external_error (connection,
                                              TALER_EC_REFUND_INCONSISTENT_AMOUNT,
                                              "Amount incorrect: not larger than the previous one");
  }

  /**
   * Return to the frontend at this point.  The frontend will then return
   * a "402 Payment required" carrying a "X-Taler-Refund-Url: www"
   * where 'www' is the URL where the wallet can automatically fetch
   * the refund permission.
   *
   * Just a "200 OK" should be fine here, as the frontend has all
   * the information needed to generate the right response.
   */

  confirmation.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND_OK);
  confirmation.purpose.size = htonl (sizeof (struct TALER_MerchantRefundConfirmationPS));
  GNUNET_CRYPTO_hash (order_id,
                      strlen (order_id),
                      &confirmation.h_order_id);

  json_decref (contract_terms);
  json_decref (root);
  GNUNET_JSON_parse_free (spec);

  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_sign (&mi->privkey.eddsa_priv,
				&confirmation.purpose,
				&sig))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to sign successful refund confirmation\n");
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_NONE, /* FIXME! */
                                              "Refund done, but failed to sign confirmation");

  }

  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:o}",
                                       "sig", GNUNET_JSON_from_data_auto (&sig));
}


/**
 * Function called with information about a refund.
 * It is responsible for packing up the data to return.
 *
 * @param cls closure
 * @param coin_pub public coin from which the refund comes from
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explaination of the refund
 * @param refund_amount refund amount which is being taken from coin_pub
 * @param refund_fee cost of this refund operation
 */
static void
process_refunds_cb (void *cls,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    uint64_t rtransaction_id,
                    const char *reason,
                    const struct TALER_Amount *refund_amount,
                    const struct TALER_Amount *refund_fee)
{
  struct ProcessRefundData *prd = cls;
  struct TALER_RefundRequestPS rr;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  json_t *element;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Found refund of %s for coin %s with reason `%s' in database\n",
              TALER_B2S (coin_pub),
              TALER_amount2s (refund_amount),
              reason);
  rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
  rr.purpose.size = htonl (sizeof (struct TALER_RefundRequestPS));
  rr.h_contract_terms = *prd->h_contract_terms;
  rr.coin_pub = *coin_pub;
  rr.merchant = prd->merchant->pubkey;
  rr.rtransaction_id = GNUNET_htonll (rtransaction_id);
  TALER_amount_hton (&rr.refund_amount,
                     refund_amount);
  TALER_amount_hton (&rr.refund_fee,
                     refund_fee);

  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_sign (&prd->merchant->privkey.eddsa_priv,
				&rr.purpose,
				&sig))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not sign refund request\n");
    prd->ec = TALER_EC_INTERNAL_LOGIC_ERROR;
    return;
  }

  element = json_pack ("{s:o, s:o, s:o, s:o, s:I, s:o, s:o}",
                       "refund_amount", TALER_JSON_from_amount (refund_amount),
                       "refund_fee", TALER_JSON_from_amount (refund_fee),
                       "h_contract_terms", GNUNET_JSON_from_data_auto (prd->h_contract_terms),
                       "coin_pub", GNUNET_JSON_from_data_auto (coin_pub),
                       "rtransaction_id", (json_int_t) rtransaction_id,
                       "merchant_pub", GNUNET_JSON_from_data_auto (&prd->merchant->pubkey),
                       "merchant_sig", GNUNET_JSON_from_data_auto (&sig));
  if (NULL == element)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not pack a response's element up\n");
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
 * Return refund situation about a contract.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_refund_lookup (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  const char *order_id;
  const char *instance;
  struct GNUNET_HashCode h_contract_terms;
  json_t *contract_terms;
  struct MerchantInstance *mi;
  enum GNUNET_DB_QueryStatus qs;

  instance = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "instance");
  if (NULL == instance)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Argument 'instance' not given\n");
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "instance");
  }

  mi = TMH_lookup_instance (instance);

  if (NULL == mi)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown instance given: %s\n",
                instance);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_REFUND_INSTANCE_UNKNOWN,
                                         "Unknown instance given");
  }

  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Argument 'order_id' not given\n");
    return TMH_RESPONSE_reply_arg_missing (connection,
					   TALER_EC_PARAMETER_MISSING,
                                           "order_id");
  }

  /* Convert order id to h_contract_terms */
  contract_terms = NULL;
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
                                              TALER_EC_REFUND_LOOKUP_DB_ERROR,
                                              "database error looking up order_id from merchant_contract_terms table");
  }

  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown order id given: %s\n",
                order_id);
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_REFUND_ORDER_ID_UNKNOWN,
                                         "Order id not found in database");
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_break (0);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not hash contract terms\n");
    json_decref (contract_terms);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_INTERNAL_LOGIC_ERROR,
                                              "Could not hash contract terms");
  }
  json_decref (contract_terms);

  {
    json_t *response;
    enum TALER_ErrorCode ec;
    const char *errmsg;

    response = TM_get_refund_json (mi,
                                   &h_contract_terms,
                                   &ec,
                                   &errmsg);
    if (NULL == response)
      return TMH_RESPONSE_reply_internal_error (connection,
                                                ec,
                                                errmsg);
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_OK,
                                         "{s:o}",
                                         "refund_permissions",
                                         response);
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
  prd.h_contract_terms = h_contract_terms;
  prd.merchant = mi;
  prd.ec = TALER_EC_NONE;
  for (unsigned int i=0;i<MAX_RETRIES;i++)
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
    *ret_errmsg = ("database hard error: looking for "
                  "h_contract_terms in merchant_refunds table");
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
