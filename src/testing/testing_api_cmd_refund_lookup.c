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
 * @file lib/testing_api_cmd_refund_lookup.c
 * @brief command to test refunds.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "refund lookup" CMD.
 */
struct RefundLookupState
{
  /**
   * Operation handle for a GET /public/refund request.
   */
  struct TALER_MERCHANT_RefundLookupOperation *rlo;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * Order id to look up.
   */
  const char *order_id;

  /**
   * Reference to a "pay" CMD, used to double-check if
   * refunded coins were actually spent:
   */
  const char *pay_reference;

  /**
   * Reference to a "refund increase" CMD that offer
   * the expected amount to be refunded; can be NULL.
   */
  const char *increase_reference;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_code;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Explicit amount to be refunded, must be defined if @a
   * increase_reference is NULL.
   */
  const char *refund_amount;
};


/**
 * Free the state of a "refund lookup" CMD, and
 * possibly cancel a pending "refund lookup" operation.
 *
 * @param cls closure
 * @param cmd command currently being freed.
 */
static void
refund_lookup_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  struct RefundLookupState *rls = cls;

  if (NULL != rls->rlo)
  {
    TALER_LOG_WARNING ("Refund-lookup operation"
                       " did not complete\n");
    TALER_MERCHANT_refund_lookup_cancel (rls->rlo);
  }
  GNUNET_free (rls);
}


/**
 * Process "GET /public/refund" (lookup) response;
 * mainly checking if the refunded amount matches the
 * expectation.
 *
 * @param cls closure
 * @param hr HTTP response we got
 * @param h_contract_terms hash of the contract terms to which the refund is applied
 * @param merchant_pub public key of the merchant
 * @param num_details length of the @a details array
 * @param details details about the refund processing
 */
