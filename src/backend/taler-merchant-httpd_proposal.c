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
 * @file backend/taler-merchant-httpd_proposal.c
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
 * Manage a GET /proposal request. Query the db and returns the
 * proposal's data related to the transaction id given as the URL's
 * parameter.
 *
 * Binds the proposal to a nonce.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_proposal_lookup (struct TMH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            void **connection_cls,
                            const char *upload_data,
                            size_t *upload_data_size,
                            struct MerchantInstance *mi)
{
  const char *order_id;
  const char *nonce;
  enum GNUNET_DB_QueryStatus qs;
  json_t *contract_terms;
  struct GNUNET_CRYPTO_EddsaSignature merchant_sig;
  const char *stored_nonce;

  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MISSING,
                                       "order_id");
  nonce = MHD_lookup_connection_value (connection,
                                       MHD_GET_ARGUMENT_KIND,
                                       "nonce");
  if (NULL == nonce)
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MISSING,
                                       "nonce");
  db->preflight (db->cls);
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
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_PROPOSAL_LOOKUP_DB_ERROR,
                                       "An error occurred while retrieving proposal data from db");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    struct GNUNET_TIME_Absolute timestamp;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
      GNUNET_JSON_spec_end ()
    };
    int res;

    db->preflight (db->cls);
    qs = db->find_order (db->cls,
                         &contract_terms,
                         order_id,
                         &mi->pubkey);
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    {
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         TALER_EC_PROPOSAL_LOOKUP_NOT_FOUND,
                                         "unknown order id");
    }
    GNUNET_assert (NULL != contract_terms);
    json_object_set_new (contract_terms,
                         "nonce",
                         json_string (nonce));

    /* extract fields we need to sign separately */
    res = TALER_MHD_parse_json_data (connection,
                                     contract_terms,
                                     spec);
    if (GNUNET_NO == res)
    {
      return MHD_YES;
    }
    if (GNUNET_SYSERR == res)
    {
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PROPOSAL_ORDER_PARSE_ERROR,
                                         "Impossible to parse the order");
    }

    for (unsigned int i = 0; i<MAX_RETRIES; i++)
    {
      db->preflight (db->cls);
      qs = db->insert_contract_terms (db->cls,
                                      order_id,
                                      &mi->pubkey,
                                      timestamp,
                                      contract_terms);
      if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
        break;
    }
    if (0 > qs)
    {
      /* Special report if retries insufficient */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PROPOSAL_STORE_DB_ERROR,
                                         "db error: could not store this proposal's data into db");
    }
    // FIXME: now we can delete (merchant_pub, order_id) from the merchant_orders table
  }

  GNUNET_assert (NULL != contract_terms);

  stored_nonce
    = json_string_value (json_object_get (contract_terms,
                                          "nonce"));

  if (NULL == stored_nonce)
  {
    GNUNET_break (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_PROPOSAL_ORDER_PARSE_ERROR,
                                       "existing proposal has no nonce");
  }

  if (0 != strcmp (stored_nonce,
                   nonce))
  {
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PROPOSAL_LOOKUP_NOT_FOUND,
                                       "mismatched nonce");
  }


  /* create proposal signature */
  {
    struct TALER_ProposalDataPS pdps = {
      .purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT),
      .purpose.size = htonl (sizeof (pdps))
    };

    if (GNUNET_OK !=
        TALER_JSON_hash (contract_terms,
                         &pdps.hash))
    {
      GNUNET_break (0);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_INTERNAL_LOGIC_ERROR,
                                         "Could not hash order");
    }

    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CRYPTO_eddsa_sign (&mi->privkey.eddsa_priv,
                                             &pdps.purpose,
                                             &merchant_sig));
  }
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{ s:o, s:o }",
                                    "contract_terms",
                                    contract_terms,
                                    "sig",
                                    GNUNET_JSON_from_data_auto (
                                      &merchant_sig));
}


/* end of taler-merchant-httpd_proposal.c */
