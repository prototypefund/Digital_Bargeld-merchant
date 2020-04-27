/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2018, 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_post-orders-ID-claim.c
 * @brief headers for POST /orders/$ID/claim handler
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd_post-orders-ID-claim.h"


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


/**
 * Run transaction to claim @a order_id for @a nonce.
 *
 * @param instance_id instance to claim order at
 * @param order_id order to claim
 * @param nonce nonce to use for the claim
 * @param[out] contract_terms set to the resulting contract terms
 *             (for any non-negative result;
 * @return transaction status code
 *         #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if the order was claimed by a different
 *         nonce (@a contract_terms set to non-NULL)
 *                OR if the order is is unknown (@a contract_terms is NULL)
 *         #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT if the order was successfully claimed
 */
static enum GNUNET_DB_QueryStatus
claim_order (const char *instance_id,
             const char *order_id,
             const char *nonce,
             json_t **contract_terms)
{
  enum GNUNET_DB_QueryStatus qs;

  if (GNUNET_OK !=
      TMH_db->start (TMH_db->cls,
                     "claim order"))
  {
    GNUNET_break (0);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  qs = TMH_db->lookup_contract_terms (TMH_db->cls,
                                      order_id,
                                      instance_id,
                                      contract_terms);
  if (0 > qs)
  {
    TMH_db->rollback (TMH_db->cls);
    return qs;
  }

  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    /* see if we have this order in our table of unclaimed orders */
    qs = TMH_db->lookup_order (TMH_db->cls,
                               instance_id,
                               order_id,
                               contract_terms);
    if (0 >= qs)
    {
      TMH_db->rollback (TMH_db->cls);
      return qs;
    }
    GNUNET_assert (NULL != contract_terms);
    GNUNET_assert (0 ==
                   json_object_set_new (*contract_terms,
                                        "nonce",
                                        json_string (nonce)));
    qs = TMH_db->insert_contract_terms (TMH_db->cls,
                                        instance_id,
                                        order_id,
                                        *contract_terms);
    if (0 > qs)
    {
      TMH_db->rollback (TMH_db->cls);
      json_decref (*contract_terms);
      *contract_terms = NULL;
      return qs;
    }
    // FIXME: should we remove the ORDER from the order table here?
    qs = TMH_db->commit (TMH_db->cls);
    if (0 > qs)
      return qs;
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  }
  else
  {
    const char *stored_nonce;

    TMH_db->rollback (TMH_db->cls);
    GNUNET_assert (NULL != *contract_terms);
    stored_nonce
      = json_string_value (json_object_get (*contract_terms,
                                            "nonce"));
    if (NULL == stored_nonce)
    {
      /* this should not be possible: contract_terms should always
         have a nonce! */
      GNUNET_break (0);
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
    if (0 != strcmp (stored_nonce,
                     nonce))
    {
      return GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
    }
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  }
}


/**
 * Manage a POST /orders/$ID/claim request.  Allows the client to
 * claim the order (unless already claims) and creates the respective
 * contract.  Returns the contract terms.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_post_orders_ID_claim (const struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          struct TMH_HandlerContext *hc)
{
  const char *order_id = hc->infix;
  const char *nonce;
  enum GNUNET_DB_QueryStatus qs;
  json_t *contract_terms;

  {
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("nonce",
                               &nonce),
      GNUNET_JSON_spec_end ()
    };
    enum GNUNET_GenericReturnValue res;

    res = TALER_MHD_parse_json_data (connection,
                                     hc->request_body,
                                     spec);
    /* json is malformed */
    if (GNUNET_NO == res)
    {
      GNUNET_break_op (0);
      return MHD_YES;
    }
    /* other internal errors might have occurred */
    if (GNUNET_SYSERR == res)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_BAD_REQUEST,
                                         TALER_EC_PARAMETER_MISSING,
                                         "nonce");
  }
  contract_terms = NULL;
  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    TMH_db->preflight (TMH_db->cls);
    qs = claim_order (hc->instance->settings.id,
                      order_id,
                      nonce,
                      &contract_terms);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_ORDERS_CLAIM_HARD_DB_ERROR,
                                       "Failed to run DB transaction to claim order");
  case GNUNET_DB_STATUS_SOFT_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_ORDERS_CLAIM_SOFT_DB_ERROR,
                                       "Failed to serialize DB transaction to claim order");
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    if (NULL == contract_terms)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         TALER_EC_ORDERS_CLAIM_NOT_FOUND,
                                         "unknown order id");
    /* already claimed! */
    json_decref (contract_terms);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_CONFLICT,
                                       TALER_EC_ORDERS_ALREADY_CLAIMED,
                                       "order already claimed");
  case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
    GNUNET_assert (NULL != contract_terms);
    break; /* Good! return signature (below) */
  }

  /* create proposal signature */
  {
    struct GNUNET_CRYPTO_EddsaSignature merchant_sig;
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

    GNUNET_CRYPTO_eddsa_sign (&hc->instance->merchant_priv.eddsa_priv,
                              &pdps,
                              &merchant_sig);
    return TALER_MHD_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{ s:o, s:o }",
                                      "contract_terms",
                                      contract_terms,
                                      "sig",
                                      GNUNET_JSON_from_data_auto (
                                        &merchant_sig));
  }
}


/* end of taler-merchant-httpd_post-orders-ID-claim.c */
