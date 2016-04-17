/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_pay.c
 * @brief Implementation of the /pay request of the merchant's HTTP API
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


/**
 * @brief A Pay Handle
 */
struct TALER_MERCHANT_Pay
{

  /**
   * The url for this request.
   */
  char *url;

  /**
   * JSON encoding of the request to POST.
   */
  char *json_enc;

  /**
   * Handle for the request.
   */
  struct GNUNET_CURL_Job *job;

  /**
   * Function to call with the result.
   */
  TALER_MERCHANT_PayCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;
};



/**
 * Function called when we're done processing the
 * HTTP /pay request.
 *
 * @param cls the `struct TALER_MERCHANT_Pay`
 * @param response_code HTTP response code, 0 on error
 * @param json response body, NULL if not in JSON
 */
static void
handle_pay_finished (void *cls,
                     long response_code,
                     const json_t *json)
{
  struct TALER_MERCHANT_Pay *ph = cls;

  ph->job = NULL;
  switch (response_code)
  {
  case 0:
    break;
  case MHD_HTTP_OK:
    break;
  case MHD_HTTP_BAD_REQUEST:
    /* This should never happen, either us or the merchant is buggy
       (or API version conflict); just pass JSON reply to the application */
    break;
  case MHD_HTTP_FORBIDDEN:
    break;
  case MHD_HTTP_UNAUTHORIZED:
    /* Nothing really to verify, merchant says one of the signatures is
       invalid; as we checked them, this should never happen, we
       should pass the JSON reply to the application */
    break;
  case MHD_HTTP_NOT_FOUND:
    /* Nothing really to verify, this should never
       happen, we should pass the JSON reply to the application */
    break;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    /* Server had an internal issue; we should retry, but this API
       leaves this to the application */
    break;
  default:
    /* unexpected response code */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u\n",
                response_code);
    GNUNET_break (0);
    response_code = 0;
    break;
  }
  ph->cb (ph->cb_cls,
          response_code,
	  "FIXME-redirect-URI",
          json);
  TALER_MERCHANT_pay_cancel (ph);
}


/**
 * Pay a merchant.  API for wallets that have the coin's private keys.
 *
 * @param ctx the execution loop context
 * @param exchange_uri URI of the exchange that the coins belong to
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param amount total value of the contract to be paid to the merchant
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_wallet (struct GNUNET_CURL_Context *ctx,
			   const char *merchant_uri,
                           const struct GNUNET_HashCode *h_contract,
                           uint64_t transaction_id,
			   const struct TALER_Amount *amount,
			   const struct TALER_Amount *max_fee,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           const struct TALER_MerchantSignatureP *merchant_sig,
                           struct GNUNET_TIME_Absolute timestamp,
                           struct GNUNET_TIME_Absolute refund_deadline,
                           const struct GNUNET_HashCode *h_wire,
			   const char *exchange_uri,
                           unsigned int num_coins,
                           const struct TALER_MERCHANT_PayCoin *coins,
                           TALER_MERCHANT_PayCallback pay_cb,
                           void *pay_cb_cls)
{
  unsigned int i;
  struct TALER_DepositRequestPS dr;
  struct TALER_MERCHANT_PaidCoin pc[num_coins];

  dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
  dr.purpose.size = htonl (sizeof (struct TALER_DepositRequestPS));
  dr.h_contract = *h_contract;
  dr.h_wire = *h_wire;
  dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  dr.transaction_id = GNUNET_htonll (transaction_id);
  dr.merchant = *merchant_pub;
  for (i=0;i<num_coins;i++)
  {
    const struct TALER_MERCHANT_PayCoin *coin = &coins[i];
    struct TALER_MERCHANT_PaidCoin *p = &pc[i];
    struct TALER_Amount fee;

    /* prepare 'dr' for this coin to generate coin signature */
    GNUNET_CRYPTO_eddsa_key_get_public (&coin->coin_priv.eddsa_priv,
					&dr.coin_pub.eddsa_pub);
    TALER_amount_hton (&dr.amount_with_fee,
		       &coin->amount_with_fee);
    if (GNUNET_SYSERR ==
	TALER_amount_subtract (&fee,
			       &coin->amount_with_fee,
			       &coin->amount_without_fee))
    {
      /* Integer underflow, fee larger than total amount?
	 This should not happen (client violated API!) */
      GNUNET_break (0);
      return NULL;
    }
    TALER_amount_hton (&dr.deposit_fee,
		       &fee);
    GNUNET_CRYPTO_eddsa_sign (&coin->coin_priv.eddsa_priv,
			      &dr.purpose,
			      &p->coin_sig.eddsa_signature);
    p->denom_pub = coin->denom_pub;
    p->denom_sig = coin->denom_sig;
    p->coin_pub = dr.coin_pub;
    p->amount_with_fee = coin->amount_with_fee;
    p->amount_without_fee = coin->amount_without_fee;
  }
  return TALER_MERCHANT_pay_frontend (ctx,
				      merchant_uri,
				      h_contract,
                                      amount,
				      max_fee,
				      transaction_id,
				      merchant_pub,
				      merchant_sig,
				      refund_deadline,
				      timestamp,
				      GNUNET_TIME_UNIT_ZERO_ABS,
				      h_wire,
				      exchange_uri,
				      num_coins,
				      pc,
				      pay_cb,
				      pay_cb_cls);
}


