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
 * @file merchant/test_merchant_db.c
 * @brief File to test merchant database helper functions.
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler_util.h"
#include "merchant_db.h"

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)

/**
 * Macro to round microseconds to seconds in GNUNET_TIME_* structs.
 */
#define ROUND_TO_SECS(name,us_field) name.us_field -= name.us_field % (1000 * 1000);

/**
 * The database handle
 */
PGconn *conn;

/**
 * Test result
 */
static int result;

static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  if (NULL != conn)
    MERCHANT_DB_disconnect (conn);
  conn = NULL;
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{
  struct GNUNET_TIME_Absolute expiry;
  struct TALER_Amount amount;
  char *desc = "A contract from GNUnet e.V to say a big Thank You for a donation of the aforementioned amount.";
  uint64_t nounce;
  uint64_t product;
  long long transaction_id;

  conn = MERCHANT_DB_connect (config);
  EXITIF (NULL == conn);
  GNUNET_SCHEDULER_add_now (&do_shutdown, NULL);
  EXITIF (GNUNET_OK != MERCHANT_DB_initialise (conn, GNUNET_YES));
  expiry = GNUNET_TIME_absolute_get ();
  expiry = GNUNET_TIME_absolute_add (expiry, GNUNET_TIME_UNIT_DAYS);
  ROUND_TO_SECS (expiry, abs_value_us);
  amount.value = 1;
  amount.fraction = 0;
  nounce = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);
  product = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);
  product &= (UINT64_MAX >> 1);
  EXITIF (-1 == (transaction_id = MERCHANT_DB_contract_create (conn,
                                                               expiry,
                                                               &amount,
                                                               desc,
                                                               nounce,
                                                               product)));
  {
    struct GNUNET_CRYPTO_EddsaPublicKey coin_pub;
    struct GNUNET_CRYPTO_EddsaSignature coin_sig;
    long long paid_product;

    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                &coin_pub, sizeof (coin_pub));
    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                &coin_sig, sizeof (coin_sig));
    EXITIF (GNUNET_SYSERR == MERCHANT_DB_checkout_create (conn,
                                                          &coin_pub,
                                                          transaction_id,
                                                          &amount,
                                                          &coin_sig));
    EXITIF (-1 == (paid_product = MERCHANT_DB_get_checkout_product (conn,
                                                                    &coin_pub)));
    EXITIF (paid_product < 0);
    EXITIF (((uint64_t) paid_product) != product);
    /* We should get -1 for product if a coin is not paid to us */
    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                &coin_pub, sizeof (coin_pub));
    EXITIF (-1 != (product = MERCHANT_DB_get_checkout_product (conn,
                                                               &coin_pub)));
  }
  result = GNUNET_OK;

 EXITIF_exit:
  return;
}

int
main (int argc, char *const argv[])
{
  char *argv2[] = {
    "test-merchant-db",
    "-c", "test_merchant.conf",
    NULL
  };
   static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };

  result = GNUNET_SYSERR;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run ((sizeof (argv2) / sizeof (char *)) - 1,
                          argv2, "test-merchant-db",
                          "File to test merchant database helper functions.",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;
}
