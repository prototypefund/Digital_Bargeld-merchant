/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
 * @file merchant/merchant_db.h
 * @brief database helper functions used by the merchant
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#ifndef MERCHANT_DB_H
#define MERCHANT_DB_H

#include <gnunet/gnunet_postgres_lib.h>
#include <taler/taler_util.h>

/**
 * Connect to postgresql database
 *
 * @param cfg the configuration handle
 * @return connection to the postgresql database; NULL upon error
 */
PGconn *
MERCHANT_DB_connect (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Disconnect from the database
 *
 * @param conn database handle to close
 */
void
MERCHANT_DB_disconnect (PGconn *conn);


/**
 * Initialize merchant tables
 *
 * @param conn the connection handle to postgres db.
 * @param tmp GNUNET_YES if the tables are to be made temporary i.e. their
 *          contents are dropped when the @a conn is closed
 * @return GNUNET_OK upon success; GNUNET_SYSERR upon failure
 */
int
MERCHANT_DB_initialize (PGconn *conn, int tmp);


/**
 * Inserts a contract record into the database and if successfull returns the
 * serial number of the inserted row.
 *
 * @param conn the database connection
 * @param expiry the time when the contract will expire
 * @param amount the taler amount corresponding to the contract
 * @param c_id this contract's identification number
 * @param desc descripition of the contract
 * @param nounce a random 64-bit nounce
 * @param product description to identify a product
 * @return GNUNET_OK on success, GNUNET_SYSERR upon error
 */

uint32_t
MERCHANT_DB_contract_create (PGconn *conn,
                             const struct GNUNET_TIME_Absolute *expiry,
                             const struct TALER_Amount *amount,
			     uint64_t c_id,
                             const char *desc,
                             uint64_t nounce,
                             uint64_t product);

long long
MERCHANT_DB_get_contract_product (PGconn *conn,
                                  uint64_t contract_id);

unsigned int
MERCHANT_DB_checkout_create (PGconn *conn,
                             struct GNUNET_CRYPTO_rsa_PublicKey *coin_pub,
                             uint64_t transaction_id,
                             struct TALER_Amount *amount,
                             struct GNUNET_CRYPTO_rsa_Signature *coin_sig);


long long
MERCHANT_DB_get_checkout_product (PGconn *conn,
                                  struct GNUNET_CRYPTO_rsa_PublicKey *coin_pub);

#endif  /* MERCHANT_DB_H */

/* end of merchant-db.h */
