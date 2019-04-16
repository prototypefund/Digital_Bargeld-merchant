/*
  This file is part of TALER
  (C) 2014-2017 INRIA

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
 * @file merchant/test_merchantdb.c
 * @brief testcase for merchant's postgres db plugin
 * @author Marcello Stanisci
 * @author Christian Grothoff
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
 * URL we use for the exchange in the database.
 * Note that an exchange does not actually have
 * to run at this address.
 */
#define EXCHANGE_URL "http://localhost:8888/"

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
 * Transaction ID used to test the db query
 * `find_contract_terms_by_date_and_range_future`
 */
const char *order_id_future;

/**
 * Proposal's hash
 */
struct GNUNET_HashCode h_contract_terms;

/**
 * Proposal's hash.
 */
struct GNUNET_HashCode h_contract_terms_future;

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
 * Wire fee of the exchange.
 */
static struct TALER_Amount wire_fee;

/**
 * Refund fee for the coin.
 */
static struct TALER_Amount refund_fee;

/**
 * Amount to be refunded.
 */
static struct TALER_Amount refund_amount;

/**
 * Amount to be refunded.  Used to trigger error about
 * subsequest refund amount being lesser than the previous
 * ones.
 */
static struct TALER_Amount little_refund_amount;


/**
 * Amount to be refunded in a call which is subsequent
 * to the good one, expected to succeed.
 */
static struct TALER_Amount right_second_refund_amount;

/**
 * Refund amount meant to raise an error because the
 * contract's coins aren't enough to pay it back
 */
static struct TALER_Amount too_big_refund_amount;

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
static json_t *contract_terms;

/**
 * Mock proposal data, not need to be well-formed
 */
static json_t *contract_terms_future;


/**
 * Function called with information about a refund.
 *
 * @param cls closure
 * @param coin_pub public coin from which the refund comes from
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explaination of the refund
 * @param refund_amount refund amount which is being taken from coin_pub
 * @param refund_fee cost of this refund operation
 */
static void
refund_cb (void *cls,
	   const struct TALER_CoinSpendPublicKeyP *coin_pub,
	   uint64_t rtransaction_id,
	   const char *reason,
	   const struct TALER_Amount *refund_amount,
	   const struct TALER_Amount *refund_fee)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "refund_cb\n");
  /* FIXME, more logic here? */
}


/**
 * Callback for `find_contract_terms_by_date`.
 *
 * @param cls closure
 * @param order_id order id
 * @param row_id row id in db
 * @param contract_terms proposal data
 */
static void
pd_cb (void *cls,
       const char *order_id,
       uint64_t row_id,
       const json_t *contract_terms)
{
  return;
}


#define CHECK(a) do { if (! (a)) { GNUNET_break (0); result = 3; } } while (0)


/**
 * Function called with information about a coin that was deposited.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param acoin_pub public key of the coin
 * @param aexchange_url exchange associated with @a acoin_pub in DB
 * @param aamount_with_fee amount the exchange will deposit for this coin
 * @param adeposit_fee fee the exchange will charge for this coin
 * @param adeposit_fee fee the exchange will charge for refunding this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
deposit_cb (void *cls,
            const struct GNUNET_HashCode *ah_contract_terms,
            const struct TALER_CoinSpendPublicKeyP *acoin_pub,
            const char *aexchange_url,
            const struct TALER_Amount *aamount_with_fee,
            const struct TALER_Amount *adeposit_fee,
            const struct TALER_Amount *arefund_fee,
            const struct TALER_Amount *awire_fee,
            const json_t *aexchange_proof)
{
  CHECK ((0 == GNUNET_memcmp (ah_contract_terms,
                              &h_contract_terms)));
  CHECK (0 == GNUNET_memcmp (acoin_pub,
                             &coin_pub));
  CHECK (0 == strcmp (aexchange_url,
                      EXCHANGE_URL));
  CHECK (0 == TALER_amount_cmp (aamount_with_fee,
                                &amount_with_fee));
  CHECK (0 == TALER_amount_cmp (adeposit_fee,
                                &deposit_fee));
  CHECK (0 == TALER_amount_cmp (awire_fee,
                                &wire_fee));
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
             const struct GNUNET_HashCode *ah_contract_terms,
             const struct TALER_CoinSpendPublicKeyP *acoin_pub,
             const struct TALER_WireTransferIdentifierRawP *awtid,
             struct GNUNET_TIME_Absolute execution_time,
             const json_t *exchange_proof)
{
  CHECK (0 == GNUNET_memcmp (ah_contract_terms,
                             &h_contract_terms));

  CHECK (0 == GNUNET_memcmp (acoin_pub,
                             &coin_pub));
  CHECK (0 == GNUNET_memcmp (awtid,
                             &wtid));
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
 * Test the wire fee storage.
 *
 * @return #GNUNET_OK on success
 */
