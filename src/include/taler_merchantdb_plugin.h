/*
  This file is part of TALER
  Copyright (C) 2014-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.GPL.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_merchantdb_plugin.h
 * @brief database access for the merchant
 * @author Florian Dold
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANTDB_PLUGIN_H
#define TALER_MERCHANTDB_PLUGIN_H

#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_db_lib.h>
#include <jansson.h>

/**
 * Handle to interact with the database.
 */
struct TALER_MERCHANTDB_Plugin;


/**
 * Details about a wire account of the merchant.
 */
struct TALER_MERCHANTDB_AccountDetails
{
  /**
   * Hash of the wire details (@e payto_uri and @e salt).
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Salt value used for hashing @e payto_uri.
   */
  struct GNUNET_HashCode salt;

  /**
   * Actual account address as a payto://-URI.
   */
  const char *payto_uri;

  /**
   * Is the account set for active use in new contracts?
   */
  bool active;

};


/**
 * General settings for an instance.
 */
struct TALER_MERCHANTDB_InstanceSettings
{
  /**
   * prefix for the instance under "/instances/"
   */
  char *id;

  /**
   * legal name of the instance
   */
  char *name;

  /**
   * Address of the business
   */
  json_t *address;

  /**
   * jurisdiction of the business
   */
  json_t *jurisdiction;

  /**
   * Default max deposit fee that the merchant is willing to
   * pay; if deposit costs more, then the customer will cover
   * the difference.
   */
  struct TALER_Amount default_max_deposit_fee;

  /**
   * Default maximum wire fee to assume, unless stated differently in the
   * proposal already.
   */
  struct TALER_Amount default_max_wire_fee;

  /**
   * Default factor for wire fee amortization.
   */
  uint32_t default_wire_fee_amortization;

  /**
   * If the frontend does NOT specify an execution date, how long should
   * we tell the exchange to wait to aggregate transactions before
   * executing the wire transfer?  This delay is added to the current
   * time when we generate the advisory execution time for the exchange.
   */
  struct GNUNET_TIME_Relative default_wire_transfer_delay;

  /**
   * If the frontend does NOT specify a payment deadline, how long should
   * offers we make be valid by default?
   */
  struct GNUNET_TIME_Relative default_pay_delay;

};


/**
 * Typically called by `lookup_instances`.
 *
 * @param cls closure
 * @param merchant_pub public key of the instance
 * @param merchant_priv private key of the instance, NULL if not available
 * @param is general instance settings
 * @param accounts_length length of the @a accounts array
 * @param accounts list of accounts of the merchant
 */
typedef void
(*TALER_MERCHANTDB_InstanceCallback)(
  void *cls,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const struct TALER_MerchantPrivateKeyP *merchant_priv,
  const struct TALER_MERCHANTDB_InstanceSettings *is,
  unsigned int accounts_length,
  const struct TALER_MERCHANTDB_AccountDetails accounts[]);


/**
 * Typically called by `find_contract_terms_by_date`.
 *
 * @param cls closure
 * @param order_id order id
 * @param row_id serial numer of the transaction in the table,
 * @param contract_terms proposal data related to order id
 */
typedef void
(*TALER_MERCHANTDB_ProposalDataCallback)(void *cls,
                                         const char *order_id,
                                         uint64_t row_id,
                                         const json_t *contract_terms);

/**
 * Function called with information about a transaction.
 *
 * @param cls closure
 * @param merchant_pub merchant's public key
 * @param h_contract_terms proposal data's hashcode
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
typedef void
(*TALER_MERCHANTDB_TransactionCallback)(
  void *cls,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct GNUNET_HashCode *h_wire,
  struct GNUNET_TIME_Absolute timestamp,
  struct GNUNET_TIME_Absolute refund,
  const struct TALER_Amount *total_amount);


/**
 * Function called with information about a coin that was deposited.
 *
 * @param cls closure
 * @param h_contract_terms proposal data's hashcode
 * @param coin_pub public key of the coin
 * @param exchange_url URL of the exchange that issued the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param refund_fee fee the exchange will charge for refunding this coin
 * @param wire_fee wire fee the exchange charges
 * @param exchange_proof proof from exchange that coin was accepted,
 *        matches the `interface DepositSuccess` of the documentation.
 */
