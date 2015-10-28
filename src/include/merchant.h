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
 * @file merchant/merchant.c
 * @brief Common utility functions for merchant
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#ifndef MERCHANT_H
#define MERCHANT_H

#include <gnunet/gnunet_common.h>
#include <gnunet/gnunet_crypto_lib.h>
#include <taler/taler_mint_service.h>

/**
 * Macro to round microseconds to seconds in GNUNET_TIME_* structs.
 */
#define ROUND_TO_SECS(name,us_field) name.us_field -= name.us_field % (1000 * 1000)

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)

/**
 * A mint
 */
struct MERCHANT_Mint
{
  /**
   * Hostname
   */
  char *hostname;

  /**
   * Flag which indicates whether some HTTP transfer between
   * this merchant and the mint is still ongoing
   */
  int pending;

  /**
   * A connection to this mint
   */
  struct TALER_MINT_Handle *conn;

};

/**
 * Parses mints from the configuration.
 *
 * @param cfg the configuration
 * @param mints the array of mints upon successful parsing.  Will be NULL upon
 *          error.
 * @return the number of mints in the above array; GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TALER_MERCHANT_parse_mints (const struct GNUNET_CONFIGURATION_Handle *cfg,
                            struct MERCHANT_Mint **mints);


GNUNET_NETWORK_STRUCT_BEGIN
struct MERCHANT_WIREFORMAT_Sepa
{
  /**
   * The international bank account number
   */
  char *iban;

  /**
   * Name of the bank account holder
   */
  char *name;

  /**
   *The bank identification code
   */
  char *bic;

  /**
   * The latest payout date when the payment corresponding to this account has
   * to take place.  A value of 0 indicates a transfer as soon as possible.
   */
  struct GNUNET_TIME_AbsoluteNBO payout;
};
GNUNET_NETWORK_STRUCT_END

/**
 * Parse the SEPA information from the configuration.  If any of the required
 * fileds is missing return NULL.
 *
 * @param cfg the configuration
 * @return Sepa details as a structure; NULL upon error
 */
struct MERCHANT_WIREFORMAT_Sepa *
TALER_MERCHANT_parse_wireformat_sepa (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Destroy and free resouces occupied by the wireformat structure
 *
 * @param wf the wireformat structure
 */
void
TALER_MERCHANT_destroy_wireformat_sepa (struct MERCHANT_WIREFORMAT_Sepa *wf);

#endif  /* MERCHANT_H */
