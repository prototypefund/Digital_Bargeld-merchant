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
 * @file include/taler_merchantdb_plugin.h
 * @brief database access for the merchant
 * @author Florian Dold
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANTDB_PLUGIN_H
#define TALER_MERCHANTDB_PLUGIN_H

#include <gnunet/gnunet_util_lib.h>

/**
 * Handle to interact with the database.
 */
struct TALER_MERCHANTDB_Plugin;

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
   * Initialize merchant tables
   *
   * @param conn the connection handle to postgres db.
   * @param tmp #GNUNET_YES if the tables are to be made temporary i.e. their
   *          contents are dropped when the @a conn is closed
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*initialize) (void *cls,
                 int tmp);

  /**
   * Inserts a contract record into the database.
   *
   * @param dbh the database connection
   * @param h_contract hash of the contract
   * @param h_wire hash of our wire details
   * @param transaction_id of the contract
   * @param timestamp time of the confirmation
   * @param refund refund deadline
   * @param amount_without_fee amount the mint will deposit
   * @param coin_pub public key of the coin
   * @param merchant_pub our public key
   * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
   */
  int
  (*store_payment) (void *cls,
                    const struct GNUNET_HashCode *h_contract,
                    const struct GNUNET_HashCode *h_wire,
                    uint64_t transaction_id,
                    struct GNUNET_TIME_Absolute timestamp,
                    struct GNUNET_TIME_Absolute refund,
                    const struct TALER_Amount *amount_without_fee,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const struct TALER_MintSignatureP *mint_sig);

};

#endif