static void
refund_lookup_cb (void *cls,
                  const struct TALER_MERCHANT_HttpResponse *hr,
                  const struct GNUNET_HashCode *h_contract_terms,
                  const struct TALER_MerchantPublicKeyP *merchant_pub,
                  unsigned int num_details,
                  const struct TALER_MERCHANT_RefundDetail *details)
{
  struct RefundLookupState *rls = cls;
  struct GNUNET_CONTAINER_MultiHashMap *map;
  const char *coin_reference;
  const char *icoin_reference;
  const char *refund_amount;
  struct TALER_Amount acc;
  struct TALER_Amount ra;

  rls->rlo = NULL;
  if (MHD_HTTP_GONE == rls->http_code)
  {
    /* special case: GONE is not the top-level code, but expected INSIDE the details */
    if (MHD_HTTP_OK != hr->http_status)
      TALER_TESTING_FAIL (rls->is);
    for (unsigned int i = 0; i<num_details; i++)
      if (MHD_HTTP_GONE != details[i].hr.http_status)
        TALER_TESTING_FAIL (rls->is);
    /* all good */
    TALER_TESTING_interpreter_next (rls->is);
    return;
  }
  if (rls->http_code != hr->http_status)
    TALER_TESTING_FAIL (rls->is);
  if (MHD_HTTP_OK != hr->http_status)
  {
    TALER_TESTING_interpreter_next (rls->is);
    return;
  }
  map = GNUNET_CONTAINER_multihashmap_create (1, GNUNET_NO);
  /* Put in array every refunded coin.  */
  for (unsigned int i = 0; i<num_details; i++)
  {
    struct GNUNET_HashCode h_coin_pub;

    if (MHD_HTTP_OK != details[i].hr.http_status)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Got unexpected status %u/%d for refunded coin %u\n",
                  details[i].hr.http_status,
                  (int) details[i].hr.ec,
                  i);
      GNUNET_CONTAINER_multihashmap_destroy (map);
      TALER_TESTING_FAIL (rls->is);
      return;
    }
    TALER_LOG_DEBUG ("Coin %s refund is %s\n",
                     TALER_B2S (&details[i].coin_pub),
                     TALER_amount2s (&details[i].refund_amount));
    GNUNET_CRYPTO_hash (&details[i].coin_pub,
                        sizeof (struct TALER_CoinSpendPublicKeyP),
                        &h_coin_pub);
    if (GNUNET_OK !=
        GNUNET_CONTAINER_multihashmap_put (
          map,
          &h_coin_pub,
          (void *) &details[i],
          GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
    {
      GNUNET_CONTAINER_multihashmap_destroy (map);
      TALER_TESTING_FAIL (rls->is);
    }
  }

  /* Compare spent coins with refunded, and if they match,
   * increase an accumulator.  */
  {
    const struct TALER_TESTING_Command *pay_cmd;

    if (NULL == (pay_cmd = TALER_TESTING_interpreter_lookup_command (
                   rls->is,
                   rls->pay_reference)))
    {
      GNUNET_CONTAINER_multihashmap_destroy (map);
      TALER_TESTING_FAIL (rls->is);
    }

    if (GNUNET_OK !=
        TALER_TESTING_get_trait_coin_reference (
          pay_cmd,
          0,
          &coin_reference))
    {
      GNUNET_CONTAINER_multihashmap_destroy (map);
      TALER_TESTING_FAIL (rls->is);
    }
  }

  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero ("EUR",
                                        &acc));
  {
    char *coin_reference_dup;

    coin_reference_dup = GNUNET_strdup (coin_reference);
    for (icoin_reference = strtok (coin_reference_dup, ";");
         NULL != icoin_reference;
         icoin_reference = strtok (NULL, ";"))
    {
      const struct TALER_CoinSpendPrivateKeyP *icoin_priv;
      struct TALER_CoinSpendPublicKeyP icoin_pub;
      struct GNUNET_HashCode h_icoin_pub;
      const struct TALER_MERCHANT_RefundDetail *idetail;
      const struct TALER_TESTING_Command *icoin_cmd;

      if (NULL ==
          (icoin_cmd =
             TALER_TESTING_interpreter_lookup_command (rls->is,
                                                       icoin_reference)) )
      {
        GNUNET_break (0);
        TALER_LOG_ERROR ("Bad reference `%s'\n",
                         icoin_reference);
        TALER_TESTING_interpreter_fail (rls->is);
        GNUNET_CONTAINER_multihashmap_destroy (map);
        return;
      }

      if (GNUNET_OK !=
          TALER_TESTING_get_trait_coin_priv (icoin_cmd,
                                             0,
                                             &icoin_priv))
      {
        GNUNET_break (0);
        TALER_LOG_ERROR ("Command `%s' failed to give coin priv trait\n",
                         icoin_reference);
        TALER_TESTING_interpreter_fail (rls->is);
        GNUNET_CONTAINER_multihashmap_destroy (map);
        return;
      }
      GNUNET_CRYPTO_eddsa_key_get_public (&icoin_priv->eddsa_priv,
                                          &icoin_pub.eddsa_pub);
      TALER_LOG_DEBUG ("Looking at coin %s\n",
                       TALER_B2S (&icoin_pub));
      GNUNET_CRYPTO_hash (&icoin_pub,
                          sizeof (struct TALER_CoinSpendPublicKeyP),
                          &h_icoin_pub);

      idetail = GNUNET_CONTAINER_multihashmap_get (map,
                                                   &h_icoin_pub);

      /* Can be NULL: not all coins are involved in refund */
      if (NULL == idetail)
        continue;
      TALER_LOG_DEBUG ("Found coin %s refund of %s\n",
                       TALER_B2S (&idetail->coin_pub),
                       TALER_amount2s (&idetail->refund_amount));
      GNUNET_assert (0 <=
                     TALER_amount_add (&acc,
                                       &acc,
                                       &idetail->refund_amount));
    }
    GNUNET_free (coin_reference_dup);
  }


  {
    const struct TALER_TESTING_Command *increase_cmd;

    if (NULL !=
        (increase_cmd
           = TALER_TESTING_interpreter_lookup_command (rls->is,
                                                       rls->increase_reference)))
    {
      if (GNUNET_OK !=
          TALER_TESTING_get_trait_string (increase_cmd,
                                          0,
                                          &refund_amount))
        TALER_TESTING_FAIL (rls->is);

      if (GNUNET_OK !=
          TALER_string_to_amount (refund_amount,
                                  &ra))
        TALER_TESTING_FAIL (rls->is);
    }
    else
    {
      GNUNET_assert (NULL != rls->refund_amount);

      if (GNUNET_OK !=
          TALER_string_to_amount (rls->refund_amount,
                                  &ra))
        TALER_TESTING_FAIL (rls->is);
    }
  }
  GNUNET_CONTAINER_multihashmap_destroy (map);

  /* Check that what the backend claims to have been refunded
   * actually matches _our_ refund expectation.  */
  if (0 != TALER_amount_cmp (&acc,
                             &ra))
  {
    char *a1;

    a1 = TALER_amount_to_string (&ra);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Incomplete refund: expected '%s', got '%s'\n",
                a1,
                TALER_amount2s (&acc));
    GNUNET_free (a1);
    TALER_TESTING_interpreter_fail (rls->is);
    return;
  }

  TALER_TESTING_interpreter_next (rls->is);
}


