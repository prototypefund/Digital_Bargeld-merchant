/*
  This file is part of TALER
  Copyright (C) 2014-2018, 2020 Taler Systems SA

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
 * @file exchange/testing_api_cmd_claim_order.c
 * @brief command to claim an order
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"

/**
 * State for a "order claim" CMD.  Not used by
 * the initial claim operation.
 */
struct OrderClaimState
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
   * /order/claim operation handle.
   */
  struct TALER_MERCHANT_OrderClaimHandle *och;

  /**
   * Reference to a order operation.  Will offer the
   * nonce for the operation.
   */
  const char *order_reference;

  /**
   * Order id to claim upon.  If null, the @a order_reference
   * will offer this value.
   */
  const char *order_id;
};


/**
 * Free the state of a "order claim" CMD, and possibly
 * cancel it if it did not complete.
 *
 * @param cls closure.
 * @param cmd command being freed.
 */
static void
order_claim_cleanup (void *cls,
                     const struct TALER_TESTING_Command *cmd)
{
  struct OrderClaimState *pls = cls;

  if (NULL != pls->och)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command '%s' did not complete\n",
                cmd->label);
    TALER_MERCHANT_order_claim_cancel (pls->och);
    pls->och = NULL;
  }
  if (NULL != pls->contract_terms)
  {
    json_decref (pls->contract_terms);
    pls->contract_terms = NULL;
  }
  GNUNET_free (pls);
}


/**
 * Callback for "order claim" operation, to check the
 * response code is as expected.
 *
 * @param cls closure
 * @param hr HTTP response we got
 * @param contract_terms the contract terms; they are the
 *        backend-filled up order minus cryptographic
 *        information.
 * @param sig merchant signature over the contract terms.
 * @param hash hash code of the contract terms.
 */
static void
order_claim_cb (void *cls,
                const struct TALER_MERCHANT_HttpResponse *hr,
                const json_t *contract_terms,
                const struct TALER_MerchantSignatureP *sig,
                const struct GNUNET_HashCode *hash)
{
  struct OrderClaimState *pls = cls;

  pls->och = NULL;
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
 * Run the "order claim" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being run.
 * @param is interpreter state.
 */
static void
order_claim_run (void *cls,
                 const struct TALER_TESTING_Command *cmd,
                 struct TALER_TESTING_Interpreter *is)
{
  struct OrderClaimState *pls = cls;
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
    const struct TALER_TESTING_Command *order_cmd;

    order_cmd
      = TALER_TESTING_interpreter_lookup_command (is,
                                                  pls->order_reference);
    if (NULL == order_cmd)
      TALER_TESTING_FAIL (is);
    if (GNUNET_OK !=
        GET_TRAIT_NONCE (order_cmd,
                         &nonce))
    {
      GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                  &dummy_nonce,
                                  sizeof (dummy_nonce));
      nonce = &dummy_nonce;
    }

    if (GNUNET_OK !=
        TALER_TESTING_get_trait_order_id (order_cmd,
                                          0,
                                          &order_id))
      TALER_TESTING_FAIL (is);
  }
  pls->och = TALER_MERCHANT_order_claim (is->ctx,
                                         pls->merchant_url,
                                         order_id,
                                         &nonce->eddsa_pub,
                                         &order_claim_cb,
                                         pls);
  GNUNET_assert (NULL != pls->och);
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
order_claim_traits (void *cls,
                    const void **ret,
                    const char *trait,
                    unsigned int index)
{
  struct OrderClaimState *pls = cls;
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
 * Make a "order claim" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant backend
 *        serving the order claim request.
 * @param http_status expected HTTP response code.
 * @param order_reference reference to a POST order CMD, can be NULL if @a order_id given
 * @param order_id order id to lookup, can be NULL (then we use @a order_reference)
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_claim_order (
  const char *label,
  const char *merchant_url,
  unsigned int http_status,
  const char *order_reference,
  const char *order_id)
{
  struct OrderClaimState *pls;

  pls = GNUNET_new (struct OrderClaimState);
  pls->http_status = http_status;
  pls->order_reference = order_reference;
  pls->merchant_url = merchant_url;
  pls->order_id = order_id;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pls,
      .label = label,
      .run = &order_claim_run,
      .cleanup = &order_claim_cleanup,
      .traits = &order_claim_traits
    };

    return cmd;
  }
}
