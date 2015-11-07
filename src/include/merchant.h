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
#include "merchant.h"

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
 * Outcome of a /deposit request for a coin
 */
struct MERCHANT_DepositConfirmation
{
  /**
   * True if this coin's outcome has been read from
   * its cb
   */
  unsigned int ackd;

  /**
   * The mint's response to this /deposit
   */
  unsigned int exit_status;

  /**
   * The mint's response body (JSON). Mainly useful in case
   * some callback needs to send back to the to the wallet the
   * outcome of an erroneous coin
   */
  json_t *proof;

};

struct MERCHANT_DepositConfirmationCls
{
  /**
   * Offset of this coin into the array of all coins outcomes
   */
  unsigned int index;

  /**
   * Pointer to the global (malloc'd) array of all coins outcomes
   */
  struct MERCHANT_DepositConfirmation *dc;

  /**
   * How many coins this paymen is made of.
   */
  unsigned int coins_cnt;

  /**
   * Transaction id
   */
  uint64_t transaction_id;

};

/**
 * Mint
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

struct MERCHANT_Auditor
{
  /**
   * Auditor's legal name
   */
  char *name;

};

/**
 * The contract sent by the merchant to the wallet
 */
struct MERCHANT_Contract
{
  /**
   * Purpose header for the signature over contract
   */
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;

  /**
   * Hash of the JSON contract in UTF-8 including 0-termination,
   * using JSON_COMPACT | JSON_SORT_KEYS
   */
  struct GNUNET_HashCode h_contract;

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

/**
 * Parses auditors from the configuration.
 *
 * @param cfg the configuration
 * @param mints the array of auditors upon successful parsing.  Will be NULL upon
 *          error.
 * @return the number of auditors in the above array; GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TALER_MERCHANT_parse_auditors (const struct GNUNET_CONFIGURATION_Handle *cfg,
                               struct MERCHANT_Auditor **auditors);

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
