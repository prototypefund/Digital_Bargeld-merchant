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
 * @file lib/testing_api_cmd_refund.c
 * @brief command to test refunds.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


struct RefundIncreaseState
{
  struct TALER_MERCHANT_RefundIncreaseOperation *rio;

  const char *merchant_url;

  struct GNUNET_CURL_Context *ctx;

  const char *order_id;

  const char *refund_amount;

  const char *refund_fee;

  const char *reason;

  struct TALER_TESTING_Interpreter *is;

  unsigned int http_code;
};

struct RefundLookupState
{
  struct TALER_MERCHANT_RefundLookupOperation *rlo;

  const char *merchant_url;

  struct GNUNET_CURL_Context *ctx;

  const char *order_id;

  const char *pay_reference;

  const char *increase_reference;

  unsigned int http_code;

  struct TALER_TESTING_Interpreter *is;

  const char *refund_amount;
};


/**
 * Clean up after the command.  Run during forced termination
 * (CTRL-C) or test failure or test success.
 *
 * @param cls closure
 */
static void
refund_increase_cleanup (void *cls,
                         const struct TALER_TESTING_Command *cmd)
{
  struct RefundIncreaseState *ris = cls;

  if (NULL != ris->rio)
  {
    TALER_LOG_WARNING ("Refund-increase operation"
                       " did not complete\n");
    TALER_MERCHANT_refund_increase_cancel (ris->rio);
  }
  GNUNET_free (ris);
}

/**
 * Clean up after the command.  Run during forced termination
 * (CTRL-C) or test failure or test success.
 *
 * @param cls closure
 */
static void
refund_lookup_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  /* FIXME: make sure no other data must be free'd */
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
 * Process POST /refund (increase) response
 *
 * @param cls closure
 * @param http_status HTTP status code
 * @param ec taler-specific error object
 * @param obj response body; is NULL on success.
 */
static void
refund_increase_cb (void *cls,
                    unsigned int http_status,
                    enum TALER_ErrorCode ec,
                    const json_t *obj)
{
  struct RefundIncreaseState *ris = cls;

  ris->rio = NULL;
  if (ris->http_code != http_status)
    TALER_TESTING_FAIL (ris->is);

  TALER_TESTING_interpreter_next (ris->is);
}

static void
refund_increase_run (void *cls,
                     const struct TALER_TESTING_Command *cmd,
                     struct TALER_TESTING_Interpreter *is)
{
  struct RefundIncreaseState *ris = cls;
  struct TALER_Amount refund_amount;

  ris->is = is;
  if (GNUNET_OK != TALER_string_to_amount (ris->refund_amount,
                                           &refund_amount))
    TALER_TESTING_FAIL (is);
  ris->rio = TALER_MERCHANT_refund_increase (ris->ctx,
                                             ris->merchant_url,
                                             ris->order_id,
                                             &refund_amount,
                                             ris->reason,
                                             "default",
                                             &refund_increase_cb,
                                             ris);
  GNUNET_assert (NULL != ris->rio);
}

/**
 * Callback that frees all the elements in the hashmap
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct TALER_Amount`
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
 * Process GET /refund (increase) response.
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
  const struct TALER_TESTING_Command *icoin_cmd;
  const struct TALER_TESTING_Command *increase_cmd;
  const char *refund_amount;
  struct TALER_Amount acc;
  struct TALER_Amount ra;
  const json_t *arr;

  rls->rlo = NULL;
  if (rls->http_code != http_status)
    TALER_TESTING_FAIL (rls->is);

  map = GNUNET_CONTAINER_multihashmap_create (1, GNUNET_NO);
  arr = json_object_get (obj, "refund_permissions");
  if (NULL == arr)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Tolerating a refund permission not found\n");
    TALER_TESTING_interpreter_next (rls->is);
    return;
  }

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

  if ( NULL ==
     ( pay_cmd = TALER_TESTING_interpreter_lookup_command
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
    struct TALER_CoinSpendPrivateKeyP *icoin_priv;
    struct TALER_CoinSpendPublicKeyP icoin_pub;
    struct GNUNET_HashCode h_icoin_pub;
    struct TALER_Amount *iamount;

    if ( NULL ==
       ( icoin_cmd = TALER_TESTING_interpreter_lookup_command
         (rls->is, icoin_reference)) )
    {
      GNUNET_break (0);
      TALER_LOG_ERROR ("Bad reference `%s'\n",
                       icoin_reference); 
      TALER_TESTING_interpreter_fail (rls->is);
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
    if (GNUNET_OK != TALER_TESTING_get_trait_amount
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


static void
refund_lookup_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct RefundLookupState *rls = cls;
  
  rls->is = is;
  rls->rlo = TALER_MERCHANT_refund_lookup (rls->ctx,
                                           rls->merchant_url,
                                           rls->order_id,
                                           "default",
                                           &refund_lookup_cb,
                                           rls);
  GNUNET_assert (NULL != rls->rlo);
}


/**
 * Extract information from a command that is useful for other
 * commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param selector more detailed information about which object
 *                 to return in case there were multiple generated
 *                 by the command
 * @return #GNUNET_OK on success
 */
static int
refund_increase_traits (void *cls,
                        void **ret,
                        const char *trait,
                        unsigned int index)
{
  struct RefundIncreaseState *ris = cls;
  
  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_amount (0, ris->refund_amount),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);

  return GNUNET_SYSERR;
}

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_increase
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   const char *reason,
   const char *order_id,
   const char *refund_amount,
   const char *refund_fee,
   unsigned int http_code)
{
  struct RefundIncreaseState *ris;
  struct TALER_TESTING_Command cmd;

  ris = GNUNET_new (struct RefundIncreaseState);
  ris->merchant_url = merchant_url;
  ris->ctx = ctx;
  ris->order_id = order_id;
  ris->refund_amount = refund_amount;
  ris->refund_fee = refund_fee;
  ris->reason = reason;
  ris->http_code = http_code;

  cmd.cls = ris;
  cmd.label = label;
  cmd.run = &refund_increase_run;
  cmd.cleanup = &refund_increase_cleanup;
  cmd.traits = &refund_increase_traits;

  return cmd;
}

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   const char *increase_reference,
   const char *pay_reference,
   const char *order_id,
   unsigned int http_code)
{
  struct RefundLookupState *rls;
  struct TALER_TESTING_Command cmd;

  rls = GNUNET_new (struct RefundLookupState);
  rls->merchant_url = merchant_url;
  rls->ctx = ctx;
  rls->order_id = order_id;
  rls->pay_reference = pay_reference;
  rls->increase_reference = increase_reference;
  rls->http_code = http_code;

  cmd.cls = rls;
  cmd.label = label;
  cmd.run = &refund_lookup_run;
  cmd.cleanup = &refund_lookup_cleanup;

  return cmd;
}

struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup_with_amount
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   const char *increase_reference,
   const char *pay_reference,
   const char *order_id,
   unsigned int http_code,
   const char *refund_amount)
{
  struct RefundLookupState *rls;
  struct TALER_TESTING_Command cmd;

  rls = GNUNET_new (struct RefundLookupState);
  rls->merchant_url = merchant_url;
  rls->ctx = ctx;
  rls->order_id = order_id;
  rls->pay_reference = pay_reference;
  rls->increase_reference = increase_reference;
  rls->http_code = http_code;
  rls->refund_amount = refund_amount;

  cmd.cls = rls;
  cmd.label = label;
  cmd.run = &refund_lookup_run;
  cmd.cleanup = &refund_lookup_cleanup;

  return cmd;
}

/* end of testing_api_cmd_refund.c */
