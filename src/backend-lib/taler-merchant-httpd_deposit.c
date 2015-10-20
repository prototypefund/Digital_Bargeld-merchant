#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_util.h>
#include "merchant.h"
#include "merchant_db.h"
#include "taler_merchant_contract_lib.h"

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
		              const struct TALER_MintPublicKeyP *mint_pub)
{
  struct TALER_DepositConfirmationPS dc;

  dc.h_contract = *h_contract;
  dc.h_wire = *h_wire;

  dc.merchant = *merchant;
  dc.coin_pub = *coin;

  dc.timestamp = GNUNET_TIME_absolute_hton (timestamp);
  dc.refund_deadline = GNUNET_TIME_absolute_hton (refund);
  TALER_amount_hton (&dc.amount_without_fee, amount_minus_fee);
  dc.transaction_id = GNUNET_htonll (trans_id);

  #ifdef DEBUG
  char *hwire_enc;
  char *hcontract_enc;
  char *merchant_enc;
  char *coinpub_enc;

  hwire_enc = GNUNET_STRINGS_data_to_string_alloc (h_wire, sizeof (struct GNUNET_HashCode));
  hcontract_enc = GNUNET_STRINGS_data_to_string_alloc (h_contract, sizeof (struct GNUNET_HashCode));
  merchant_enc = GNUNET_STRINGS_data_to_string_alloc (&merchant.eddsa_pub, sizeof (struct GNUNET_CRYPTO_EddsaPublicKey));
  coinpub_enc = GNUNET_STRINGS_data_to_string_alloc (&coin.eddsa_pub, sizeof (struct GNUNET_CRYPTO_EddsaPublicKey));

  printf ("Signing Confirmation:\nH_wire: %s\nH_contract: %s\nmerchant_pub: %s\ncoin_pub: %s\n"
          "timestamp: %llu,\nrefund: %llu,\namount: %s %llu.%lu,\ntrid: %llu\n",
          hwire_enc,
          hcontract_enc,
          merchant_enc,
          coinpub_enc,
          timestamp_abs.abs_value_us,
          refund_abs.abs_value_us,
          amount_minus_fee->currency,
          amount_minus_fee->value,
          amount_minus_fee->fraction,
          trans_id);
  #endif

  dc.purpose.purpose = htonl (TALER_SIGNATURE_MINT_CONFIRM_DEPOSIT);
  dc.purpose.size = htonl (sizeof (struct TALER_DepositConfirmationPS));

  if (GNUNET_SYSERR ==
      GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MINT_CONFIRM_DEPOSIT,
                                  &dc.purpose,
                                  sig,
				  &mint_pub->eddsa_pub))
    return GNUNET_NO;
  return GNUNET_OK;
}

