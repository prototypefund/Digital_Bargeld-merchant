/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/test_merchantdb_postgres.c
 * @brief testcase for merchant's postgres db plugin
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_util.h>
#include "taler_merchantdb_lib.h"
#include <jansson.h>

#define FAILIF(cond)                            \
  do {                                          \
    if (!(cond)){ break;}                       \
    GNUNET_break (0);                           \
    goto drop;                                  \
  } while (0)

#define RND_BLK(ptr)                                                    \
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK, ptr, sizeof (*ptr))

#define CURRENCY "EUR"

static int result;
static struct TALER_MERCHANTDB_Plugin *plugin;

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure with config
 */
static void
run (void *cls)
{
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  /* Data for 'store_payment()' */
  struct  GNUNET_HashCode h_contract;
  struct  GNUNET_HashCode h_wire;
  uint64_t transaction_id;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute refund;
  struct TALER_Amount amount_without_fee;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  json_t *exchange_proof = NULL;

  FAILIF (NULL == (plugin = TALER_MERCHANTDB_plugin_load (cfg)));
  FAILIF (GNUNET_OK != plugin->initialize (plugin->cls));

  /* Prepare data for 'store_payment()' */
  RND_BLK (&h_contract);
  RND_BLK (&h_wire);
  RND_BLK (&transaction_id);
  timestamp = GNUNET_TIME_absolute_get();
  refund = GNUNET_TIME_absolute_get();
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.000010",
                                         &amount_without_fee));
  RND_BLK (&coin_pub);
  exchange_proof = json_object ();
  GNUNET_assert (0 == json_object_set (exchange_proof, "test", json_string ("backenddb test")));
  FAILIF (GNUNET_OK != plugin->store_payment (plugin->cls,
                                              &h_contract,
                                              &h_wire,
                                              transaction_id,
                                              timestamp,
                                              refund,
                                              &amount_without_fee,
                                              &coin_pub,
                                              exchange_proof));
   FAILIF (GNUNET_OK != plugin->check_payment (plugin->cls, transaction_id));
   result = 0;

 drop:
  TALER_MERCHANTDB_plugin_unload (plugin);
  plugin = NULL;
  if (NULL != exchange_proof)
    json_decref(exchange_proof);
}

int
main (int argc,
      char *const argv[])
{

  const char *plugin_name;
  char *config_filename;
  char *testname;
  struct GNUNET_CONFIGURATION_Handle *cfg;

  result = -1;
  if (NULL == (plugin_name = strrchr (argv[0], (int) '-')))
  {
    GNUNET_break (0);
    return -1;
  }
  GNUNET_log_setup (argv[0], "WARNING", NULL);
  plugin_name++;
  (void) GNUNET_asprintf (&testname,
                          "test-merchantdb-%s", plugin_name);
  (void) GNUNET_asprintf (&config_filename,
                          "%s.conf", testname);
  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_parse (cfg,
                                  config_filename))
  {
    GNUNET_break (0);
    GNUNET_free (config_filename);
    GNUNET_free (testname);
    return 2;
  }
  GNUNET_SCHEDULER_run (&run, cfg);
  GNUNET_CONFIGURATION_destroy (cfg);
  GNUNET_free (config_filename);
  GNUNET_free (testname);
  return result;
}
