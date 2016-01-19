/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_merchant_service.h
 * @brief C interface of libtalermerchant, a C library to use merchant's HTTP API
 * @author Christian Grothoff
 */
#ifndef _TALER_MERCHANT_SERVICE_H
#define _TALER_MERCHANT_SERVICE_H

#include <taler/taler_util.h>

/* ********************* event loop *********************** */

/**
 * @brief Handle to this library context.  This is where the
 * main event loop logic lives.
 */
struct TALER_MERCHANT_Context;


/**
 * Initialise a context.  A context should be used for each thread and should
 * not be shared among multiple threads.
 *
 * @return the context, NULL on error (failure to initialize)
 */
struct TALER_MERCHANT_Context *
TALER_MERCHANT_init (void);


/**
 * Obtain the information for a select() call to wait until
 * #TALER_MERCHANT_perform() is ready again.  Note that calling
 * any other TALER_MERCHANT-API may also imply that the library
 * is again ready for #TALER_MERCHANT_perform().
 *
 * Basically, a client should use this API to prepare for select(),
 * then block on select(), then call #TALER_MERCHANT_perform() and then
 * start again until the work with the context is done.
 *
 * This function will NOT zero out the sets and assumes that @a max_fd
 * and @a timeout are already set to minimal applicable values.  It is
 * safe to give this API FD-sets and @a max_fd and @a timeout that are
 * already initialized to some other descriptors that need to go into
 * the select() call.
 *
 * @param ctx context to get the event loop information for
 * @param read_fd_set will be set for any pending read operations
 * @param write_fd_set will be set for any pending write operations
 * @param except_fd_set is here because curl_multi_fdset() has this argument
 * @param max_fd set to the highest FD included in any set;
 *        if the existing sets have no FDs in it, the initial
 *        value should be "-1". (Note that `max_fd + 1` will need
 *        to be passed to select().)
 * @param timeout set to the timeout in milliseconds (!); -1 means
 *        no timeout (NULL, blocking forever is OK), 0 means to
 *        proceed immediately with #TALER_MERCHANT_perform().
 */
void
TALER_MERCHANT_get_select_info (struct TALER_MERCHANT_Context *ctx,
                                fd_set *read_fd_set,
                                fd_set *write_fd_set,
                                fd_set *except_fd_set,
                                int *max_fd,
                                long *timeout);


/**
 * Run the main event loop for the Taler interaction.
 *
 * @param ctx the library context
 */
void
TALER_MERCHANT_perform (struct TALER_MERCHANT_Context *ctx);


/**
 * Cleanup library initialisation resources.  This function should be called
 * after using this library to cleanup the resources occupied during library's
 * initialisation.
 *
 * @param ctx the library context
 */
void
TALER_MERCHANT_fini (struct TALER_MERCHANT_Context *ctx);


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
                               json_t *obj);


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
   * Mint’s unblinded signature of the coin
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
 * @param merchant the merchant context
 * @param merchant_uri URI of the merchant
 * @param mint_uri URI of the mint that the coins belong to
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
TALER_MERCHANT_pay_wallet (struct TALER_MERCHANT_Context *merchant,
			   const char *merchant_uri,
			   const char *mint_uri,
                           const struct GNUNET_HashCode *h_wire,
                           const struct GNUNET_HashCode *h_contract,
                           struct GNUNET_TIME_Absolute timestamp,
                           uint64_t transaction_id,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           struct GNUNET_TIME_Absolute refund_deadline,
                           unsigned int num_coins,
                           const struct TALER_MERCHANT_PayCoin *coins,
			   const struct TALER_Amount *max_fee,
			   const struct TALER_Amount *amount,
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
   * Mint’s unblinded signature of the coin
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
 * @param merchant the merchant context
 * @param merchant_uri URI of the merchant
 * @param mint_uri URI of the mint that the coins belong to
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param execution_deadline date by which the merchant would like the mint to execute the transaction (can be zero if there is no specific date desired by the frontend)
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
TALER_MERCHANT_pay_frontend (struct TALER_MERCHANT_Context *merchant,
			     const char *merchant_uri,
			     const char *mint_uri,
                             const struct GNUNET_HashCode *h_wire,
                             const struct GNUNET_HashCode *h_contract,
                             struct GNUNET_TIME_Absolute timestamp,
                             uint64_t transaction_id,
                             const struct TALER_MerchantPublicKeyP *merchant_pub,
                             struct GNUNET_TIME_Absolute refund_deadline,
                             struct GNUNET_TIME_Absolute execution_deadline,
                             unsigned int num_coins,
                             const struct TALER_MERCHANT_PaidCoin *coins,
			     const struct TALER_Amount *max_fee,
			     const struct TALER_Amount *amount,
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
