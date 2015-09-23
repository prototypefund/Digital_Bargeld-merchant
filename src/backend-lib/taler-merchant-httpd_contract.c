#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <gnunet/gnunet_util_lib.h>
#include "merchant.h"
#include "merchant_db.h"
#include "taler_merchant_contract_lib.h"


/* TODO: make this file a library, and programmatically call the following
 * functions */

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
* Generate the hash containing the information (= a nounce + merchant's IBAN) to
* redeem money from mint in a subsequent /deposit operation
* @param nounce the nounce
* @param wire the merchant's wire details
* @return the hash to be included in the contract's blob
*
*/

static struct GNUNET_HashCode
hash_wireformat (uint64_t nounce, const struct MERCHANT_WIREFORMAT_Sepa *wire)
{
  struct GNUNET_HashContext *hc;
  struct GNUNET_HashCode hash;

  hc = GNUNET_CRYPTO_hash_context_start ();
  GNUNET_CRYPTO_hash_context_read (hc, wire->iban, strlen (wire->iban));
  GNUNET_CRYPTO_hash_context_read (hc, wire->name, strlen (wire->name));
  GNUNET_CRYPTO_hash_context_read (hc, wire->bic, strlen (wire->bic));
  nounce = GNUNET_htonll (nounce);
  GNUNET_CRYPTO_hash_context_read (hc, &nounce, sizeof (nounce));
  GNUNET_CRYPTO_hash_context_finish (hc, &hash);
  return hash;
}

/**
* Take from the frontend the (partly) generated contract and fill
* the missing values in it; for example, the SEPA details.
* Moreover, it stores the contract in the DB.
* @param j_contract parsed contract, originated by the frontend. It will be
* hold the new values.
* @param db_conn the handle to the local DB
* @param wire merchant's bank's details
* @param contract where to store the (subset of the) contract to be (still) signed
* @return GNUNET_OK on success; GNUNET_SYSERR upon errors
*/

/**
* TODO: inspect reference counting and, accordingly, free those json_t*(s)
* still allocated */

uint32_t
MERCHANT_handle_contract (json_t *j_contract,
                          PGconn *db_conn,
                          const struct MERCHANT_WIREFORMAT_Sepa *wire,
			  struct Contract *contract)
{
  json_t *j_tmp;
  json_t *j_details;
  json_t *j_timestamp;
  json_t *jh_wire;
  json_t *j_amount;
  json_t *j_mints;
  json_t *j_max_fee;
  json_int_t j_trans_id;

  uint64_t nounce;
  json_t *j_product_id;
  json_t *j_items_tmp;
  char *a;
  #define DaEBUG
  #ifdef DEBUG
  char *str;
  #endif
  struct GNUNET_HashCode h_wire;
  struct GNUNET_TIME_Absolute timestamp;
  struct TALER_Amount amount;
  struct TALER_AmountNBO amount_nbo;

  nounce = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);
  // timing mgmt
  timestamp = GNUNET_TIME_absolute_add (GNUNET_TIME_absolute_get (),
                                     GNUNET_TIME_UNIT_DAYS);
  ROUND_TO_SECS (timestamp, abs_value_us);
  j_timestamp = TALER_json_from_abs (timestamp);
  // wireformat mgmt
  h_wire = hash_wireformat (nounce, wire);
  jh_wire = TALER_json_from_data (&h_wire, sizeof (struct GNUNET_HashCode));
  /* adding the generated values in this JSON */
  if (NULL == (j_tmp = json_pack ("{s:o, s:o}",
                                 "h_wire", jh_wire,
                                 "timestamp", j_timestamp)))
    return GNUNET_SYSERR;

  if (-1 == json_object_update (j_contract, j_tmp))
    return GNUNET_SYSERR;
  /* needed for DB work */
  j_amount = json_object_get (j_contract, "amount");
  TALER_json_to_amount (j_amount, &amount); // produces a WARNING..

  a = json_dumps (j_contract, JSON_COMPACT | JSON_PRESERVE_ORDER);

  #ifdef DEBUG
  str = json_dumps (j_amount, JSON_COMPACT | JSON_PRESERVE_ORDER);
  printf ("amount : \n%s", str);
  return GNUNET_SYSERR;
  #endif

  /* TODO

  Add a further field to the 'contract' table, indicating the timestamp
  of this contract being finalized

  */

  GNUNET_CRYPTO_hash (a, strlen (a) + 1, &contract->h_contract_details);
  contract->purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract->purpose.size = htonl (sizeof (struct Contract));

  // DB mgmt
  if (GNUNET_SYSERR == MERCHANT_DB_contract_create (db_conn,
                                                    &timestamp,
                                                    &amount,
                                                    &contract->h_contract_details,
 					            (uint64_t) j_trans_id, // safe?
                                                    a,
                                                    nounce,
                                                    json_integer_value (j_product_id)))
    return GNUNET_SYSERR;

  free (a);

  #ifdef OBSOLETE
  contract->h_wire = h_wire;
  TALER_amount_hton (&amount_nbo, &amount);
  contract->amount = amount_nbo;
  contract->t = GNUNET_TIME_absolute_hton (timestamp);
  contract->m = GNUNET_htonll ((uint64_t) j_trans_id); // safe?
  #endif

  #ifdef OBSOLETE
  contract->purpose.size = htonl (sizeof (struct ContractNBO));
  #endif

  return GNUNET_OK;
}
