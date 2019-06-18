/*
  This file is part of TALER
  (C) 2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_check-payment.c
 * @brief implementation of /check-payment handler
 * @author Florian Dold
 */
#include "platform.h"
#include <string.h>
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_check-payment.h"

/**
 * Maximum number of retries for database operations.
 */
#define MAX_RETRIES 5


/**
 * Function called with information about a refund.
 * It is responsible for summing up the refund amount.
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
  struct TALER_Amount *acc_amount = cls;

  GNUNET_assert (GNUNET_SYSERR !=
                 TALER_amount_add (acc_amount,
                                   acc_amount,
                                   refund_amount));
}


/**
 * The client did not yet pay, send it the payment request.
 *
 * @param connection connection to send on
 * @param final_contract_url where to get the contract
 * @param session_id session of the client
 * @param resource_url where the resource will be (?), can be NULL!
 * @param h_contract_terms_str hash of the contract terms, stringified
 * @return #MHD_YES on success
 */
static int
send_pay_request (struct MHD_Connection *connection,
                  const char *final_contract_url,
                  const char *session_id,
                  const char *resource_url,
                  const char *h_contract_terms_str)
{
  int ret;
  char *url = TALER_url_absolute_mhd (connection,
                                      "public/trigger-pay",
                                      "contract_url", final_contract_url,
                                      "session_id", session_id,
                                      "resource_url", resource_url,
                                      "h_contract_terms", h_contract_terms_str,
                                      NULL);
  GNUNET_assert (NULL != url);
  ret = TMH_RESPONSE_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{s:s, s:b}",
                                      "payment_redirect_url",
                                      url,
                                      "paid",
                                      0);
  GNUNET_free (url);
  return ret;
}


/**
 * Check that we are aware of @a order_id and if so request the payment,
 * otherwise generate an error response.
 *
 * @param connection where to send the response
 * @param mi the merchant's instance
 * @param final_contract_url where to redirect for the contract
 * @param session_id the session_id
 * @param resource_url where the resource will be (?), can be NULL!
 * @param order_id the order to look up
 * @return #MHD_YES on success
 */
static int
check_order_and_request_payment (struct MHD_Connection *connection,
                                 struct MerchantInstance *mi,
                                 const char *final_contract_url,
                                 const char *session_id,
                                 const char *resource_url,
                                 const char *order_id)
{
  enum GNUNET_DB_QueryStatus qs;
  json_t *contract_terms;
  struct GNUNET_HashCode h_contract_terms;
  char *h_contract_terms_str;
  int ret;

  qs = db->find_order (db->cls,
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
                                              TALER_EC_CHECK_PAYMENT_DB_FETCH_ORDER_ERROR,
                                              "db error fetching order");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_CHECK_PAYMENT_ORDER_ID_UNKNOWN,
                                         "unknown order_id");
  }
  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
  GNUNET_break (0);
  json_decref (contract_terms);
  return TMH_RESPONSE_reply_internal_error (connection,
                                            TALER_EC_CHECK_PAYMENT_FAILED_COMPUTE_PROPOSAL_HASH,
                                            "Failed to hash proposal");
  }
  /* Offer was not picked up yet, but we ensured that it exists */
  h_contract_terms_str = GNUNET_STRINGS_data_to_string_alloc (&h_contract_terms,
                                                              sizeof (struct GNUNET_HashCode));
  ret = send_pay_request (connection,
                          final_contract_url,
                          session_id,
                          resource_url,
                          h_contract_terms_str);
  GNUNET_free_non_null (h_contract_terms_str);
  json_decref (contract_terms);
  return ret;
}


