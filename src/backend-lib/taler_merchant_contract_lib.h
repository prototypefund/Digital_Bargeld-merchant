/**
 * The contract sent by the merchant to the wallet
 */
struct Contract
{
  /**
   * Purpose header for the signature over contract
   */
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;

  /**
   * Hash of the JSON contract in UTF-8 including 0-termination,
   * using JSON_COMPACT encoding with sorted fields.
   */
  struct GNUNET_HashCode h_contract_details;

};

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
                        struct GNUNET_TIME_Absolute edate);

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
			  uint64_t nounce);
