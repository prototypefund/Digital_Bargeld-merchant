#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <gnunet/gnunet_util_lib.h>
#include "merchant.h"
#include "merchant_db.h"
#include "taler_merchant_contract_lib.h"

/**
 * Take the global wire details and return a JSON containing them,
 * compliantly with the Taler's API.
 * @param wire the merchant's wire details
 * @param nounce the nounce for hashing the wire details with
 * @param edate when the beneficiary wants this transfer to take place
 * @return JSON representation of the wire details, NULL upon errors
 */

json_t *
MERCHANT_get_wire_json (const struct MERCHANT_WIREFORMAT_Sepa *wire,
                        uint64_t nounce,
                        const struct GNUNET_TIME_Absolute edate)

{
  
  json_t *root;
  json_t *j_edate;
  json_t *j_nounce;

  j_nounce = json_integer (nounce);
  j_edate = TALER_json_from_abs (edate);

  if (NULL == (root = json_pack ("{s:s, s:s, s:s, s:s, s:o, s:I}",
                                 "type", "SEPA",
		                 "IBAN", wire->iban,
		                 "name", wire->name,
		                 "bic", wire->bic,
		                 "edate", j_edate,
		                 "r", json_integer_value (j_nounce))))
    return NULL;

  return root;
}



/**
* Take from the frontend the (partly) generated contract and fill
* the missing values in it; for example, the SEPA details.
* Moreover, it stores the contract in the DB.
* @param j_contract parsed contract, originated by the frontend. It will be
* hold the new values.
* @param db_conn the handle to the local DB
* @param contract where to store the (subset of the) contract to be (still) signed
* @param timestamp contract's timestamp (shall be generated by the merchant)
* @param expiry the time when the contract will expire
* @param edate when the merchant wants to receive the wire transfer corresponding
* to this deal (this value is also a field inside the 'wire' JSON format)
* @param refund deadline until which the merchant can return the paid amount
* @param nounce the nounce used to hash the wire details
* @param a will be pointed to the (allocated) stringified 0-terminated contract
* @return GNUNET_OK on success, GNUNET_NO if attempting to double insert the
* same contract, GNUNET_SYSERR in case of other (mostly DB related) errors.
*/

/**
* TODO: inspect reference counting and, accordingly, free those json_t*(s)
* still allocated */

uint32_t
MERCHANT_handle_contract (json_t *j_contract,
                          PGconn *db_conn,
			  struct Contract *contract,
			  struct GNUNET_TIME_Absolute timestamp,
			  struct GNUNET_TIME_Absolute expiry,
			  struct GNUNET_TIME_Absolute edate,
			  struct GNUNET_TIME_Absolute refund,
			  char **a,
			  uint64_t nounce)
{
  json_t *j_amount;
  json_int_t j_product_id;
  json_int_t j_trans_id;
  char *contract_str;

  struct TALER_Amount amount;



  /* Extracting values useful for DB work. Only gettable from the JSON
  since they are generated by the frontend */
  if (-1 == json_unpack (j_contract, "{s:o, s:I, s:{s:I}}",
                         "amount", &j_amount,
			 "trans_id", &j_trans_id,
			 "details", "product_id",
			 &j_product_id))
  {
    printf ("no unpacking\n");
    return GNUNET_SYSERR;
  }

  TALER_json_to_amount (j_amount, &amount);
  contract_str = json_dumps (j_contract, JSON_COMPACT | JSON_PRESERVE_ORDER);
  *a = contract_str;
  GNUNET_CRYPTO_hash (contract_str, strlen (contract_str) + 1,
                      &contract->h_contract_details);
  contract->purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract->purpose.size = htonl (sizeof (struct Contract));

  // DB mgmt
  return MERCHANT_DB_contract_create (db_conn,
                                      timestamp,
                                      expiry,
	                              edate,
				      refund,
                                      &amount,
                                      &contract->h_contract_details,
 				      (uint64_t) j_trans_id, // safe?
                                      contract_str,
                                      nounce,
                                      (uint64_t) j_product_id);
}