/**
 * Manages a /check-payment call, checking the status
 * of a payment and, if necessary, constructing the URL
 * for a payment redirect URL.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_check_payment (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  const char *order_id;
  const char *contract_url;
  const char *session_id;
  const char *session_sig_str;
  const char *instance_str;
  const char *resource_url;
  char *final_contract_url;
  char *h_contract_terms_str;
  struct MerchantInstance *mi;
  enum GNUNET_DB_QueryStatus qs;
  json_t *contract_terms;
  struct GNUNET_HashCode h_contract_terms;
  struct TALER_Amount refund_amount;
  char *last_session_id;
  int ret;
  int refunded;

  instance_str = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "instance");
  if (NULL == instance_str)
    instance_str = "default";
  mi = TMH_lookup_instance (instance_str);
  if (NULL == mi)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           TALER_EC_CHECK_PAYMENT_INSTANCE_UNKNOWN,
                                           "merchant instance unknown");
  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
  {
    /* order_id is required but missing */
    GNUNET_break_op (0);
    return TMH_RESPONSE_reply_bad_request (connection,
                                           TALER_EC_PARAMETER_MISSING,
                                           "order_id required");
  }
  contract_url = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "contract_url");
  if (NULL == contract_url)
  {
    final_contract_url = TALER_url_absolute_mhd (connection,
                                                 "/public/proposal",
                                                 "instance", instance_str,
                                                 "order_id", order_id,
                                                 NULL);
    GNUNET_assert (NULL != final_contract_url);
  }
  else
  {
    final_contract_url = GNUNET_strdup (contract_url);
  }
  resource_url = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "resource_url");
  /* NULL is allowed for resource_url! */
  session_id = MHD_lookup_connection_value (connection,
                                                MHD_GET_ARGUMENT_KIND,
                                                "session_id");
  session_sig_str = MHD_lookup_connection_value (connection,
                                                MHD_GET_ARGUMENT_KIND,
                                                "session_sig");
  if (NULL != session_id)
  {
    struct GNUNET_CRYPTO_EddsaSignature sig;
    struct TALER_MerchantPaySessionSigPS mps;
    /* If the session id is given, the frontend wants us
       to verify the session signature. */
    if (NULL == session_sig_str)
    {
      /* pay session signature required but missing */
      GNUNET_break_op (0);
      GNUNET_free (final_contract_url);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MISSING,
                                             "session_sig required if session_id given");
    }
    if (GNUNET_OK !=
        GNUNET_STRINGS_string_to_data (session_sig_str,
                                       strlen (session_sig_str),
                                       &sig,
                                       sizeof (struct GNUNET_CRYPTO_EddsaSignature)))
    {
      GNUNET_break_op (0);
      GNUNET_free (final_contract_url);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MALFORMED,
                                             "session_sig malformed");
    }
    mps.purpose.size = htonl (sizeof (struct TALER_MerchantPaySessionSigPS));
    mps.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAY_SESSION);
    GNUNET_CRYPTO_hash (order_id, strlen (order_id), &mps.h_order_id);
    GNUNET_CRYPTO_hash (session_id, strlen (session_id), &mps.h_session_id);
    if (GNUNET_OK != GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_PAY_SESSION,
                                                 &mps.purpose,
                                                 &sig,
                                                 &mi->pubkey.eddsa_pub))
    {
      /* pay session signature invalid */
      GNUNET_break_op (0);
      GNUNET_free (final_contract_url);
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_CHECK_PAYMENT_SESSION_SIGNATURE_INVALID,
                                             "session_sig fails to verify");
    }
  }

  db->preflight (db->cls);
  qs = db->find_contract_terms (db->cls,
                                &contract_terms,
                                &last_session_id,
                                order_id,
                                &mi->pubkey);
  if (0 > qs)
  {
    /* single, read-only SQL statements should never cause
       serialization problems */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    GNUNET_free (final_contract_url);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_CHECK_PAYMENT_DB_FETCH_CONTRACT_TERMS_ERROR,
                                              "db error fetching contract terms");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    /* Check that we're at least aware of the order */
    ret = check_order_and_request_payment (connection,
                                           mi,
                                           final_contract_url,
                                           session_id,
                                           resource_url,
                                           order_id);
    GNUNET_free (final_contract_url);
    return ret;
  }

  GNUNET_assert (NULL != contract_terms);
  GNUNET_assert (NULL != last_session_id);

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_break (0);
    json_decref (contract_terms);
    GNUNET_free (last_session_id);
    GNUNET_free (final_contract_url);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_CHECK_PAYMENT_FAILED_COMPUTE_PROPOSAL_HASH,
                                              "Failed to hash proposal");
  }

  h_contract_terms_str = GNUNET_STRINGS_data_to_string_alloc (&h_contract_terms,
                                                              sizeof (struct GNUNET_HashCode));

  /* Check if paid */
  {
    json_t *xcontract_terms = NULL;

    qs = db->find_paid_contract_terms_from_hash (db->cls,
                                                 &xcontract_terms,
                                                 &h_contract_terms,
                                                 &mi->pubkey);
    if (0 > qs)
    {
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      GNUNET_free_non_null (h_contract_terms_str);
      GNUNET_free (final_contract_url);
      json_decref (contract_terms);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                                "Merchant database error");
    }
    if (0 == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "not paid yet\n");
      ret = send_pay_request (connection,
                              final_contract_url,
                              session_id,
                              resource_url,
                              h_contract_terms_str);
      GNUNET_free_non_null (h_contract_terms_str);
      GNUNET_free (final_contract_url);
      json_decref (contract_terms);
      GNUNET_free (last_session_id);
      return ret;

    }
    GNUNET_break (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs);
    GNUNET_assert (NULL != xcontract_terms);
    json_decref (xcontract_terms);
  }

  {
    struct TALER_Amount amount;
    struct GNUNET_JSON_Specification spec[] = {
      TALER_JSON_spec_amount ("amount", &amount),
      GNUNET_JSON_spec_end ()
    };

    if (GNUNET_OK != GNUNET_JSON_parse (contract_terms, spec, NULL, NULL))
    {
      GNUNET_free_non_null (h_contract_terms_str);
      GNUNET_free (final_contract_url);
      json_decref (contract_terms);
      GNUNET_free (last_session_id);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_CHECK_PAYMENT_DB_FETCH_CONTRACT_TERMS_ERROR,
                                                "Merchant database error (contract terms corrupted)");
    }
    TALER_amount_get_zero (amount.currency, &refund_amount);
  }

  for (unsigned int i=0;i<MAX_RETRIES;i++)
  {
    qs = db->get_refunds_from_contract_terms_hash (db->cls,
                                                   &mi->pubkey,
                                                   &h_contract_terms,
                                                   &process_refunds_cb,
                                                   &refund_amount);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Database hard error on refunds_from_contract_terms_hash lookup: %s\n",
                GNUNET_h2s (&h_contract_terms));
    GNUNET_free_non_null (h_contract_terms_str);
    GNUNET_free (final_contract_url);
    json_decref (contract_terms);
    GNUNET_free (last_session_id);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                              "Merchant database error");
  }
  GNUNET_free_non_null (h_contract_terms_str);

  refunded = (0 != refund_amount.value) || (0 != refund_amount.fraction);

  ret = TMH_RESPONSE_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{s:o, s:b, s:b, s:o, s:s}",
                                      "contract_terms", contract_terms,
                                      "paid", 1,
                                      "refunded", refunded,
                                      "refund_amount", TALER_JSON_from_amount (&refund_amount),
                                      "last_session_id", last_session_id);
  GNUNET_free (final_contract_url);
  GNUNET_free (last_session_id);
  return ret;
}