static int
test_wire_fee ()
{
  struct TALER_MasterPublicKeyP exchange_pub;
  struct GNUNET_HashCode h_wire_method;
  struct GNUNET_TIME_Absolute contract_date;
  struct TALER_Amount wire_fee1;
  struct TALER_Amount closing_fee1;
  struct TALER_Amount wire_fee2;
  struct TALER_Amount closing_fee2;
  struct TALER_Amount wire_fee3;
  struct TALER_Amount closing_fee3;
  struct GNUNET_TIME_Absolute date1;
  struct GNUNET_TIME_Absolute date2;
  struct GNUNET_TIME_Absolute date3;
  struct GNUNET_TIME_Absolute start_date;
  struct GNUNET_TIME_Absolute end_date;
  struct TALER_MasterSignatureP exchange_sig;
  struct TALER_MasterSignatureP exchange_sig2;

  RND_BLK (&exchange_pub);
  RND_BLK (&h_wire_method);
  RND_BLK (&exchange_sig);
  date1 = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&date1);
  date2 = GNUNET_TIME_absolute_add (date1,
				    GNUNET_TIME_UNIT_DAYS);
  date3 = GNUNET_TIME_absolute_add (date2,
				    GNUNET_TIME_UNIT_DAYS);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":5",
                                         &closing_fee1));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":4",
                                         &wire_fee1));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":3",
                                         &closing_fee2));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":2",
                                         &wire_fee2));
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->store_wire_fee_by_exchange (plugin->cls,
					  &exchange_pub,
					  &h_wire_method,
					  &wire_fee1,
					  &closing_fee1,
					  date1,
					  date2,
					  &exchange_sig))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->store_wire_fee_by_exchange (plugin->cls,
					  &exchange_pub,
					  &h_wire_method,
					  &wire_fee2,
					  &closing_fee2,
					  date2,
					  date3,
					  &exchange_sig))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  contract_date = date2; /* test inclusive/exclusive range */
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->lookup_wire_fee (plugin->cls,
			       &exchange_pub,
			       &h_wire_method,
			       contract_date,
			       &wire_fee3,
			       &closing_fee3,
			       &start_date,
			       &end_date,
			       &exchange_sig2))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if ( (start_date.abs_value_us != date2.abs_value_us) ||
       (end_date.abs_value_us != date3.abs_value_us) ||
       (0 != GNUNET_memcmp (&exchange_sig,
                            &exchange_sig2)) ||
       (0 != TALER_amount_cmp (&wire_fee2,
			       &wire_fee3)) ||
       (0 != TALER_amount_cmp (&closing_fee2,
			       &closing_fee3)) )
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  contract_date = GNUNET_TIME_absolute_add (date1,
					    GNUNET_TIME_UNIT_SECONDS);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->lookup_wire_fee (plugin->cls,
			       &exchange_pub,
			       &h_wire_method,
			       contract_date,
			       &wire_fee3,
			       &closing_fee3,
			       &start_date,
			       &end_date,
			       &exchange_sig2))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if ( (start_date.abs_value_us != date1.abs_value_us) ||
       (end_date.abs_value_us != date2.abs_value_us) ||
       (0 != GNUNET_memcmp (&exchange_sig,
                            &exchange_sig2)) ||
       (0 != TALER_amount_cmp (&wire_fee1,
			       &wire_fee3)) ||
       (0 != TALER_amount_cmp (&closing_fee1,
			       &closing_fee3)) )
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  contract_date = date3; /* outside of valid range! */
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS !=
      plugin->lookup_wire_fee (plugin->cls,
			       &exchange_pub,
			       &h_wire_method,
			       contract_date,
			       &wire_fee3,
			       &closing_fee3,
			       &start_date,
			       &end_date,
			       &exchange_sig2))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Test APIs related to tipping.
 *
 * @return #GNUNET_OK upon success
 */
