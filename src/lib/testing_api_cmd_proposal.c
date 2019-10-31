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
 * @file exchange/testing_api_cmd_exec_merchant.c
 * @brief command to execute the merchant backend service.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"

/**
 * State for a "proposal" CMD.
 */
struct ProposalState
{

  /**
   * The order.
   */
  const char *order;

  /**
   * Expected status code.
   */
  unsigned int http_status;

  /**
   * Order id.
   */
  const char *order_id;

  /**
   * Contract terms obtained from the backend.
   */
  json_t *contract_terms;

  /**
   * Contract terms hash code.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * The /proposal operation handle.
   */
  struct TALER_MERCHANT_ProposalOperation *po;

  /**
   * The (initial) /proposal/lookup operation handle.
   * The logic is such that after a proposal creation,
   * it soon makes a proposal lookup in order to check
   * if the merchant backend is actually aware.
   */
  struct TALER_MERCHANT_ProposalLookupOperation *plo;

  /**
   * The nonce.
   */
  struct GNUNET_CRYPTO_EddsaPublicKey nonce;

  /**
   * URL of the merchant backend.
   */
  const char *merchant_url;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Merchant signature over the proposal.
   */
  struct TALER_MerchantSignatureP merchant_sig;

  /**
   * Merchant public key.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;
};


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
 * Offer internal data to other commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
proposal_traits (void *cls,
                 const void **ret,
                 const char *trait,
                 unsigned int index)
{

  struct ProposalState *ps = cls;
  #define MAKE_TRAIT_NONCE(ptr) \
  TALER_TESTING_make_trait_peer_key_pub (1, ptr)

  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_order_id (0, ps->order_id),
    TALER_TESTING_make_trait_contract_terms
      (0, ps->contract_terms),
    TALER_TESTING_make_trait_h_contract_terms
      (0, &ps->h_contract_terms),
    TALER_TESTING_make_trait_merchant_sig (0, &ps->merchant_sig),
    TALER_TESTING_make_trait_peer_key_pub
      (0, &ps->merchant_pub.eddsa_pub),
    MAKE_TRAIT_NONCE (&ps->nonce),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


/**
 * Used to fill the "proposal" CMD state with backend-provided
 * values.  Also double-checks that the proposal was correctly
 * created.
 *
 * @param cls closure
 * @param http_status HTTP status code we got
 * @param json full response we got
 */
static void
proposal_lookup_initial_cb
  (void *cls,
  unsigned int http_status,
  const json_t *json,
  const json_t *contract_terms,
  const struct TALER_MerchantSignatureP *sig,
  const struct GNUNET_HashCode *hash)
{
  struct ProposalState *ps = cls;
  struct TALER_MerchantPublicKeyP merchant_pub;
  const char *error_name;
  unsigned int error_line;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("merchant_pub",
                                 &merchant_pub),
    GNUNET_JSON_spec_end ()
  };

  ps->plo = NULL;
  if (ps->http_status != http_status)
    TALER_TESTING_FAIL (ps->is);

  ps->contract_terms = json_deep_copy (contract_terms);
  ps->h_contract_terms = *hash;
  ps->merchant_sig = *sig;

  if (GNUNET_OK !=
      GNUNET_JSON_parse (contract_terms,
                         spec,
                         &error_name,
                         &error_line))
  {
    char *log;

    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Parser failed on %s:%u\n",
                error_name,
                error_line);
    log = json_dumps (ps->contract_terms,
                      JSON_INDENT (1));
    fprintf (stderr,
             "%s\n",
             log);
    free (log);
    TALER_TESTING_FAIL (ps->is);
  }

  ps->merchant_pub = merchant_pub;

  TALER_TESTING_interpreter_next (ps->is);
}


/**
 * Callback that processes the response following a
 * proposal's put.  NOTE: no contract terms are included
 * here; they need to be taken via the "proposal lookup"
 * method.
 *
 * @param cls closure.
 * @param http_status HTTP response code coming from
 *        the backend.
 * @param ec error code.
 * @param obj when successful, it has the format:
 *        '{"order_id": "<order_id>"}'
 * @param order_id order id of the proposal.
 */
static void
proposal_cb (void *cls,
             unsigned int http_status,
             enum TALER_ErrorCode ec,
             const json_t *obj,
             const char *order_id)
{
  struct ProposalState *ps = cls;

  ps->po = NULL;

  if (ps->http_status != http_status)
  {
    TALER_LOG_ERROR ("Given vs expected: %u vs %u\n",
                     http_status,
                     ps->http_status);
    TALER_TESTING_FAIL (ps->is);
  }

  if (0 == ps->http_status)
  {
    TALER_LOG_DEBUG ("/proposal, expected 0 status code\n");
    TALER_TESTING_interpreter_next (ps->is);
    return;
  }

  switch (http_status)
  {
  case MHD_HTTP_OK:
    ps->order_id = GNUNET_strdup (order_id);
    break;
  default:
    {
      char *s = json_dumps (obj, JSON_COMPACT);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unexpected status code from /proposal:" \
                  " %u (%d). Command %s, response: %s\n",
                  http_status,
                  ec,
                  TALER_TESTING_interpreter_get_current_label (
                    ps->is),
                  s);
      GNUNET_free_non_null (s);
      /**
       * Not failing, as test cases are _supposed_
       * to create non 200 OK situations.
       */
      TALER_TESTING_interpreter_next (ps->is);
    }
    return;
  }

  if (NULL ==
      (ps->plo = TALER_MERCHANT_proposal_lookup
                   (ps->is->ctx,
                   ps->merchant_url,
                   ps->order_id,
                   &ps->nonce,
                   &proposal_lookup_initial_cb,
                   ps)))
    TALER_TESTING_FAIL (ps->is);
}


