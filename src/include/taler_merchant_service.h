/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LIB.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_merchant_service.h
 * @brief C interface of libtalermerchant, a C library to use merchant's HTTP API
 * @author Christian Grothoff
 */
#ifndef _TALER_MERCHANT_SERVICE_H
#define _TALER_MERCHANT_SERVICE_H

#include <taler/taler_util.h>
#include <gnunet/gnunet_curl_lib.h>
#include <jansson.h>

/* *********************  /contract *********************** */


/**
 * @brief Handle to a /contract operation at a merchant's backend.
 */
struct TALER_MERCHANT_ContractOperation;


/**
 * Callbacks of this type are used to serve the result of submitting a
 * /contract request to a merchant.
 *
 * @param cls closure
 * @param http_status HTTP response code, 200 indicates success;
 *                    0 if the backend's reply is bogus (fails to follow the protocol)
 * @param obj the full received JSON reply, or
 *            error details if the request failed
 * @param contract completed contract, NULL on error
 * @param sig merchant's signature over the contract, NULL on error
 * @param h_contract hash of the contract, NULL on error
 */
typedef void
(*TALER_MERCHANT_ContractCallback) (void *cls,
                                    unsigned int http_status,
                                    const json_t *obj,
                                    const json_t *contract,
                                    const struct TALER_MerchantSignatureP *sig,
                                    const struct GNUNET_HashCode *h_contract);


/**
 * Request backend to sign a contract (and add fields like wire transfer
 * details).
 *
 * @param ctx execution context
 * @param backend_uri URI of the backend
 * @param contract prototype of the contract
 * @param contract_cb the callback to call when a reply for this request is available
 * @param contract_cb_cls closure for @a contract_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_ContractOperation *
TALER_MERCHANT_contract_sign (struct GNUNET_CURL_Context *ctx,
                              const char *backend_uri,
                              const json_t *contract,
                              TALER_MERCHANT_ContractCallback contract_cb,
                              void *contract_cb_cls);


/**
 * Cancel a /contract request.
 *
 * @param co the contract operation handle
 */
void
TALER_MERCHANT_contract_sign_cancel (struct TALER_MERCHANT_ContractOperation *co);


/* *********************  /pay *********************** */


/**
 * @brief Handle to a /pay operation at a merchant.  Note that we use
 * the same handle for interactions with frontends (API for wallets)
 * or backends (API for frontends).  The difference is that for the
 * frontend API, we need the private keys of the coins, while for
 * the backend API we need the public keys and signatures received
 * from the wallet.  Also, the frontend returns a redirect URI on
 * success, while the backend just returns a success status code.
 */
struct TALER_MERCHANT_Pay;


/**
 * Callbacks of this type are used to serve the result of submitting a
 * /pay request to a merchant.
 *
 * @param cls closure
 * @param http_status HTTP response code, 200 or 300-level response codes
 *                    can indicate success, depending on whether the interaction
 *                    was with a merchant frontend or backend;
 *                    0 if the merchant's reply is bogus (fails to follow the protocol)
 * @param redirect_uri URI for the redirect, if the request was successful and we were talking to a frontend;
 *                     NULL if the request failed or if were were talking to a backend
 * @param obj the received JSON reply, with error details if the request failed
 */
typedef void
(*TALER_MERCHANT_PayCallback) (void *cls,
                               unsigned int http_status,
                               const char *redirect_uri,
                               const json_t *obj);


/**
 * Information we need from the wallet for each coin when doing the
 * payment.
 */
struct TALER_MERCHANT_PayCoin
{
  /**
   * Denomination key with which the coin is signed
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Exchange’s unblinded signature of the coin
   */
  struct TALER_DenominationSignature denom_sig;

  /**
   * Coin's private key.
   */
  struct TALER_CoinSpendPrivateKeyP coin_priv;

  /**
   * Amount this coin is to contribute (including fee).
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Amount this coin is to contribute (without fee).
   */
  struct TALER_Amount amount_without_fee;
};


/**
 * Pay a merchant.  API for wallets that have the coin's private keys.
 *
 * @param ctx execution context
 * @param merchant_uri URI of the merchant
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig signature from the merchant over the original contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param exchange_uri URI of the exchange that the coins belong to
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
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
                           void *pay_cb_cls);


/**
 * Information we need from the frontend when forwarding
 * a payment to the backend.
 */
struct TALER_MERCHANT_PaidCoin
{
  /**
   * Denomination key with which the coin is signed
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Exchange’s unblinded signature of the coin
   */
  struct TALER_DenominationSignature denom_sig;

  /**
   * Coin's public key.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Coin's signature key.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Amount this coin is to contribute (including fee).
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Amount this coin is to contribute (without fee).
   */
  struct TALER_Amount amount_without_fee;

};


/**
 * Pay a merchant.  API for frontends talking to backends. Here,
 * the frontend does not have the coin's private keys, but just
 * the public keys and signatures.  Note the sublte difference
 * in the type of @a coins compared to #TALER_MERCHANT_pay().
 *
 * @param ctx execution context
 * @param merchant_uri URI of the merchant
 * @param h_contract hash of the contact of the merchant with the customer
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig the signature of the merchant over the original contract
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param execution_deadline date by which the merchant would like the exchange to execute the transaction (can be zero if there is no specific date desired by the frontend)
 * @param h_wire hash of the merchant’s account details
 * @param exchange_uri URI of the exchange that the coins belong to
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
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
                             void *pay_cb_cls);


/**
 * Cancel a /pay request.  Note that if you cancel a request like
 * this, you have no assurance that the request has not yet been
 * forwarded to the merchant. Thus, the payment may still succeed or
 * fail.  Re-issue the original /pay request to resume/retry and
 * obtain a definitive result, or /refresh the coins involved to
 * ensure that the merchant can no longer complete the payment.
 *
 * @param wh the wire information request handle
 */
void
TALER_MERCHANT_pay_cancel (struct TALER_MERCHANT_Pay *ph);


#endif  /* _TALER_MERCHANT_SERVICE_H */
