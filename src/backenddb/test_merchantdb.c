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
  struct TALER_Amount amount_with_fee;
  struct TALER_Amount deposit_fee;
  struct TALER_CoinSpendPublicKeyP coin_pub;
  struct TALER_ExchangePublicKeyP signkey_pub;
  struct TALER_WireTransferIdentifierRawP wtid;
  json_t *deposit_proof = NULL;
  json_t *transfer_proof = NULL;

  FAILIF (NULL == (plugin = TALER_MERCHANTDB_plugin_load (cfg)));
  (void)  plugin->drop_tables (plugin->cls);
  FAILIF (GNUNET_OK != plugin->initialize (plugin->cls));

  /* Prepare data for 'store_payment()' */
  RND_BLK (&h_contract);
  RND_BLK (&h_wire);
  RND_BLK (&transaction_id);
  RND_BLK (&signkey_pub);
  RND_BLK (&wtid);
  timestamp = GNUNET_TIME_absolute_get();
  refund = GNUNET_TIME_absolute_get();
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.000010",
                                         &amount_with_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &deposit_fee));
  RND_BLK (&coin_pub);
  deposit_proof = json_object ();
  GNUNET_assert (0 ==
                 json_object_set (deposit_proof,
                                  "test",
                                  json_string ("backenddb test A")));
  transfer_proof = json_object ();
  GNUNET_assert (0 ==
                 json_object_set (transfer_proof,
                                  "test",
                                  json_string ("backenddb test B")));
  FAILIF (GNUNET_OK !=
          plugin->store_transaction (plugin->cls,
                                     transaction_id,
                                     "http://localhost:8888/",
                                     &h_contract,
                                     &h_wire,
                                     timestamp,
                                     refund,
                                     &amount_with_fee));
  FAILIF (GNUNET_OK !=
          plugin->store_deposit (plugin->cls,
                                 transaction_id,
                                 &coin_pub,
                                 &amount_with_fee,
                                 &deposit_fee,
                                 &signkey_pub,
                                 deposit_proof));
  FAILIF (GNUNET_OK !=
          plugin->store_coin_to_transfer (plugin->cls,
                                          transaction_id,
                                          &coin_pub,
                                          &wtid));
  FAILIF (GNUNET_OK !=
          plugin->store_transfer_to_proof (plugin->cls,
                                           "http://localhost:8888/",
                                           &wtid,
                                           &signkey_pub,
                                           transfer_proof));
#if 0
  FAILIF (GNUNET_OK !=
          plugin->check_payment (plugin->cls,
                                 transaction_id));
#endif
  result = 0;

 drop:
  GNUNET_break (GNUNET_OK == plugin->drop_tables (plugin->cls));
  TALER_MERCHANTDB_plugin_unload (plugin);
  plugin = NULL;
  if (NULL != deposit_proof)
    json_decref (deposit_proof);
  if (NULL != transfer_proof)
    json_decref (transfer_proof);
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
                          "test-merchantdb-%s",
                          plugin_name);
  (void) GNUNET_asprintf (&config_filename,
                          "%s.conf",
                          testname);
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