/**
 * Run a "proposal" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being run.
 * @param is interpreter state.
 */
static void
proposal_run (void *cls,
              const struct TALER_TESTING_Command *cmd,
              struct TALER_TESTING_Interpreter *is)
{
  struct ProposalState *ps = cls;
  json_t *order;
  json_error_t error;

  ps->is = is;
  order = json_loads (ps->order,
                      JSON_REJECT_DUPLICATES,
                      &error);
  if (NULL == order)
  {
    // human error here.
    GNUNET_break (0);
    fprintf (stderr, "%s\n", error.text);
    TALER_TESTING_interpreter_fail (is);
    return;
  }

  if (NULL == json_object_get (order,
                               "order_id"))
  {
    struct GNUNET_TIME_Absolute now;
    char *order_id;

    // FIXME: should probably use get_monotone() to ensure uniqueness!
    now = GNUNET_TIME_absolute_get ();
    order_id = GNUNET_STRINGS_data_to_string_alloc
                 (&now.abs_value_us,
                 sizeof (now.abs_value_us));
    json_object_set_new (order,
                         "order_id",
                         json_string (order_id));
    GNUNET_free (order_id);
  }

  GNUNET_CRYPTO_random_block
    (GNUNET_CRYPTO_QUALITY_WEAK,
    &ps->nonce,
    sizeof (struct GNUNET_CRYPTO_EddsaPublicKey));

  ps->po = TALER_MERCHANT_order_put (is->ctx,
                                     ps->merchant_url,
                                     order,
                                     &proposal_cb,
                                     ps);
  json_decref (order);
  GNUNET_assert (NULL != ps->po);
}


/**
 * Free the state of a "proposal" CMD, and possibly
 * cancel it if it did not complete.
 *
 * @param cls closure.
 * @param cmd command being freed.
 */
static void
proposal_cleanup (void *cls,
                  const struct TALER_TESTING_Command *cmd)
{
  struct ProposalState *ps = cls;

  if (NULL != ps->po)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command '%s' did not complete (proposal put)\n",
                cmd->label);
    TALER_MERCHANT_proposal_cancel (ps->po);
    ps->po = NULL;
  }

  if (NULL != ps->plo)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command '%s' did not complete"
                " (proposal lookup)\n",
                cmd->label);
    TALER_MERCHANT_proposal_lookup_cancel (ps->plo);
    ps->plo = NULL;
  }

  json_decref (ps->contract_terms);
  GNUNET_free_non_null ((void *) ps->order_id);
  GNUNET_free (ps);
}


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
  }
  GNUNET_free (pls);
}


/**
 * Make the "proposal" command.
 *
 * @param label command label
 * @param merchant_url base URL of the merchant serving
 *        the proposal request.
 * @param http_status expected HTTP status.
 * @param order the order to PUT to the merchant.
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_proposal (const char *label,
                            const char *merchant_url,
                            unsigned int http_status,
                            const char *order)
{
  struct ProposalState *ps;

  ps = GNUNET_new (struct ProposalState);
  ps->order = order;
  ps->http_status = http_status;
  ps->merchant_url = merchant_url;

  struct TALER_TESTING_Command cmd = {
    .cls = ps,
    .label = label,
    .run = &proposal_run,
    .cleanup = &proposal_cleanup,
    .traits = &proposal_traits
  };

  return cmd;
}


/**
 * Callback for "proposal lookup" operation, to check the
 * response code is as expected.
 *
 * @param cls closure
 * @param http_status HTTP status code we got
 * @param json full response we got
 * @param contract_terms the contract terms; they are the
 *        backend-filled up proposal minus cryptographic
 *        information.
 * @param sig merchant signature over the contract terms.
 * @param hash hash code of the contract terms.
 */
static void
proposal_lookup_cb (void *cls,
                    unsigned int http_status,
                    const json_t *json,
                    const json_t *contract_terms,
                    const struct TALER_MerchantSignatureP *sig,
                    const struct GNUNET_HashCode *hash)
{
  struct ProposalLookupState *pls = cls;

  pls->plo = NULL;
  if (pls->http_status != http_status)
    TALER_TESTING_FAIL (pls->is);

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
  const struct GNUNET_CRYPTO_EddsaPublicKey *nonce;
  /* Only used if we do NOT use the nonce from traits.  */
  struct GNUNET_CRYPTO_EddsaPublicKey dummy_nonce;
  #define GET_TRAIT_NONCE(cmd,ptr) \
  TALER_TESTING_get_trait_peer_key_pub (cmd, 1, ptr)

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
      TALER_TESTING_FAIL (is);

    if (GNUNET_OK != TALER_TESTING_get_trait_order_id
          (proposal_cmd, 0, &order_id))
      TALER_TESTING_FAIL (is);
  }
  pls->plo = TALER_MERCHANT_proposal_lookup (is->ctx,
                                             pls->merchant_url,
                                             order_id,
                                             nonce,
                                             &proposal_lookup_cb,
                                             pls);
  GNUNET_assert (NULL != pls->plo);
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

  struct TALER_TESTING_Command cmd = {
    .cls = pls,
    .label = label,
    .run = &proposal_lookup_run,
    .cleanup = &proposal_lookup_cleanup
  };

  return cmd;
}
