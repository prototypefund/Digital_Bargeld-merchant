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
 * Obtain the URL to use for an API request.
 *
 * @param h the exchange handle to query
 * @param path Taler API path (i.e. "/reserve/withdraw")
 * @return the full URL to use with cURL
 */
char *
MAH_path_to_url (struct TALER_EXCHANGE_Handle *h,
                 const char *path);

struct TipPickupState
{
  const char *merchant_url;

  const char *exchange_url;

  struct GNUNET_CURL_Context *ctx;

  unsigned int http_status;

  const char *authorize_reference;

  /**
   * Set to non-NULL to a label of another pick up operation
   * that we should replay.
   */
  const char *replay_reference;

  struct TALER_MERCHANT_TipPickupOperation *tpo;

  struct TALER_TESTING_Interpreter *is;

  const char **amounts;

  struct TALER_Amount *amounts_obj;

  unsigned int num_coins;
  
  const struct TALER_EXCHANGE_DenomPublicKey **dks;

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

  enum TALER_ErrorCode expected_ec;

  struct TALER_EXCHANGE_Handle *exchange;
};


struct TipQueryState
{
  const char *merchant_url;

  struct GNUNET_CURL_Context *ctx;

  unsigned int http_status;

  const char *instance;

  struct TALER_MERCHANT_TipQueryOperation *tqo;

  struct TALER_TESTING_Interpreter *is;

  const char *expected_amount_picked_up;

  const char *expected_amount_authorized;

  const char *expected_amount_available;
};


struct TipAuthorizeState
{
  const char *merchant_url;

  struct GNUNET_CURL_Context *ctx;

  unsigned int http_status;

  const char *instance;

  const char *justification;

  const char *amount;

  enum TALER_ErrorCode expected_ec;

  const char *exchange_url;

  struct GNUNET_HashCode tip_id;

  struct GNUNET_TIME_Absolute tip_expiration;

  struct TALER_MERCHANT_TipAuthorizeOperation *tao;

  struct TALER_TESTING_Interpreter *is;
};

/**
 * Callback for a /tip-authorize request.  Returns the result
 * of the operation.
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant backend
 * @param ec taler-specific error code
 * @param tip_id which tip ID should be used to pickup the tip
 * @param tip_expiration when does the tip expire (needs to be
 *        picked up before this time)
 * @param exchange_url at what exchange can the tip be picked up
 */
static void
tip_authorize_cb (void *cls,
                  unsigned int http_status,
                  enum TALER_ErrorCode ec,
                  const struct GNUNET_HashCode *tip_id,
                  struct GNUNET_TIME_Absolute tip_expiration,
                  const char *exchange_url)
{
  struct TipAuthorizeState *tas = cls; 

  tas->tao = NULL;
  if (tas->http_status != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d)"
                " to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label
                  (tas->is));

    TALER_TESTING_interpreter_fail (tas->is);
    return;
  }

  if (tas->expected_ec != ec)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected error code %d (%u) to command %s\n",
                ec,
                http_status,
                TALER_TESTING_interpreter_get_current_label
                  (tas->is));
    TALER_TESTING_interpreter_fail (tas->is);
    return;
  }
  if ( (MHD_HTTP_OK == http_status) &&
       (TALER_EC_NONE == ec) )
  {
    if (0 != strcmp (exchange_url,
                     tas->exchange_url))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unexpected exchange URL %s to command %s\n",
                  exchange_url,
                  TALER_TESTING_interpreter_get_current_label
                    (tas->is));
      TALER_TESTING_interpreter_fail (tas->is);
      return;
    }
    tas->tip_id = *tip_id;
    tas->tip_expiration = tip_expiration;
  }

  TALER_TESTING_interpreter_next (tas->is);
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
tip_authorize_traits (void *cls,
                      void **ret,
                      const char *trait,
                      unsigned int index)
{
  struct TipAuthorizeState *tas = cls;

  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_tip_id (0, &tas->tip_id),
    TALER_TESTING_trait_end (),
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
  return GNUNET_SYSERR;
}

/**
 * Runs the command.  Note that upon return, the interpreter
 * will not automatically run the next command, as the command
 * may continue asynchronously in other scheduler tasks.  Thus,
 * the command must ensure to eventually call
 * #TALER_TESTING_interpreter_next() or
 * #TALER_TESTING_interpreter_fail().
 *
 * @param is interpreter state
 */