/**
 * Pay a merchant.  API for frontends talking to backends. Here,
 * the frontend does not have the coin's private keys, but just
 * the public keys and signatures.  Note the sublte difference
 * in the type of @a coins compared to #TALER_MERCHANT_pay().
 *
 * @param ctx the execution loop context
 * @param exchange_uri URI of the exchange that the coins belong to
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param execution_deadline date by which the merchant would like the exchange to execute the transaction (can be zero if there is no specific date desired by the frontend)
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param amount total value of the contract to be paid to the merchant
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_frontend (struct GNUNET_CURL_Context *ctx,
			     const char *merchant_uri,
                             const struct GNUNET_HashCode *h_contract,
			     const struct TALER_Amount *amount,
			     const struct TALER_Amount *max_fee,
                             uint64_t transaction_id,
                             const struct TALER_MerchantPublicKeyP *merchant_pub,
                             const struct TALER_MerchantSignatureP *merchant_sig,
                             struct GNUNET_TIME_Absolute refund_deadline,
                             struct GNUNET_TIME_Absolute timestamp,
                             struct GNUNET_TIME_Absolute execution_deadline,
                             const struct GNUNET_HashCode *h_wire,
			     const char *exchange_uri,
                             unsigned int num_coins,
                             const struct TALER_MERCHANT_PaidCoin *coins,
                             TALER_MERCHANT_PayCallback pay_cb,
                             void *pay_cb_cls)
{
  struct TALER_MERCHANT_Pay *ph;
  json_t *pay_obj;
  json_t *j_coins;
  CURL *eh;
  struct TALER_Amount total_fee;
  struct TALER_Amount total_amount;
  unsigned int i;

  if (0 == num_coins)
  {
    GNUNET_break (0);
    return NULL;
  }
  j_coins = json_array ();
  for (i=0;i<num_coins;i++)
  {
    json_t *j_coin;
    const struct TALER_MERCHANT_PaidCoin *pc = &coins[i];
    struct TALER_Amount fee;

    if (GNUNET_SYSERR ==
	TALER_amount_subtract (&fee,
			       &pc->amount_with_fee,
			       &pc->amount_without_fee))
    {
      /* Integer underflow, fee larger than total amount?
	 This should not happen (client violated API!) */
      GNUNET_break (0);
      json_decref (j_coins);
      return NULL;
    }
    if (0 == i)
    {
      total_fee = fee;
      total_amount = pc->amount_with_fee;
    }
    else
    {
      if ( (GNUNET_OK !=
	    TALER_amount_add (&total_fee,
			      &total_fee,
			      &fee)) ||
	   (GNUNET_OK !=
	    TALER_amount_add (&total_amount,
			      &total_amount,
			      &pc->amount_with_fee)) )
      {
	/* integer overflow */
	GNUNET_break (0);
	json_decref (j_coins);
	return NULL;
      }
    }

    /* create JSON for this coin */
    j_coin = json_pack ("{s:o, s:o," /* f/coin_pub */
			" s:o, s:o," /* denom_pub / ub_sig */
			" s:o}",     /* coin_sig */
			"f", TALER_JSON_from_amount (&pc->amount_with_fee),
			"coin_pub", GNUNET_JSON_from_data (&pc->coin_pub,
							  sizeof (struct TALER_CoinSpendPublicKeyP)),
			"denom_pub", GNUNET_JSON_from_rsa_public_key (pc->denom_pub.rsa_public_key),
			"ub_sig", GNUNET_JSON_from_rsa_signature (pc->denom_sig.rsa_signature),
			"coin_sig", GNUNET_JSON_from_data (&pc->coin_sig,
							  sizeof (struct TALER_CoinSpendSignatureP))
			);
    json_array_append (j_coins,
		       j_coin);
  }

  { /* Sanity check that total_amount and total_fee
       match amount/max_fee requirements */
    struct TALER_Amount fee_left;

    if (GNUNET_OK ==
	TALER_amount_subtract (&fee_left,
			       &total_fee,
			       max_fee))
    {
      /* Wallet must cover part of the fee! */
      struct TALER_Amount new_amount;

      if (GNUNET_OK !=
	  TALER_amount_add (&new_amount,
			    &fee_left,
			    amount))
      {
	/* integer overflow */
	GNUNET_break (0);
	json_decref (j_coins);
	return NULL;
      }
      if (1 ==
	  TALER_amount_cmp (&new_amount,
			    &total_amount))
      {
	/* new_amount > total_amount: all of the coins (total_amount)
	   do not add up to at least the new_amount owed to the
	   merchant, this request is bogus, abort */
	GNUNET_break (0);
	json_decref (j_coins);
	return NULL;
      }
    }
    else
    {
      /* Full fee covered by merchant, but our total
	 must at least cover the total contract amount */
      if (1 ==
	  TALER_amount_cmp (amount,
			    &total_amount))
	{
	  /* amount > total_amount: all of the coins (total_amount) do
	   not add up to at least the amount owed to the merchant,
	   this request is bogus, abort */
	GNUNET_break (0);
	json_decref (j_coins);
	return NULL;
      }
    }
  } /* end of sanity check */
  pay_obj = json_pack ("{s:o, s:o," /* H_wire/H_contract */
                       " s:I, s:o," /* transaction id, timestamp */
                       " s:o, s:s," /* refund_deadline, exchange */
                       " s:o, s:o," /* coins, max_fee */
                       " s:o}",     /* amount */
                       "H_wire", GNUNET_JSON_from_data (&h_wire,
                                                       sizeof (h_wire)),
                       "H_contract", GNUNET_JSON_from_data (h_contract,
                                                           sizeof (struct GNUNET_HashCode)),
                       "transaction_id", (json_int_t) transaction_id,
                       "timestamp", GNUNET_JSON_from_time_abs (timestamp),
                       "refund_deadline", GNUNET_JSON_from_time_abs (refund_deadline),
		       "exchange", exchange_uri,
		       "coins", j_coins,
                       "max_fee", TALER_JSON_from_amount (max_fee),
                       "amount", TALER_JSON_from_amount (amount)
                       );

  if (0 != execution_deadline.abs_value_us)
  {
    /* Frontend did have an execution date in mind, add it */
    json_object_set_new (pay_obj,
			 "edate",
			 GNUNET_JSON_from_time_abs (execution_deadline));
  }

  ph = GNUNET_new (struct TALER_MERCHANT_Pay);
  ph->ctx = ctx;
  ph->cb = pay_cb;
  ph->cb_cls = pay_cb_cls;
  ph->url = GNUNET_strdup (merchant_uri);

  eh = curl_easy_init ();
  GNUNET_assert (NULL != (ph->json_enc =
                          json_dumps (pay_obj,
                                      JSON_COMPACT)));
  json_decref (pay_obj);
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_URL,
                                   ph->url));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDS,
                                   ph->json_enc));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_POSTFIELDSIZE,
                                   strlen (ph->json_enc)));
  ph->job = GNUNET_CURL_job_add (ctx,
                                 eh,
                                 GNUNET_YES,
                                 &handle_pay_finished,
                                 ph);
  return ph;
}


/**
 * Cancel a pay permission request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param pay the pay permission request handle
 */
void
TALER_MERCHANT_pay_cancel (struct TALER_MERCHANT_Pay *pay)
{
  if (NULL != pay->job)
  {
    GNUNET_CURL_job_cancel (pay->job);
    pay->job = NULL;
  }
  GNUNET_free (pay->url);
  GNUNET_free (pay->json_enc);
  GNUNET_free (pay);
}


/* end of merchant_api_pay.c */
