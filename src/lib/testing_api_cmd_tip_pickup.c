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
 * @file lib/testing_api_cmd_tip.c
 * @brief command to test the tipping.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"

/**
 * State for a /tip-pickup CMD.
 */
struct TipPickupState
{
  /**
   * Merchant base URL.
   */
  const char *merchant_url;

  /**
   * Exchange base URL.
   */
  const char *exchange_url;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

  /**
   * Reference to a /tip/authorize CMD.  This will be used to
   * get the tip id to make the request with.
   */
  const char *authorize_reference;

  /**
   * If set to non NULL, it references another pickup CMD
   * that will provide all the data which is needed to issue
   * the request (like planchet secrets, denomination keys..).
   */
  const char *replay_reference;

  /**
   * Handle to a on-going /tip/pickup request.
   */
  struct TALER_MERCHANT_TipPickupOperation *tpo;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * An array of string-defined amounts that indicates
   * which denominations are going to be used to receive
   * tips.
   */
  const char **amounts;

  /**
   * The object version of the above @a amounts.
   */
  struct TALER_Amount *amounts_obj;

  /**
   * How many coins are involved in the tipping operation.
   */
  unsigned int num_coins;

  /**
   * The array of denomination keys, in the same order of @a
   * amounts.
   */
  const struct TALER_EXCHANGE_DenomPublicKey **dks;

  /**
   * The array of planchet secrets, in the same order of @a
   * amounts.
   */
  struct TALER_PlanchetSecretsP *psa;

  /**
   * Temporary data structure of @e num_coins entries for the
   * withdraw operations.
   */
  struct WithdrawHandle *withdraws;

  /**
   * Set (by the interpreter) to an array of @a num_coins
   * signatures created from the (successful) tip operation.
   */
  struct TALER_DenominationSignature *sigs;

  /**
   * Expected Taler error code (NOTE: this is NOT the HTTP
   * response code).
   */
  enum TALER_ErrorCode expected_ec;
};


/**
 * Internal withdraw handle used when withdrawing tips.
 */
struct WithdrawHandle
{
  /**
   * Withdraw operation this handle represents.
   */
  struct TALER_EXCHANGE_WithdrawHandle *wsh;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Offset of this withdraw operation in the current
   * @e is command.
   */
  unsigned int off;

  /**
   * Internal state of the "pickup" CMD.
   */
  struct TipPickupState *tps;
};


/**
 * This callback handles the response of a withdraw operation
 * from the exchange, that is the final step in getting the tip.
 *
 * @param cls closure, a `struct WithdrawHandle *`
 * @param http_status HTTP response code, #MHD_HTTP_OK (200)
 *        for successful status request, 0 if the exchange's
 *        reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param sig signature over the coin, NULL on error
 * @param full_response full response from the exchange
 *        (for logging, in case of errors)
 */
static void
pickup_withdraw_cb (void *cls,
                    unsigned int http_status,
                    enum TALER_ErrorCode ec,
                    const struct TALER_DenominationSignature *sig,
                    const json_t *full_response)
{
  struct WithdrawHandle *wh = cls;
  struct TALER_TESTING_Interpreter *is = wh->is;

  struct TipPickupState *tps = wh->tps;

  wh->wsh = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Withdraw operation %u completed with %u (%d)\n",
              wh->off,
              http_status,
              ec);
  GNUNET_assert (wh->off < tps->num_coins);
  if ( (MHD_HTTP_OK != http_status) ||
       (TALER_EC_NONE != ec) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d)"
                " to command %s when withdrawing\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label (is));
    TALER_TESTING_interpreter_fail (is);
    return;
  }
  if (NULL == tps->sigs)
    tps->sigs = GNUNET_new_array
                  (tps->num_coins, struct TALER_DenominationSignature);

  GNUNET_assert (NULL == tps->sigs[wh->off].rsa_signature);
  tps->sigs[wh->off].rsa_signature
    = GNUNET_CRYPTO_rsa_signature_dup (sig->rsa_signature);

  for (unsigned int i = 0; i<tps->num_coins; i++)
    if (NULL != tps->withdraws[wh->off].wsh)
      return;
  /* still some ops ongoing */

  GNUNET_free (tps->withdraws);
  tps->withdraws = NULL;
  TALER_TESTING_interpreter_next (is);
}


