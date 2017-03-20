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
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/test_merchantdb_postgres.c
 * @brief testcase for merchant's postgres db plugin
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
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


/**
 * Currency we use for the coins.
 */
#define CURRENCY "EUR"

/**
 * URI we use for the exchange in the database.
 * Note that an exchange does not actually have
 * to run at this address.
 */
#define EXCHANGE_URI "http://localhost:8888/"

/**
 * Global return value for the test.  Initially -1, set to 0 upon
 * completion.  Other values indicate some kind of error.
 */
static int result;

/**
 * Handle to the plugin we are testing.
 */
static struct TALER_MERCHANTDB_Plugin *plugin;

/**
 * Hash of the wire transfer address.  Set to some random value.
 */
static struct GNUNET_HashCode h_wire;

/**
 * Transaction ID.
 */
const char *order_id;

/**
 * Proposal's hash
 */
struct GNUNET_HashCode h_proposal_data;

/**
 * Proposal's hash.
 */
struct GNUNET_HashCode h_proposal_data2;

/**
 * Time of the transaction.
 */
static struct GNUNET_TIME_Absolute timestamp;

/**
 * Delta aimed to test the "by_date" query on transactions.
 */
static struct GNUNET_TIME_Relative delta;

/**
 * Deadline until which refunds are allowed.
 */
static struct GNUNET_TIME_Absolute refund_deadline;

/**
 * Total amount, including deposit fee.
 */
static struct TALER_Amount amount_with_fee;

/**
 * Deposit fee for the coin.
 */
static struct TALER_Amount deposit_fee;

/**
 * Public key of the coin.  Set to some random value.
 */
static struct TALER_CoinSpendPublicKeyP coin_pub;

/**
 * Public key of the exchange.  Set to some random value.
 */
static struct TALER_ExchangePublicKeyP signkey_pub;

/**
 * Public Key of the merchant. Set to some random value.
 * Used as merchant instances now do store their keys.
 */
static struct TALER_MerchantPublicKeyP merchant_pub;

/**
 * Wire transfer identifier.  Set to some random value.
 */
static struct TALER_WireTransferIdentifierRawP wtid;

/**
 * "Proof" of deposit from the exchange. Set to some valid JSON.
 */
static json_t *deposit_proof;

/**
 * "Proof" of wire transfer from the exchange. Set to some valid JSON.
 */
static json_t *transfer_proof;

/**
 * A mock contract, not need to be well-formed
 */
static json_t *contract;

/**
 * Mock proposal data, not need to be well-formed
 */
static json_t *proposal_data;



/**
 * Function called with information about a transaction.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param merchant_pub public key of the merchant
 * @param exchange_uri URI of the exchange
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund_deadline refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
static void
transaction_cb (void *cls,
		const struct TALER_MerchantPublicKeyP *amerchant_pub,
                const char *aexchange_uri,
                const struct GNUNET_HashCode *ah_proposal_data,
                const struct GNUNET_HashCode *ah_wire,
                struct GNUNET_TIME_Absolute atimestamp,
                struct GNUNET_TIME_Absolute arefund_deadline,
                const struct TALER_Amount *atotal_amount)
{
#define CHECK(a) do { if (! (a)) { GNUNET_break (0); result = 3; } } while (0)
  CHECK (0 == memcmp (amerchant_pub,
                      &merchant_pub,
		      sizeof (struct TALER_MerchantPublicKeyP)));
  CHECK (0 == memcmp (ah_proposal_data,
                      &h_proposal_data,
                      sizeof (struct GNUNET_HashCode)));
  CHECK (0 == strcmp (aexchange_uri,
                      EXCHANGE_URI));
  CHECK (0 == memcmp (ah_wire,
                      &h_wire,
                      sizeof (struct GNUNET_HashCode)));
  CHECK (atimestamp.abs_value_us == timestamp.abs_value_us);
  CHECK (arefund_deadline.abs_value_us == refund_deadline.abs_value_us);
  CHECK (0 == TALER_amount_cmp (atotal_amount,
                                &amount_with_fee));
}

/**
 * Callback for `find_proposal_data_by_date`.
 *
 * @param cls closure
 * @param order_id order id
 * @param row_id row id in db
 * @param proposal_data proposal data
 */
