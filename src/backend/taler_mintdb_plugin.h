/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_mintdb_plugin.h
 * @brief Low-level (statement-level) database access for the mint
 * @author Florian Dold
 * @author Christian Grothoff
 */
#ifndef TALER_MINTDB_PLUGIN_H
#define TALER_MINTDB_PLUGIN_H

#include <gnunet/gnunet_util_lib.h>
#include "taler_mintdb_lib.h"


/**
 * @brief Information we keep on bank transfer(s) that established a reserve.
 */
struct TALER_MINTDB_BankTransfer
{

  /**
   * Public key of the reserve that was filled.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * Amount that was transferred to the mint.
   */
  struct TALER_Amount amount;

  /**
   * When did the mint receive the incoming transaction?
   * (This is the execution date of the mint's database,
   * the execution date of the bank should be in @e wire).
   */
  struct GNUNET_TIME_Absolute execution_date;

  /**
   * Detailed wire information about the transaction.
   */
  json_t *wire;

};


/**
 * @brief A summary of a Reserve
 */
struct TALER_MINTDB_Reserve
{
  /**
   * The reserve's public key.  This uniquely identifies the reserve
   */
  struct TALER_ReservePublicKeyP pub;

  /**
   * The balance amount existing in the reserve
   */
  struct TALER_Amount balance;

  /**
   * The expiration date of this reserve
   */
  struct GNUNET_TIME_Absolute expiry;
};


/**
 * @brief Information we keep for a withdrawn coin to reproduce
 * the /withdraw operation if needed, and to have proof
 * that a reserve was drained by this amount.
 */
struct TALER_MINTDB_CollectableBlindcoin
{

  /**
   * Our signature over the (blinded) coin.
   */
  struct TALER_DenominationSignature sig;

  /**
   * Denomination key (which coin was generated).
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Value of the coin being minted (matching the denomination key)
   * plus the transaction fee.  We include this in what is being
   * signed so that we can verify a reserve's remaining total balance
   * without needing to access the respective denomination key
   * information each time.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Withdrawl fee charged by the mint.  This must match the Mint's
   * denomination key's withdrawl fee.  If the client puts in an
   * invalid withdrawl fee (too high or too low) that does not match
   * the Mint's denomination key, the withdraw operation is invalid
   * and will be rejected by the mint.  The @e amount_with_fee minus
   * the @e withdraw_fee is must match the value of the generated
   * coin.  We include this in what is being signed so that we can
   * verify a mint's accounting without needing to access the
   * respective denomination key information each time.
   */
  struct TALER_Amount withdraw_fee;

  /**
   * Public key of the reserve that was drained.
   */
  struct TALER_ReservePublicKeyP reserve_pub;

  /**
   * Hash over the blinded message, needed to verify
   * the @e reserve_sig.
   */
  struct GNUNET_HashCode h_coin_envelope;

  /**
   * Signature confirming the withdrawl, matching @e reserve_pub,
   * @e denom_pub and @e h_coin_envelope.
   */
  struct TALER_ReserveSignatureP reserve_sig;
};



/**
 * @brief Types of operations on a reserved.
 */
enum TALER_MINTDB_ReserveOperation
{
  /**
   * Money was deposited into the reserve via a bank transfer.
   */
  TALER_MINTDB_RO_BANK_TO_MINT = 0,

  /**
   * A Coin was withdrawn from the reserve using /withdraw.
   */
  TALER_MINTDB_RO_WITHDRAW_COIN = 1
};


/**
 * @brief Reserve history as a linked list.  Lists all of the transactions
 * associated with this reserve (such as the bank transfers that
 * established the reserve and all /withdraw operations we have done
 * since).
 */
struct TALER_MINTDB_ReserveHistory
{

  /**
   * Next entry in the reserve history.
   */
  struct TALER_MINTDB_ReserveHistory *next;

  /**
   * Type of the event, determins @e details.
   */
  enum TALER_MINTDB_ReserveOperation type;

  /**
   * Details of the operation, depending on @e type.
   */
  union
  {

    /**
     * Details about a bank transfer to the mint.
     */
    struct TALER_MINTDB_BankTransfer *bank;

    /**
     * Details about a /withdraw operation.
     */
    struct TALER_MINTDB_CollectableBlindcoin *withdraw;

  } details;

};