/**
 * Callback for a /tip-pickup request, it mainly checks if
 * values returned from the backend are as expected, and if so
 * (and if the status was 200 OK) proceede with the withdrawal.
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant
 *        backend, "200 OK" on success
 * @param ec taler-specific error code
 * @param reserve_pub public key of the reserve that made the
 *        @a reserve_sigs, NULL on error
 * @param num_reserve_sigs length of the @a reserve_sigs array,
 *        0 on error
 * @param reserve_sigs array of signatures authorizing withdrawals,
 *        NULL on error
 * @param json original json response
 */
static void
pickup_cb (void *cls,
           unsigned int http_status,
           enum TALER_ErrorCode ec,
           const struct TALER_ReservePublicKeyP *reserve_pub,
           unsigned int num_reserve_sigs,
           const struct TALER_ReserveSignatureP *reserve_sigs,
           const json_t *json)
{
  struct TipPickupState *tps = cls;

  tps->tpo = NULL;
  if (http_status != tps->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label
                  (tps->is));
    TALER_TESTING_FAIL (tps->is);
  }

  if (ec != tps->expected_ec)
    TALER_TESTING_FAIL (tps->is);

  /* Safe to go ahead: http status was expected.  */
  if ( (MHD_HTTP_OK != http_status) ||
       (TALER_EC_NONE != ec) )
  {
    TALER_TESTING_interpreter_next (tps->is);
    return;
  }
  if (num_reserve_sigs != tps->num_coins)
    TALER_TESTING_FAIL (tps->is);

  /* pickup successful, now withdraw! */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Obtained %u signatures for withdrawal"
              " from picking up a tip\n",
              num_reserve_sigs);

  GNUNET_assert (NULL == tps->withdraws);
  tps->withdraws = GNUNET_new_array
                     (num_reserve_sigs, struct WithdrawHandle);

  for (unsigned int i = 0; i<num_reserve_sigs; i++)
  {
    struct WithdrawHandle *wh = &tps->withdraws[i];

    wh->off = i;
    wh->is = tps->is;
    wh->tps = tps;
    GNUNET_assert
      ( (NULL == wh->wsh) &&
      ( (NULL == tps->sigs) ||
        (NULL == tps->sigs[wh->off].rsa_signature) ) );
    wh->wsh = TALER_EXCHANGE_withdraw2
                (tps->is->exchange,
                tps->dks[i],
                &reserve_sigs[i],
                reserve_pub,
                &tps->psa[i],
                &pickup_withdraw_cb,
                wh);
    if (NULL == wh->wsh)
      TALER_TESTING_FAIL (tps->is);
  }
  if (0 == num_reserve_sigs)
    TALER_TESTING_interpreter_next (tps->is);
}


/**
 * Run a /tip-pickup CMD.
 *
 * @param cls closure
 * @param cmd the current /tip-pickup CMD.
 * @param is interpreter state.
 */
static void
tip_pickup_run (void *cls,
                const struct TALER_TESTING_Command *cmd,
                struct TALER_TESTING_Interpreter *is)
{
  struct TipPickupState *tps = cls;
  unsigned int num_planchets;
  const struct TALER_TESTING_Command *replay_cmd;
  const struct TALER_TESTING_Command *authorize_cmd;
  const struct GNUNET_HashCode *tip_id;

