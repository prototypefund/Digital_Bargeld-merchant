/*
  This file is part of TALER
  Copyright (C) 2014-2017, 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_tip_pickup.c
 * @brief Implementation of the /tip-pickup request of the merchant's HTTP API
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include <taler/taler_curl_lib.h>


/**
 * Data we keep per planchet.
 */
struct PlanchetData
{
  /**
   * Secrets of the planchet.
   */
  struct TALER_PlanchetSecretsP ps;

  /**
   * Denomination key we are withdrawing.
   */
  struct TALER_EXCHANGE_DenomPublicKey pk;

  /**
   * Hash of the public key of the coin we are signing.
   */
  struct GNUNET_HashCode c_hash;
};


/**
 * Handle for a /tip-pickup operation.
 */
struct TALER_MERCHANT_TipPickupOperation
{

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_TipPickupCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Handle for the actual (internal) withdraw operation.
   */
  struct TALER_MERCHANT_TipPickup2Operation *tpo2;

  /**
   * Number of planchets/coins used for this operation.
   */
  unsigned int num_planchets;

  /**
   * Array of length @e num_planchets.
   */
  struct PlanchetData *planchets;

};


/**
 * Callback for a /tip-pickup request.  Returns the result of the operation.
 * Note that the client MUST still do the unblinding of the @a blind_sigs.
 *
 * @param cls closure, a `struct TALER_MERCHANT_TipPickupOperation *`
 * @param hr HTTP response details
 * @param num_blind_sigs length of the @a reserve_sigs array, 0 on error
 * @param blind_sigs array of blind signatures over the planchets, NULL on error
 */
static void
pickup_done_cb (void *cls,
                const struct TALER_MERCHANT_HttpResponse *hr,
                unsigned int num_blind_sigs,
                const struct TALER_MERCHANT_BlindSignature *blind_sigs)
{
  struct TALER_MERCHANT_TipPickupOperation *tp = cls;

  tp->tpo2 = NULL;
  if (NULL == blind_sigs)
  {
    tp->cb (tp->cb_cls,
            hr,
            0,
            NULL);
    TALER_MERCHANT_tip_pickup_cancel (tp);
    return;
  }
  {
    struct TALER_DenominationSignature sigs[num_blind_sigs];
    int ok;

    ok = GNUNET_OK;
    memset (sigs,
            0,
            sizeof (sigs));
    for (unsigned int i = 0; i<num_blind_sigs; i++)
    {
      struct TALER_FreshCoin fc;

      if (GNUNET_OK !=
          TALER_planchet_to_coin (&tp->planchets[i].pk.key,
                                  blind_sigs[i].blind_sig,
                                  &tp->planchets[i].ps,
                                  &tp->planchets[i].c_hash,
                                  &fc))
      {
        ok = GNUNET_SYSERR;
        break;
      }
      sigs[i] = fc.sig;
    }
    if (GNUNET_OK == ok)
    {
      tp->cb (tp->cb_cls,
              hr,
              num_blind_sigs,
              sigs);
    }
    else
    {
      struct TALER_MERCHANT_HttpResponse hrx = {
        .reply = hr->reply,
        .http_status = 0,
        .ec = TALER_EC_TIP_PICKUP_UNBLIND_FAILURE
      };

      tp->cb (tp->cb_cls,
              &hrx,
              0,
              NULL);
    }
    for (unsigned int i = 0; i<num_blind_sigs; i++)
      if (NULL != sigs[i].rsa_signature)
        GNUNET_CRYPTO_rsa_signature_free (sigs[i].rsa_signature);
  }
  TALER_MERCHANT_tip_pickup_cancel (tp);
}


/**
 * Issue a /tip-pickup request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param tip_id unique identifier for the tip
 * @param num_planches number of planchets provided in @a pds
 * @param pds array of planchet secrets to be signed into existence for the tip
 * @param pickup_cb callback which will work the response gotten from the backend
 * @param pickup_cb_cls closure to pass to @a pickup_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipPickupOperation *
TALER_MERCHANT_tip_pickup (struct GNUNET_CURL_Context *ctx,
                           const char *backend_url,
                           const struct GNUNET_HashCode *tip_id,
                           unsigned int num_planchets,
                           const struct TALER_MERCHANT_PlanchetData *pds,
                           TALER_MERCHANT_TipPickupCallback pickup_cb,
                           void *pickup_cb_cls)
{
  struct TALER_MERCHANT_TipPickupOperation *tp;
  struct TALER_PlanchetDetail details[GNUNET_NZL (num_planchets)];

  if (0 == num_planchets)
  {
    GNUNET_break (0);
    return NULL;
  }
  tp = GNUNET_new (struct TALER_MERCHANT_TipPickupOperation);
  GNUNET_array_grow (tp->planchets,
                     tp->num_planchets,
                     num_planchets);
  for (unsigned int i = 0; i<num_planchets; i++)
  {
    tp->planchets[i].ps = pds[i].ps;
    if (GNUNET_OK !=
        TALER_planchet_prepare (&pds[i].pk->key,
                                &tp->planchets[i].ps,
                                &tp->planchets[i].c_hash,
                                &details[i]))
    {
      GNUNET_break (0);
      GNUNET_array_grow (tp->planchets,
                         tp->num_planchets,
                         0);
      GNUNET_free (tp);
      return NULL;
    }
  }
  for (unsigned int i = 0; i<num_planchets; i++)
  {
    tp->planchets[i].pk = *pds[i].pk;
    tp->planchets[i].pk.key.rsa_public_key
      = GNUNET_CRYPTO_rsa_public_key_dup (pds[i].pk->key.rsa_public_key);
  }
  tp->cb = pickup_cb;
  tp->cb_cls = pickup_cb_cls;
  tp->tpo2 = TALER_MERCHANT_tip_pickup2 (ctx,
                                         backend_url,
                                         tip_id,
                                         num_planchets,
                                         details,
                                         &pickup_done_cb,
                                         tp);
  if (NULL == tp->tpo2)
  {
    GNUNET_break (0);
    TALER_MERCHANT_tip_pickup_cancel (tp);
    return NULL;
  }
  return tp;
}


/**
 * Cancel a pending /tip-pickup request
 *
 * @param tp handle from the operation to cancel
 */
void
TALER_MERCHANT_tip_pickup_cancel (struct TALER_MERCHANT_TipPickupOperation *tp)
{
  for (unsigned int i = 0; i<tp->num_planchets; i++)
    GNUNET_CRYPTO_rsa_public_key_dup (tp->planchets[i].pk.key.rsa_public_key);
  GNUNET_array_grow (tp->planchets,
                     tp->num_planchets,
                     0);
  if (NULL != tp->tpo2)
  {
    TALER_MERCHANT_tip_pickup2_cancel (tp->tpo2);
    tp->tpo2 = NULL;
  }
  GNUNET_free (tp);
}


/* end of merchant_api_tip_pickup.c */
