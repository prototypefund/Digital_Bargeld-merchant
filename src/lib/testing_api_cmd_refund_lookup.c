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
 * Callback that frees all the elements in the hashmap
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct TALER_Amount`
 *
 * @return always #GNUNET_YES (continue to iterate)
 */
static int
hashmap_free (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  struct TALER_Amount *refund_amount = value;

  GNUNET_free (refund_amount);
  return GNUNET_YES;
}


/**
 * Process "GET /public/refund" (lookup) response;
 * mainly checking if the refunded amount matches the
 * expectation.
 *
 * @param cls closure
 * @param http_status HTTP status code
 * @param ec taler-specific error object
 * @param obj response body; is NULL on error.
 */
static void
refund_lookup_cb (void *cls,
                  unsigned int http_status,
                  enum TALER_ErrorCode ec,
                  const json_t *obj)
{
  struct RefundLookupState *rls = cls;
  struct GNUNET_CONTAINER_MultiHashMap *map;
  size_t index;
  json_t *elem;
  const char *error_name;
  unsigned int error_line;
  struct GNUNET_HashCode h_coin_pub;
  const char *coin_reference;
  char *coin_reference_dup;
  const char *icoin_reference;
  const struct TALER_TESTING_Command *pay_cmd;
  const struct TALER_TESTING_Command *increase_cmd;
  const char *refund_amount;
  struct TALER_Amount acc;
  struct TALER_Amount ra;
  const json_t *arr;

  rls->rlo = NULL;
  if (rls->http_code != http_status)
    TALER_TESTING_FAIL (rls->is);

  arr = json_object_get (obj, "refund_permissions");
  if (NULL == arr)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Tolerating a refund permission not found\n");
    TALER_TESTING_interpreter_next (rls->is);
    return;
  }
  map = GNUNET_CONTAINER_multihashmap_create (1, GNUNET_NO);

  /* Put in array every refunded coin.  */
  json_array_foreach (arr, index, elem)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount *irefund_amount = GNUNET_new
                                            (struct TALER_Amount);
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("coin_pub", &coin_pub),
      TALER_JSON_spec_amount ("refund_amount", irefund_amount),
      GNUNET_JSON_spec_end ()
    };

    GNUNET_assert (GNUNET_OK == GNUNET_JSON_parse (elem,
                                                   spec,
                                                   &error_name,
                                                   &error_line));
    GNUNET_CRYPTO_hash (&coin_pub,
                        sizeof (struct TALER_CoinSpendPublicKeyP),
                        &h_coin_pub);
    GNUNET_assert (GNUNET_OK == GNUNET_CONTAINER_multihashmap_put
                     (map,
                     &h_coin_pub, // which
                     irefund_amount, // how much
                     GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  };

  /* Compare spent coins with refunded, and if they match,
   * increase an accumulator.  */
  if (NULL ==
      (pay_cmd = TALER_TESTING_interpreter_lookup_command
                   (rls->is, rls->pay_reference)))
    TALER_TESTING_FAIL (rls->is);

  if (GNUNET_OK != TALER_TESTING_get_trait_coin_reference
        (pay_cmd, 0, &coin_reference))
    TALER_TESTING_FAIL (rls->is);

  GNUNET_assert (GNUNET_OK == TALER_amount_get_zero ("EUR",
                                                     &acc));
  coin_reference_dup = GNUNET_strdup (coin_reference);
  for (icoin_reference = strtok (coin_reference_dup, ";");
       NULL != icoin_reference;
       icoin_reference = strtok (NULL, ";"))
  {
    const struct TALER_CoinSpendPrivateKeyP *icoin_priv;
    struct TALER_CoinSpendPublicKeyP icoin_pub;
    struct GNUNET_HashCode h_icoin_pub;
    struct TALER_Amount *iamount;
    const struct TALER_TESTING_Command *icoin_cmd;

    if (NULL ==
        (icoin_cmd = TALER_TESTING_interpreter_lookup_command
                       (rls->is, icoin_reference)) )
    {
      GNUNET_break (0);
      TALER_LOG_ERROR ("Bad reference `%s'\n",
                       icoin_reference);
      TALER_TESTING_interpreter_fail (rls->is);
      GNUNET_CONTAINER_multihashmap_destroy (map);
      return;
    }

    if (GNUNET_OK != TALER_TESTING_get_trait_coin_priv
          (icoin_cmd, 0, &icoin_priv))
    {
      GNUNET_break (0);
      TALER_LOG_ERROR ("Command `%s' failed to give coin"
                       " priv trait\n",
                       icoin_reference);
      TALER_TESTING_interpreter_fail (rls->is);
      GNUNET_CONTAINER_multihashmap_destroy (map);
      return;
    }

    GNUNET_CRYPTO_eddsa_key_get_public (&icoin_priv->eddsa_priv,
                                        &icoin_pub.eddsa_pub);
    GNUNET_CRYPTO_hash (&icoin_pub,
                        sizeof (struct TALER_CoinSpendPublicKeyP),
                        &h_icoin_pub);

    iamount = GNUNET_CONTAINER_multihashmap_get
                (map, &h_icoin_pub);

    /* Can be NULL: not all coins are involved in refund */
    if (NULL == iamount)
      continue;

    GNUNET_assert (GNUNET_OK == TALER_amount_add (&acc,
                                                  &acc,
                                                  iamount));
  }

  GNUNET_free (coin_reference_dup);

  if (NULL !=
      (increase_cmd = TALER_TESTING_interpreter_lookup_command
                        (rls->is, rls->increase_reference)))
  {
    if (GNUNET_OK != TALER_TESTING_get_trait_string
          (increase_cmd, 0, &refund_amount))
      TALER_TESTING_FAIL (rls->is);

    if (GNUNET_OK != TALER_string_to_amount
          (refund_amount, &ra))
      TALER_TESTING_FAIL (rls->is);
  }
  else
  {
    GNUNET_assert (NULL != rls->refund_amount);

    if (GNUNET_OK != TALER_string_to_amount
          (rls->refund_amount, &ra))
      TALER_TESTING_FAIL (rls->is);
  }

  GNUNET_CONTAINER_multihashmap_iterate (map,
                                         &hashmap_free,
                                         NULL);
  GNUNET_CONTAINER_multihashmap_destroy (map);

  /* Check that what the backend claims to have been refunded
   * actually matches _our_ refund expectation.  */
  if (0 != TALER_amount_cmp (&acc,
                             &ra))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Incomplete refund: expected '%s', got '%s'\n",
                TALER_amount_to_string (&ra),
                TALER_amount_to_string (&acc));
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
TALER_TESTING_cmd_refund_lookup
  (const char *label,
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
TALER_TESTING_cmd_refund_lookup_with_amount
  (const char *label,
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