/**
 * Run the "refund lookup" CMD.
 *
 * @param cls closure.
 * @param cmd command being currently run.
 * @param is interpreter state.
 */
static void
refund_lookup_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct RefundLookupState *rls = cls;

  rls->is = is;
  rls->rlo = TALER_MERCHANT_refund_lookup (is->ctx,
                                           rls->merchant_url,
                                           rls->order_id,
                                           &refund_lookup_cb,
                                           rls);
  GNUNET_assert (NULL != rls->rlo);
}


/**
 * Define a "refund lookup" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        "refund lookup" request.
 * @param increase_reference reference to a "refund increase" CMD
 *        that will offer the amount to check the looked up refund
 *        against.  Must NOT be NULL.
 * @param pay_reference reference to the "pay" CMD whose coins got
 *        refunded.  It is used to double-check if the refunded
 *        coins were actually spent in the first place.
 * @param order_id order id whose refund status is to be looked up.
 * @param http_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup (
  const char *label,
  const char *merchant_url,
  const char *increase_reference,
  const char *pay_reference,
  const char *order_id,
  unsigned int http_code)
{
  struct RefundLookupState *rls;

  rls = GNUNET_new (struct RefundLookupState);
  rls->merchant_url = merchant_url;
  rls->order_id = order_id;
  rls->pay_reference = pay_reference;
  rls->increase_reference = increase_reference;
  rls->http_code = http_code;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = rls,
      .label = label,
      .run = &refund_lookup_run,
      .cleanup = &refund_lookup_cleanup
    };

    return cmd;
  }
}


/**
 * Define a "refund lookup" CMD, equipped with a expected refund
 * amount.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        "refund lookup" request.
 * @param increase_reference reference to a "refund increase" CMD
 *        that will offer the amount to check the looked up refund
 *        against.  Can be NULL, takes precedence over @a
 *        refund_amount.
 * @param pay_reference reference to the "pay" CMD whose coins got
 *        refunded.  It is used to double-check if the refunded
 *        coins were actually spent in the first place.
 * @param order_id order id whose refund status is to be looked up.
 * @param http_code expected HTTP response code.
 * @param refund_amount expected refund amount.  Must be defined
 *        if @a increase_reference is NULL.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup_with_amount (
  const char *label,
  const char *merchant_url,
  const char *increase_reference,
  const char *pay_reference,
  const char *order_id,
  unsigned int http_code,
  const char *refund_amount)
{
  struct RefundLookupState *rls;

  rls = GNUNET_new (struct RefundLookupState);
  rls->merchant_url = merchant_url;
  rls->order_id = order_id;
  rls->pay_reference = pay_reference;
  rls->increase_reference = increase_reference;
  rls->http_code = http_code;
  rls->refund_amount = refund_amount;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = rls,
      .label = label,
      .run = &refund_lookup_run,
      .cleanup = &refund_lookup_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_refund_lookup.c */
