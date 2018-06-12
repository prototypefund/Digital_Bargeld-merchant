/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016, 2017 GNUnet e.V. and INRIA

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
#include <taler/taler_exchange_service.h>


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
   * Function to call with the result in "pay" @e mode.
   */
  TALER_MERCHANT_PayCallback pay_cb;

  /**
   * Closure for @a pay_cb.
   */
  void *pay_cb_cls;

  /**
   * Function to call with the result in "abort-refund" @e mode.
   */
  TALER_MERCHANT_PayRefundCallback abort_cb;

  /**
   * Closure for @a abort_cb.
   */
  void *abort_cb_cls;

  /**
   * Operational mode, either "pay" or "abort-refund".
   */
  const char *mode;

  /**
   * Reference to the execution context.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * The coins we are paying with.
   */
  struct TALER_MERCHANT_PaidCoin *coins;

  /**
   * Number of @e coins we are paying with.
   */
  unsigned int num_coins;

  /**
   * Hash of the contract, only available in "abort-refund" mode.
   */
  struct GNUNET_HashCode h_contract_terms;

};


/**
 * Check that the response for a /pay refund is well-formed,
 * and call the application callback with the result if it is
 * OK. Otherwise returns #GNUNET_SYSERR.
 *
 * @param ph handle to operation that created the reply
 * @param json the reply to parse
 * @return #GNUNET_OK on success
 */
static int
check_abort_refund (struct TALER_MERCHANT_Pay *ph,
		    const json_t *json)
{
  json_t *refunds;
  unsigned int num_refunds;
  struct TALER_MerchantPublicKeyP merchant_pub;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("refund_permissions", &refunds),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub", &merchant_pub),
    GNUNET_JSON_spec_end()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  num_refunds = json_array_size (refunds);
  {
    struct TALER_MERCHANT_RefundEntry res[GNUNET_NZL(num_refunds)];

    for (unsigned int i=0;i<num_refunds;i++)
    {
      struct TALER_MerchantSignatureP *sig = &res[i].merchant_sig;
      json_t *refund = json_array_get (refunds, i);
      struct GNUNET_JSON_Specification spec_detail[] = {
        GNUNET_JSON_spec_fixed_auto ("merchant_sig",
				     sig),
        GNUNET_JSON_spec_fixed_auto ("coin_pub",
				     &res[i].coin_pub),
	GNUNET_JSON_spec_uint64 ("rtransaction_id",
				 &res[i].rtransaction_id),
        GNUNET_JSON_spec_end()
      };
      struct TALER_RefundRequestPS rr;
      int found;

      if (GNUNET_OK !=
          GNUNET_JSON_parse (refund,
                             spec_detail,
                             NULL, NULL))
      {
        GNUNET_break_op (0);
	GNUNET_JSON_parse_free (spec);
        return GNUNET_SYSERR;
      }

      rr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_REFUND);
      rr.purpose.size = htonl (sizeof (struct TALER_RefundRequestPS));
      rr.h_contract_terms = ph->h_contract_terms;
      rr.coin_pub = res[i].coin_pub;
      rr.merchant = merchant_pub;
      rr.rtransaction_id = GNUNET_htonll (res[i].rtransaction_id);
      found = -1;
      for (unsigned int j=0;j<ph->num_coins;j++)
      {
	if (0 == memcmp (&ph->coins[j].coin_pub,
			 &res[i].coin_pub,
			 sizeof (struct TALER_CoinSpendPublicKeyP)))
	{
	  TALER_amount_hton (&rr.refund_amount,
			     &ph->coins[j].amount_with_fee);
	  TALER_amount_hton (&rr.refund_fee,
			     &ph->coins[j].refund_fee);
	  found = j;
	}
      }
      if (-1 == found)
      {
	GNUNET_break_op (0);
	return GNUNET_SYSERR;
      }

      if (GNUNET_OK !=
	  GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_REFUND,
				      &rr.purpose,
				      &sig->eddsa_sig,
				      &merchant_pub.eddsa_pub))
      {
	GNUNET_break_op (0);
	return GNUNET_SYSERR;
      }
    }
    ph->abort_cb (ph->abort_cb_cls,
		  MHD_HTTP_OK,
		  TALER_EC_NONE,
		  &merchant_pub,
		  &ph->h_contract_terms,
		  num_refunds,
		  res,
		  json);
    ph->abort_cb = NULL;
  }
  GNUNET_JSON_parse_free (spec);
  return GNUNET_OK;
}