static void
tip_authorize_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct TipAuthorizeState *tas = cls;
  struct TALER_Amount amount; 

  tas->is = is;
  if (GNUNET_OK != TALER_string_to_amount (tas->amount,
                                           &amount))
    TALER_TESTING_FAIL (is);

  tas->tao = TALER_MERCHANT_tip_authorize
    (tas->ctx,
     tas->merchant_url,
     "http://merchant.com/pickup",
     "http://merchant.com/continue",
     &amount,
     tas->instance,
     tas->justification,
     tip_authorize_cb,
     tas);

  GNUNET_assert (NULL != tas->tao);
}


/**
 * FIXME
 */
static void
tip_authorize_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  struct TipAuthorizeState *tas = cls;

  if (NULL != tas->tao)
  {
    TALER_LOG_WARNING ("Tip-autorize operation"
                       " did not complete\n");
    TALER_MERCHANT_tip_authorize_cancel (tas->tao);
  }
  GNUNET_free (tas);
}


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize_with_ec
  (const char *label,
   const char *merchant_url,
   const char *exchange_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *instance,
   const char *justification,
   const char *amount,
   enum TALER_ErrorCode ec)
{
  struct TipAuthorizeState *tas;
  struct TALER_TESTING_Command cmd;

  tas = GNUNET_new (struct TipAuthorizeState);
  tas->merchant_url = merchant_url;
  tas->exchange_url = exchange_url;
  tas->ctx = ctx;
  tas->instance = instance;
  tas->justification = justification;
  tas->amount = amount;
  tas->http_status = http_status;
  tas->expected_ec = ec;

  cmd.label = label;
  cmd.cls = tas;
  cmd.run = &tip_authorize_run;
  cmd.cleanup = &tip_authorize_cleanup;
  cmd.traits = &tip_authorize_traits;
  
  return cmd;
}


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize (const char *label,
                                 const char *merchant_url,
                                 const char *exchange_url,
                                 struct GNUNET_CURL_Context *ctx,
                                 unsigned int http_status,
                                 const char *instance,
                                 const char *justification,
                                 const char *amount)
{
  struct TipAuthorizeState *tas;
  struct TALER_TESTING_Command cmd;

  tas = GNUNET_new (struct TipAuthorizeState);
  tas->merchant_url = merchant_url;
  tas->exchange_url = exchange_url;
  tas->ctx = ctx;
  tas->instance = instance;
  tas->justification = justification;
  tas->amount = amount;
  tas->http_status = http_status;

  cmd.label = label;
  cmd.cls = tas;
  cmd.run = &tip_authorize_run;
  cmd.cleanup = &tip_authorize_cleanup;
  cmd.traits = &tip_authorize_traits;
  
  return cmd;
}

/**
 * Callback to process a GET /tip-query request
 *
 * @param cls closure
 * @param http_status HTTP status code for this request
 * @param ec Taler-specific error code
 * @param raw raw response body
 */
static void
tip_query_cb (void *cls,
              unsigned int http_status,
              enum TALER_ErrorCode ec,
              const json_t *raw,
              struct GNUNET_TIME_Absolute reserve_expiration,
              struct TALER_ReservePublicKeyP *reserve_pub,
              struct TALER_Amount *amount_authorized,
              struct TALER_Amount *amount_available,
              struct TALER_Amount *amount_picked_up)
{
  struct TipQueryState *tqs = cls;
  struct TALER_Amount a;