typedef void
(*TALER_MERCHANTDB_CoinDepositCallback)(
  void *cls,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct TALER_CoinSpendPublicKeyP *coin_pub,
  const char *exchange_url,
  const struct TALER_Amount *amount_with_fee,
  const struct TALER_Amount *deposit_fee,
  const struct TALER_Amount *refund_fee,
  const struct TALER_Amount *wire_fee,
  const json_t *exchange_proof);


/**
 * Information about the wire transfer corresponding to
 * a deposit operation.  Note that it is in theory possible
 * that we have a @a h_contract_terms and @a coin_pub in the
 * result that do not match a deposit that we know about,
 * for example because someone else deposited funds into
 * our account.
 *
 * @param cls closure
 * @param h_contract_terms hashcode of the proposal data
 * @param coin_pub public key of the coin
 * @param wtid identifier of the wire transfer in which the exchange
 *             send us the money for the coin deposit
 * @param execution_time when was the wire transfer executed?
 * @param exchange_proof proof from exchange about what the deposit was for
 *             NULL if we have not asked for this signature
 */
typedef void
(*TALER_MERCHANTDB_TransferCallback)(
  void *cls,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct TALER_CoinSpendPublicKeyP *coin_pub,
  const struct TALER_WireTransferIdentifierRawP *wtid,
  struct GNUNET_TIME_Absolute execution_time,
  const json_t *exchange_proof);


/**
 * Function called with information about a wire transfer identifier.
 *
 * @param cls closure
 * @param proof proof from exchange about what the wire transfer was for
 */
typedef void
(*TALER_MERCHANTDB_ProofCallback)(void *cls,
                                  const json_t *proof);


/**
 * Function called with information about a refund.
 *
 * @param cls closure
 * @param coin_pub public coin from which the refund comes from
 * @param exchange_url URL of the exchange that issued @a coin_pub
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explanation of the refund
 * @param refund_amount refund amount which is being taken from @a coin_pub
 * @param refund_fee cost of this refund operation
 */
typedef void
(*TALER_MERCHANTDB_RefundCallback)(
  void *cls,
  const struct TALER_CoinSpendPublicKeyP *coin_pub,
  const char *exchange_url,
  uint64_t rtransaction_id,
  const char *reason,
  const struct TALER_Amount *refund_amount,
  const struct TALER_Amount *refund_fee);


/**
 * Handle to interact with the database.
 *
 * Functions ending with "_TR" run their OWN transaction scope
 * and MUST NOT be called from within a transaction setup by the
 * caller.  Functions ending with "_NT" require the caller to
 * setup a transaction scope.  Functions without a suffix are
 * simple, single SQL queries that MAY be used either way.
 */
struct TALER_MERCHANTDB_Plugin
{

  /**
   * Closure for all callbacks.
   */
  void *cls;

  /**
   * Name of the library which generated this plugin.  Set by the
   * plugin loader.
   */
  char *library_name;

  /**
   * Drop merchant tables. Used for testcases.
   *
   * @param cls closure
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*drop_tables) (void *cls);

  /**
   * Do a pre-flight check that we are not in an uncommitted transaction.
   * If we are, try to commit the previous transaction and output a warning.
   * Does not return anything, as we will continue regardless of the outcome.
   *
   * @param cls the `struct PostgresClosure` with the plugin-specific state
   */
  void
  (*preflight) (void *cls);


  /**
   * Start a transaction.
   *
   * @param cls the `struct PostgresClosure` with the plugin-specific state
   * @param name unique name identifying the transaction (for debugging),
   *             must point to a constant
   * @return #GNUNET_OK on success
   */
  int
  (*start) (void *cls,
            const char *name);


  /**
   * Roll back the current transaction of a database connection.
   *
   * @param cls the `struct PostgresClosure` with the plugin-specific state
   * @return #GNUNET_OK on success
   */
  void
  (*rollback) (void *cls);


