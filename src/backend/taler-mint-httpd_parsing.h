/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-mint-httpd_parsing.h
 * @brief functions to parse incoming requests
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_PARSING_H
#define TALER_MINT_HTTPD_PARSING_H

#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_util.h>

/**
 * Process a POST request containing a JSON object.  This
 * function realizes an MHD POST processor that will
 * (incrementally) process JSON data uploaded to the HTTP
 * server.  It will store the required state in the
 * "connection_cls", which must be cleaned up using
 * #TMH_PARSE_post_cleanup_callback().
 *
 * @param connection the MHD connection
 * @param con_cls the closure (points to a `struct Buffer *`)
 * @param upload_data the POST data
 * @param upload_data_size number of bytes in @a upload_data
 * @param json the JSON object for a completed request
 * @return
 *    #GNUNET_YES if json object was parsed or at least
 *               may be parsed in the future (call again);
 *               `*json` will be NULL if we need to be called again,
 *                and non-NULL if we are done.
 *    #GNUNET_NO is request incomplete or invalid
 *               (error message was generated)
 *    #GNUNET_SYSERR on internal error
 *               (we could not even queue an error message,
 *                close HTTP session with MHD_NO)
 */
int
TMH_PARSE_post_json (struct MHD_Connection *connection,
                     void **con_cls,
                     const char *upload_data,
                     size_t *upload_data_size,
                     json_t **json);


/**
 * Function called whenever we are done with a request
 * to clean up our state.
 *
 * @param con_cls value as it was left by
 *        #TMH_PARSE_post_json(), to be cleaned up
 */
void
TMH_PARSE_post_cleanup_callback (void *con_cls);


/**
 * Constants for JSON navigation description.
 */
enum TMH_PARSE_JsonNavigationCommand
{
  /**
   * Access a field.
   * Param: const char *
   */
  TMH_PARSE_JNC_FIELD,

  /**
   * Access an array index.
   * Param: int
   */
  TMH_PARSE_JNC_INDEX,

  /**
   * Return base32crockford encoded data of
   * constant size.
   * Params: (void *, size_t)
   */
  TMH_PARSE_JNC_RET_DATA,

  /**
   * Return base32crockford encoded data of
   * variable size.
   * Params: (void **, size_t *)
   */
  TMH_PARSE_JNC_RET_DATA_VAR,

  /**
   * Return a json object, which must be
   * of the given type (JSON_* type constants,
   * or -1 for any type).
   * Params: (int, json_t **)
   */
  TMH_PARSE_JNC_RET_TYPED_JSON,

  /**
   * Return a `struct GNUNET_CRYPTO_rsa_PublicKey` which was
   * encoded as variable-size base32crockford encoded data.
   */
  TMH_PARSE_JNC_RET_RSA_PUBLIC_KEY,

  /**
   * Return a `struct GNUNET_CRYPTO_rsa_Signature` which was
   * encoded as variable-size base32crockford encoded data.
   */
  TMH_PARSE_JNC_RET_RSA_SIGNATURE,

  /**
   * Return a `struct TALER_Amount` which was
   * encoded within its own json object.
   */
  TMH_PARSE_JNC_RET_AMOUNT,

  /**
   * Return a `struct GNUNET_TIME_Absolute` which was
   * encoded within its own json object.
   * Param: struct GNUNET_TIME_Absolute *
   */
  TMH_PARSE_JNC_RET_TIME_ABSOLUTE,

  /**
   * Return a `uint64_t` which was
   * encoded as a JSON integer.
   * Param: uint64_t *
   */
  TMH_PARSE_JNC_RET_UINT64,
  /**
   * Return a 'char *' as returned from 'json_string_value ()'.
   * So it will live as long as the containg JSON is not freed
   */
  TMH_PARSE_JNC_RET_STRING

};


/**
 * Navigate through a JSON tree.
 *
 * Sends an error response if navigation is impossible (i.e.
 * the JSON object is invalid)
 *
 * @param connection the connection to send an error response to
 * @param root the JSON node to start the navigation at.
 * @param ... navigation specification (see `enum TMH_PARSE_JsonNavigationCommand`)
 * @return
 *    #GNUNET_YES if navigation was successful
 *    #GNUNET_NO if json is malformed, error response was generated
 *    #GNUNET_SYSERR on internal error
 */
int
TMH_PARSE_navigate_json (struct MHD_Connection *connection,
                         const json_t *root,
                         ...);


/**
 * @brief Specification for how to parse a JSON field.
 */
struct TMH_PARSE_FieldSpecification
{
  /**
   * Name of the field.  NULL only to terminate array.
   */
  const char *field_name;

  /**
   * Where to store the result.  Must have exactly
   * @e destination_size bytes, except if @e destination_size is zero.
   * NULL to skip assignment (but check presence of the value).
   */
  void *destination;

  /**
   * How big should the result be, 0 for variable size.  In
   * this case, @e destination must be a "void **", pointing
   * to a location that is currently NULL and is to be allocated.
   */
  size_t destination_size_in;

  /**
   * @e destination_size_out will then be set to the size of the
   * value that was stored in @e destination (useful for
   * variable-size allocations).
   */
  size_t *destination_size_out;

