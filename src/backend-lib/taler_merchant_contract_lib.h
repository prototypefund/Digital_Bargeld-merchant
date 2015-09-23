/**
 * Simplified version of the contract to be signed, meant to obsolete
 * 'struct ContractNBO'.
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

GNUNET_NETWORK_STRUCT_BEGIN

struct ContractNBO
{
  /**
   * Purpose header for the signature over contract
   */
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;

  /**
   * The transaction identifier. NOTE: it was m[13]. TODO:
   * change the API accordingly!
   */
  uint64_t m;

  /**
   * Expiry time
   */
  struct GNUNET_TIME_AbsoluteNBO t;

  /**
   * The invoice amount
   */
  struct TALER_AmountNBO amount;

  /**
   * The hash of the merchant's wire details (bank account information), with a nounce
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Hash of the JSON contract in UTF-8 including 0-termination,
   * using JSON_COMPACT encoding with sorted fields.
   */
  struct GNUNET_HashCode h_contract_details;

};

GNUNET_NETWORK_STRUCT_END

/**
* Take from the frontend the (partly) generated contract and fill
* the missing values in it; for example, the SEPA-aware values.
* Moreover, it stores the contract in the DB and does the signature of it.
* @param contract parsed contract, originated by the frontend
* @param db_conn the handle to the local DB
* @param kpriv merchant's private key
* @param wire merchant's bank's details
* @param sig where to store the signature
* @return GNUNET_OK if successful, GNUNET_SYSERR upon errors
*
*/

/* TODO: this handles a simplified version (for debugging purposes)
         of the contract. To expand to the full fledged version */

uint32_t
MERCHANT_handle_contract (json_t *j_contract,
                          PGconn *db_conn,
                          const struct MERCHANT_WIREFORMAT_Sepa *wire,
                          struct Contract *contract);