  /**
   * Commit the current transaction of a database connection.
   *
   * @param cls the `struct PostgresClosure` with the plugin-specific state
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*commit)(void *cls);


  /**
   * Lookup all of the instances this backend has configured.
   *
   * @param cls closure
   * @param active_only only find 'active' instances
   * @param cb function to call on all instances found
   * @param cb_cls closure for @a cb
   */
  enum GNUNET_DB_QueryStatus
  (*lookup_instances)(void *cls,
                      bool active_only,
                      TALER_MERCHANTDB_InstanceCallback cb,
                      void *cb_cls);


  /**
   * Insert information about an instance into our database.
   *
   * @param cls closure
   * @param merchant_pub public key of the instance
   * @param merchant_priv private key of the instance
   * @param is details about the instance
   * @return database result code
   */
  enum GNUNET_DB_QueryStatus
  (*insert_instance)(void *cls,
                     const struct TALER_MerchantPublicKeyP *merchant_pub,
                     const struct TALER_MerchantPrivateKeyP *merchant_priv,
                     const struct TALER_MERCHANTDB_InstanceSettings *is);

  /**
   * Insert information about an instance's account into our database.
   *
   * @param cls closure
   * @param id identifier of the instance
   * @param account_details details about the account
   * @return database result code
   */
  enum GNUNET_DB_QueryStatus
  (*insert_account)(
    void *cls,
    const char *id,
    const struct TALER_MERCHANTDB_AccountDetails *account_details);

  /**
   * Delete private key of an instance from our database.
   *
   * @param cls closure
   * @param merchant_id identifier of the instance
   * @return database result code
   */
  enum GNUNET_DB_QueryStatus
  (*delete_instance_private_key)(
    void *cls,
    const char *merchant_id);


  /**
   * Purge an instance and all associated information from our database.
   * Highly likely to cause undesired data loss. Use with caution.
   *
   * @param cls closure
   * @param merchant_id identifier of the instance
   * @return database result code
   */
  enum GNUNET_DB_QueryStatus
  (*purge_instance)(void *cls,
                    const char *merchant_id);

  /**
   * Update information about an instance into our database.
   *
   * @param cls closure
   * @param is details about the instance
   * @return database result code
   */
  enum GNUNET_DB_QueryStatus
  (*patch_instance)(void *cls,
                    const struct TALER_MERCHANTDB_InstanceSettings *is);

  /**
   * Set an instance's account in our database to "inactive".
   *
   * @param cls closure
   * @param h_wire hash of the wire account to set to inactive
   * @return database result code
   */
  enum GNUNET_DB_QueryStatus
  (*inactivate_account)(void *cls,
                        const struct GNUNET_HashCode *h_wire);


  /* ****************** OLD API ******************** */

