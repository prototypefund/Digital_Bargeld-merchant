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
 * @file mint/taler-mint-httpd_db.h
 * @brief High-level (transactional-layer) database operations for the mint
 * @author Chrisitan Grothoff
 */
#ifndef TALER_MINT_HTTPD_DB_H
#define TALER_MINT_HTTPD_DB_H

#include <microhttpd.h>
#include "taler_mintdb_plugin.h"


/**
 * Execute a "/deposit".  The validity of the coin and signature
 * have already been checked.  The database must now check that
 * the coin is not (double or over) spent, and execute the
 * transaction (record details, generate success or failure response).
 *
 * @param connection the MHD connection to handle
 * @param deposit information about the deposit
 * @return MHD result code
 */
int
TMH_DB_execute_deposit (struct MHD_Connection *connection,
                        const struct TALER_MINTDB_Deposit *deposit);


/**
 * Execute a "/withdraw/status".  Given the public key of a reserve,
 * return the associated transaction history.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve to check
 * @return MHD result code
 */
int
TMH_DB_execute_withdraw_status (struct MHD_Connection *connection,
                                const struct TALER_ReservePublicKeyP *reserve_pub);


/**
 * Execute a "/withdraw/sign".  Given a reserve and a properly signed
 * request to withdraw a coin, check the balance of the reserve and
 * if it is sufficient, store the request and return the signed
 * blinded envelope.
 *
 * @param connection the MHD connection to handle
 * @param reserve public key of the reserve
 * @param denomination_pub public key of the denomination requested
 * @param blinded_msg blinded message to be signed
 * @param blinded_msg_len number of bytes in @a blinded_msg
 * @param signature signature over the withdraw request, to be stored in DB
 * @return MHD result code
 */
int
TMH_DB_execute_withdraw_sign (struct MHD_Connection *connection,
                              const struct TALER_ReservePublicKeyP *reserve,
                              const struct TALER_DenominationPublicKey *denomination_pub,
                              const char *blinded_msg,
                              size_t blinded_msg_len,
                              const struct TALER_ReserveSignatureP *signature);


/**
 * @brief Details about a melt operation of an individual coin.
 */
struct TMH_DB_MeltDetails
{

  /**
   * Information about the coin being melted.
   */
  struct TALER_CoinPublicInfo coin_info;

  /**
   * Signature allowing the melt (using
   * a `struct TALER_MINTDB_RefreshMeltConfirmSignRequestBody`) to sign over.
   */
  struct TALER_CoinSpendSignatureP melt_sig;

  /**
   * How much of the coin's value did the client allow to be melted?
   * This amount includes the fees, so the final amount contributed
   * to the melt is this value minus the fee for melting the coin.
   */
  struct TALER_Amount melt_amount_with_fee;
};


/**
 * Execute a "/refresh/melt". We have been given a list of valid
 * coins and a request to melt them into the given
 * @a refresh_session_pub.  Check that the coins all have the
 * required value left and if so, store that they have been
 * melted and confirm the melting operation to the client.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash code of the session the coins are melted into
 * @param num_new_denoms number of entries in @a denom_pubs, size of y-dimension of @a commit_coin array
 * @param denom_pubs array of public denomination keys for the refresh (?)
 * @param coin_count number of entries in @ a coin_melt_details, size of y-dimension of @a commit_link array
 * @param coin_melt_details signatures and (residual) value of and information about the respective coin to be melted
 * @param commit_coin 2d array of coin commitments (what the mint is to sign
 *                    once the "/refres/reveal" of cut and choose is done)
 * @param commit_link 2d array of coin link commitments (what the mint is
 *                    to return via "/refresh/link" to enable linkage in the
 *                    future)
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_melt (struct MHD_Connection *connection,
                             const struct GNUNET_HashCode *session_hash,
                             unsigned int num_new_denoms,
                             const struct TALER_DenominationPublicKey *denom_pubs,
                             unsigned int coin_count,
                             const struct TMH_DB_MeltDetails *coin_melt_details,
                             struct TALER_MINTDB_RefreshCommitCoin *const* commit_coin,
                             struct TALER_MINTDB_RefreshCommitLinkP *const* commit_link);


/**
 * Execute a "/refresh/reveal".  The client is revealing to us the
 * transfer keys for #TALER_CNC_KAPPA-1 sets of coins.  Verify that the
 * revealed transfer keys would allow linkage to the blinded coins,
 * and if so, return the signed coins for corresponding to the set of
 * coins that was not chosen.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash over the refresh session
 * @param num_oldcoins size of y-dimension of @a transfer_privs array
 * @param transfer_privs array with the revealed transfer keys, #TALER_CNC_KAPPA is 1st-dimension
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_reveal (struct MHD_Connection *connection,
                               const struct GNUNET_HashCode *session_hash,
                               unsigned int num_oldcoins,
                               struct TALER_TransferPrivateKeyP **transfer_privs);


/**
 * Execute a "/refresh/link".  Returns the linkage information that
 * will allow the owner of a coin to follow the refresh trail to the
 * refreshed coin.
 *
 * @param connection the MHD connection to handle
 * @param coin_pub public key of the coin to link
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_link (struct MHD_Connection *connection,
                             const struct TALER_CoinSpendPublicKeyP *coin_pub);



/**
 * Add an incoming transaction to the database.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve
 * @param amount amount to add to the reserve
 * @param execution_time when did we receive the wire transfer
 * @param wire details about the wire transfer
 * @return MHD result code
 */
int
TMH_DB_execute_admin_add_incoming (struct MHD_Connection *connection,
                                   const struct TALER_ReservePublicKeyP *reserve_pub,
                                   const struct TALER_Amount *amount,
                                   struct GNUNET_TIME_Absolute execution_time,
                                   json_t *wire);


#endif
/* TALER_MINT_HTTPD_DB_H */
