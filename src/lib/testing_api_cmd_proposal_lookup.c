/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file exchange/testing_api_cmd_proposal_lookup.c
 * @brief command to execute a proposal lookup
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"

/**
 * State for a "proposal lookup" CMD.  Not used by
 * the initial lookup operation.
 */
struct ProposalLookupState
{
  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * URL of the merchant backend.
   */
  const char *merchant_url;

  /**
   * Contract terms we downloaded. Only set if we got #MHD_HTTP_OK.
   */
  json_t *contract_terms;

  /**
   * Hash over the contract terms. Only set if we got #MHD_HTTP_OK.
   */
  struct GNUNET_HashCode contract_terms_hash;

  /**
   * Signature of the merchant. Only set if we got #MHD_HTTP_OK.
   */
  struct TALER_MerchantSignatureP merchant_sig;

  /**
   * Public key of the merchant. Only set if we got #MHD_HTTP_OK.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Expected status code.
   */
  unsigned int http_status;

  /**
   * /proposal/lookup operation handle.
   */
  struct TALER_MERCHANT_ProposalLookupOperation *plo;

  /**
   * Reference to a proposal operation.  Will offer the
   * nonce for the operation.
   */
  const char *proposal_reference;

  /**
   * Order id to lookup upon.  If null, the @a proposal_reference
   * will offer this value.
   */
  const char *order_id;
};


/**
 * Free the state of a "proposal lookup" CMD, and possibly
 * cancel it if it did not complete.
 *
 * @param cls closure.
 * @param cmd command being freed.
 */
static void
proposal_lookup_cleanup (void *cls,
                         const struct TALER_TESTING_Command *cmd)
{
  struct ProposalLookupState *pls = cls;

  if (NULL != pls->plo)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command '%s' did not complete\n",
                cmd->label);
    TALER_MERCHANT_proposal_lookup_cancel (pls->plo);
    pls->plo = NULL;
  }
  if (NULL != pls->contract_terms)
  {
    json_decref (pls->contract_terms);
    pls->contract_terms = NULL;
  }
  GNUNET_free (pls);
}


/**
 * Callback for "proposal lookup" operation, to check the
 * response code is as expected.
 *
 * @param cls closure
 * @param hr HTTP response we got
 * @param contract_terms the contract terms; they are the
 *        backend-filled up proposal minus cryptographic
 *        information.
 * @param sig merchant signature over the contract terms.
 * @param hash hash code of the contract terms.
 */
static void
proposal_lookup_cb (void *cls,
                    const struct TALER_MERCHANT_HttpResponse *hr,
                    const json_t *contract_terms,
                    const struct TALER_MerchantSignatureP *sig,
                    const struct GNUNET_HashCode *hash)
{
  struct ProposalLookupState *pls = cls;

  pls->plo = NULL;
  if (pls->http_status != hr->http_status)
    TALER_TESTING_FAIL (pls->is);
  if (MHD_HTTP_OK == hr->http_status)
  {
    pls->contract_terms = json_object_get (hr->reply,
                                           "contract_terms");
    if (NULL == pls->contract_terms)
      TALER_TESTING_FAIL (pls->is);
    json_incref (pls->contract_terms);
    pls->contract_terms_hash = *hash;
    pls->merchant_sig = *sig;
    {
      const char *error_name;
      unsigned int error_line;
      struct GNUNET_JSON_Specification spec[] = {
        GNUNET_JSON_spec_fixed_auto ("merchant_pub",
                                     &pls->merchant_pub),
        GNUNET_JSON_spec_end ()
      };

      if (GNUNET_OK !=
          GNUNET_JSON_parse (contract_terms,
                             spec,
                             &error_name,
                             &error_line))
        TALER_TESTING_FAIL (pls->is);
    }
  }
  TALER_TESTING_interpreter_next (pls->is);
}


/**
 * Run the "proposal lookup" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being run.
 * @param is interpreter state.
 */
static void
proposal_lookup_run (void *cls,
                     const struct TALER_TESTING_Command *cmd,
                     struct TALER_TESTING_Interpreter *is)
{
  struct ProposalLookupState *pls = cls;
  const char *order_id;
  const struct TALER_MerchantPublicKeyP *nonce;
  /* Only used if we do NOT use the nonce from traits.  */
  struct TALER_MerchantPublicKeyP dummy_nonce;
  #define GET_TRAIT_NONCE(cmd,ptr) \
  TALER_TESTING_get_trait_merchant_pub (cmd, 1, ptr)

  pls->is = is;

  if (NULL != pls->order_id)
  {
    order_id = pls->order_id;
    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                &dummy_nonce,
                                sizeof (dummy_nonce));
    nonce = &dummy_nonce;
  }
  else
  {
    const struct TALER_TESTING_Command *proposal_cmd;

    proposal_cmd = TALER_TESTING_interpreter_lookup_command
                     (is, pls->proposal_reference);

    if (NULL == proposal_cmd)
      TALER_TESTING_FAIL (is);

    if (GNUNET_OK != GET_TRAIT_NONCE (proposal_cmd,
                                      &nonce))
    {
      GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                  &dummy_nonce,
                                  sizeof (dummy_nonce));
      nonce = &dummy_nonce;
    }

    if (GNUNET_OK != TALER_TESTING_get_trait_order_id
          (proposal_cmd, 0, &order_id))
      TALER_TESTING_FAIL (is);
  }
  pls->plo = TALER_MERCHANT_proposal_lookup (is->ctx,
                                             pls->merchant_url,
                                             order_id,
                                             &nonce->eddsa_pub,
                                             &proposal_lookup_cb,
                                             pls);
  GNUNET_assert (NULL != pls->plo);
}


/**
 * Offer internal data to other commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
proposal_lookup_traits (void *cls,
                        const void **ret,
                        const char *trait,
                        unsigned int index)
{
  struct ProposalLookupState *pls = cls;
  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_contract_terms (0,
                                             pls->contract_terms),
    TALER_TESTING_make_trait_h_contract_terms (0,
                                               &pls->contract_terms_hash),
    TALER_TESTING_make_trait_merchant_sig (0,
                                           &pls->merchant_sig),
    TALER_TESTING_make_trait_merchant_pub (0,
                                           &pls->merchant_pub),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


/**
 * Make a "proposal lookup" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant backend
 *        serving the proposal lookup request.
 * @param http_status expected HTTP response code.
 * @param proposal_reference reference to a "proposal" CMD.
 * @param order_id order id to lookup, can be NULL.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_proposal_lookup
  (const char *label,
  const char *merchant_url,
  unsigned int http_status,
  const char *proposal_reference,
  const char *order_id)
{
  struct ProposalLookupState *pls;

  pls = GNUNET_new (struct ProposalLookupState);
  pls->http_status = http_status;
  pls->proposal_reference = proposal_reference;
  pls->merchant_url = merchant_url;
  pls->order_id = order_id;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pls,
      .label = label,
      .run = &proposal_lookup_run,
      .cleanup = &proposal_lookup_cleanup,
      .traits = &proposal_lookup_traits
    };

    return cmd;
  }
}