  /**
   * Navigation command to use to extract the value.  Note that
   * #TMH_PARSE_JNC_RET_DATA or #TMH_PARSE_JNC_RET_DATA_VAR must be used for @e
   * destination_size_in and @e destination_size_out to have a
   * meaning.  #TMH_PARSE_JNC_FIELD and #TMH_PARSE_JNC_INDEX must not be used here!
   */
  enum TMH_PARSE_JsonNavigationCommand command;

  /**
   * JSON type to use, only meaningful in connection with a @e command
   * value of #TMH_PARSE_JNC_RET_TYPED_JSON.  Typical values are
   * #JSON_ARRAY and #JSON_OBJECT.
   */
  int type;

};


/**
 * Parse JSON object into components based on the given field
 * specification.
 *
 * @param connection the connection to send an error response to
 * @param root the JSON node to start the navigation at.
 * @param spec field specification for the parser
 * @return
 *    #GNUNET_YES if navigation was successful (caller is responsible
 *                for freeing allocated variable-size data using
 *                #TMH_PARSE_release_data() when done)
 *    #GNUNET_NO if json is malformed, error response was generated
 *    #GNUNET_SYSERR on internal error
 */
int
TMH_PARSE_json_data (struct MHD_Connection *connection,
                     const json_t *root,
                     struct TMH_PARSE_FieldSpecification *spec);


/**
 * Release all memory allocated for the variable-size fields in
 * the parser specification.
 *
 * @param spec specification to free
 */
void
TMH_PARSE_release_data (struct TMH_PARSE_FieldSpecification *spec);


/**
 * Generate line in parser specification for fixed-size value.
 *
 * @param field name of the field
 * @param value where to store the value
 */
#define TMH_PARSE_member_fixed(field,value) { field, value, sizeof (*value), NULL, TMH_PARSE_JNC_RET_DATA, 0 }


/**
 * Generate line in parser specification for variable-size value.
 *
 * @param field name of the field
 * @param[out] ptr pointer to initialize
 * @param[out] ptr_size size to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_variable (const char *field,
                           void **ptr,
                           size_t *ptr_size);


/**
 * Generate line in parser specification for 64-bit integer
 * given as an integer in JSON.
 *
 * @param field name of the field
 * @param[out] u64 integer to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_uint64 (const char *field,
                         uint64_t *u64);


/**
 * Generate line in parser specification for JSON array value.
 *
 * @param field name of the field
 * @param[out] jsonp address of JSON pointer to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_array (const char *field,
                        json_t **jsonp);


/**
 * Generate line in parser specification for JSON object value.
 *
 * @param field name of the field
 * @param[out] jsonp address of pointer to JSON to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_object (const char *field,
                         json_t **jsonp);


/**
 * Generate line in parser specification for RSA public key.
 *
 * @param field name of the field
 * @param[out] pk key to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_denomination_public_key (const char *field,
                                          struct TALER_DenominationPublicKey *pk);


/**
 * Generate line in parser specification for RSA public key.
 *
 * @param field name of the field
 * @param sig the signature to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_denomination_signature (const char *field,
                                         struct TALER_DenominationSignature *sig);


/**
 * Generate line in parser specification for an amount.
 *
 * @param field name of the field
 * @param[out] amount a `struct TALER_Amount *` to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_amount (const char *field,
                         struct TALER_Amount *amount);


/**
 * Generate line in parser specification for an absolute time.
 *
 * @param field name of the field
 * @param[out] atime time to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_time_abs (const char *field,
                           struct GNUNET_TIME_Absolute *atime);



/**
 * Generate line in parser specification indicating the end of the spec.
 */
#define TMH_PARSE_MEMBER_END { NULL, NULL, 0, NULL, TMH_PARSE_JNC_FIELD, 0 }


/**
 * Extraxt fixed-size base32crockford encoded data from request.
 *
 * Queues an error response to the connection if the parameter is missing or
 * invalid.
 *
 * @param connection the MHD connection
 * @param param_name the name of the parameter with the key
 * @param[out] out_data pointer to store the result
 * @param out_size expected size of @a out_data
 * @return
 *   #GNUNET_YES if the the argument is present
 *   #GNUNET_NO if the argument is absent or malformed
 *   #GNUNET_SYSERR on internal error (error response could not be sent)
 */
int
TMH_PARSE_mhd_request_arg_data (struct MHD_Connection *connection,
                                const char *param_name,
                                void *out_data,
                                size_t out_size);


/**
 * Extraxt variable-size base32crockford encoded data from request.
 *
 * Queues an error response to the connection if the parameter is missing
 * or the encoding is invalid.
 *
 * @param connection the MHD connection
 * @param param_name the name of the parameter with the key
 * @param[out] out_data pointer to allocate buffer and store the result
 * @param[out] out_size set to the size of the buffer allocated in @a out_data
 * @return
 *   #GNUNET_YES if the the argument is present
 *   #GNUNET_NO if the argument is absent or malformed
 *   #GNUNET_SYSERR on internal error (error response could not be sent)
 */
int
TMH_PARSE_mhd_request_var_arg_data (struct MHD_Connection *connection,
                                    const char *param_name,
                                    void **out_data,
                                    size_t *out_size);




#endif /* TALER_MINT_HTTPD_PARSING_H */
