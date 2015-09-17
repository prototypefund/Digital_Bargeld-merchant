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
* the missing values in it; for example, the SEPA-aware values.
* Moreover, it stores the contract in the DB.
* @param j_contract parsed contract, originated by the frontend
* @param db_conn the handle to the local DB
* @param wire merchant's bank's details
* @param where to store the (subset of the) contract to be (still) signed
* @return pointer to the complete JSON; NULL upon errors
*/

/**
* TODO: inspection of reference counting and, accordingly, free those json_t*(s)
* still allocated */

json_t *
MERCHANT_handle_contract (json_t *j_contract,
                          PGconn *db_conn,
                          const struct MERCHANT_WIREFORMAT_Sepa *wire,
			  struct ContractNBO *contract)
{
  json_t *root;
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
  char *h_wire_enc;
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
  h_wire_enc = GNUNET_STRINGS_data_to_string_alloc (&h_wire,
                                                    sizeof (struct GNUNET_HashCode));
  jh_wire = json_string (h_wire_enc);
  if (-1 == json_unpack (j_contract, "{s:o, s:o, s:I, s:o, s:o}",
                         "amount", &j_amount,
                         "max fee", &j_max_fee,
                         "trans_id", &j_trans_id,
			 "mints", &j_mints,
                         "details", &j_details))

    return NULL;

  /* needed for DB stuff */
  TALER_json_to_amount (j_amount, &amount);
  j_items_tmp = json_object_get (j_details, "items");
  j_product_id = json_object_get (j_items_tmp, "product_id");

  #ifdef DEBUG
  printf ("prod id is at %p, eval to %d\n", j_product_id, json_integer_value (j_product_id));
  return NULL;
  #endif




  /* adding the generated values in this JSON */
  root = json_pack ("{s:o, s:o, s:I, s:s, s:o, s:o, s:o}",
             "amount", j_amount,
             "max fee", j_max_fee,
	     "trans_id", j_trans_id,
	     "h_wire", jh_wire,
	     "timestamp", j_timestamp,
             "mints", j_mints,
	     "details", j_details);

  a = json_dumps (root, JSON_COMPACT | JSON_PRESERVE_ORDER);

  // DB mgmt
  if (GNUNET_SYSERR == MERCHANT_DB_contract_create (db_conn,
                                                    &timestamp,
                                                    &amount,
 					            (uint64_t) j_trans_id, // safe?
                                                    a,
                                                    nounce,
                                                    json_integer_value (j_product_id)));
  contract->h_wire = h_wire;
  TALER_amount_hton (&amount_nbo, &amount);
  contract->amount = amount_nbo;
  contract->t = GNUNET_TIME_absolute_hton (timestamp);
  contract->m = GNUNET_htonll ((uint64_t) j_trans_id); // safe?
  GNUNET_CRYPTO_hash (a, strlen (a) + 1, &contract->h_contract_details);
  free (a);
  contract->purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract->purpose.size = htonl (sizeof (struct ContractNBO));

  return root;
}