  /**
   * Insert order into db.
   *
   * @param cls closure
   * @param order_id alphanumeric string that uniquely identifies the proposal
   * @param merchant_pub merchant's public key
   * @param timestamp timestamp of this proposal data
   * @param contract_terms proposal data to store
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_order)(void *cls,
                  const char *order_id,
                  const struct TALER_MerchantPublicKeyP *merchant_pub,
                  struct GNUNET_TIME_Absolute timestamp,
                  const json_t *contract_terms);


  /**
   * Insert proposal data into db; the routine will internally hash and
   * insert the proposal data's hashcode into the same row.
   *
   * @param cls closure
   * @param order_id alphanumeric string that uniquely identifies the proposal
   * @param merchant_pub merchant's public key
   * @param timestamp timestamp of this proposal data
   * @param contract_terms proposal data to store
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_contract_terms)(void *cls,
                           const char *order_id,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           struct GNUNET_TIME_Absolute timestamp,
                           const json_t *contract_terms);

  /**
   * Mark contract terms as paid.  Needed by /history as only paid
   * contracts must be shown.
   *
   * NOTE: we can't get the list of (paid) contracts from the
   * transactions table because it lacks contract_terms plain JSON.
   * In facts, the protocol doesn't allow to store contract_terms in
   * transactions table, as /pay handler doesn't receive this data
   * (only /proposal does).
   *
   * @param cls closure
   * @param h_contract_terms hash of the contract that is now paid
   * @param merchant_pub merchant's public key
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*mark_proposal_paid)(void *cls,
                        const struct GNUNET_HashCode *h_contract_terms,
                        const struct TALER_MerchantPublicKeyP *merchant_pub);

  /**
   * Store the order ID that was used to pay for a resource within a session.
   *
   * @param cls closure
   * @param session_id session id
   * @param fulfillment_url URL that canonically identifies the resource
   *        being paid for
   * @param order_id the order ID that was used when paying for the resource URL
   * @param merchant_pub public key of the merchant, identifying the instance
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*insert_session_info)(void *cls,
                         const char *session_id,
                         const char *fulfillment_url,
                         const char *order_id,
                         const struct TALER_MerchantPublicKeyP *merchant_pub);

  /**
   * Retrieve the order ID that was used to pay for a resource within a session.
   *
   * @param cls closure
   * @param[out] order_id location to store the order ID that was used when
   *             paying for the resource URL
   * @param session_id session id
   * @param fulfillment_url URL that canonically identifies the resource
   *        being paid for
   * @param merchant_pub public key of the merchant, identifying the instance
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_session_info)(void *cls,
                       char **order_id,
                       const char *session_id,
                       const char *fulfillment_url,
                       const struct TALER_MerchantPublicKeyP *merchant_pub);

  /**
   * Retrieve proposal data given its order ID.
   *
   * @param cls closure
   * @param[out] contract_terms where to store the result
   * @param order_id order_id used to lookup.
   * @param merchant_pub instance's public key.
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_contract_terms)(void *cls,
                         json_t **contract_terms,
                         const char *order_id,
                         const struct TALER_MerchantPublicKeyP *merchant_pub);

  /**
   * Retrieve order given its order id and the instance's merchant public key.
   *
   * @param cls closure
   * @param[out] contract_terms where to store the retrieved contract terms
   * @param order id order id used to perform the lookup
   * @param merchant_pub merchant public key that identifies the instance
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_order)(void *cls,
                json_t **contract_terms,
                const char *order_id,
                const struct TALER_MerchantPublicKeyP *merchant_pub);


  /**
   * Retrieve proposal data given its hashcode
   *
   * @param cls closure
   * @param[out] contract_terms where to store the result
   * @param h_contract_terms hashcode used to lookup.
   * @param merchant_pub instance's public key.
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_contract_terms_from_hash)(
    void *cls,
    json_t **contract_terms,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_MerchantPublicKeyP *merchant_pub);


  /**
   * Retrieve paid contract terms data given its hashcode.
   *
   * @param cls closure
   * @param[out] contract_terms where to store the result
   * @param h_contract_terms hashcode used to lookup.
   * @param merchant_pub instance's public key.
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_paid_contract_terms_from_hash)(
    void *cls,
    json_t **contract_terms,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_MerchantPublicKeyP *merchant_pub);


  /**
   * Return proposals whose timestamps are younger than `date`.
   * Among those proposals, only those ones being between the
   * start-th and (start-nrows)-th record are returned.  The rows
   * are sorted having the youngest first.
   *
   * @param cls our plugin handle.
   * @param date only results younger than this date are returned.
   * @param merchant_pub instance's public key; only rows related to this
   * instance are returned.
   * @param start only rows with serial id less than start are returned.
   * @param nrows only nrows rows are returned.
   * @param past if set to #GNUNET_YES, retrieves rows older than `date`.
   * @param ascending if GNUNET_YES, then results will be sorted with youngest first.
   * This is typically used to show live updates on the merchant's backoffice
   * @param cb function to call with transaction data, can be NULL.
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_contract_terms_by_date_and_range)(
    void *cls,
    struct GNUNET_TIME_Absolute date,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    uint64_t start,
    uint64_t nrows,
    int past,
    unsigned int ascending,
    TALER_MERCHANTDB_ProposalDataCallback cb,
    void *cb_cls);

  /**
   * Lookup for a proposal, respecting the signature used by the
   * /history's db methods.
   *
   * @param cls db plugin handle
   * @param order_id order id used to search for the proposal data
   * @param merchant_pub public key of the merchant using this method
   * @param cb the callback
   * @param cb_cls closure to pass to @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_contract_terms_history)(
    void *cls,
    const char *order_id,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    TALER_MERCHANTDB_ProposalDataCallback cb,
    void *cb_cls);


  /**
   * Return proposals whose timestamp are older than `date`.
   * The rows are sorted having the youngest first.*
   *
   * @param cls our plugin handle.
   * @param date only results older than this date are returned.
   * @param merchant_pub instance's public key; only rows related to this
   * instance are returned.
   * @param nrows only nrows rows are returned.
   * @param cb function to call with transaction data, can be NULL.
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_contract_terms_by_date)(
    void *cls,
    struct GNUNET_TIME_Absolute date,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    uint64_t nrows,
    TALER_MERCHANTDB_ProposalDataCallback cb,
    void *cb_cls);


  /**
   * Insert payment confirmation from the exchange into the database.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key
   * @param coin_pub public key of the coin
   * @param exchange_url URL of the exchange that issued @a coin_pub
   * @param amount_with_fee amount the exchange will deposit for this coin
   * @param deposit_fee fee the exchange will charge for this coin
   * @param wire_fee wire fee the exchange charges
   * @param signkey_pub public key used by the exchange for @a exchange_proof
   * @param exchange_proof proof from exchange that coin was accepted
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*store_deposit)(void *cls,
                   const struct GNUNET_HashCode *h_contract_terms,
                   const struct TALER_MerchantPublicKeyP *merchant_pub,
                   const struct TALER_CoinSpendPublicKeyP *coin_pub,
                   const char *exchange_url,
                   const struct TALER_Amount *amount_with_fee,
                   const struct TALER_Amount *deposit_fee,
                   const struct TALER_Amount *refund_fee,
                   const struct TALER_Amount *wire_fee,
                   const struct TALER_ExchangePublicKeyP *signkey_pub,
                   const json_t *exchange_proof);


  /**
   * Insert mapping of @a coin_pub and @a h_contract_terms to
   * corresponding @a wtid.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param coin_pub public key of the coin
   * @param wtid identifier of the wire transfer in which the exchange
   *             send us the money for the coin deposit
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*store_coin_to_transfer)(
    void *cls,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_CoinSpendPublicKeyP *coin_pub,
    const struct TALER_WireTransferIdentifierRawP *wtid);


  /**
   * Insert wire transfer confirmation from the exchange into the database.
   *
   * @param cls closure
   * @param exchange_url from which exchange did we get the @a exchange_proof
   * @param wtid identifier of the wire transfer
   * @param execution_time when was @a wtid executed
   * @param signkey_pub public key used by the exchange for @a exchange_proof
   * @param exchange_proof proof from exchange about what the deposit was for
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*store_transfer_to_proof)(
    void *cls,
    const char *exchange_url,
    const struct TALER_WireTransferIdentifierRawP *wtid,
    struct GNUNET_TIME_Absolute execution_time,
    const struct TALER_ExchangePublicKeyP *signkey_pub,
    const json_t *exchange_proof);


  /**
   * Store information about wire fees charged by an exchange,
   * including signature (so we have proof).
   *
   * @param cls closure
   * @param exchange_pub public key of the exchange
   * @param h_wire_method hash of wire method
   * @param wire_fee wire fee charged
   * @param closing_fee closing fee charged (irrelevant for us,
   *              but needed to check signature)
   * @param start_date start of fee being used
   * @param end_date end of fee being used
   * @param exchange_sig signature of exchange over fee structure
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*store_wire_fee_by_exchange)(
    void *cls,
    const struct TALER_MasterPublicKeyP *exchange_pub,
    const struct GNUNET_HashCode *h_wire_method,
    const struct TALER_Amount *wire_fee,
    const struct TALER_Amount *closing_fee,
    struct GNUNET_TIME_Absolute start_date,
    struct GNUNET_TIME_Absolute end_date,
    const struct TALER_MasterSignatureP *exchange_sig);


  /**
   * Lookup information about coin payments by proposal data's hashcode.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key. It's AND'd with @a h_contract_terms
   *        in order to find the result.
   * @param cb function to call with payment data
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_payments)(void *cls,
                   const struct GNUNET_HashCode *h_contract_terms,
                   const struct TALER_MerchantPublicKeyP *merchant_pub,
                   TALER_MERCHANTDB_CoinDepositCallback cb,
                   void *cb_cls);


  /**
   * Lookup information about coin payments by h_contract_terms and coin.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key. It's AND'd with @a h_contract_terms
   *        in order to find the result.
   * @param coin_pub public key to use for the search
   * @param cb function to call with payment data
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_payments_by_hash_and_coin)(
    void *cls,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    const struct TALER_CoinSpendPublicKeyP *coin_pub,
    TALER_MERCHANTDB_CoinDepositCallback cb,
    void *cb_cls);


  /**
   * Lookup information about a transfer by @a h_contract_terms.  Note
   * that in theory there could be multiple wire transfers for a
   * single @a h_contract_terms, as the transaction may have involved
   * multiple coins and the coins may be spread over different wire
   * transfers.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param cb function to call with transfer data
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_transfers_by_hash)(void *cls,
                            const struct GNUNET_HashCode *h_contract_terms,
                            TALER_MERCHANTDB_TransferCallback cb,
                            void *cb_cls);


  /**
   * Lookup information about a coin deposits by @a wtid.
   *
   * @param cls closure
   * @param wtid wire transfer identifier to find matching transactions for
   * @param cb function to call with payment data
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_deposits_by_wtid)(void *cls,
                           const struct TALER_WireTransferIdentifierRawP *wtid,
                           TALER_MERCHANTDB_CoinDepositCallback cb,
                           void *cb_cls);


  /**
   * Lookup proof information about a wire transfer.
   *
   * @param cls closure
   * @param exchange_url from which exchange are we looking for proof
   * @param wtid wire transfer identifier for the search
   * @param cb function to call with proof data
   * @param cb_cls closure for @a cb
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*find_proof_by_wtid)(void *cls,
                        const char *exchange_url,
                        const struct TALER_WireTransferIdentifierRawP *wtid,
                        TALER_MERCHANTDB_ProofCallback cb,
                        void *cb_cls);


  /**
   * Obtain information about wire fees charged by an exchange,
   * including signature (so we have proof).
   *
   * @param cls closure
   * @param exchange_pub public key of the exchange
   * @param h_wire_method hash of wire method
   * @param contract_date date of the contract to use for the lookup
   * @param[out] wire_fee wire fee charged
   * @param[out] closing_fee closing fee charged (irrelevant for us,
   *              but needed to check signature)
   * @param[out] start_date start of fee being used
   * @param[out] end_date end of fee being used
   * @param[out] exchange_sig signature of exchange over fee structure
   * @return transaction status code
   */
  enum GNUNET_DB_QueryStatus
  (*lookup_wire_fee)(void *cls,
                     const struct TALER_MasterPublicKeyP *exchange_pub,
                     const struct GNUNET_HashCode *h_wire_method,
                     struct GNUNET_TIME_Absolute contract_date,
                     struct TALER_Amount *wire_fee,
                     struct TALER_Amount *closing_fee,
                     struct GNUNET_TIME_Absolute *start_date,
                     struct GNUNET_TIME_Absolute *end_date,
                     struct TALER_MasterSignatureP *exchange_sig);