  tps->is = is;
  tps->exchange_url = TALER_EXCHANGE_get_base_url (is->exchange);
  if (NULL == tps->replay_reference)
  {
    replay_cmd = NULL;

    /* Count planchets. */
    for (num_planchets = 0;
         NULL != tps->amounts[num_planchets];
         num_planchets++)
      ;
  }
  else
  {
    const unsigned int *np;
    if (NULL ==  /* looking for "parent" tip-pickup command */
        (replay_cmd = TALER_TESTING_interpreter_lookup_command
                        (is, tps->replay_reference)) )
      TALER_TESTING_FAIL (is);

    if (GNUNET_OK != TALER_TESTING_get_trait_uint
          (replay_cmd, 0, &np))
      TALER_TESTING_FAIL (is);
    num_planchets = *np;
  }

  if (NULL ==
      (authorize_cmd = TALER_TESTING_interpreter_lookup_command
                         (is, tps->authorize_reference)) )
    TALER_TESTING_FAIL (is);

  tps->num_coins = num_planchets;
  {
    struct TALER_PlanchetDetail planchets[num_planchets];

    tps->psa = GNUNET_new_array (num_planchets,
                                 struct TALER_PlanchetSecretsP);
    tps->dks = GNUNET_new_array
                 (num_planchets,
                 const struct TALER_EXCHANGE_DenomPublicKey *);

    tps->amounts_obj = GNUNET_new_array
                         (num_planchets, struct TALER_Amount);

    for (unsigned int i = 0; i<num_planchets; i++)
    {
      if (NULL == replay_cmd)
      {
        GNUNET_assert (GNUNET_OK == TALER_string_to_amount
                         (tps->amounts[i], &tps->amounts_obj[i]));

        tps->dks[i] = TALER_TESTING_find_pk
                        (is->keys,
                        &tps->amounts_obj[i]);

        if (NULL == tps->dks[i])
          TALER_TESTING_FAIL (is);

        TALER_planchet_setup_random (&tps->psa[i]);
      }
      else
      {
        if (GNUNET_OK != TALER_TESTING_get_trait_denom_pub
              (replay_cmd, i, &tps->dks[i]))
          TALER_TESTING_FAIL (is);

        struct TALER_PlanchetSecretsP *ps;

        if (GNUNET_OK != TALER_TESTING_get_trait_planchet_secrets
              (replay_cmd, i, &ps))
          TALER_TESTING_FAIL (is);
        tps->psa[i] = *ps;
      }

      if (GNUNET_OK != TALER_planchet_prepare (&tps->dks[i]->key,
                                               &tps->psa[i],
                                               &planchets[i]))
        TALER_TESTING_FAIL (is);
    }

    if (GNUNET_OK != TALER_TESTING_get_trait_tip_id
          (authorize_cmd, 0, &tip_id))
      TALER_TESTING_FAIL (is);

    tps->tpo = TALER_MERCHANT_tip_pickup (is->ctx,
                                          tps->merchant_url,
                                          tip_id,
                                          num_planchets,
                                          planchets,
                                          &pickup_cb,
                                          tps);
    for (unsigned int i = 0; i<num_planchets; i++)
    {
      GNUNET_free (planchets[i].coin_ev);
      planchets[i].coin_ev = NULL;
      planchets[i].coin_ev_size = 0;
    }
    GNUNET_assert (NULL != tps->tpo);
  }
}


/**
 * Free a /tip-pickup CMD state, and possibly cancel a
 * pending /tip-pickup request.
 *
 * @param cls closure.
 * @param cmd current CMD to be freed.
 */
