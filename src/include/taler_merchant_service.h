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
  TALER; see the file COPYING.LIB.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_merchant_service.h
 * @brief C interface of libtalermerchant, a C library to use merchant's HTTP API
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#ifndef _TALER_MERCHANT_SERVICE_H
#define _TALER_MERCHANT_SERVICE_H

#include <taler/taler_util.h>
#include <taler/taler_error_codes.h>
#include <gnunet/gnunet_curl_lib.h>
#include <jansson.h>


/* ********************* /map/{in,out} *********************** */

/* *********************  /proposal *********************** */


/**
 * Handle to a PUT /proposal operation
 */
struct TALER_MERCHANT_ProposalOperation;

/**
 * Handle to a GET /proposal operation
 */
struct TALER_MERCHANT_ProposalLookupOperation;

/**
 * Callbacks of this type are used to serve the result of submitting a
 * /contract request to a merchant.
 *
 * @param cls closure
 * @param http_status HTTP response code, 200 indicates success;
 *                    0 if the backend's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code 
 * @param obj raw JSON reply, or error details if the request failed
 * @param proposal_data completed contract, NULL on error
 * @param sig merchant's signature over the contract, NULL on error
 * @param hash proposal data's hashcode, NULL on error
 */
typedef void
(*TALER_MERCHANT_ProposalCallback) (void *cls,
                                    unsigned int http_status,
				    enum TALER_ErrorCode ec,
                                    const json_t *obj,
                                    const json_t *proposal_data,
                                    const struct TALER_MerchantSignatureP *sig,
                                    const struct GNUNET_HashCode *hash);


/**
 * Callback called to work a GET /proposal response.
 *
 * @param cls closure
 * @param http_status HTTP status code of the request
 * @param body JSON containing the response's payload.
 * In case of errors, it contains the appropriate error encoding.
 */
typedef void
(*TALER_MERCHANT_ProposalLookupOperationCallback) (void *cls,
                                                   unsigned int http_status,
                                                   const json_t *body);

/**
 * PUT an order to the backend and receives the related proposal.
 *
 * @param ctx execution context
 * @param backend_uri URI of the backend
 * @param order basic information about this purchase, to be extended by the
 * backend
 * @param proposal_cb the callback to call when a reply for this request is available
 * @param proposal_cb_cls closure for @a proposal_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_ProposalOperation *
TALER_MERCHANT_order_put (struct GNUNET_CURL_Context *ctx,
                          const char *backend_uri,
                          const json_t *order,
                          TALER_MERCHANT_ProposalCallback proposal_cb,
                          void *proposal_cb_cls);


/**
 * Cancel a PUT /proposal request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param po the proposal operation request handle
 */
void
TALER_MERCHANT_proposal_cancel (struct TALER_MERCHANT_ProposalOperation *po);


/**
 * Calls the GET /proposal API at the backend.  That is,
 * retrieve a proposal data by providing its transaction id.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param transaction_id transaction id used to perform the lookup
 * @param plo_cb callback which will work the response gotten from the backend
 * @param plo_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_ProposalLookupOperation *
TALER_MERCHANT_proposal_lookup (struct GNUNET_CURL_Context *ctx,
                                const char *backend_uri,
                                const char *transaction_id,
                                const char *instance,
                                TALER_MERCHANT_ProposalLookupOperationCallback plo_cb,
                                void *plo_cb_cls);


/**
 * Cancel a GET /proposal request.
 *
 * @param plo handle to the request to be canceled
 */
void
TALER_MERCHANT_proposal_lookup_cancel (struct TALER_MERCHANT_ProposalLookupOperation *plo);



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
 * @param ec taler-specific error code 
 * @param obj the received JSON reply, with error details if the request failed
 */
typedef void
(*TALER_MERCHANT_PayCallback) (void *cls,
                               unsigned int http_status,
			       enum TALER_ErrorCode ec,
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
   * Overall value that coins of this @e denom_pub have.
   */
  struct TALER_Amount denom_value;

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
 * @param merchant_uri base URI of the merchant
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
			   const char *exchange_uri,
                           const char *order_id,
                           unsigned int num_coins,
                           const struct TALER_MERCHANT_PayCoin *coins,
                           TALER_MERCHANT_PayCallback pay_cb,
                           void *pay_cb_cls);


/**
 * Information we need from the frontend (ok, the frontend sends just JSON)
 * when forwarding a payment to the backend.
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
   * Overall value that coins of this @e denom_pub have.
   */
  struct TALER_Amount denom_value;

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
 * @param merchant_uri base URI of the merchant
 * @param instance which merchant instance will receive this payment
 * @param h_contract hash of the contact of the merchant with the customer
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param merchant_sig the signature of the merchant over the original contract
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param pay_deadline maximum time limit to pay for this contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param wire_transfer_deadline date by which the merchant would like the exchange to execute the wire transfer (can be zero if there is no specific date desired by the frontend). If non-zero, must be larger than @a refund_deadline.
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
                             const struct TALER_MerchantPublicKeyP *merchant_pub,
			     const char *order_id,
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


/* ********************* /track/transfer *********************** */

/**
 * @brief Handle to a /track/transfer operation at a merchant's backend.
 */
struct TALER_MERCHANT_TrackTransferHandle;

