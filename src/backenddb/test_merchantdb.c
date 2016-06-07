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

#define EXCHANGE_URI "http://localhost:8888/"

static int result;

static struct TALER_MERCHANTDB_Plugin *plugin;

static struct GNUNET_HashCode h_contract;

static struct GNUNET_HashCode h_wire;

static uint64_t transaction_id;

static struct GNUNET_TIME_Absolute timestamp;

static struct GNUNET_TIME_Absolute refund;

static struct TALER_Amount amount_with_fee;

static struct TALER_Amount deposit_fee;

static struct TALER_CoinSpendPublicKeyP coin_pub;

static struct TALER_ExchangePublicKeyP signkey_pub;

static struct TALER_WireTransferIdentifierRawP wtid;

static json_t *deposit_proof = NULL;

static json_t *transfer_proof = NULL;


/**
 * Function called with information about a transaction.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param exchange_uri URI of the exchange
 * @param h_contract hash of the contract
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
static void
transaction_cb (void *cls,
                uint64_t transaction_id,
                const char *exchange_uri,
                const struct GNUNET_HashCode *h_contract,
                const struct GNUNET_HashCode *h_wire,
                struct GNUNET_TIME_Absolute timestamp,
                struct GNUNET_TIME_Absolute refund,
                const struct TALER_Amount *total_amount)
{
}


/**
 * Function called with information about a coin that was deposited.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
deposit_cb (void *cls,
            uint64_t transaction_id,
            const struct TALER_CoinSpendPublicKeyP *coin_pub,
            const struct TALER_Amount *amount_with_fee,
            const struct TALER_Amount *deposit_fee,
            const json_t *exchange_proof)
{
}


/**
 * Information about the wire transfer corresponding to
 * a deposit operation.  Note that it is in theory possible
 * that we have a @a transaction_id and @a coin_pub in the
 * result that do not match a deposit that we know about,
 * for example because someone else deposited funds into
 * our account.
 *
 * @param cls closure
 * @param transaction_id ID of the contract
 * @param coin_pub public key of the coin
 * @param wtid identifier of the wire transfer in which the exchange
 *             send us the money for the coin deposit
 * @param exchange_proof proof from exchange about what the deposit was for
 *             NULL if we have not asked for this signature
 */
static void
transfer_cb (void *cls,
             uint64_t transaction_id,
             const struct TALER_CoinSpendPublicKeyP *coin_pub,
             const struct TALER_WireTransferIdentifierRawP *wtid,
             const json_t *exchange_proof)
{
}


/**
 * Function called with information about a wire transfer identifier.
 *
 * @param cls closure
 * @param proof proof from exchange about what the wire transfer was for
 */
static void
proof_cb (void *cls,
          const json_t *proof)
{
}


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
                                     EXCHANGE_URI,
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
                                           EXCHANGE_URI,
                                           &wtid,
                                           &signkey_pub,
                                           transfer_proof));
  FAILIF (GNUNET_OK !=
          plugin->find_transaction_by_id (plugin->cls,
                                          transaction_id,
                                          &transaction_cb,
                                          NULL));
  FAILIF (GNUNET_OK !=
          plugin->find_payments_by_id (plugin->cls,
                                       transaction_id,
                                       &deposit_cb,
                                       NULL));
  FAILIF (GNUNET_OK !=
          plugin->find_transfers_by_id (plugin->cls,
                                        transaction_id,
                                        &transfer_cb,
                                        NULL));
  FAILIF (GNUNET_OK !=
          plugin->find_deposits_by_wtid (plugin->cls,
                                         &wtid,
                                         &deposit_cb,
                                         NULL));
  FAILIF (GNUNET_OK !=
          plugin->find_proof_by_wtid (plugin->cls,
                                      EXCHANGE_URI,
                                      &wtid,
                                      &proof_cb,
                                      NULL));
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