  /**
   * Function called when some backoffice staff decides to award or
   * increase the refund on an existing contract.  This function
   * MUST be called from within a transaction scope setup by the
   * caller as it executes multiple SQL statements (NT).
   *
   * @param cls closure
   * @param merchant_pub merchant's instance public key
   * @param h_contract_terms
   * @param merchant_pub merchant's instance public key
   * @param refund maximum refund to return to the customer for this contract
   * @param reason 0-terminated UTF-8 string giving the reason why the customer
   *               got a refund (free form, business-specific)
   * @return transaction status
   *        #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if @a refund is ABOVE the amount we
   *        were originally paid and thus the transaction failed;
   *        #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT if the request is valid,
   *        regardless of whether it actually increased the refund beyond
   *        what was already refunded (idempotency!)
   */
  enum GNUNET_DB_QueryStatus
  (*increase_refund_for_contract_NT)(
    void *cls,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    const struct TALER_Amount *refund,
    const char *reason);


  /**
   * Obtain refunds associated with a contract.
   *
   * @param cls closure, typically a connection to the db
   * @param merchant_pub public key of the merchant instance
   * @param h_contract_terms hash code of the contract
   * @param rc function to call for each coin on which there is a refund
   * @param rc_cls closure for @a rc
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_refunds_from_contract_terms_hash)(
    void *cls,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    const struct GNUNET_HashCode *h_contract_terms,
    TALER_MERCHANTDB_RefundCallback rc,
    void *rc_cls);


  /**
   * Obtain refund proofs associated with a refund operation on a
   * coin.
   *
   * @param cls closure, typically a connection to the db
   * @param merchant_pub public key of the merchant instance
   * @param h_contract_terms hash code of the contract
   * @param coin_pub public key of the coin
   * @param rtransaction_id identificator of the refund
   * @param[out] exchange_pub public key of the exchange affirming the refund
   * @param[out] exchange_sig signature of the exchange affirming the refund
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*get_refund_proof)(
    void *cls,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_CoinSpendPublicKeyP *coin_pub,
    uint64_t rtransaction_id,
    struct TALER_ExchangePublicKeyP *exchange_pub,
    struct TALER_ExchangeSignatureP *exchange_sig);


  /**
   * Store refund proofs associated with a refund operation on a
   * coin.
   *
   * @param cls closure, typically a connection to the db
   * @param merchant_pub public key of the merchant instance
   * @param h_contract_terms hash code of the contract
   * @param coin_pub public key of the coin
   * @param rtransaction_id identificator of the refund
   * @param exchange_pub public key of the exchange affirming the refund
   * @param exchange_sig signature of the exchange affirming the refund
   * @return transaction status
   */
  enum GNUNET_DB_QueryStatus
  (*put_refund_proof)(
    void *cls,
    const struct TALER_MerchantPublicKeyP *merchant_pub,
    const struct GNUNET_HashCode *h_contract_terms,
    const struct TALER_CoinSpendPublicKeyP *coin_pub,
    uint64_t rtransaction_id,
    const struct TALER_ExchangePublicKeyP *exchange_pub,
    const struct TALER_ExchangeSignatureP *exchange_sig);