/**
 * We got a 403 response back from the exchange (or the merchant).
 * Now we need to check the provided cryptographic proof that the
 * coin was actually already spent!
 *
 * @param pc handle of the original coin we paid with
 * @param json cryptographic proof of coin's transaction history as
 *        was returned by the exchange/merchant
 * @return #GNUNET_OK if proof checks out
 */
static int
check_coin_history (const struct TALER_MERCHANT_PaidCoin *pc,
                    json_t *json)
{
  struct TALER_Amount spent;
  struct TALER_Amount spent_plus_contrib;

  if (GNUNET_OK !=
      TALER_EXCHANGE_verify_coin_history (pc->amount_with_fee.currency,
                                          &pc->coin_pub,
                                          json,
                                          &spent))
  {
    /* Exchange's history fails to verify */
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      TALER_amount_add (&spent_plus_contrib,
                        &spent,
                        &pc->amount_with_fee))
  {
    /* We got an integer overflow? Bad application! */
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (-1 != TALER_amount_cmp (&pc->denom_value,
                              &spent_plus_contrib))
  {
    /* according to our calculations, the transaction should
       have still worked, exchange error! */
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Accepting proof of double-spending\n");
  return GNUNET_OK;
}


/**
 * We got a 403 response back from the exchange (or the merchant).
 * Now we need to check the provided cryptographic proof that the
 * coin was actually already spent!
 *
 * @param ph handle of the original pay operation
 * @param json cryptographic proof returned by the exchange/merchant
 * @return #GNUNET_OK if proof checks out
 */
static int
check_forbidden (struct TALER_MERCHANT_Pay *ph,
                 const json_t *json)
{
  json_t *history;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("history", &history),
    GNUNET_JSON_spec_fixed_auto ("coin_pub", &coin_pub),
    GNUNET_JSON_spec_end()
  };
  int ret;

  if (GNUNET_OK !=
      GNUNET_JSON_parse (json,
                         spec,
                         NULL, NULL))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  for (unsigned int i=0;i<ph->num_coins;i++)
  {
    if (0 == memcmp (&ph->coins[i].coin_pub,
                     &coin_pub,
                     sizeof (struct TALER_CoinSpendPublicKeyP)))
    {
      ret = check_coin_history (&ph->coins[i],
                                history);
      GNUNET_JSON_parse_free (spec);
      return ret;
    }
  }
  GNUNET_break_op (0); /* complaint is not about any of the coins
                          that we actually paid with... */
  GNUNET_JSON_parse_free (spec);
  return GNUNET_SYSERR;
}


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
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
	      "/pay completed with response code %u\n",
	      (unsigned int) response_code);
  if (0 == strcasecmp (ph->mode,
		       "pay"))
  {
    switch (response_code)
    {
    case 0:
      break;
    case MHD_HTTP_OK:
    /* Tolerating Not Acceptable because sometimes
     * - especially in tests - we might want to POST
     * coins one at a time.  */
    case MHD_HTTP_NOT_ACCEPTABLE:
      break;
    case MHD_HTTP_BAD_REQUEST:
      /* This should never happen, either us or the merchant is buggy
	 (or API version conflict); just pass JSON reply to the application */
      break;
    case MHD_HTTP_FORBIDDEN:
      if (GNUNET_OK != check_forbidden (ph,
					json))
      {
	GNUNET_break_op (0);
	response_code = 0;
      }
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
		  (unsigned int) response_code);
      GNUNET_break (0);
      response_code = 0;
      break;
    }
    ph->pay_cb (ph->pay_cb_cls,
		response_code,
		TALER_JSON_get_error_code (json),
		json);
  }
  else
  {
    GNUNET_assert (0 == strcasecmp (ph->mode,
				    "abort-refund"));

    switch (response_code)
    {
    case 0:
      break;
    case MHD_HTTP_OK:
      if (GNUNET_OK ==
	  check_abort_refund (ph,
			      json))
      {
	TALER_MERCHANT_pay_cancel (ph);
	return;
      }
      response_code = 0;
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
		  (unsigned int) response_code);
      GNUNET_break (0);
      response_code = 0;
      break;
    }
    ph->abort_cb (ph->abort_cb_cls,
		  response_code,
		  TALER_JSON_get_error_code (json),
		  NULL,
		  NULL,
		  0,
		  NULL,
		  json);
  }

  TALER_MERCHANT_pay_cancel (ph);
}


