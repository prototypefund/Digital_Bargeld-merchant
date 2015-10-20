/**
* Verify the signature on a successful deposit permission
* @param h_contract the hashed stringification of this contract
* @param h_wire the hashed 'wire' object holdign the merchant bank's details
* @param timestamp the 32bit wide number representing the number of seconds
* since the Epoch
* @param refund the refund deadline for this deal, expressed in seconds as @a
* timestamp
* @param trans_id an id number for this deal
* @param amount_minus_fee what paid minus its deposit fee
* @param coin_pub the coin's public key
* @param sig the mint's signature 
* @param mint_pub mint's key to verify this signature against
* @return GNUNET_OK if the verification succeeds, GNUNET_NO if not,
* GNUNET_SYSERR upon errors
*/

uint32_t
MERCHANT_verify_confirmation (const struct GNUNET_HashCode *h_contract,
                              const struct GNUNET_HashCode *h_wire,
			      struct GNUNET_TIME_Absolute timestamp,
			      struct GNUNET_TIME_Absolute refund,
			      uint64_t trans_id,
		              const struct TALER_Amount *amount_minus_fee,
		              const struct TALER_CoinSpendPublicKeyP *coin,
		              const struct TALER_MerchantPublicKeyP *merchant,
                              const struct GNUNET_CRYPTO_EddsaSignature *sig,
		              const struct TALER_MintPublicKeyP *mint_pub);
