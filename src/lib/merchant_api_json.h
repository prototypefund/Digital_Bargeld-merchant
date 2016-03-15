/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.LGPL.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_json.h
 * @brief functions to parse incoming requests (JSON snippets)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_util.h>
#include <jansson.h>


/**
 * Enumeration with the various commands for the
 * #MAJ_parse_json interpreter.
 */
enum MAJ_Command
{

  /**
   * End of command list.
   */
  MAJ_CMD_END,

  /**
   * Parse amount at current position.
   */
  MAJ_CMD_AMOUNT,

  /**
   * Parse absolute time at current position.
   */
  MAJ_CMD_TIME_ABSOLUTE,

  /**
   * Parse fixed binary value at current position.
   */
  MAJ_CMD_BINARY_FIXED,

  /**
   * Parse variable-size binary value at current position.
   */
  MAJ_CMD_BINARY_VARIABLE,

  /**
   * Parse RSA public key at current position.
   */
  MAJ_CMD_RSA_PUBLIC_KEY,

  /**
   * Parse RSA signature at current position.
   */
  MAJ_CMD_RSA_SIGNATURE,

  /**
   * Parse `const char *` JSON string at current position.
   */
  MAJ_CMD_STRING,

  /**
   * Parse `uint16_t` integer at the current position.
   */
  MAJ_CMD_UINT16,

  /**
   * Parse JSON object at the current position.
   */
  MAJ_CMD_JSON_OBJECT,

  /**
   * Parse ??? at current position.
   */
  MAJ_CMD_C

};


/**
 * @brief Entry in parser specification for #MAJ_parse_json.
 */
struct MAJ_Specification
{

  /**
   * Command to execute.
   */
  enum MAJ_Command cmd;

  /**
   * Name of the field to access.
   */
  const char *field;

  /**
   * Further details for the command.
   */
  union {

    /**
     * Where to store amount for #MAJ_CMD_AMOUNT.
     */
    struct TALER_Amount *amount;

    /**
     * Where to store time, for #MAJ_CMD_TIME_ABSOLUTE.
     */
    struct GNUNET_TIME_Absolute *abs_time;

    /**
     * Where to write binary data, for #MAJ_CMD_BINARY_FIXED.
     */
    struct {
      /**
       * Where to write the data.
       */
      void *dest;

      /**
       * How many bytes to write to @e dest.
       */
      size_t dest_size;

    } fixed_data;

    /**
     * Where to write binary data, for #MAJ_CMD_BINARY_VARIABLE.
     */
    struct {
      /**
       * Where to store the pointer with the data (is allocated).
       */
      void **dest_p;

      /**
       * Where to store the number of bytes allocated at `*dest`.
       */
      size_t *dest_size_p;

    } variable_data;

    /**
     * Where to store the RSA public key for #MAJ_CMD_RSA_PUBLIC_KEY
     */
    struct GNUNET_CRYPTO_rsa_PublicKey **rsa_public_key;

    /**
     * Where to store the RSA signature for #MAJ_CMD_RSA_SIGNATURE
     */
    struct GNUNET_CRYPTO_rsa_Signature **rsa_signature;

    /**
     * Details for #MAJ_CMD_EDDSA_SIGNATURE
     */
    struct {

      /**
       * Where to store the purpose.
       */
      struct GNUNET_CRYPTO_EccSignaturePurpose **purpose_p;

      /**
       * Key to verify the signature against.
       */
      const struct GNUNET_CRYPTO_EddsaPublicKey *pub_key;

    } eddsa_signature;

    /**
     * Where to store a pointer to the string.
     */
    const char **strptr;

    /**
     * Where to store 16-bit integer.
     */
    uint16_t *u16;

    /**
     * Where to store a JSON object.
     */
    json_t **obj;

  } details;

};


/**
 * Navigate and parse data in a JSON tree.
 *
 * @param root the JSON node to start the navigation at.
 * @param spec parse specification array
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
int
MAJ_parse_json (const json_t *root,
                struct MAJ_Specification *spec);


/**
 * Free all elements allocated during a
 * #MAJ_parse_json() operation.
 *
 * @param spec specification of the parse operation
 */
void
MAJ_parse_free (struct MAJ_Specification *spec);


/**
 * End of a parser specification.
 */
#define MAJ_spec_end { .cmd = MAJ_CMD_END }

/**
 * Fixed size object (in network byte order, encoded using Crockford
 * Base32hex encoding).
 *
 * @param name name of the JSON field
 * @param obj pointer where to write the data (type of `*obj` will determine size)
 */
#define MAJ_spec_fixed_auto(name,obj) { .cmd = MAJ_CMD_BINARY_FIXED, .field = name, .details.fixed_data.dest = obj, .details.fixed_data.dest_size = sizeof (*obj) }


/**
 * Variable size object (in network byte order, encoded using Crockford
 * Base32hex encoding).
 *
 * @param name name of the JSON field
 * @param obj pointer where to write the data (a `void **`)
 * @param size where to store the number of bytes allocated for @a obj (of type `size_t *`
 */
#define MAJ_spec_varsize(name,obj,size) { .cmd = MAJ_CMD_BINARY_VARIABLE, .field = name, .details.variable_data.dest_p = obj, .details.variable_data.dest_size_p = size }


/**
 * The expected field stores a string.
 *
 * @param name name of the JSON field
 * @param strptr where to store a pointer to the field
 */
struct MAJ_Specification
MAJ_spec_string (const char *name,
                 const char **strptr);


/**
 * Absolute time.
 *
 * @param name name of the JSON field
 * @param[out] at where to store the absolute time found under @a name
 */
struct MAJ_Specification
MAJ_spec_absolute_time (const char *name,
                        struct GNUNET_TIME_Absolute *at);


/**
 * 16-bit integer.
 *
 * @param name name of the JSON field
 * @param[out] u16 where to store the integer found under @a name
 */
struct MAJ_Specification
MAJ_spec_uint16 (const char *name,
                 uint16_t *u16);


/**
 * JSON object.
 *
 * @param name name of the JSON field
 * @param[out] jsonp where to store the JSON found under @a name
 */
struct MAJ_Specification
MAJ_spec_json (const char *name,
               json_t **jsonp);


/**
 * Specification for parsing an amount value.
 *
 * @param name name of the JSON field
 * @param amount where to store the amount under @a name
 */
struct MAJ_Specification
MAJ_spec_amount (const char *name,
                 struct TALER_Amount *amount);


/**
 * Specification for parsing an RSA public key.
 *
 * @param name name of the JSON field
 * @param pk where to store the RSA key found under @a name
 */
struct MAJ_Specification
MAJ_spec_rsa_public_key (const char *name,
                         struct GNUNET_CRYPTO_rsa_PublicKey **pk);


/**
 * Specification for parsing an RSA signature.
 *
 * @param name name of the JSON field
 * @param sig where to store the RSA signature found under @a name
 */
struct MAJ_Specification
MAJ_spec_rsa_signature (const char *name,
                        struct GNUNET_CRYPTO_rsa_Signature **sig);




/* end of merchant_api_json.h */