/**
 * Issue /pay request. Generic version for the various
 * variants of the API.
 *
 * @param ctx the execution loop context
 * @param merchant_url base URL of the merchant's backend
 * @param merchant_pub public key of the merchant
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param mode mode string to use ("pay" or "abort-refund").
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @param abort_cb callback to call for the abort-refund variant
 * @param abort_cb_cls closure for @a abort_cb
 * @return a handle for this request
 */
static struct TALER_MERCHANT_Pay *
request_pay_generic (struct GNUNET_CURL_Context *ctx,
		     const char *merchant_url,
		     const struct TALER_MerchantPublicKeyP *merchant_pub,
		     const char *order_id,
		     unsigned int num_coins,
		     const struct TALER_MERCHANT_PaidCoin *coins,
		     const char *mode,
		     TALER_MERCHANT_PayCallback pay_cb,
		     void *pay_cb_cls,
		     TALER_MERCHANT_PayRefundCallback abort_cb,
		     void *abort_cb_cls)
{
  struct TALER_MERCHANT_Pay *ph;
  json_t *pay_obj;
  json_t *j_coins;
  CURL *eh;
  struct curl_slist *list = NULL;
  struct TALER_Amount total_fee;
  struct TALER_Amount total_amount;

  if (0 == num_coins)
  {
    GNUNET_break (0);
    return NULL;
  }
  j_coins = json_array ();
  for (unsigned int i=0;i<num_coins;i++)
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
    j_coin = json_pack ("{s:o, s:o," /* contribution/coin_pub */
			" s:s, s:o," /* exchange_url / denom_pub */
			" s:o, s:o}", /* ub_sig / coin_sig */
			"contribution", TALER_JSON_from_amount (&pc->amount_with_fee),
			"coin_pub", GNUNET_JSON_from_data_auto (&pc->coin_pub),
			"exchange_url", pc->exchange_url,
			"denom_pub", GNUNET_JSON_from_rsa_public_key (pc->denom_pub.rsa_public_key),
			"ub_sig", GNUNET_JSON_from_rsa_signature (pc->denom_sig.rsa_signature),
			"coin_sig", GNUNET_JSON_from_data_auto (&pc->coin_sig)
			);
    if (0 !=
        json_array_append_new (j_coins,
                               j_coin))
    {
      GNUNET_break (0);
      json_decref (j_coins);
      return NULL;
    }
  }

  pay_obj = json_pack ("{"
		       " s:s," /* mode */
                       " s:o," /* coins */
                       " s:s," /* order_id */
                       " s:o," /* merchant_pub */
                       "}",
		       "mode", mode,
		       "coins", j_coins,
                       "order_id", order_id,
                       "merchant_pub", GNUNET_JSON_from_data_auto (merchant_pub));
  if (NULL == pay_obj)
  {
    GNUNET_break (0);
    return NULL;
  }
  ph = GNUNET_new (struct TALER_MERCHANT_Pay);
  ph->ctx = ctx;
  ph->mode = mode;
  ph->abort_cb = abort_cb;
  ph->abort_cb_cls = abort_cb_cls;
  ph->pay_cb = pay_cb;
  ph->pay_cb_cls = pay_cb_cls;
  ph->url = TALER_url_join (merchant_url, "/public/pay", NULL);
  ph->num_coins = num_coins;
  ph->coins = GNUNET_new_array (num_coins,
                                struct TALER_MERCHANT_PaidCoin);
  memcpy (ph->coins,
          coins,
          num_coins * sizeof (struct TALER_MERCHANT_PaidCoin));

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

  list = curl_slist_append (list, "Authorization: ApiKey sandbox");

  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_HTTPHEADER,
                                   list));
  ph->job = GNUNET_CURL_job_add (ctx,
                                 eh,
                                 GNUNET_YES,
                                 &handle_pay_finished,
                                 ph);
  return ph;
}