/**
 * @brief Specification for a /deposit operation.  The combination of
 * the coin's public key, the merchant's public key and the
 * transaction ID must be unique.  While a coin can (theoretically) be
 * deposited at the same merchant twice (with partial spending), the
 * merchant must either use a different public key or a different
 * transaction ID for the two transactions.  The same coin must not
 * be used twice at the same merchant for the same transaction
 * (as determined by transaction ID).  (Note: we might want to
 * fix #3819 and include at least h_contract as well.)
 */
struct TALER_MINTDB_Deposit
{
  /**
   * Information about the coin that is being deposited.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * ECDSA signature affirming that the customer intends
   * this coin to be deposited at the merchant identified
   * by @e h_wire in relation to the contract identified
   * by @e h_contract.
   */
  struct TALER_CoinSpendSignatureP csig;

  /**
   * Public key of the merchant.  Enables later identification
   * of the merchant in case of a need to rollback transactions.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Hash over the contract between merchant and customer
   * (remains unknown to the Mint).
   */
  struct GNUNET_HashCode h_contract;

  /**
   * Hash of the (canonical) representation of @e wire, used
   * to check the signature on the request.  Generated by
   * the mint from the detailed wire data provided by the
   * merchant.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Detailed wire information for executing the transaction.
   */
  json_t *wire;

  /**
   * Merchant-generated transaction ID to detect duplicate
   * transactions.
   */
  uint64_t transaction_id;

  /**
   * Time when this request was generated.  Used, for example, to
   * assess when (roughly) the income was achieved for tax purposes.
   * Note that the Mint will only check that the timestamp is not "too
   * far" into the future (i.e. several days).  The fact that the
   * timestamp falls within the validity period of the coin's
   * denomination key is irrelevant for the validity of the deposit
   * request, as obviously the customer and merchant could conspire to
   * set any timestamp.  Also, the Mint must accept very old deposit
   * requests, as the merchant might have been unable to transmit the
   * deposit request in a timely fashion (so back-dating is not
   * prevented).
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * How much time does the merchant have to issue a refund request?
   * Zero if refunds are not allowed.  After this time, the coin
   * cannot be refunded.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * Fraction of the coin's remaining value to be deposited, including
   * depositing fee (if any).  The coin is identified by @e coin_pub.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Depositing fee.
   */
  struct TALER_Amount deposit_fee;

};


/**
 * @brief Global information for a refreshing session.  Includes
 * dimensions of the operation, security parameters and
 * client signatures from "/refresh/melt" and "/refresh/commit".
 */
struct TALER_MINTDB_RefreshSession
{

  /**
   * Number of coins we are melting.
   */
  uint16_t num_oldcoins;

  /**
   * Number of new coins we are creating.
   */
  uint16_t num_newcoins;

  /**
   * Index (smaller #TALER_CNC_KAPPA) which the mint has chosen to not
   * have revealed during cut and choose.
   */
  uint16_t noreveal_index;

};


/**
 * @brief Specification for coin in a /refresh/melt operation.
 */
struct TALER_MINTDB_RefreshMelt
{
  /**
   * Information about the coin that is being melted.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * Signature over the melting operation.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Hash of the refresh session this coin is melted into.
   */
  struct GNUNET_HashCode session_hash;

  /**
   * How much value is being melted?  This amount includes the fees,
   * so the final amount contributed to the melt is this value minus
   * the fee for melting the coin.  We include the fee in what is
   * being signed so that we can verify a reserve's remaining total
   * balance without needing to access the respective denomination key
   * information each time.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Melting fee charged by the mint.  This must match the Mint's
   * denomination key's melting fee.  If the client puts in an invalid
   * melting fee (too high or too low) that does not match the Mint's
   * denomination key, the melting operation is invalid and will be
   * rejected by the mint.  The @e amount_with_fee minus the @e
   * melt_fee is the amount that will be credited to the melting
   * session.
   */
  struct TALER_Amount melt_fee;

};


/**
 * @brief We have as many `struct TALER_MINTDB_RefreshCommitCoin` as there are new
 * coins being created by the refresh (for each of the #TALER_CNC_KAPPA
 * sets).  These are the coins we ask the mint to sign if the
 * respective set is selected.
 */
struct TALER_MINTDB_RefreshCommitCoin
{

  /**
   * Encrypted data allowing those able to decrypt it to derive
   * the private keys of the new coins created by the refresh.
   */
  struct TALER_RefreshLinkEncrypted *refresh_link;

  /**
   * Blinded message to be signed (in envelope), with @e coin_env_size bytes.
   */
  char *coin_ev;

