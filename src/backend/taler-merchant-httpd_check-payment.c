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

  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  contract_url = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "contract_url");
  session_id = MHD_lookup_connection_value (connection,
                                                MHD_GET_ARGUMENT_KIND,
                                                "session_id");
  session_sig_str = MHD_lookup_connection_value (connection,
                                                MHD_GET_ARGUMENT_KIND,
                                                "session_sig");
  instance_str = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "instance");

  if (NULL == instance_str)
    instance_str = "default";

  // Will only be set if we actually have a contract!
  char *h_contract_terms_str = NULL;

  struct MerchantInstance *mi = TMH_lookup_instance (instance_str);
  if (NULL == mi)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           TALER_EC_PAY_INSTANCE_UNKNOWN,
                                           "merchant instance unknown");

  if (NULL == order_id)
  {
    if (NULL == contract_url)
      return TMH_RESPONSE_reply_bad_request (connection,
                                             TALER_EC_PARAMETER_MISSING,
                                             "either order_id or contract_url must be given");
    goto do_pay;
    // No order_id given, redirect to a page that gives the wallet a new
    // contract.
  }

  if (NULL != session_id)
  {
    // If the session id is given, the frontend wants us
    // to verify the session signature.
    if (NULL == session_sig_str)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "pay session signature required but missing\n");
      goto do_pay;
    }

    struct GNUNET_CRYPTO_EddsaSignature sig;
    if (GNUNET_OK !=
        GNUNET_STRINGS_string_to_data (session_sig_str,
                                       strlen (session_sig_str),
                                       &sig,
                                       sizeof (struct GNUNET_CRYPTO_EddsaSignature)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "pay session signature malformed\n");
      goto do_pay;
    }
    struct TALER_MerchantPaySessionSigPS mps;
    mps.purpose.size = htonl (sizeof (struct TALER_MerchantPaySessionSigPS));
    mps.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAY_SESSION);
    GNUNET_assert (NULL != order_id);
    GNUNET_assert (NULL != session_id);
    GNUNET_CRYPTO_hash (order_id, strlen (order_id), &mps.h_order_id);
    GNUNET_CRYPTO_hash (session_id, strlen (session_id), &mps.h_session_id);
    if (GNUNET_OK != GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_PAY_SESSION,
                                                 &mps.purpose,
                                                 &sig,
                                                 &mi->pubkey.eddsa_pub))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "pay session signature invalid\n");
      goto do_pay;
    }
  }

  GNUNET_assert (NULL != order_id);

  enum GNUNET_DB_QueryStatus qs;
  json_t *contract_terms;
  struct GNUNET_HashCode h_contract_terms;

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
					      TALER_EC_PAY_DB_FETCH_PAY_ERROR,
					      "db error to previous /pay data");

  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    return TMH_RESPONSE_reply_not_found (connection,
                                         TALER_EC_PAY_DB_STORE_PAY_ERROR,
                                         "Proposal not found");
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_PAY_FAILED_COMPUTE_PROPOSAL_HASH,
                                              "Failed to hash proposal");
  }

  h_contract_terms_str = GNUNET_STRINGS_data_to_string_alloc (&h_contract_terms,
                                                              sizeof (struct GNUNET_HashCode));


  /* Check if transaction is already known, if not store it. */
  {
    struct GNUNET_HashCode h_xwire;
    struct GNUNET_TIME_Absolute xtimestamp;
    struct GNUNET_TIME_Absolute xrefund;
    struct TALER_Amount xtotal_amount;
    
    qs = db->find_transaction (db->cls,
			       &h_contract_terms,
			       &mi->pubkey,
			       &h_xwire,
			       &xtimestamp,
			       &xrefund,
			       &xtotal_amount);
    if (0 > qs)
    {
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      GNUNET_free_non_null (h_contract_terms_str);
      return TMH_RESPONSE_reply_internal_error (connection,
                                         TALER_EC_PAY_DB_FETCH_TRANSACTION_ERROR,
                                         "Merchant database error");
    }
    if (0 == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "not paid yet\n");
      goto do_pay;
    }
    GNUNET_break (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs);
  }

  GNUNET_free_non_null (h_contract_terms_str);

  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:b}",
                                       "paid",
                                       1);

do_pay:
  {
    char *url = TMH_make_absolute_backend_url (connection, "trigger-pay",
                                               "contract_url", contract_url,
                                               "session_id", session_id,
                                               "h_contract_terms", h_contract_terms_str,
                                               NULL);
    int ret = TMH_RESPONSE_reply_json_pack (connection,
                                            MHD_HTTP_OK,
                                            "{s:s}",
                                            "payment_redirect_url",
                                            url);
    GNUNET_free_non_null (h_contract_terms_str);
    GNUNET_free (url);
    return ret;
  }
}