  /**
   * Add @a credit to a reserve to be used for tipping.  Note that
   * this function does not actually perform any wire transfers to
   * credit the reserve, it merely tells the merchant backend that
   * a reserve was topped up.  This has to happen before tips can be
   * authorized.
   *
   * @param cls closure, typically a connection to the db
   * @param reserve_priv which reserve is topped up or created
   * @param credit_uuid unique identifier for the credit operation
   * @param credit how much money was added to the reserve
   * @param expiration when does the reserve expire?
   * @return transaction status, usually
   *      #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT for success
   *      #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if @a credit_uuid already known
   */
  enum GNUNET_DB_QueryStatus
  (*enable_tip_reserve_TR)(void *cls,
                           const struct TALER_ReservePrivateKeyP *reserve_priv,
                           const struct GNUNET_HashCode *credit_uuid,
                           const struct TALER_Amount *credit,
                           struct GNUNET_TIME_Absolute expiration);


  /**
   * Authorize a tip over @a amount from reserve @a reserve_priv.  Remember
   * the authorization under @a tip_id for later, together with the
   * @a justification.
   *
   * @param cls closure, typically a connection to the db
   * @param justification why was the tip approved
   * @param extra extra data that will be given to the customer's wallet
   * @param amount how high is the tip (with fees)
   * @param reserve_priv which reserve is debited
   * @param exchange_url which exchange manages the tip
   * @param[out] expiration set to when the tip expires
   * @param[out] tip_id set to the unique ID for the tip
   * @return transaction status,
   *      #TALER_EC_TIP_AUTHORIZE_RESERVE_EXPIRED if the reserve is known but has expired
   *      #TALER_EC_TIP_AUTHORIZE_RESERVE_UNKNOWN if the reserve is not known
   *      #TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS if the reserve has insufficient funds left
   *      #TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR on hard DB errors
   *      #TALER_EC_TIP_AUTHORIZE_DB_SOFT_ERROR on soft DB errors (client should retry)
   *      #TALER_EC_NONE upon success
   */
  enum TALER_ErrorCode
  (*authorize_tip_TR)(void *cls,
                      const char *justification,
                      const json_t *extra,
                      const struct TALER_Amount *amount,
                      const struct TALER_ReservePrivateKeyP *reserve_priv,
                      const char *exchange_url,
                      struct GNUNET_TIME_Absolute *expiration,
                      struct GNUNET_HashCode *tip_id);

