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
  TALER; see the file COPYING.GPL.  If not, If not, see <http://www.gnu.org/licenses/>
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
   * @param cls closure
   * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
   */
  int
  (*initialize) (void *cls);

  /**
   * Insert payment confirmation from the exchange into the database.
   *
   * @param cls closure
   * @param h_contract hash of the contract
   * @param h_wire hash of our wire details
   * @param transaction_id of the contract
   * @param timestamp time of the confirmation
   * @param refund refund deadline
   * @param amount_without_fee amount the exchange will deposit
   * @param coin_pub public key of the coin
   * @param exchange_proof proof from exchange that coin was accepted
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
		    const json_t *exchange_proof);

  /**
   * Check whether a payment has already been stored
   *
   * @param cls our plugin handle
   * @param transaction_id the transaction id to search into
   * the db
   *
   * @return #GNUNET_OK if found, #GNUNET_NO if not, #GNUNET_SYSERR
   * upon error
   */
  int
  (*check_payment) (void *cls,
                    uint64_t transaction_id);

};
#endif
