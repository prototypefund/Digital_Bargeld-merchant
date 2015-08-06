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
 * @file mint/taler-mint-httpd_keystate.h
 * @brief management of our private signing keys (denomination keys)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_KEYSTATE_H
#define TALER_MINT_HTTPD_KEYSTATE_H

#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-mint-httpd.h"
#include "taler_mintdb_lib.h"


/**
 * Snapshot of the (coin and signing)
 * keys (including private keys) of the mint.
 */
struct TMH_KS_StateHandle;


/**
 * Acquire the key state of the mint.  Updates keys if necessary.
 * For every call to #TMH_KS_acquire(), a matching call
 * to #TMH_KS_release() must be made.
 *
 * @return the key state
 */
struct TMH_KS_StateHandle *
TMH_KS_acquire (void);


/**
 * Release key state, free if necessary (if reference count gets to zero).
 *
 * @param key_state the key state to release
 */
void
TMH_KS_release (struct TMH_KS_StateHandle *key_state);


/**
 * Denomination key lookups can be for signing of fresh coins
 * or to validate signatures on existing coins.  As the validity
 * periods for a key differ, the caller must specify which
 * use is relevant for the current operation.
 */
enum TMH_KS_DenominationKeyUse {

  /**
   * The key is to be used for a /withdraw/sign or /refresh (mint)
   * operation.
   */
  TMH_KS_DKU_WITHDRAW,

  /**
   * The key is to be usd for a /deposit or /refresh (melt) operation.
   */
  TMH_KS_DKU_DEPOSIT

};


/**
 * Look up the issue for a denom public key.  Note that the result
 * is only valid while the @a key_state is not released!
 *
 * @param key_state state to look in
 * @param denom_pub denomination public key
 * @param use purpose for which the key is being located
 * @return the denomination key issue,
 *         or NULL if denom_pub could not be found (or is not valid at this time for the given @a use)
 */
struct TALER_MINTDB_DenominationKeyIssueInformation *
TMH_KS_denomination_key_lookup (const struct TMH_KS_StateHandle *key_state,
                                const struct TALER_DenominationPublicKey *denom_pub,
				enum TMH_KS_DenominationKeyUse use);


/**
 * Read signals from a pipe in a loop, and reload keys from disk if
 * SIGUSR1 is received, terminate if SIGTERM/SIGINT is received, and
 * restart if SIGHUP is received.
 *
 * @return #GNUNET_SYSERR on errors,
 *         #GNUNET_OK to terminate normally
 *         #GNUNET_NO to restart an update version of the binary
 */
int
TMH_KS_loop (void);


/**
 * Sign the message in @a purpose with the mint's signing
 * key.
 *
 * @param purpose the message to sign
 * @param[out] pub set to the current public signing key of the mint
 * @param[out] sig signature over purpose using current signing key
 */
void
TMH_KS_sign (const struct GNUNET_CRYPTO_EccSignaturePurpose *purpose,
             struct TALER_MintPublicKeyP *pub,
             struct TALER_MintSignatureP *sig);


/**
 * Handle a "/keys" request
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_KS_handler_keys (struct TMH_RequestHandler *rh,
                     struct MHD_Connection *connection,
                     void **connection_cls,
                     const char *upload_data,
                     size_t *upload_data_size);


#endif