/**
 * Pay a merchant.  API for wallets that have the coin's private keys.
 * _NOTE_: this function does NOT calculate each coin amount in order
 * to match the contract total price.  This calculation is to be made
 * by the logic using this library.
 *
 * @param ctx the execution loop context
 * @param merchant_url base URL of the merchant's backend
 * @param instance which merchant instance will receive this payment
 * @param h_contract_terms hashcode of the proposal being paid
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig signature from the merchant over the original contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param pay_deadline maximum time limit to pay for this contract
 * @param h_wire hash of the merchant’s account details
 * @param order_id order id of the proposal being paid
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
static struct TALER_MERCHANT_Pay *
prepare_pay_generic (struct GNUNET_CURL_Context *ctx,
		     const char *merchant_url,
		     const char *instance,
		     const struct GNUNET_HashCode *h_contract_terms,
		     const struct TALER_Amount *amount,
		     const struct TALER_Amount *max_fee,
		     const struct TALER_MerchantPublicKeyP *merchant_pub,
		     const struct TALER_MerchantSignatureP *merchant_sig,
		     struct GNUNET_TIME_Absolute timestamp,
		     struct GNUNET_TIME_Absolute refund_deadline,
		     struct GNUNET_TIME_Absolute pay_deadline,
		     const struct GNUNET_HashCode *h_wire,
		     const char *order_id,
		     unsigned int num_coins,
		     const struct TALER_MERCHANT_PayCoin *coins,
		     const char *mode,
		     TALER_MERCHANT_PayCallback pay_cb,
		     void *pay_cb_cls,
		     TALER_MERCHANT_PayRefundCallback abort_cb,
		     void *abort_cb_cls)
{
  struct TALER_DepositRequestPS dr;
  struct TALER_MERCHANT_PaidCoin pc[num_coins];

  (void) GNUNET_TIME_round_abs (&timestamp);
  (void) GNUNET_TIME_round_abs (&pay_deadline);
  (void) GNUNET_TIME_round_abs (&refund_deadline);

  if (GNUNET_YES !=
      TALER_amount_cmp_currency (amount,
                                 max_fee))
  {
    GNUNET_break (0);
    return NULL;
  }

  dr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_COIN_DEPOSIT);
  dr.purpose.size = htonl (sizeof (struct TALER_DepositRequestPS));
  dr.h_contract_terms = *h_contract_terms;
  dr.h_wire = *h_wire;
  dr.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dr.refund_deadline = GNUNET_TIME_absolute_hton (refund_deadline);
  dr.merchant = *merchant_pub;

  for (unsigned int i=0;i<num_coins;i++)
  {
    const struct TALER_MERCHANT_PayCoin *coin = &coins[i]; // coin priv.
    struct TALER_MERCHANT_PaidCoin *p = &pc[i]; // coin pub.
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
    {
      TALER_LOG_DEBUG ("... amount_with_fee was %s\n",
                       TALER_amount2s (&coin->amount_with_fee));
      TALER_LOG_DEBUG ("... fee was %s\n",
                       TALER_amount2s (&fee));
    }

    GNUNET_CRYPTO_eddsa_sign (&coin->coin_priv.eddsa_priv,
			      &dr.purpose,
			      &p->coin_sig.eddsa_signature);
    p->denom_pub = coin->denom_pub;
    p->denom_sig = coin->denom_sig;
    p->denom_value = coin->denom_value;
    p->coin_pub = dr.coin_pub;
    p->amount_with_fee = coin->amount_with_fee;
    p->amount_without_fee = coin->amount_without_fee;
    p->refund_fee = coin->refund_fee;
    p->exchange_url = coin->exchange_url;
  }
  return request_pay_generic (ctx,
			      merchant_url,
			      merchant_pub,
			      order_id,
			      num_coins,
			      pc,
			      mode,
			      pay_cb,
			      pay_cb_cls,
			      abort_cb,
			      abort_cb_cls);
}


/**
 * Pay a merchant.  API for wallets that have the coin's private keys.
 * _NOTE_: this function does NOT calculate each coin amount in order
 * to match the contract total price.  This calculation is to be made
 * by the logic using this library.
 *
 * @param ctx the execution loop context
 * @param merchant_url base URL of the merchant's backend
 * @param instance which merchant instance will receive this payment
 * @param h_contract_terms hashcode of the proposal being paid
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig signature from the merchant over the original contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param pay_deadline maximum time limit to pay for this contract
 * @param h_wire hash of the merchant’s account details
 * @param order_id order id of the proposal being paid
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_wallet (struct GNUNET_CURL_Context *ctx,
			   const char *merchant_url,
			   const char *instance,
                           const struct GNUNET_HashCode *h_contract_terms,
			   const struct TALER_Amount *amount,
			   const struct TALER_Amount *max_fee,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           const struct TALER_MerchantSignatureP *merchant_sig,
                           struct GNUNET_TIME_Absolute timestamp,
                           struct GNUNET_TIME_Absolute refund_deadline,
                           struct GNUNET_TIME_Absolute pay_deadline,
                           const struct GNUNET_HashCode *h_wire,
                           const char *order_id,
                           unsigned int num_coins,
                           const struct TALER_MERCHANT_PayCoin *coins,
                           TALER_MERCHANT_PayCallback pay_cb,
                           void *pay_cb_cls)
{
  return prepare_pay_generic (ctx,
			      merchant_url,
			      instance,
			      h_contract_terms,
			      amount,
			      max_fee,
			      merchant_pub,
			      merchant_sig,
			      timestamp,
			      refund_deadline,
			      pay_deadline,
			      h_wire,
			      order_id,
			      num_coins,
			      coins,
			      "pay",
			      pay_cb,
			      pay_cb_cls,
			      NULL,
			      NULL);
}


/**
 * Run a payment abort operation, asking for refunds for coins
 * that were previously spend on a /pay that failed to go through.
 *
 * @param ctx execution context
 * @param merchant_url base URL of the merchant
 * @param instance which merchant instance will receive this payment
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig signature from the merchant over the original contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param pay_deadline maximum time limit to pay for this contract
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param payref_cb the callback to call when a reply for this request is available
 * @param payref_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_abort (struct GNUNET_CURL_Context *ctx,
			  const char *merchant_url,
			  const char *instance,
			  const struct GNUNET_HashCode *h_contract,
			  const struct TALER_Amount *amount,
			  const struct TALER_Amount *max_fee,
			  const struct TALER_MerchantPublicKeyP *merchant_pub,
			  const struct TALER_MerchantSignatureP *merchant_sig,
			  struct GNUNET_TIME_Absolute timestamp,
			  struct GNUNET_TIME_Absolute refund_deadline,
			  struct GNUNET_TIME_Absolute pay_deadline,
			  const struct GNUNET_HashCode *h_wire,
			  const char *order_id,
			  unsigned int num_coins,
			  const struct TALER_MERCHANT_PayCoin *coins,
			  TALER_MERCHANT_PayRefundCallback payref_cb,
			  void *payref_cb_cls)
{
  struct TALER_MERCHANT_Pay *ph;

  ph = prepare_pay_generic (ctx,
			    merchant_url,
			    instance,
			    h_contract,
			    amount,
			    max_fee,
			    merchant_pub,
			    merchant_sig,
			    timestamp,
			    refund_deadline,
			    pay_deadline,
			    h_wire,
			    order_id,
			    num_coins,
			    coins,
			    "abort-refund",
			    NULL,
			    NULL,
			    payref_cb,
			    payref_cb_cls);
  ph->h_contract_terms = *h_contract;
  return ph;
}


/**
 * PAY a merchant.  API for frontends talking to backends. Here,
 * the frontend does not have the coin's private keys, but just
 * the public keys and signatures.  Note the subtle difference
 * in the type of @a coins compared to #TALER_MERCHANT_pay().
 *
 * @param ctx the execution loop context
 * @param merchant_url base URL of the merchant's backend
 * @param merchant_pub public key of the merchant
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_frontend (struct GNUNET_CURL_Context *ctx,
			     const char *merchant_url,
                             const struct TALER_MerchantPublicKeyP *merchant_pub,
                             const char *order_id,
                             unsigned int num_coins,
                             const struct TALER_MERCHANT_PaidCoin *coins,
                             TALER_MERCHANT_PayCallback pay_cb,
                             void *pay_cb_cls)
{
  return request_pay_generic (ctx,
			      merchant_url,
			      merchant_pub,
			      order_id,
			      num_coins,
			      coins,
			      "pay",
			      pay_cb,
			      pay_cb_cls,
			      NULL,
			      NULL);
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
  GNUNET_free (pay->coins);
  GNUNET_free (pay->url);
  GNUNET_free (pay->json_enc);
  GNUNET_free (pay);
}


/* end of merchant_api_pay.c */
