/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_json_lib.h
 * @brief helper functions for JSON processing using libjansson
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#ifndef TALER_json_LIB_H_
#define TALER_json_LIB_H_

#include <jansson.h>

/**
 * Print JSON parsing related error information
 */
#define TALER_json_warn(error)                                         \
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,                                \
              "JSON parsing failed at %s:%u: %s (%s)\n",                  \
              __FILE__, __LINE__, error.text, error.source)


/**
 * Convert a TALER amount to a JSON object.
 *
 * @param amount the amount
 * @return a json object describing the amount
 */
json_t *
TALER_json_from_amount (const struct TALER_Amount *amount);


/**
 * Convert absolute timestamp to a json string.
 *
 * @param stamp the time stamp
 * @return a json string with the timestamp in @a stamp
 */
json_t *
TALER_json_from_abs (struct GNUNET_TIME_Absolute stamp);


/**
 * Convert a signature (with purpose) to a JSON object representation.
 *
 * @param purpose purpose of the signature
 * @param signature the signature
 * @return the JSON reporesentation of the signature with purpose
 */
json_t *
TALER_json_from_eddsa_sig (const struct GNUNET_CRYPTO_EccSignaturePurpose *purpose,
                           const struct GNUNET_CRYPTO_EddsaSignature *signature);


/**
 * Convert RSA public key to JSON.
 *
 * @param pk public key to convert
 * @return corresponding JSON encoding
 */
json_t *
TALER_json_from_rsa_public_key (struct GNUNET_CRYPTO_rsa_PublicKey *pk);


/**
 * Convert RSA signature to JSON.
 *
 * @param sig signature to convert
 * @return corresponding JSON encoding
 */
json_t *
TALER_json_from_rsa_signature (struct GNUNET_CRYPTO_rsa_Signature *sig);


/**
 * Convert binary data to a JSON string
 * with the base32crockford encoding.
 *
 * @param data binary data
 * @param size size of @a data in bytes
 * @return json string that encodes @a data
 */
json_t *
TALER_json_from_data (const void *data, size_t size);


/**
 * Parse given JSON object to Amount
 *
 * @param json the json object representing Amount
 * @param[out] r_amount where the amount has to be written
 * @return #GNUNET_OK upon successful parsing; #GNUNET_SYSERR upon error
 */
int
TALER_json_to_amount (json_t *json,
                      struct TALER_Amount *r_amount);

/**
 * Parse given JSON object to absolute time.
 *
 * @param json the json object representing absolute time in seconds
 * @param[out] abs where the time has to be written
 * @return #GNUNET_OK upon successful parsing; #GNUNET_SYSERR upon error
 */
int
TALER_json_to_abs (json_t *json,
                   struct GNUNET_TIME_Absolute *abs);

/**
 * Parse given JSON object to data
 *
 * @param json the json object representing data
 * @param out the pointer to hold the parsed data.
 * @param out_size the size of @a out
 * @return #GNUNET_OK upon successful parsing; #GNUNET_SYSERR upon error
 */
int
TALER_json_to_data (json_t *json,
                    void *out,
                    size_t out_size);


/**
 * Convert JSON to RSA public key.
 *
 * @param json JSON encoding to convert
 * @return corresponding public key
 */
struct GNUNET_CRYPTO_rsa_PublicKey *
TALER_json_to_rsa_public_key (json_t *json);


/**
 * Convert JSON to RSA signature.
 *
 * @param json JSON encoding to convert
 * @return corresponding signature
 */
struct GNUNET_CRYPTO_rsa_Signature *
TALER_json_to_rsa_signature (json_t *json);


/**
 * Hash a JSON for binary signing.
 *
 * @param[in] json some JSON value to hash
 * @param[out] hc resulting hash code
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
int
TALER_hash_json (json_t *json,
                 struct GNUNET_HashCode *hc);


/**
 * Check if the given wire format JSON object is correctly formatted
 *
 * @param type the type of the wire format
 * @param wire the JSON wire format object
 * @return #GNUNET_YES if correctly formatted; #GNUNET_NO if not
 */
int
TALER_json_validate_wireformat (const char *type,
                                const json_t *wire);


#endif /* TALER_json_LIB_H_ */

/* End of taler_json_lib.h */
