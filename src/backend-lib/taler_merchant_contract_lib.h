/**
 * Take the global wire details and return a JSON containing them,
 * compliantly with the Taler's API.
 * @param wire the merchant's wire details
 * @param salt the nounce for hashing the wire details with
 * @return JSON representation of the wire details, NULL upon errors
 */

json_t *
MERCHANT_get_wire_json (const struct MERCHANT_WIREFORMAT_Sepa *wire,
                        uint64_t salt);
