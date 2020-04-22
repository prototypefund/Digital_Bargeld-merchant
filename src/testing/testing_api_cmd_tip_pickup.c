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
 * Callback for a /tip-pickup request, it mainly checks if
 * values returned from the backend are as expected, and if so
 * (and if the status was 200 OK) proceede with the withdrawal.
 *
 * @param cls closure
 * @param hr HTTP response
 * @param num_sigs length of the @a sigs array,
 *        0 on error
 * @param sigs array of signatures over the coins, NULL on error
 */
static void
pickup_cb (void *cls,
           const struct TALER_MERCHANT_HttpResponse *hr,
           unsigned int num_sigs,
           const struct TALER_DenominationSignature *sigs)
{
  struct TipPickupState *tps = cls;

  tps->tpo = NULL;
  if (hr->http_status != tps->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                (int) hr->ec,
                TALER_TESTING_interpreter_get_current_label (tps->is));
    TALER_TESTING_FAIL (tps->is);
  }

  if (hr->ec != tps->expected_ec)
    TALER_TESTING_FAIL (tps->is);

  /* Safe to go ahead: http status was expected.  */
  if ( (MHD_HTTP_OK != hr->http_status) ||
       (TALER_EC_NONE != hr->ec) )
  {
    TALER_TESTING_interpreter_next (tps->is);
    return;
  }
  if (num_sigs != tps->num_coins)
    TALER_TESTING_FAIL (tps->is);
  tps->sigs = GNUNET_new_array (tps->num_coins,
                                struct TALER_DenominationSignature);
  for (unsigned int i = 0; i<num_sigs; i++)
    tps->sigs[i].rsa_signature
      = GNUNET_CRYPTO_rsa_signature_dup (sigs[i].rsa_signature);
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
        (replay_cmd
           = TALER_TESTING_interpreter_lookup_command (is,
                                                       tps->replay_reference)) )
      TALER_TESTING_FAIL (is);

    if (GNUNET_OK !=
        TALER_TESTING_get_trait_uint (replay_cmd,
                                      0,
                                      &np))
      TALER_TESTING_FAIL (is);
    num_planchets = *np;
  }

  if (NULL ==
      (authorize_cmd
         = TALER_TESTING_interpreter_lookup_command (is,
                                                     tps->authorize_reference)) )
    TALER_TESTING_FAIL (is);

  tps->num_coins = num_planchets;
  {
    struct TALER_MERCHANT_PlanchetData planchets[num_planchets];

    tps->psa = GNUNET_new_array (num_planchets,
                                 struct TALER_PlanchetSecretsP);
    tps->dks = GNUNET_new_array (num_planchets,
                                 const struct TALER_EXCHANGE_DenomPublicKey *);
    tps->amounts_obj = GNUNET_new_array (num_planchets,
                                         struct TALER_Amount);
    for (unsigned int i = 0; i<num_planchets; i++)
    {
      if (NULL == replay_cmd)
      {
        GNUNET_assert (GNUNET_OK ==
                       TALER_string_to_amount (tps->amounts[i],
                                               &tps->amounts_obj[i]));
        tps->dks[i] = TALER_TESTING_find_pk (is->keys,
                                             &tps->amounts_obj[i]);
        if (NULL == tps->dks[i])
          TALER_TESTING_FAIL (is);
        TALER_planchet_setup_random (&tps->psa[i]);
      }
      else
      {
        struct TALER_PlanchetSecretsP *ps;

        if (GNUNET_OK !=
            TALER_TESTING_get_trait_denom_pub (replay_cmd,
                                               i,
                                               &tps->dks[i]))
          TALER_TESTING_FAIL (is);
        if (GNUNET_OK !=
            TALER_TESTING_get_trait_planchet_secrets (replay_cmd,
                                                      i,
                                                      &ps))
          TALER_TESTING_FAIL (is);
        tps->psa[i] = *ps;
      }
      planchets[i].pk = tps->dks[i];
      planchets[i].ps = tps->psa[i];
    }
    if (GNUNET_OK !=
        TALER_TESTING_get_trait_tip_id (authorize_cmd,
                                        0,
                                        &tip_id))
      TALER_TESTING_FAIL (is);

    tps->tpo = TALER_MERCHANT_tip_pickup (is->ctx,
                                          tps->merchant_url,
                                          tip_id,
                                          num_planchets,
                                          planchets,
                                          &pickup_cb,
                                          tps);
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
  if (NULL != tps->sigs)
  {
    for (unsigned int i = 0; i<tps->num_coins; i++)
      if (NULL != tps->sigs[i].rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (tps->sigs[i].rsa_signature);
    GNUNET_free (tps->sigs);
  }
  if (NULL != tps->tpo)
  {
    TALER_LOG_WARNING ("Tip-pickup operation did not complete\n");
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
    traits[i] = TALER_TESTING_make_trait_planchet_secrets (i,
                                                           &tps->psa[i]);
    traits[i + tps->num_coins] =
      TALER_TESTING_make_trait_coin_priv (i, &tps->psa[i].coin_priv);
    traits[i + (tps->num_coins * 2)] =
      TALER_TESTING_make_trait_denom_pub (i, tps->dks[i]);
    traits[i + (tps->num_coins * 3)] =
      TALER_TESTING_make_trait_denom_sig (i, &tps->sigs[i]);
    traits[i + (tps->num_coins * 4)] =
      TALER_TESTING_make_trait_amount_obj (i, &tps->amounts_obj[i]);
  }
  traits[NUM_TRAITS - 2]
    = TALER_TESTING_make_trait_url (TALER_TESTING_UT_EXCHANGE_BASE_URL,
                                    tps->exchange_url);
  traits[NUM_TRAITS - 1] = TALER_TESTING_trait_end ();
  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
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
TALER_TESTING_cmd_tip_pickup (const char *label,
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
  {
    struct TALER_TESTING_Command cmd = {
      .cls = tps,
      .label = label,
      .run = &tip_pickup_run,
      .cleanup = &tip_pickup_cleanup,
      .traits = &tip_pickup_traits
    };

    return cmd;
  }
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
TALER_TESTING_cmd_tip_pickup_with_ec (const char *label,
                                      const char *merchant_url,
                                      unsigned int http_status,
                                      const char *authorize_reference,
                                      const char **amounts,
                                      enum TALER_ErrorCode ec)
{
  struct TALER_TESTING_Command cmd;
  struct TipPickupState *tps;

  cmd = TALER_TESTING_cmd_tip_pickup (label,
                                      merchant_url,
                                      http_status,
                                      authorize_reference,
                                      amounts);
  tps = cmd.cls;
  tps->expected_ec = ec;
  return cmd;
}


/* end of testing_api_cmd_tip_pickup.c */