static void
tip_pickup_cleanup (void *cls,
                    const struct TALER_TESTING_Command *cmd)
{
  struct TipPickupState *tps = cls;

  GNUNET_free_non_null (tps->amounts_obj);
  GNUNET_free_non_null (tps->dks);
  GNUNET_free_non_null (tps->psa);
  GNUNET_free_non_null (tps->withdraws);
  if (NULL != tps->sigs)
  {
    for (unsigned int i = 0; i<tps->num_coins; i++)
      if (NULL != tps->sigs[i].rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (tps->sigs[i].rsa_signature);
    GNUNET_free (tps->sigs);
  }

  if (NULL != tps->tpo)
  {
    TALER_LOG_WARNING ("Tip-pickup operation"
                       " did not complete\n");
    TALER_MERCHANT_tip_pickup_cancel (tps->tpo);
  }

  GNUNET_free (tps);
}


/**
 * Offers information from the /tip-pickup CMD state to other
 * commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
tip_pickup_traits (void *cls,
                   const void **ret,
                   const char *trait,
                   unsigned int index)
{
  struct TipPickupState *tps = cls;
  #define NUM_TRAITS (tps->num_coins * 5) + 2
  struct TALER_TESTING_Trait traits[NUM_TRAITS];

  for (unsigned int i = 0; i<tps->num_coins; i++)
  {
    traits[i] = TALER_TESTING_make_trait_planchet_secrets
                  (i, &tps->psa[i]);

    traits[i + tps->num_coins] =
      TALER_TESTING_make_trait_coin_priv
        (i, &tps->psa[i].coin_priv);

    traits[i + (tps->num_coins * 2)] =
      TALER_TESTING_make_trait_denom_pub (i, tps->dks[i]);

    traits[i + (tps->num_coins * 3)] =
      TALER_TESTING_make_trait_denom_sig (i, &tps->sigs[i]);

    traits[i + (tps->num_coins * 4)] =
      TALER_TESTING_make_trait_amount_obj
        (i, &tps->amounts_obj[i]);

  }
  traits[NUM_TRAITS - 2] = TALER_TESTING_make_trait_url
                             (TALER_TESTING_UT_EXCHANGE_BASE_URL,
                             tps->exchange_url);
  traits[NUM_TRAITS - 1] = TALER_TESTING_trait_end ();

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


/**
 * Define a /tip-pickup CMD, equipped with the expected error
 * code.
 *
 * @param label the command label
 * @param merchant_url base URL of the backend which will serve
 *        the /tip-pickup request.
 * @param http_status expected HTTP response code.
 * @param authorize_reference reference to a /tip-autorize CMD
 *        that offers a tip id to pick up.
 * @param amounts array of string-defined amounts that specifies
 *        which denominations will be accepted for tipping.
 * @param exchange connection handle to the exchange that will
 *        eventually serve the withdraw operation.
 * @param ec expected Taler error code.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup_with_ec
  (const char *label,
  const char *merchant_url,
  unsigned int http_status,
  const char *authorize_reference,
  const char **amounts,
  enum TALER_ErrorCode ec)
{
  struct TipPickupState *tps;

  tps = GNUNET_new (struct TipPickupState);
  tps->merchant_url = merchant_url;
  tps->authorize_reference = authorize_reference;
  tps->amounts = amounts;
  tps->http_status = http_status;
  tps->expected_ec = ec;

  struct TALER_TESTING_Command cmd = {
    .cls = tps,
    .label = label,
    .run = &tip_pickup_run,
    .cleanup = &tip_pickup_cleanup,
    .traits = &tip_pickup_traits
  };

  return cmd;
}


/**
 * Define a /tip-pickup CMD.
 *
 * @param label the command label
 * @param merchant_url base URL of the backend which will serve
 *        the /tip-pickup request.
 * @param http_status expected HTTP response code.
 * @param authorize_reference reference to a /tip-autorize CMD
 *        that offers a tip id to pick up.
 * @param amounts array of string-defined amounts that specifies
 *        which denominations will be accepted for tipping.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup
  (const char *label,
  const char *merchant_url,
  unsigned int http_status,
  const char *authorize_reference,
  const char **amounts)
{
  struct TipPickupState *tps;

  tps = GNUNET_new (struct TipPickupState);
  tps->merchant_url = merchant_url;
  tps->authorize_reference = authorize_reference;
  tps->amounts = amounts;
  tps->http_status = http_status;

  struct TALER_TESTING_Command cmd = {
    .cls = tps,
    .label = label,
    .run = &tip_pickup_run,
    .cleanup = &tip_pickup_cleanup,
    .traits = &tip_pickup_traits
  };

  return cmd;
}


/* end of testing_api_cmd_tip_pickup.c */