static void
pd_cb (void *cls,
       const char *order_id,
       unsigned int row_id,
       const json_t *proposal_data)
{
  return;
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
            const struct GNUNET_HashCode *ah_proposal_data,
            const struct TALER_CoinSpendPublicKeyP *acoin_pub,
            const struct TALER_Amount *aamount_with_fee,
            const struct TALER_Amount *adeposit_fee,
            const json_t *aexchange_proof)
{
  CHECK ((0 == memcmp (ah_proposal_data,
                       &h_proposal_data,
                       sizeof (struct GNUNET_HashCode))));
  CHECK (0 == memcmp (acoin_pub,
                      &coin_pub,
                      sizeof (struct TALER_CoinSpendPublicKeyP)));
  CHECK (0 == TALER_amount_cmp (aamount_with_fee,
                                &amount_with_fee));
  CHECK (0 == TALER_amount_cmp (adeposit_fee,
                                &deposit_fee));
  CHECK (1 == json_equal ((json_t *) aexchange_proof,
                          deposit_proof));
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
 * @param execution_time when was the @a wtid transfer executed
 * @param exchange_proof proof from exchange about what the deposit was for
 *             NULL if we have not asked for this signature
 */
static void
transfer_cb (void *cls,
             const struct GNUNET_HashCode *ah_proposal_data,
             const struct TALER_CoinSpendPublicKeyP *acoin_pub,
             const struct TALER_WireTransferIdentifierRawP *awtid,
             struct GNUNET_TIME_Absolute execution_time,
             const json_t *exchange_proof)
{
  CHECK (0 == memcmp (ah_proposal_data,
                      &h_proposal_data,
                      sizeof (struct GNUNET_HashCode)));

  CHECK (0 == memcmp (acoin_pub,
                      &coin_pub,
                      sizeof (struct TALER_CoinSpendPublicKeyP)));
  CHECK (0 == memcmp (awtid,
                      &wtid,
                      sizeof (struct TALER_WireTransferIdentifierRawP)));
  CHECK (1 == json_equal ((json_t *) exchange_proof,
                          transfer_proof));
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
  CHECK (1 == json_equal ((json_t *) proof,
                          transfer_proof));
}
#undef CHECK


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure with config
 */
static void
run (void *cls)
{
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  struct GNUNET_TIME_Absolute fake_now;
  /* Data for 'store_payment()' */

  if (NULL == (plugin = TALER_MERCHANTDB_plugin_load (cfg)))
  {
    result = 77;
    return;
  }
  if (GNUNET_OK != plugin->drop_tables (plugin->cls))
  {
    result = 77;
    return;
  }
  if (GNUNET_OK != plugin->initialize (plugin->cls))
  {
    result = 77;
    return;
  }

  /* Prepare data for 'store_payment()' */
  RND_BLK (&h_wire);
  RND_BLK (&h_proposal_data);
  order_id = "test_ID";
  RND_BLK (&signkey_pub);
  RND_BLK (&merchant_pub);
  RND_BLK (&wtid);
  timestamp = GNUNET_TIME_absolute_get();
  GNUNET_TIME_round_abs (&timestamp);
  delta = GNUNET_TIME_UNIT_MINUTES;
  fake_now = GNUNET_TIME_absolute_add (timestamp, delta);
  refund_deadline = GNUNET_TIME_absolute_get();
  GNUNET_TIME_round_abs (&refund_deadline);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1.000010",
                                         &amount_with_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &deposit_fee));
  RND_BLK (&coin_pub);
  deposit_proof = json_object ();
  GNUNET_assert (0 ==
                 json_object_set_new (deposit_proof,
                                      "test",
                                      json_string ("backenddb test A")));
  transfer_proof = json_object ();
  GNUNET_assert (0 ==
                 json_object_set_new (transfer_proof,
                                      "test",
                                      json_string ("backenddb test B")));
  contract = json_object ();
  proposal_data = json_object ();

  TALER_JSON_hash (proposal_data,
                   &h_proposal_data2);

  FAILIF (GNUNET_OK !=
          plugin->insert_proposal_data (plugin->cls,
                                        order_id,
                                        &merchant_pub,
                                        timestamp,
                                        proposal_data));

  json_t *out;

  FAILIF (GNUNET_OK !=
          plugin->find_proposal_data (plugin->cls,
                                      &out, // plain data
                                      order_id,
                                      &merchant_pub));

  FAILIF (GNUNET_OK !=
          plugin->find_proposal_data_from_hash (plugin->cls,
                                                &out, // plain data
                                                &h_proposal_data2,
                                                &merchant_pub));
  FAILIF (1 !=
          plugin->find_proposal_data_by_date_and_range (plugin->cls,
                                                        fake_now,
                                                        &merchant_pub,
                                                        2,
                                                        1,
                                                        pd_cb,
                                                        NULL));

  FAILIF (1 !=
          plugin->find_proposal_data_by_date (plugin->cls,
                                              fake_now,
                                              &merchant_pub,
                                              1,
                                              pd_cb,
                                              NULL));

  FAILIF (GNUNET_OK !=
          plugin->store_transaction (plugin->cls,
                                     &h_proposal_data,
				     &merchant_pub,
                                     EXCHANGE_URI,
                                     &h_wire,
                                     timestamp,
                                     refund_deadline,
                                     &amount_with_fee));
  FAILIF (GNUNET_OK !=
          plugin->store_deposit (plugin->cls,
                                 &h_proposal_data,
				 &merchant_pub,
                                 &coin_pub,
                                 &amount_with_fee,
                                 &deposit_fee,
                                 &signkey_pub,
                                 deposit_proof));
  FAILIF (GNUNET_OK !=
          plugin->store_coin_to_transfer (plugin->cls,
                                          &h_proposal_data,
                                          &coin_pub,
                                          &wtid));
  FAILIF (GNUNET_OK !=
          plugin->store_transfer_to_proof (plugin->cls,
                                           EXCHANGE_URI,
                                           &wtid,
                                           GNUNET_TIME_UNIT_ZERO_ABS,
                                           &signkey_pub,
                                           transfer_proof));
  FAILIF (GNUNET_OK !=
          plugin->find_transaction (plugin->cls,
                                    &h_proposal_data,
				    &merchant_pub,
                                    &transaction_cb,
                                    NULL));

  FAILIF (GNUNET_OK !=
          plugin->find_payments (plugin->cls,
                                 &h_proposal_data,
                                 &merchant_pub,
                                 &deposit_cb,
                                 NULL));
  FAILIF (GNUNET_OK !=
          plugin->find_transfers_by_hash (plugin->cls,
                                        &h_proposal_data,
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
  if (-1 == result)
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
  GNUNET_log_setup (argv[0], "DEBUG", NULL);
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

/* end of test_merchantdb.c */