  /**
   * Number of bytes in @e coin_ev.
   */
  size_t coin_ev_size;

};


GNUNET_NETWORK_STRUCT_BEGIN

/**
 * @brief For each (old) coin being melted, we have a `struct
 * RefreshCommitLinkP` that allows the user to find the shared secret
 * to decrypt the respective refresh links for the new coins in the
 * `struct TALER_MINTDB_RefreshCommitCoin`.
 */
struct TALER_MINTDB_RefreshCommitLinkP
{
  /**
   * Transfer public key, used to decrypt the @e shared_secret_enc
   * in combintation with the corresponding private key of the
   * coin.
   */
  struct TALER_TransferPublicKeyP transfer_pub;

  /**
   * Encrypted shared secret to decrypt the link.
   */
  struct TALER_EncryptedLinkSecretP shared_secret_enc;
};

GNUNET_NETWORK_STRUCT_END



/**
 * @brief Linked list of refresh information linked to a coin.
 */
struct TALER_MINTDB_LinkDataList
{
  /**
   * Information is stored in a NULL-terminated linked list.
   */
  struct TALER_MINTDB_LinkDataList *next;

  /**
   * Link data, used to recover the private key of the coin
   * by the owner of the old coin.
   */
  struct TALER_RefreshLinkEncrypted *link_data_enc;

  /**
   * Denomination public key, determines the value of the coin.
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Signature over the blinded envelope.
   */
  struct TALER_DenominationSignature ev_sig;
};


/**
 * @brief Specification for a /lock operation.
 */
struct TALER_MINTDB_LockOperation
{
  /**
   * Information about the coin that is being locked.
   */
  struct TALER_CoinPublicInfo coin;

  /**
   * Signature over the locking operation.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * How much value is being locked?
   */
  struct TALER_Amount amount;

  // FIXME: more needed...
};


/**
 * @brief Enumeration to classify the different types of transactions
 * that can be done with a coin.
 */
enum TALER_MINTDB_TransactionType
{
  /**
   * /deposit operation.
   */
  TALER_MINTDB_TT_DEPOSIT = 0,

  /**
   * /refresh/melt operation.
   */
  TALER_MINTDB_TT_REFRESH_MELT = 1,

  /**
   * /lock operation.
   */
  TALER_MINTDB_TT_LOCK = 2
};


/**
 * @brief List of transactions we performed for a particular coin.
 */
struct TALER_MINTDB_TransactionList
{

  /**
   * Next pointer in the NULL-terminated linked list.
   */
  struct TALER_MINTDB_TransactionList *next;

  /**
   * Type of the transaction, determines what is stored in @e details.
   */
  enum TALER_MINTDB_TransactionType type;

  /**
   * Details about the transaction, depending on @e type.
   */
  union
  {

    /**
     * Details if transaction was a /deposit operation.
     */
    struct TALER_MINTDB_Deposit *deposit;

    /**
     * Details if transaction was a /refresh/melt operation.
     */
    struct TALER_MINTDB_RefreshMelt *melt;

    /**
     * Details if transaction was a /lock operation.
     */
    struct TALER_MINTDB_LockOperation *lock;

  } details;

};


/**
 * @brief All of the information from a /refresh/melt commitment.
 */
struct TALER_MINTDB_MeltCommitment
{

  /**
   * Number of coins we are melting.
   */
  uint16_t num_oldcoins;

  /**
   * Number of new coins we are creating.
   */
  uint16_t num_newcoins;

  /**
   * Array of @e num_oldcoins melt operation details.
   */
  struct TALER_MINTDB_RefreshMelt *melts;

  /**
   * Array of @e num_newcoins denomination keys
   */
  struct TALER_DenominationPublicKey *denom_pubs;

  /**
   * 2D-Array of #TALER_CNC_KAPPA and @e num_newcoins commitments.
   */
  struct TALER_MINTDB_RefreshCommitCoin *commit_coins[TALER_CNC_KAPPA];

  /**
   * 2D-Array of #TALER_CNC_KAPPA and @e new_oldcoins links.
   */
  struct TALER_MINTDB_RefreshCommitLinkP *commit_links[TALER_CNC_KAPPA];
};


/**
 * @brief Handle for a database session (per-thread, for transactions).
 */
struct TALER_MINTDB_Session;