static int
test_tipping ()
{
  struct TALER_ReservePrivateKeyP tip_reserve_priv;
  struct TALER_ReservePrivateKeyP pres;
  struct GNUNET_HashCode tip_id;
  struct GNUNET_HashCode tip_credit_uuid;
  struct GNUNET_HashCode pickup_id;
  struct GNUNET_TIME_Absolute tip_expiration;
  struct GNUNET_TIME_Absolute reserve_expiration;
  struct TALER_Amount total;
  struct TALER_Amount amount;
  struct TALER_Amount inc;
  char *url;

  RND_BLK (&tip_reserve_priv);
  if (TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS !=
      plugin->authorize_tip_TR (plugin->cls,
                                "testing tips reserve unknown",
                                &amount,
                                &tip_reserve_priv,
                                "http://localhost:8081/",
                                &tip_expiration,
                                &tip_id))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  RND_BLK (&tip_credit_uuid);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":5",
                                         &total));
  /* Pick short expiration, but long enough to
     run 2 DB interactions even on very slow systems. */
  reserve_expiration = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS,
                                                                                        2));
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->enable_tip_reserve_TR (plugin->cls,
                                     &tip_reserve_priv,
                                     &tip_credit_uuid,
                                     &total,
                                     reserve_expiration))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  /* check idempotency */
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS !=
      plugin->enable_tip_reserve_TR (plugin->cls,
                                     &tip_reserve_priv,
                                     &tip_credit_uuid,
                                     &total,
                                     reserve_expiration))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  /* Make sure it has expired, so at this point the value is back at ZERO! */
  sleep (3);
  if (TALER_EC_TIP_AUTHORIZE_RESERVE_EXPIRED !=
      plugin->authorize_tip_TR (plugin->cls,
                                "testing tips too late",
                                &amount,
                                &tip_reserve_priv,
                                "http://localhost:8081/",
                                &tip_expiration,
                                &tip_id))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* Re-add some funds again */
  RND_BLK (&tip_credit_uuid);
  reserve_expiration = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS,
                                                                                        2));
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->enable_tip_reserve_TR (plugin->cls,
                                     &tip_reserve_priv,
                                     &tip_credit_uuid,
                                     &total,
                                     reserve_expiration))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  /* top it up by adding more with a fresh UUID
     and even longer expiration time (until end of test) */
  RND_BLK (&tip_credit_uuid);
  reserve_expiration = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_UNIT_DAYS);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->enable_tip_reserve_TR (plugin->cls,
                                     &tip_reserve_priv,
                                     &tip_credit_uuid,
                                     &total,
                                     reserve_expiration))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* Now authorize some tips */
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":4",
                                         &amount));
  if (TALER_EC_NONE !=
      plugin->authorize_tip_TR (plugin->cls,
                                "testing tips",
                                &amount,
                                &tip_reserve_priv,
                                "http://localhost:8081/",
                                &tip_expiration,
                                &tip_id))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (tip_expiration.abs_value_us != reserve_expiration.abs_value_us)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
      plugin->lookup_tip_by_id (plugin->cls,
				      &tip_id,
				      &url,
                                      NULL, NULL))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (0 != strcmp ("http://localhost:8081/",
		   url))
  {
    GNUNET_free (url);
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  GNUNET_free (url);
  if (TALER_EC_NONE !=
      plugin->authorize_tip_TR (plugin->cls,
                                "testing tips more",
                                &amount,
                                &tip_reserve_priv,
                                "http://localhost:8081/",
                                &tip_expiration,
                                &tip_id))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (tip_expiration.abs_value_us != reserve_expiration.abs_value_us)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* Let's try to pick up the authorized tip in 2 increments */
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":2",
                                         &inc));
  RND_BLK (&pickup_id);
  if (TALER_EC_NONE !=
      plugin->pickup_tip_TR (plugin->cls,
                             &inc,
                             &tip_id,
                             &pickup_id,
                             &pres))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (0 != GNUNET_memcmp (&pres,
                          &tip_reserve_priv))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  RND_BLK (&pickup_id);
  if (TALER_EC_NONE !=
      plugin->pickup_tip_TR (plugin->cls,
                             &inc,
                             &tip_id,
                             &pickup_id,
                             &pres))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  if (0 != GNUNET_memcmp (&pres,
                          &tip_reserve_priv))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* Third attempt should fail, as we've picked up 4/4 in amount */
  RND_BLK (&pickup_id);
  if (TALER_EC_TIP_PICKUP_NO_FUNDS !=
      plugin->pickup_tip_TR (plugin->cls,
                             &inc,
                             &tip_id,
                             &pickup_id,
                             &pres))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* We authorized 8 out of 10, so going for another 4 should fail with insufficient funds */
  if (TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS !=
      plugin->authorize_tip_TR (plugin->cls,
                                "testing tips insufficient funds",
                                &amount,
                                &tip_reserve_priv,
                                "http://localhost:8081/",
                                &tip_expiration,
                                &tip_id))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  /* Test that picking up with random (unauthorized) tip_id fails as well */
  RND_BLK (&tip_id);
  RND_BLK (&pickup_id);
  if (TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN !=
      plugin->pickup_tip_TR (plugin->cls,
                             &inc,
                             &tip_id,
                             &pickup_id,
                             &pres))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
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
  struct GNUNET_TIME_Absolute fake_now;
  json_t *out;
  /* Data for 'store_payment()' */

  if (NULL == (plugin = TALER_MERCHANTDB_plugin_load (cfg)))
  {
    result = 77;
    return;
  }

  if (GNUNET_OK != plugin->drop_tables (plugin->cls))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Dropping tables failed\n");
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
  RND_BLK (&h_contract_terms);
  order_id = "test_ID";
  order_id_future = "test_ID_future";
  RND_BLK (&signkey_pub);
  RND_BLK (&merchant_pub);
  RND_BLK (&wtid);
  timestamp = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&timestamp);
  delta = GNUNET_TIME_UNIT_MINUTES;
  fake_now = GNUNET_TIME_absolute_add (timestamp, delta);
  refund_deadline = GNUNET_TIME_absolute_get();
  (void) GNUNET_TIME_round_abs (&refund_deadline);
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":5",
                                         &amount_with_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &deposit_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000001",
                                         &wire_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":0.000010",
                                         &refund_fee));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":2",
                                         &refund_amount));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":1",
                                         &little_refund_amount));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":3",
                                         &right_second_refund_amount));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (CURRENCY ":30",
                                         &too_big_refund_amount));
  RND_BLK (&coin_pub);
  deposit_proof = json_object ();
  GNUNET_assert (0 ==
                 json_object_set_new (deposit_proof,
                                      "x-taler-bank",
                                      json_string ("backenddb test A")));
  transfer_proof = json_object ();
  GNUNET_assert (0 ==
                 json_object_set_new (transfer_proof,
                                      "x-taler-bank",
                                      json_string ("backenddb test B")));
  contract = json_object ();
  contract_terms = json_object ();
  GNUNET_assert (0 ==
                 json_object_set_new (contract_terms,
                                      "order",
                                      json_string ("1")));

  contract_terms_future = json_object ();
  GNUNET_assert (0 ==
                 json_object_set_new (contract_terms_future,
                                      "order",
                                      json_string ("2")));

  GNUNET_assert (GNUNET_OK ==
                 TALER_JSON_hash (contract_terms,
                                  &h_contract_terms));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->insert_contract_terms (plugin->cls,
					 order_id,
					 &merchant_pub,
					 timestamp,
					 contract_terms));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS !=
          plugin->find_paid_contract_terms_from_hash (plugin->cls,
                                                      &out,
                                                      &h_contract_terms,
                                                      &merchant_pub));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->mark_proposal_paid (plugin->cls,
                                      &h_contract_terms,
                                      &merchant_pub,
                                      "my-session-123"));


  {
    char *last_session_id;
    FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
            plugin->find_contract_terms (plugin->cls,
                                         &out,
                                         &last_session_id,
                                         order_id,
                                         &merchant_pub));
    FAILIF (0 != strcmp (last_session_id, "my-session-123"));
    GNUNET_free (last_session_id);
  }

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->mark_proposal_paid (plugin->cls,
                                      &h_contract_terms,
                                      &merchant_pub,
                                      NULL));


  {
    char *last_session_id;
    FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
            plugin->find_contract_terms (plugin->cls,
                                         &out,
                                         &last_session_id,
                                         order_id,
                                         &merchant_pub));
    FAILIF (0 != strcmp (last_session_id, ""));
    GNUNET_free (last_session_id);
  }

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
	  plugin->find_contract_terms_history (plugin->cls,
					       order_id,
					       &merchant_pub,
					       &pd_cb,
					       NULL));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->find_paid_contract_terms_from_hash (plugin->cls,
                                                      &out,
                                                      &h_contract_terms,
                                                      &merchant_pub));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->find_contract_terms_from_hash (plugin->cls,
						 &out,
						 &h_contract_terms,
						 &merchant_pub));
  FAILIF (1 !=
          plugin->find_contract_terms_by_date_and_range (plugin->cls,
							 fake_now,
							 &merchant_pub,
							 2,
							 1,
							 GNUNET_YES,
							 GNUNET_NO,
							 &pd_cb,
							 NULL));
  timestamp = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&timestamp);

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->insert_contract_terms (plugin->cls,
					 order_id_future,
					 &merchant_pub,
					 timestamp,
					 contract_terms_future));

  fake_now = GNUNET_TIME_absolute_subtract (timestamp, delta);

  GNUNET_assert (GNUNET_OK ==
                 TALER_JSON_hash (contract_terms_future,
                                  &h_contract_terms_future));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->mark_proposal_paid (plugin->cls,
                                      &h_contract_terms_future,
                                      &merchant_pub,
                                      "hello"));
  FAILIF (2 !=
          plugin->find_contract_terms_by_date_and_range (plugin->cls,
							 fake_now,
							 &merchant_pub,
							 0,
							 5,
							 GNUNET_NO,
							 GNUNET_NO,
							 &pd_cb,
							 NULL));

  FAILIF (0 !=
          plugin->find_contract_terms_by_date (plugin->cls,
					       fake_now,
					       &merchant_pub,
					       1,
					       &pd_cb,
					       NULL));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->store_deposit (plugin->cls,
                                 &h_contract_terms,
				 &merchant_pub,
                                 &coin_pub,
				 EXCHANGE_URL,
                                 &amount_with_fee,
                                 &deposit_fee,
                                 &refund_fee,
				 &wire_fee,
                                 &signkey_pub,
                                 deposit_proof));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->store_coin_to_transfer (plugin->cls,
                                          &h_contract_terms,
                                          &coin_pub,
                                          &wtid));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->store_transfer_to_proof (plugin->cls,
                                           EXCHANGE_URL,
                                           &wtid,
                                           GNUNET_TIME_UNIT_ZERO_ABS,
                                           &signkey_pub,
                                           transfer_proof));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->find_payments (plugin->cls,
                                 &h_contract_terms,
                                 &merchant_pub,
                                 &deposit_cb,
                                 NULL));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->find_transfers_by_hash (plugin->cls,
                                          &h_contract_terms,
                                          &transfer_cb,
                                          NULL));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->find_deposits_by_wtid (plugin->cls,
                                         &wtid,
                                         &deposit_cb,
                                         NULL));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->find_proof_by_wtid (plugin->cls,
                                      EXCHANGE_URL,
                                      &wtid,
                                      &proof_cb,
                                      NULL));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS !=
          plugin->get_refunds_from_contract_terms_hash (plugin->cls,
                                                        &merchant_pub,
                                                        &h_contract_terms,
                                                        &refund_cb,
                                                        NULL));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->increase_refund_for_contract_NT (plugin->cls,
                                                   &h_contract_terms,
                                                   &merchant_pub,
                                                   &refund_amount,
                                                   "refund testing"));

  FAILIF (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS !=
          plugin->increase_refund_for_contract_NT (plugin->cls,
                                                   &h_contract_terms,
                                                   &merchant_pub,
                                                   &refund_amount,
                                                   "same refund amount as "
                                                   "the previous one, should succeed without changes (0)"));

  /*Should fail as this refund a lesser amount respect to the previous one*/
  FAILIF (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS !=
          plugin->increase_refund_for_contract_NT (plugin->cls,
                                                   &h_contract_terms,
                                                   &merchant_pub,
                                                   &little_refund_amount,
                                                   "lower refund amount as the previous one, should succeed without changes (0)"));
  FAILIF (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          plugin->increase_refund_for_contract_NT (plugin->cls,
                                                   &h_contract_terms,
                                                   &merchant_pub,
                                                   &right_second_refund_amount,
                                                   "right refund increase"));

  FAILIF (GNUNET_DB_STATUS_HARD_ERROR !=
          plugin->increase_refund_for_contract_NT (plugin->cls,
                                                   &h_contract_terms,
                                                   &merchant_pub,
                                                   &too_big_refund_amount,
                                                   "make refund testing fail due"
                                                   " to too big refund amount"));

  FAILIF (GNUNET_OK !=
	  test_wire_fee ());
  FAILIF (GNUNET_OK !=
	  test_tipping ());


  if (-1 == result)
    result = 0;

 drop:
  GNUNET_break (GNUNET_OK ==
		plugin->drop_tables (plugin->cls));
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
