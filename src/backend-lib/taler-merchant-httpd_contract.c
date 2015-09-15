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
* Moreover, it stores the contract in the DB and does the signature of it.
* @param contract parsed contract, originated by the frontend
* @param db_conn the handle to the local DB
* @param kpriv merchant's private key
* @param wire merchant's bank's details
* @param sig where to store the signature
* @return pointer to the complete JSON; NULL upon errors
*/

/* TODO: this handles a simplified version (for debugging purposes)
         of the contract. To expand to the full fledged version */

json_t *
MERCHANT_handle_contract (json_t *contract,
                          PGconn *db_conn,
                          struct GNUNET_CRYPTO_EddsaPrivateKey *kpriv,
                          const struct MERCHANT_WIREFORMAT_Sepa *wire,
			  struct GNUNET_CRYPTO_EddsaSignature *sig)
{
  json_t *root;
  json_t *j_timestamp;
  json_t *jh_wire;
  json_t *j_amount;
  json_int_t j_trans_id;

  uint64_t nounce;
  uint64_t product_id;
  char *a;
  char *h_wire_enc;
  char *str;
  struct GNUNET_HashCode h_wire;
  struct GNUNET_TIME_Absolute timestamp;
  struct TALER_Amount amount;
  struct TALER_AmountNBO amount_nbo;
  struct ContractNBO contract_nbo;

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

  str = json_dumps (contract, JSON_INDENT(2) | JSON_PRESERVE_ORDER);


  json_unpack (contract, "{s:o, s:I}",
               "amount", &j_amount,
	       "trans_id", &j_trans_id);

  /* needed for DB stuff */
  TALER_json_to_amount (j_amount, &amount);
  /* temporary way of getting this value. To be adapted to the expanded contract
  format. See 'TODO' above. */
  product_id = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);
  /* adding the generated values in this JSON */
  root = json_pack ("{s:o, s:I, s:s, s:o}",
             "amount", j_amount,
	     "trans_id", j_trans_id,
	     "h_wire", jh_wire,
	     "timestamp", j_timestamp);
  a = json_dumps (root, JSON_COMPACT | JSON_PRESERVE_ORDER);

  // DB mgmt
  if (GNUNET_SYSERR == MERCHANT_DB_contract_create (db_conn,
                                                    &timestamp,
                                                    &amount,
 					            (uint64_t) j_trans_id, // safe?
                                                    a,
                                                    nounce,
                                                    product_id));
  contract_nbo.h_wire = h_wire;
  TALER_amount_hton (&amount_nbo, &amount);
  contract_nbo.amount = amount_nbo;
  contract_nbo.t = GNUNET_TIME_absolute_hton (timestamp);
  contract_nbo.m = GNUNET_htonll ((uint64_t) j_trans_id); // safe?
  GNUNET_CRYPTO_hash (a, strlen (a) + 1, &contract_nbo.h_contract_details);
  free (a);
  contract_nbo.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract_nbo.purpose.size = htonl (sizeof (struct ContractNBO));
  GNUNET_CRYPTO_eddsa_sign (kpriv, &contract_nbo.purpose, sig);

  return root;
}
