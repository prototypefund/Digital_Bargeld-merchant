/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 INRIA

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
#include <jansson.h>

/**
 * Handle to interact with the database.
 */
struct TALER_MERCHANTDB_Plugin;


/**
 * Tipically called by `find_contract_terms_by_date`.
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
 * @param exchange_uri URI of the exchange
 * @param h_contract_terms proposal data's hashcode
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
typedef void
(*TALER_MERCHANTDB_TransactionCallback)(void *cls,
					const struct TALER_MerchantPublicKeyP *merchant_pub,
                                        const char *exchange_uri,
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
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted,
 *        matches the `interface DepositSuccess` of the documentation.
 */
typedef void
(*TALER_MERCHANTDB_CoinDepositCallback)(void *cls,
                                        const struct GNUNET_HashCode *h_contract_terms,
                                        const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                        const struct TALER_Amount *amount_with_fee,
                                        const struct TALER_Amount *deposit_fee,
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
(*TALER_MERCHANTDB_TransferCallback)(void *cls,
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
 * Handle to interact with the database.
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
   * Initialize merchant tables
   *
   * @param cls closure
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*initialize) (void *cls);


  /**
   * Insert proposal data into db; the routine will internally hash and
   * insert the proposal data's hashcode into the same row.
   *
   * @param cls closure
   * @param order_id alphanumeric string that uniquely identifies the proposal
   * @param merchant_pub merchant's public key
   * @param timestamp timestamp of this proposal data
   * @param contract_terms proposal data to store
   * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
   */
  int
  (*insert_contract_terms) (void *cls,
                           const char *order_id,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           struct GNUNET_TIME_Absolute timestamp,
                           const json_t *contract_terms);

  /**
   * Retrieve proposal data given its order ID.
   *
   * @param cls closure
   * @param[out] contract_terms where to store the result
   * @param order_id order_id used to lookup.
   * @param merchant_pub instance's public key.
   * @return #GNUNET_OK on success, #GNUNET_NO if no contract is
   * found, #GNUNET_SYSERR upon error
   */
  int
  (*find_contract_terms) (void *cls,
                         json_t **contract_terms,
                         const char *order_id,
                         const struct TALER_MerchantPublicKeyP *merchant_pub);


  /**
   * Retrieve proposal data given its hashcode
   *
   * @param cls closure
   * @param contract_terms where to store the result
   * @param h_contract_terms hashcode used to lookup.
   * @param merchant_pub instance's public key.
   * @return #GNUNET_OK on success, #GNUNET_NO if no contract is
   * found, #GNUNET_SYSERR upon error
   */
  int
  (*find_contract_terms_from_hash) (void *cls,
                                   json_t **contract_terms,
                                   const struct GNUNET_HashCode *h_contract_terms,
                                   const struct TALER_MerchantPublicKeyP *merchant_pub);



  /**
   * Return proposals whose timestamp are older than `date`.
   * Among those proposals, only those ones being between the
   * start-th and (start-nrows)-th record are returned.  The rows
   * are sorted having the youngest first.
   *
   * @param cls our plugin handle.
   * @param date only results older than this date are returned.
   * @param merchant_pub instance's public key; only rows related to this
   * instance are returned.
   * @param start only rows with serial id less than start are returned.
   * @param nrows only nrows rows are returned.
   * @param future if set to GNUNET_YES, retrieves rows younger than `date`.
   * This is tipically used to show live updates on the merchant's backoffice
   * @param cb function to call with transaction data, can be NULL.
   * @param cb_cls closure for @a cb
   * @return numer of found tuples, #GNUNET_SYSERR upon error
   */
  int
  (*find_contract_terms_by_date_and_range) (void *cls,
                                           struct GNUNET_TIME_Absolute date,
                                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                                           unsigned int start,
                                           unsigned int nrows,
                                           unsigned int future,
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
   * @param cb_cls closure to pass to the callback
   * @return GNUNET_YES, GNUNET_NO, GNUNET_SYSERR according to the
   * query being successful, unsuccessful, or generated errors.
   */
  int
  (*find_contract_terms_history) (void *cls,
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
   * @return numer of found tuples, #GNUNET_SYSERR upon error
   */
  int
  (*find_contract_terms_by_date) (void *cls,
                                 struct GNUNET_TIME_Absolute date,
                                 const struct TALER_MerchantPublicKeyP *merchant_pub,
                                 unsigned int nrows,
                                 TALER_MERCHANTDB_ProposalDataCallback cb,
                                 void *cb_cls);


  /**
   * Insert transaction data into the database.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key
   * @param exchange_uri URI of the exchange
   * @param h_wire hash of our wire details
   * @param timestamp time of the confirmation
   * @param refund refund deadline
   * @param total_amount total amount we receive for the contract after fees
   * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
   */
  int
  (*store_transaction) (void *cls,
                        const struct GNUNET_HashCode *h_contract_terms,
			const struct TALER_MerchantPublicKeyP *merchant_pub,
                        const char *exchange_uri,
                        const struct GNUNET_HashCode *h_wire,
                        struct GNUNET_TIME_Absolute timestamp,
                        struct GNUNET_TIME_Absolute refund,
                        const struct TALER_Amount *total_amount);


  /**
   * Insert payment confirmation from the exchange into the database.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key
   * @param coin_pub public key of the coin
   * @param amount_with_fee amount the exchange will deposit for this coin
   * @param deposit_fee fee the exchange will charge for this coin
   * @param signkey_pub public key used by the exchange for @a exchange_proof
   * @param exchange_proof proof from exchange that coin was accepted
   * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
   */
  int
  (*store_deposit) (void *cls,
                    const struct GNUNET_HashCode *h_contract_terms,
                    const struct TALER_MerchantPublicKeyP *merchant_pub,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const struct TALER_Amount *amount_with_fee,
                    const struct TALER_Amount *deposit_fee,
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
   * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
   */
  int
  (*store_coin_to_transfer) (void *cls,
                             const struct GNUNET_HashCode *h_contract_terms,
                             const struct TALER_CoinSpendPublicKeyP *coin_pub,
                             const struct TALER_WireTransferIdentifierRawP *wtid);


  /**
   * Insert wire transfer confirmation from the exchange into the database.
   *
   * @param cls closure
   * @param exchange_uri from which exchange did we get the @a exchange_proof
   * @param wtid identifier of the wire transfer
   * @param execution_time when was @a wtid executed
   * @param signkey_pub public key used by the exchange for @a exchange_proof
   * @param exchange_proof proof from exchange about what the deposit was for
   * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
   */
  int
  (*store_transfer_to_proof) (void *cls,
                              const char *exchange_uri,
                              const struct TALER_WireTransferIdentifierRawP *wtid,
                              struct GNUNET_TIME_Absolute execution_time,
                              const struct TALER_ExchangePublicKeyP *signkey_pub,
                              const json_t *exchange_proof);


  /**
   * Find information about a transaction.
   *
   * @param cls our plugin handle
   * @param date limit to transactions' age
   * @param cb function to call with transaction data
   * @param cb_cls closure for @a cb
   * @return #GNUNET_OK if found, #GNUNET_NO if not, #GNUNET_SYSERR
   *         upon error
   */
  int
  (*find_transactions_by_date) (void *cls,
                                struct GNUNET_TIME_Absolute date,
                                TALER_MERCHANTDB_TransactionCallback cb,
                                void *cb_cls);

  /**
   * Find information about a transaction.
   *
   * @param cls our plugin handle
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key.
   * @param cb function to call with transaction data
   * @param cb_cls closure for @a cb
   * @return number of found tuples, #GNUNET_SYSERR upon error
   */
  int
  (*find_transaction) (void *cls,
                       const struct GNUNET_HashCode *h_contract_terms,
		       const struct TALER_MerchantPublicKeyP *merchant_pub,
                       TALER_MERCHANTDB_TransactionCallback cb,
                       void *cb_cls);


  /**
   * Lookup information about coin payments by proposal data's hashcode.
   *
   * @param cls closure
   * @param h_contract_terms proposal data's hashcode
   * @param merchant_pub merchant's public key. It's AND'd with @a h_contract_terms
   *        in order to find the result.
   * @param cb function to call with payment data
   * @param cb_cls closure for @a cb
   * @return #GNUNET_OK on success, #GNUNET_NO if h_contract_terms is unknown,
   *         #GNUNET_SYSERR on hard errors
   */
  int
  (*find_payments) (void *cls,
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
   * @return #GNUNET_OK on success, #GNUNET_NO if h_contract_terms is unknown,
   *         #GNUNET_SYSERR on hard errors
   */
  int
  (*find_payments_by_hash_and_coin) (void *cls,
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
   * @return #GNUNET_OK on success, #GNUNET_NO if h_contract_terms is unknown,
   *         #GNUNET_SYSERR on hard errors
   */
  int
  (*find_transfers_by_hash) (void *cls,
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
   * @return #GNUNET_OK on success, #GNUNET_NO if h_contract_terms is unknown,
   *         #GNUNET_SYSERR on hard errors
   */
  int
  (*find_deposits_by_wtid) (void *cls,
                            const struct TALER_WireTransferIdentifierRawP *wtid,
                            TALER_MERCHANTDB_CoinDepositCallback cb,
                            void *cb_cls);


  /**
   * Lookup proof information about a wire transfer.
   *
   * @param cls closure
   * @param exchange_uri from which exchange are we looking for proof
   * @param wtid wire transfer identifier for the search
   * @param cb function to call with proof data
   * @param cb_cls closure for @a cb
   * @return #GNUNET_OK on success, #GNUNET_NO if h_contract_terms is unknown,
   *         #GNUNET_SYSERR on hard errors
   */
  int
  (*find_proof_by_wtid) (void *cls,
                         const char *exchange_uri,
                         const struct TALER_WireTransferIdentifierRawP *wtid,
                         TALER_MERCHANTDB_ProofCallback cb,
                         void *cb_cls);
};


#endif