/**
 * Function called with the session hashes and transfer secret
 * information for a given coin.
 *
 * @param cls closure
 * @param session_hash a session the coin was melted in
 * @param transfer_pub public transfer key for the session
 * @param shared_secret_enc set to shared secret for the session
 */
typedef void
(*TALER_MINTDB_TransferDataCallback)(void *cls,
                                     const struct GNUNET_HashCode *session_hash,
                                     const struct TALER_TransferPublicKeyP *transfer_pub,
                                     const struct TALER_EncryptedLinkSecretP *shared_secret_enc);


/**
 * @brief The plugin API, returned from the plugin's "init" function.
 * The argument given to "init" is simply a configuration handle.
 */
struct TALER_MINTDB_Plugin
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
   * Get the thread-local database-handle.
   * Connect to the db if the connection does not exist yet.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param temporary #GNUNET_YES to use a temporary schema; #GNUNET_NO to use the
   *        database default one
   * @param the database connection, or NULL on error
   */
  struct TALER_MINTDB_Session *
  (*get_session) (void *cls,
                  int temporary);


  /**
   * Drop the temporary taler schema.  This is only useful for testcases.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*drop_temporary) (void *cls,
                     struct TALER_MINTDB_Session *db);


  /**
   * Create the necessary tables if they are not present
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param temporary should we use a temporary schema
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*create_tables) (void *cls,
                    int temporary);


  /**
   * Start a transaction.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param session connection to use
   * @return #GNUNET_OK on success
   */
  int
  (*start) (void *cls,
            struct TALER_MINTDB_Session *session);


  /**
   * Commit a transaction.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion connection to use
   * @return #GNUNET_OK on success
   */
  int
  (*commit) (void *cls,
             struct TALER_MINTDB_Session *sesssion);


  /**
   * Abort/rollback a transaction.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion connection to use
   */
  void
  (*rollback) (void *cls,
               struct TALER_MINTDB_Session *sesssion);


  /**
   * Insert information about a denomination key and in particular
   * the properties (value, fees, expiration times) the coins signed
   * with this key have.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion connection to use
   * @param denom_pub the public key used for signing coins of this denomination
   * @param issue issuing information with value, fees and other info about the coin
   * @return #GNUNET_OK on success; #GNUNET_SYSERR on failure
   */
  int
  (*insert_denomination_info) (void *cls,
                               struct TALER_MINTDB_Session *session,
                               const struct TALER_DenominationPublicKey *denom_pub,
                               const struct TALER_MINTDB_DenominationKeyInformationP *issue);


  /**
   * Fetch information about a denomination key.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion connection to use
   * @param denom_pub the public key used for signing coins of this denomination
   * @param[out] issue set to issue information with value, fees and other info about the coin, can be NULL
   * @return #GNUNET_OK on success; #GNUNET_NO if no record was found, #GNUNET_SYSERR on failure
   */
  int
  (*get_denomination_info) (void *cls,
                            struct TALER_MINTDB_Session *session,
                            const struct TALER_DenominationPublicKey *denom_pub,
                            struct TALER_MINTDB_DenominationKeyInformationP *issue);


  /**
   * Get the summary of a reserve.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param db the database connection handle
   * @param[in,out] reserve the reserve data.  The public key of the reserve should be set
   *          in this structure; it is used to query the database.  The balance
   *          and expiration are then filled accordingly.
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*reserve_get) (void *cls,
                  struct TALER_MINTDB_Session *db,
                  struct TALER_MINTDB_Reserve *reserve);


  /**
   * Insert a incoming transaction into reserves.  New reserves are
   * also created through this function.  Note that this API call
   * starts (and stops) its own transaction scope (so the application
   * must not do so).
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param db the database connection handle
   * @param reserve_pub public key of the reserve
   * @param balance the amount that has to be added to the reserve
   * @param execution_time when was the amount added
   * @param details bank transaction details justifying the increment,
   *        must be unique for each incoming transaction
   * @return #GNUNET_OK upon success; #GNUNET_NO if the given
   *         @a details are already known for this @a reserve_pub,
   *         #GNUNET_SYSERR upon failures (DB error, incompatible currency)
   */
  int
  (*reserves_in_insert) (void *cls,
                         struct TALER_MINTDB_Session *db,
                         const struct TALER_ReservePublicKeyP *reserve_pub,
                         const struct TALER_Amount *balance,
                         struct GNUNET_TIME_Absolute execution_time,
                         const json_t *details);


  /**
   * Locate the response for a /withdraw request under the
   * key of the hash of the blinded message.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param h_blind hash of the blinded coin to be signed (will match
   *                `h_coin_envelope` in the @a collectable to be returned)
   * @param collectable corresponding collectable coin (blind signature)
   *                    if a coin is found
   * @return #GNUNET_SYSERR on internal error
   *         #GNUNET_NO if the collectable was not found
   *         #GNUNET_YES on success
   */
  int
  (*get_withdraw_info) (void *cls,
                        struct TALER_MINTDB_Session *sesssion,
                        const struct GNUNET_HashCode *h_blind,
                        struct TALER_MINTDB_CollectableBlindcoin *collectable);


  /**
   * Store collectable bit coin under the corresponding
   * hash of the blinded message.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param collectable corresponding collectable coin (blind signature)
   *                    if a coin is found
   * @return #GNUNET_SYSERR on internal error
   *         #GNUNET_NO if the collectable was not found
   *         #GNUNET_YES on success
   */
  int
  (*insert_withdraw_info) (void *cls,
                           struct TALER_MINTDB_Session *sesssion,
                           const struct TALER_MINTDB_CollectableBlindcoin *collectable);


  /**
   * Get all of the transaction history associated with the specified
   * reserve.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion connection to use
   * @param reserve_pub public key of the reserve
   * @return known transaction history (NULL if reserve is unknown)
   */
  struct TALER_MINTDB_ReserveHistory *
  (*get_reserve_history) (void *cls,
                          struct TALER_MINTDB_Session *sesssion,
                          const struct TALER_ReservePublicKeyP *reserve_pub);


  /**
   * Free memory associated with the given reserve history.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param rh history to free.
   */
  void
  (*free_reserve_history) (void *cls,
                           struct TALER_MINTDB_ReserveHistory *rh);


  /**
   * Check if we have the specified deposit already in the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param deposit deposit to search for
   * @return #GNUNET_YES if we know this operation,
   *         #GNUNET_NO if this deposit is unknown to us,
   *         #GNUNET_SYSERR on DB error or if same coin(pub), merchant(pub) and
   *                        transaction ID are already in DB, but for different
   *                        other transaction details (contract, wiring details,
   *                        amount, etc.)
   */
  int
  (*have_deposit) (void *cls,
                   struct TALER_MINTDB_Session *sesssion,
                   const struct TALER_MINTDB_Deposit *deposit);


  /**
   * Insert information about deposited coin into the
   * database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion connection to the database
   * @param deposit deposit information to store
   * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
   */
  int
  (*insert_deposit) (void *cls,
                     struct TALER_MINTDB_Session *sesssion,
                     const struct TALER_MINTDB_Deposit *deposit);


  /**
   * Lookup refresh session data under the given @a session_hash.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database handle to use
   * @param session_hash hash over the melt to use for the lookup
   * @param[out] refresh_session where to store the result
   * @return #GNUNET_YES on success,
   *         #GNUNET_NO if not found,
   *         #GNUNET_SYSERR on DB failure
   */
  int
  (*get_refresh_session) (void *cls,
                          struct TALER_MINTDB_Session *sesssion,
                          const struct GNUNET_HashCode *session_hash,
                          struct TALER_MINTDB_RefreshSession *refresh_session);


  /**
   * Store new refresh session data under the given @a session_hash.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database handle to use
   * @param session_hash hash over the melt to use to locate the session
   * @param refresh_session session data to store
   * @return #GNUNET_YES on success,
   *         #GNUNET_SYSERR on DB failure
   */
  int
  (*create_refresh_session) (void *cls,
                             struct TALER_MINTDB_Session *sesssion,
                             const struct GNUNET_HashCode *session_hash,
                             const struct TALER_MINTDB_RefreshSession *refresh_session);


  /**
   * Store the given /refresh/melt request in the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param oldcoin_index index of the coin to store
   * @param melt coin melt operation details to store; includes
   *             the session hash of the melt
   * @return #GNUNET_OK on success
   *         #GNUNET_SYSERR on internal error
   */
  int
  (*insert_refresh_melt) (void *cls,
                          struct TALER_MINTDB_Session *sesssion,
                          uint16_t oldcoin_index,
                          const struct TALER_MINTDB_RefreshMelt *melt);


  /**
   * Get information about melted coin details from the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param session_hash hash to identify refresh session
   * @param oldcoin_index index of the coin to retrieve
   * @param melt melt data to fill in, can be NULL
   * @return #GNUNET_OK on success
   *         #GNUNET_SYSERR on internal error
   */
  int
  (*get_refresh_melt) (void *cls,
                       struct TALER_MINTDB_Session *sesssion,
                       const struct GNUNET_HashCode *session_hash,
                       uint16_t oldcoin_index,
                       struct TALER_MINTDB_RefreshMelt *melt);


  /**
   * Store in the database which coin(s) we want to create
   * in a given refresh operation.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param session_hash hash to identify refresh session
   * @param num_newcoins number of coins to generate, size of the @a denom_pubs array
   * @param denom_pubs array denominations of the coins to create
   * @return #GNUNET_OK on success
   *         #GNUNET_SYSERR on internal error
   */
  int
  (*insert_refresh_order) (void *cls,
                           struct TALER_MINTDB_Session *sesssion,
                           const struct GNUNET_HashCode *session_hash,
                           uint16_t num_newcoins,
                           const struct TALER_DenominationPublicKey *denom_pubs);


  /**
   * Lookup in the database for the @a num_newcoins coins that we want to
   * create in the given refresh operation.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param session_hash hash to identify refresh session
   * @param num_newcoins size of the @a denom_pubs array
   * @param[out] denom_pubs where to write @a num_newcoins denomination keys
   * @return #GNUNET_OK on success
   *         #GNUNET_SYSERR on internal error
   */
  int
  (*get_refresh_order) (void *cls,
                        struct TALER_MINTDB_Session *sesssion,
                        const struct GNUNET_HashCode *session_hash,
                        uint16_t num_newcoins,
                        struct TALER_DenominationPublicKey *denom_pubs);


  /**
   * Store information about the commitments of the given index @a i
   * for the given refresh session in the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param session_hash hash to identify refresh session
   * @param cnc_index cut and choose index (1st dimension), relating to #TALER_CNC_KAPPA
   * @param num_newcoins coin index size of the @a commit_coins array
   * @param commit_coin array of coin commitments to store
   * @return #GNUNET_OK on success
   *         #GNUNET_SYSERR on error
   */
  int
  (*insert_refresh_commit_coins) (void *cls,
                                  struct TALER_MINTDB_Session *sesssion,
                                  const struct GNUNET_HashCode *session_hash,
                                  uint16_t cnc_index,
                                  uint16_t num_newcoins,
                                  const struct TALER_MINTDB_RefreshCommitCoin *commit_coins);


  /**
   * Obtain information about the commitment of the
   * given coin of the given refresh session from the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param session_hash hash to identify refresh session
   * @param cnc_index cut and choose set index (1st dimension)
   * @param num_coins size of the @a commit_coins array
   * @param[out] commit_coins array of coin commitments to return
   * @return #GNUNET_OK on success
   *         #GNUNET_NO if not found
   *         #GNUNET_SYSERR on error
   */
  int
  (*get_refresh_commit_coins) (void *cls,
                               struct TALER_MINTDB_Session *sesssion,
                               const struct GNUNET_HashCode *session_hash,
                               uint16_t cnc_index,
                               uint16_t num_coins,
                               struct TALER_MINTDB_RefreshCommitCoin *commit_coins);


  /**
   * Store the commitment to the given (encrypted) refresh link data
   * for the given refresh session.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param session_hash hash to identify refresh session
   * @param cnc_index cut and choose index (1st dimension), relating to #TALER_CNC_KAPPA
   * @param num_links size of the @a commit_link array
   * @param commit_links array of link information to store
   * @return #GNUNET_SYSERR on internal error, #GNUNET_OK on success
   */
  int
  (*insert_refresh_commit_links) (void *cls,
                                  struct TALER_MINTDB_Session *sesssion,
                                  const struct GNUNET_HashCode *session_hash,
                                  uint16_t cnc_index,
                                  uint16_t num_links,
                                  const struct TALER_MINTDB_RefreshCommitLinkP *commit_links);

  /**
   * Obtain the commited (encrypted) refresh link data
   * for the given refresh session.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param session_hash hash to identify refresh session
   * @param cnc_index cut and choose index (1st dimension)
   * @param num_links size of the @a links array to return
   * @param[out] links array link information to return
   * @return #GNUNET_SYSERR on internal error,
   *         #GNUNET_NO if commitment was not found
   *         #GNUNET_OK on success
   */
  int
  (*get_refresh_commit_links) (void *cls,
                               struct TALER_MINTDB_Session *sesssion,
                               const struct GNUNET_HashCode *session_hash,
                               uint16_t cnc_index,
                               uint16_t num_links,
                               struct TALER_MINTDB_RefreshCommitLinkP *links);


  /**
   * Get all of the information from the given melt commit operation.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection to use
   * @param session_hash hash to identify refresh session
   * @return NULL if the @a session_hash does not correspond to any known melt
   *         operation
   */
  struct TALER_MINTDB_MeltCommitment *
  (*get_melt_commitment) (void *cls,
                          struct TALER_MINTDB_Session *sesssion,
                          const struct GNUNET_HashCode *session_hash);


  /**
   * Free information about a melt commitment.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param mc melt commitment data to free
   */
  void
  (*free_melt_commitment) (void *cls,
                           struct TALER_MINTDB_MeltCommitment *mc);


  /**
   * Insert signature of a new coin generated during refresh into
   * the database indexed by the refresh session and the index
   * of the coin.  This data is later used should an old coin
   * be used to try to obtain the private keys during "/refresh/link".
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param session_hash hash to identify refresh session
   * @param newcoin_index coin index
   * @param ev_sig coin signature
   * @return #GNUNET_OK on success
   */
  int
  (*insert_refresh_out) (void *cls,
                                 struct TALER_MINTDB_Session *sesssion,
                                 const struct GNUNET_HashCode *session_hash,
                                 uint16_t newcoin_index,
                                 const struct TALER_DenominationSignature *ev_sig);


  /**
   * Obtain the link data of a coin, that is the encrypted link
   * information, the denomination keys and the signatures.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param session_hash session to get linkage data for
   * @return all known link data for the session
   */
  struct TALER_MINTDB_LinkDataList *
  (*get_link_data_list) (void *cls,
                         struct TALER_MINTDB_Session *sesssion,
                         const struct GNUNET_HashCode *session_hash);


  /**
   * Free memory of the link data list.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param ldl link data list to release
   */
  void
  (*free_link_data_list) (void *cls,
                          struct TALER_MINTDB_LinkDataList *ldl);


  /**
   * Obtain shared secret and transfer public key from the public key of
   * the coin.  This information and the link information returned by
   * @e get_link_data_list() enable the owner of an old coin to determine
   * the private keys of the new coins after the melt.
   *
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param coin_pub public key of the coin
   * @param tdc function to call for each session the coin was melted into
   * @param tdc_cls closure for @a tdc
   * @return #GNUNET_OK on success,
   *         #GNUNET_NO on failure (not found)
   *         #GNUNET_SYSERR on internal failure (database issue)
   */
  int
  (*get_transfer) (void *cls,
                   struct TALER_MINTDB_Session *sesssion,
                   const struct TALER_CoinSpendPublicKeyP *coin_pub,
                   TALER_MINTDB_TransferDataCallback tdc,
                   void *tdc_cls);



  /**
   * Test if the given /lock request is known to us.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param lock lock operation
   * @return #GNUNET_YES if known,
   *         #GNUNET_NO if not,
   *         #GNUNET_SYSERR on internal error
   */
  int
  (*have_lock) (void *cls,
                struct TALER_MINTDB_Session *sesssion,
                const struct TALER_MINTDB_LockOperation *lock);


  /**
   * Store the given /lock request in the database.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param lock lock operation
   * @return #GNUNET_OK on success
   *         #GNUNET_SYSERR on internal error
   */
  int
  (*insert_lock) (void *cls,
                  struct TALER_MINTDB_Session *sesssion,
                  const struct TALER_MINTDB_LockOperation *lock);


  /**
   * Compile a list of all (historic) transactions performed
   * with the given coin (/refresh/melt and /deposit operations).
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param sesssion database connection
   * @param coin_pub coin to investigate
   * @return list of transactions, NULL if coin is fresh
   */
  struct TALER_MINTDB_TransactionList *
  (*get_coin_transactions) (void *cls,
                            struct TALER_MINTDB_Session *sesssion,
                            const struct TALER_CoinSpendPublicKeyP *coin_pub);


  /**
   * Free linked list of transactions.
   *
   * @param cls the @e cls of this struct with the plugin-specific state
   * @param list list to free
   */
  void
  (*free_coin_transaction_list) (void *cls,
                                 struct TALER_MINTDB_TransactionList *list);


};


#endif /* _NEURO_MINT_DB_H */