  tqs->tqo = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Tip query callback at command `%s'\n",
              TALER_TESTING_interpreter_get_current_label
                (tqs->is));

  GNUNET_assert (NULL != reserve_pub);
  GNUNET_assert (NULL != amount_authorized);
  GNUNET_assert (NULL != amount_available);
  GNUNET_assert (NULL != amount_picked_up);

  if (tqs->expected_amount_available)
  {
    GNUNET_assert (GNUNET_OK == TALER_string_to_amount
      (tqs->expected_amount_available, &a));
    TALER_LOG_INFO ("expected available %s, actual %s\n",
                    TALER_amount_to_string (&a),
                    TALER_amount_to_string (amount_available));
    if (0 != TALER_amount_cmp (amount_available, &a))
      TALER_TESTING_FAIL (tqs->is);
  }

  if (tqs->expected_amount_authorized)
  {
    GNUNET_assert (GNUNET_OK == TALER_string_to_amount
      (tqs->expected_amount_authorized, &a));
    TALER_LOG_INFO ("expected authorized %s, actual %s\n",
                    TALER_amount_to_string (&a),
                    TALER_amount_to_string (amount_authorized));
    if (0 != TALER_amount_cmp (amount_authorized, &a))
      TALER_TESTING_FAIL (tqs->is);
  }

  if (tqs->expected_amount_picked_up)
  {
    GNUNET_assert (GNUNET_OK == TALER_string_to_amount
      (tqs->expected_amount_picked_up, &a));
    TALER_LOG_INFO ("expected picked_up %s, actual %s\n",
                    TALER_amount_to_string (&a),
                    TALER_amount_to_string (amount_picked_up));
    if (0 != TALER_amount_cmp (amount_picked_up, &a))
      TALER_TESTING_FAIL (tqs->is);
  }

  if (tqs->http_status != http_status)
    TALER_TESTING_FAIL (tqs->is);

  TALER_TESTING_interpreter_next (tqs->is);
}

/**
 * FIXME
 */
static void
tip_query_cleanup (void *cls,
                   const struct TALER_TESTING_Command *cmd)
{
  struct TipQueryState *tqs = cls;

  if (NULL != tqs->tqo)
  {
    TALER_LOG_WARNING ("Tip-query operation"
                       " did not complete\n");
    TALER_MERCHANT_tip_query_cancel (tqs->tqo);
  }
  GNUNET_free (tqs);
}

/**
 * FIXME
 */
static void
tip_query_run (void *cls,
               const struct TALER_TESTING_Command *cmd,
               struct TALER_TESTING_Interpreter *is)
{
  struct TipQueryState *tqs = cls;
  
  tqs->is = is;
  tqs->tqo = TALER_MERCHANT_tip_query (tqs->ctx,
                                       tqs->merchant_url,
                                       tqs->instance,
                                       &tip_query_cb,
                                       tqs);
  GNUNET_assert (NULL != tqs->tqo);
}


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query_with_amounts
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *instance,
   const char *expected_amount_picked_up,
   const char *expected_amount_authorized,
   const char *expected_amount_available)
{
  struct TipQueryState *tqs;
  struct TALER_TESTING_Command cmd;

  tqs = GNUNET_new (struct TipQueryState);
  tqs->merchant_url = merchant_url;
  tqs->ctx = ctx;
  tqs->instance = instance;
  tqs->http_status = http_status;
  tqs->expected_amount_picked_up = expected_amount_picked_up;
  tqs->expected_amount_authorized = expected_amount_authorized;
  tqs->expected_amount_available = expected_amount_available;

  cmd.cls = tqs;
  cmd.label = label;
  cmd.run = &tip_query_run;
  cmd.cleanup = &tip_query_cleanup;
  
  return cmd;
}


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query (const char *label,
                             const char *merchant_url,
                             struct GNUNET_CURL_Context *ctx,
                             unsigned int http_status,
                             const char *instance)
{
  struct TipQueryState *tqs;
  struct TALER_TESTING_Command cmd;

  tqs = GNUNET_new (struct TipQueryState);
  tqs->merchant_url = merchant_url;
  tqs->ctx = ctx;
  tqs->instance = instance;
  tqs->http_status = http_status;

  cmd.cls = tqs;
  cmd.label = label;
  cmd.run = &tip_query_run;
  cmd.cleanup = &tip_query_cleanup;
  
  return cmd;
}

/**
 * Internal withdraw handle used when withdrawing tips.
 */
struct WithdrawHandle
{
  /**
   * Withdraw operation this handle represents.
   */
  struct TALER_EXCHANGE_ReserveWithdrawHandle *wsh;

  /**
   * Interpreter state we are part of.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Offset of this withdraw operation in the current
   * @e is command.
   */
  unsigned int off;

};

/**
 * Callbacks of this type are used to serve the result of
 * submitting a withdraw request to a exchange.
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
  struct TipPickupState *tps = is->commands[is->ip].cls;

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

  for (unsigned int i=0; i<tps->num_coins; i++)
    if (NULL != tps->withdraws[wh->off].wsh)
      return; /* still some ops ongoing */

  GNUNET_free (tps->withdraws);
  tps->withdraws = NULL;
  TALER_TESTING_interpreter_next (is);
}


/**
 * Callback for a /tip-pickup request.  Returns the result of
 * the operation.
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

  for (unsigned int i=0;i<num_reserve_sigs;i++)
  {
    struct WithdrawHandle *wh = &tps->withdraws[i];

    wh->off = i;
    wh->is = tps->is;
    GNUNET_assert
      ( (NULL == wh->wsh) &&
        ( (NULL == tps->sigs) ||
          (NULL == tps->sigs[wh->off].rsa_signature) ) );
    wh->wsh = TALER_EXCHANGE_reserve_withdraw2
      (tps->exchange,
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
  tps->exchange_url = MAH_path_to_url (tps->exchange, "/");
  if (NULL == tps->replay_reference)
  {
    replay_cmd = NULL;

    /* Count planchets. */
    for (num_planchets=0;
         NULL != tps->amounts[num_planchets];
         num_planchets++);
  }
  else
  {
    unsigned int *np;
    if ( NULL == /* looking for "parent" tip-pickup command */
       ( replay_cmd = TALER_TESTING_interpreter_lookup_command
         (is, tps->replay_reference)) )
    TALER_TESTING_FAIL (is);   
    
    if (GNUNET_OK != TALER_TESTING_get_trait_uint
        (replay_cmd, 0, &np))
      TALER_TESTING_FAIL (is);
    num_planchets = *np;
  }

  if (NULL ==
     ( authorize_cmd = TALER_TESTING_interpreter_lookup_command
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

    for (unsigned int i=0;i<num_planchets;i++)
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

    tps->tpo = TALER_MERCHANT_tip_pickup (tps->ctx,
                                          tps->merchant_url,
                                          tip_id,
                                          num_planchets,
                                          planchets,
                                          &pickup_cb,
                                          tps);
    GNUNET_assert (NULL != tps->tpo);
  }
}


static void
tip_pickup_cleanup (void *cls,
                    const struct TALER_TESTING_Command *cmd)
{
  struct TipPickupState *tps = cls;
  #warning free elements *in* the state!
  if (NULL != tps->tpo)
  {
    TALER_LOG_WARNING ("Tip-pickup operation"
                       " did not complete\n");
    TALER_MERCHANT_tip_pickup_cancel (tps->tpo);
  }

  GNUNET_free (tps);
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
tip_pickup_traits (void *cls,
                   void **ret,
                   const char *trait,
                   unsigned int index)
{
  struct TipPickupState *tps = cls;
  #define NUM_TRAITS (tps->num_coins * 5) + 2
  struct TALER_TESTING_Trait traits[NUM_TRAITS];
  
  for (unsigned int i=0; i<tps->num_coins; i++)
  {
    traits[i] = TALER_TESTING_make_trait_planchet_secrets
      (i, &tps->psa[i]);

    traits[i + tps->num_coins] =
      TALER_TESTING_make_trait_coin_priv
        (i, &tps->psa[i].coin_priv);

    traits[i + (tps->num_coins * 2)] =
      TALER_TESTING_make_trait_denom_pub (i, tps->dks[i]);

    traits[i + (tps->num_coins *3)] =
      TALER_TESTING_make_trait_denom_sig (i, &tps->sigs[i]);

    traits[i + (tps->num_coins *4)] =
      TALER_TESTING_make_trait_amount_obj
        (i, &tps->amounts_obj[i]);

  }
  traits[NUM_TRAITS - 2] = TALER_TESTING_make_trait_url
    (0, tps->exchange_url);
  traits[NUM_TRAITS - 1] = TALER_TESTING_trait_end ();

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
TALER_TESTING_cmd_tip_pickup_with_ec
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *authorize_reference,
   const char **amounts,
   struct TALER_EXCHANGE_Handle *exchange,
   enum TALER_ErrorCode ec)
{
  struct TipPickupState *tps;
  struct TALER_TESTING_Command cmd;

  tps = GNUNET_new (struct TipPickupState);
  tps->merchant_url = merchant_url;
  tps->ctx = ctx;
  tps->authorize_reference = authorize_reference;
  tps->amounts = amounts;
  tps->exchange = exchange;
  tps->http_status = http_status;
  tps->expected_ec = ec;

  cmd.cls = tps;
  cmd.label = label;
  cmd.run = &tip_pickup_run;
  cmd.cleanup = &tip_pickup_cleanup;
  cmd.traits = &tip_pickup_traits;
  
  return cmd;
}


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *authorize_reference,
   const char **amounts,
   struct TALER_EXCHANGE_Handle *exchange)
{
  struct TipPickupState *tps;
  struct TALER_TESTING_Command cmd;

  tps = GNUNET_new (struct TipPickupState);
  tps->merchant_url = merchant_url;
  tps->ctx = ctx;
  tps->authorize_reference = authorize_reference;
  tps->amounts = amounts;
  tps->exchange = exchange;
  tps->http_status = http_status;

  cmd.cls = tps;
  cmd.label = label;
  cmd.run = &tip_pickup_run;
  cmd.cleanup = &tip_pickup_cleanup;
  cmd.traits = &tip_pickup_traits;
  
  return cmd;
}


/* end of testing_api_cmd_tip.c */