  /**
   * Get the total amount of authorized tips for a tipping reserve.
   *
   * @param cls closure, typically a connection to the db
   * @param reserve_priv which reserve to check
   * @param[out] authorzed_amount amount we've authorized so far for tips
   * @return transaction status, usually
   *      #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT for success
   *      #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if the reserve_priv
   *      does not identify a known tipping reserve
   */
  enum GNUNET_DB_QueryStatus
  (*get_authorized_tip_amount)(void *cls,
                               const struct
                               TALER_ReservePrivateKeyP *reserve_priv,
                               struct TALER_Amount *authorized_amount);


  /**
   * Find out tip authorization details associated with @a tip_id
   *
   * @param cls closure, typically a connection to the d
   * @param tip_id the unique ID for the tip
   * @param[out] exchange_url set to the URL of the exchange (unless NULL)
   * @param[out] extra extra data to pass to the wallet
   * @param[out] amount set to the authorized amount (unless NULL)
   * @param[out] amount_left set to the amount left (unless NULL)
   * @param[out] timestamp set to the timestamp of the tip authorization (unless NULL)
   * @return transaction status, usually
   *      #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT for success
   *      #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if @a credit_uuid already known
   */
  enum GNUNET_DB_QueryStatus
  (*lookup_tip_by_id)(void *cls,
                      const struct GNUNET_HashCode *tip_id,
                      char **exchange_url,
                      json_t **extra,
                      struct TALER_Amount *amount,
                      struct TALER_Amount *amount_left,
                      struct GNUNET_TIME_Absolute *timestamp);


  /**
   * Pickup a tip over @a amount using pickup id @a pickup_id.
   *
   * @param cls closure, typically a connection to the db
   * @param amount how high is the amount picked up (with fees)
   * @param tip_id the unique ID from the tip authorization
   * @param pickup_id the unique ID identifying the pick up operation
   *        (to allow replays, hash over the coin envelope and denomination key)
   * @param[out] reserve_priv which reserve key to use to sign
   * @return taler error code
   *      #TALER_EC_TIP_PICKUP_ID_UNKNOWN if @a tip_id is unknown
   *      #TALER_EC_TIP_PICKUP_NO_FUNDS if @a tip_id has insufficient funds left
   *      #TALER_EC_TIP_PICKUP_DB_ERROR on database errors
   *      #TALER_EC_NONE upon success (@a reserve_priv was set)
   */
  enum TALER_ErrorCode
  (*pickup_tip_TR)(void *cls,
                   const struct TALER_Amount *amount,
                   const struct GNUNET_HashCode *tip_id,
                   const struct GNUNET_HashCode *pickup_id,
                   struct TALER_ReservePrivateKeyP *reserve_priv);


};

#endif