/**
 * Callbacks of this type are used to work the result of submitting a /track/transfer request to a merchant
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param ec taler-specific error code 
 * @param sign_key exchange key used to sign @a json, or NULL
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param h_wire hash of the wire transfer address the transfer went to, or NULL on error
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
typedef void
(*TALER_MERCHANT_TrackTransferCallback) (void *cls,
                                         unsigned int http_status,
					 enum TALER_ErrorCode ec,
                                         const struct TALER_ExchangePublicKeyP *sign_key,
                                         const json_t *json,
                                         const struct GNUNET_HashCode *h_wire,
                                         const struct TALER_Amount *total_amount,
                                         unsigned int details_length,
                                         const struct TALER_TrackTransferDetails *details);

/**
 * Request backend to return deposits associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_uri base URI of the backend
 * @param instance which merchant instance is going to be tracked
 * @param wtid base32 string indicating a wtid
 * @param exchange base URL of the exchange in charge of returning the wanted information
 * @param track_transfer_cb the callback to call when a reply for this request is available
 * @param track_transfer_cb_cls closure for @a track_transfer_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransferHandle *
TALER_MERCHANT_track_transfer (struct GNUNET_CURL_Context *ctx,
                               const char *backend_uri,
                               const char *instance,
                               const struct TALER_WireTransferIdentifierRawP *wtid,
                               const char *exchange_uri,
                               TALER_MERCHANT_TrackTransferCallback track_transfer_cb,
                               void *track_transfer_cb_cls);


/**
 * Cancel a /track/transfer request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param co the deposit's tracking operation
 */
void
TALER_MERCHANT_track_transfer_cancel (struct TALER_MERCHANT_TrackTransferHandle *tdo);


/* ********************* /track/transaction *********************** */

/**
 * @brief Handle to a /track/transaction operation at a merchant's backend.
 */
struct TALER_MERCHANT_TrackTransactionHandle;

/**
 * Information about a coin aggregated in a wire transfer for a
 * /track/transaction response.
 */
struct TALER_MERCHANT_CoinWireTransfer
{

  /**
   * Public key of the coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Value of the coin including deposit fee.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Deposit fee for the coin.
   */
  struct TALER_Amount deposit_fee;

};

/**
 * Information about a wire transfer for a /track/transaction response.
 */
struct TALER_MERCHANT_TransactionWireTransfer
{

  /**
   * Wire transfer identifier this struct is about.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * When was this wire transfer executed?
   */
  struct GNUNET_TIME_Absolute execution_time;

  /**
   * Number of coins of the selected transaction that
   * is covered by this wire transfer.
   */
  unsigned int num_coins;

  /**
   * Information about the coins of the selected transaction
   * that are part of the wire transfer.
   */
  struct TALER_MERCHANT_CoinWireTransfer *coins;

};


/**
 * Callbacks of this type are used to work the result of submitting a
 * /track/transaction request to a merchant
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param ec taler-specific error code 
 * @param json original json reply from the backend
 * @param num_transfers number of wire transfers the exchange used for the transaction
 * @param transfers details about each transfer and which coins are aggregated in it
*/
typedef void
(*TALER_MERCHANT_TrackTransactionCallback) (void *cls,
                                            unsigned int http_status,
					    enum TALER_ErrorCode ec,
                                            const json_t *json);


/**
 * Request backend to return deposits associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_uri base URI of the backend
 * @param instance which merchant instance is going to be tracked
 * @param transaction_id which transaction should we trace
 * @param track_transaction_cb the callback to call when a reply for this request is available
 * @param track_transaction_cb_cls closure for @a track_transaction_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransactionHandle *
TALER_MERCHANT_track_transaction (struct GNUNET_CURL_Context *ctx,
                                  const char *backend_uri,
                                  const char *instance,
                                  const char *order_id,
                                  TALER_MERCHANT_TrackTransactionCallback track_transaction_cb,
                                  void *track_transaction_cb_cls);


/**
 * Cancel a /track/transaction request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param co the deposit's tracking operation
 */
void
TALER_MERCHANT_track_transaction_cancel (struct TALER_MERCHANT_TrackTransactionHandle *tdo);

/* ********************* /history *********************** */

struct TALER_MERCHANT_HistoryOperation;

/**
 * Callback for a /history request. It's up to this function how
 * to render the array containing transactions details (FIXME link to
 * documentation)
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant backend
 * @param ec taler-specific error code 
 * @param json actual body containing history
 */
typedef void
(*TALER_MERCHANT_HistoryOperationCallback) (void *cls,
                                            unsigned int http_status,
					    enum TALER_ErrorCode ec,
                                            const json_t *json);

/**
 * Issue a /history request to the backend.
 *
 * @param ctx execution context
 * @param backend_uri base URL of the merchant backend
 * @param instance which merchant instance is performing this call
 * @param start return `delta` records starting from position `start`
 * @param delta return `delta` records starting from position `start`
 * @param date only transactions younger than/equals to date will be returned
 * @param history_cb callback which will work the response gotten from the backend
 * @param history_cb_cls closure to pass to history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_HistoryOperation *
TALER_MERCHANT_history (struct GNUNET_CURL_Context *ctx,
                        const char *backend_uri,
                        const char *instance,
                        unsigned int start,
                        unsigned int delta,
                        struct GNUNET_TIME_Absolute date,
                        TALER_MERCHANT_HistoryOperationCallback history_cb,
                        void *history_cb_cls);



/**
 * Cancel a pending /history request
 *
 * @param handle from the operation to cancel
 */
void
TALER_MERCHANT_history_cancel (struct TALER_MERCHANT_HistoryOperation *ho);

#endif  /* _TALER_MERCHANT_SERVICE_H */
