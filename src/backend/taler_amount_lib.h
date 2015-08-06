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
 * @file include/taler_amount_lib.h
 * @brief amount-representation utility functions
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#ifndef TALER_AMOUNT_LIB_H
#define TALER_AMOUNT_LIB_H

#ifdef __cplusplus
extern "C"
{
#if 0                           /* keep Emacsens' auto-indent happy */
}
#endif
#endif

#include <gnunet/platform.h>


/**
 * @brief Number of characters (plus 1 for 0-termination) we use to
 * represent currency names (i.e. EUR, USD, etc.).  We use 8+4 for
 * alignment in the `struct TALER_Amount`.  The amount is typically an
 * ISO 4217 currency code when an alpha-numeric 3-digit code is used.
 * For regional currencies, the first character should be a "*" followed
 * by a region-specific name (i.e. "*BRETAGNEFR").
 */
#define TALER_CURRENCY_LEN 12

/**
 * Taler currency length as a string.
 */
#define TALER_CURRENCY_LEN_STR "12"

/**
 * @brief The "fraction" value in a `struct TALER_Amount` represents which
 * fraction of the "main" value?
 *
 * Note that we need sub-cent precision here as transaction fees might
 * be that low, and as we want to support microdonations.
 */
#define TALER_AMOUNT_FRAC_BASE 1000000

/**
 * @brief How many digits behind the comma are required to represent the
 * fractional value in human readable decimal format?  Must match
 * lg(#TALER_AMOUNT_FRAC_BASE).
 */
#define TALER_AMOUNT_FRAC_LEN 6


GNUNET_NETWORK_STRUCT_BEGIN


/**
 * @brief Amount, encoded for network transmission.
 */
struct TALER_AmountNBO
{
  /**
   * Value in the main currency, in NBO.
   */
  uint64_t value GNUNET_PACKED;

  /**
   * Additinal fractional value, in NBO.
   */
  uint32_t fraction GNUNET_PACKED;

  /**
   * Type of the currency being represented.
   */
  char currency[TALER_CURRENCY_LEN];
};

GNUNET_NETWORK_STRUCT_END


/**
 * @brief Representation of monetary value in a given currency.
 */
struct TALER_Amount
{
  /**
   * Value (numerator of fraction)
   */
  uint64_t value;

  /**
   * Fraction (denominator of fraction)
   */
  uint32_t fraction;

  /**
   * Currency string, left adjusted and padded with zeros.  All zeros
   * for "invalid" values.
   */
  char currency[TALER_CURRENCY_LEN];
};


/**
 * Parse denomination description, in the format "T:V.F".
 *
 * @param str denomination description
 * @param denom denomination to write the result to
 * @return #GNUNET_OK if the string is a valid denomination specification,
 *         #GNUNET_SYSERR if it is invalid.
 */
int
TALER_string_to_amount (const char *str,
                        struct TALER_Amount *denom);


/**
 * Get the value of "zero" in a particular currency.
 *
 * @param cur currency description
 * @param denom denomination to write the result to
 * @return #GNUNET_OK if @a cur is a valid currency specification,
 *         #GNUNET_SYSERR if it is invalid.
 */
int
TALER_amount_get_zero (const char *cur,
                       struct TALER_Amount *denom);


/**
 * Convert amount from host to network representation.
 *
 * @param res where to store amount in network representation
 * @param d amount in host representation
 */
void
TALER_amount_hton (struct TALER_AmountNBO *res,
                   const struct TALER_Amount *d);


/**
 * Convert amount from network to host representation.
 *
 * @param res where to store amount in host representation
 * @param dn amount in network representation
 */
void
TALER_amount_ntoh (struct TALER_Amount *res,
                   const struct TALER_AmountNBO *dn);


/**
 * Compare the value/fraction of two amounts.  Does not compare the currency.
 * Comparing amounts of different currencies will cause the program to abort().
 * If unsure, check with #TALER_amount_cmp_currency() first to be sure that
 * the currencies of the two amounts are identical.
 *
 * @param a1 first amount
 * @param a2 second amount
 * @return result of the comparison
 *         -1 if `a1 < a2`
 *          1 if `a1 > a2`
 *          0 if `a1 == a2`.
 */
int
TALER_amount_cmp (const struct TALER_Amount *a1,
                  const struct TALER_Amount *a2);


/**
 * Test if @a a1 and @a a2 are the same currency.
 *
 * @param a1 amount to test
 * @param a2 amount to test
 * @return #GNUNET_YES if @a a1 and @a a2 are the same currency
 *         #GNUNET_NO if the currencies are different
 *         #GNUNET_SYSERR if either amount is invalid
 */
int
TALER_amount_cmp_currency (const struct TALER_Amount *a1,
                           const struct TALER_Amount *a2);


/**
 * Test if @a a1 and @a a2 are the same currency, NBO variant.
 *
 * @param a1 amount to test
 * @param a2 amount to test
 * @return #GNUNET_YES if @a a1 and @a a2 are the same currency
 *         #GNUNET_NO if the currencies are different
 *         #GNUNET_SYSERR if either amount is invalid
 */
int
TALER_amount_cmp_currency_nbo (const struct TALER_AmountNBO *a1,
                               const struct TALER_AmountNBO *a2);


/**
 * Perform saturating subtraction of amounts.
 *
 * @param diff where to store (@a a1 - @a a2), or invalid if @a a2 > @a a1
 * @param a1 amount to subtract from
 * @param a2 amount to subtract
 * @return #GNUNET_OK if the subtraction worked,
 *         #GNUNET_NO if @a a1 = @a a2
 *         #GNUNET_SYSERR if @a a2 > @a a1 or currencies are incompatible;
 *                        @a diff is set to invalid
 */
int
TALER_amount_subtract (struct TALER_Amount *diff,
                       const struct TALER_Amount *a1,
                       const struct TALER_Amount *a2);


/**
 * Perform addition of amounts.
 *
 * @param sum where to store @a a1 + @a a2, set to "invalid" on overflow
 * @param a1 first amount to add
 * @param a2 second amount to add
 * @return #GNUNET_OK if the addition worked,
 *         #GNUNET_SYSERR on overflow
 */
int
TALER_amount_add (struct TALER_Amount *sum,
                  const struct TALER_Amount *a1,
                  const struct TALER_Amount *a2);


/**
 * Normalize the given amount.
 *
 * @param amount amount to normalize
 * @return #GNUNET_OK if normalization worked
 *         #GNUNET_NO if value was already normalized
 *         #GNUNET_SYSERR if value was invalid or could not be normalized
 */
int
TALER_amount_normalize (struct TALER_Amount *amount);


/**
 * Convert amount to string.
 *
 * @param amount amount to convert to string
 * @return freshly allocated string representation,
 *         NULL if the @a amount was invalid
 */
char *
TALER_amount_to_string (const struct TALER_Amount *amount);

#if 0                           /* keep Emacsens' auto-indent happy */
{
#endif
#ifdef __cplusplus
}
#endif


#endif
