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
 * @file taler-mint-httpd_parsing.c
 * @brief functions to parse incoming requests (MHD arguments and JSON snippets)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_responses.h"

/* Although the following declaration isn't in any case useful
  to a merchant's activity, it's needed here to make the function
  'TMH_PARSE_nagivate_json ()' compile fine; so its value will be
  kept on some merchant's accepted currency. For multi currencies
  merchants, that of course would require a patch */
extern char *TMH_merchant_currency_string;

/**
 * Initial size for POST request buffer.
 */
#define REQUEST_BUFFER_INITIAL (2*1024)

/**
 * Maximum POST request size.
 */
#define REQUEST_BUFFER_MAX (1024*1024)


/**
 * Buffer for POST requests.
 */
struct Buffer
{
  /**
   * Allocated memory
   */
  char *data;

  /**
   * Number of valid bytes in buffer.
   */
  size_t fill;

  /**
   * Number of allocated bytes in buffer.
   */
  size_t alloc;
};


/**
 * Initialize a buffer.
 *
 * @param buf the buffer to initialize
 * @param data the initial data
 * @param data_size size of the initial data
 * @param alloc_size size of the buffer
 * @param max_size maximum size that the buffer can grow to
 * @return a GNUnet result code
 */
static int
buffer_init (struct Buffer *buf,
             const void *data,
             size_t data_size,
             size_t alloc_size,
             size_t max_size)
{
  if (data_size > max_size || alloc_size > max_size)
    return GNUNET_SYSERR;
  if (data_size > alloc_size)
    alloc_size = data_size;
  buf->data = GNUNET_malloc (alloc_size);
  memcpy (buf->data, data, data_size);
  return GNUNET_OK;
}


/**
 * Free the data in a buffer.  Does *not* free
 * the buffer object itself.
 *
 * @param buf buffer to de-initialize
 */
static void
buffer_deinit (struct Buffer *buf)
{
  GNUNET_free (buf->data);
  buf->data = NULL;
}


/**
 * Append data to a buffer, growing the buffer if necessary.
 *
 * @param buf the buffer to append to
 * @param data the data to append
 * @param data_size the size of @a data
 * @param max_size maximum size that the buffer can grow to
 * @return #GNUNET_OK on success,
 *         #GNUNET_NO if the buffer can't accomodate for the new data
 */
static int
buffer_append (struct Buffer *buf,
               const void *data,
               size_t data_size,
               size_t max_size)
{
  if (buf->fill + data_size > max_size)
    return GNUNET_NO;
  if (data_size + buf->fill > buf->alloc)
  {
    char *new_buf;
    size_t new_size = buf->alloc;
    while (new_size < buf->fill + data_size)
      new_size += 2;
    if (new_size > max_size)
      return GNUNET_NO;
    new_buf = GNUNET_malloc (new_size);
    memcpy (new_buf, buf->data, buf->fill);
    GNUNET_free (buf->data);
    buf->data = new_buf;
    buf->alloc = new_size;
  }
  memcpy (buf->data + buf->fill, data, data_size);
  buf->fill += data_size;
  return GNUNET_OK;
}

/**
 * Function called whenever we are done with a request
 * to clean up our state.
 *
 * @param con_cls value as it was left by
 *        #TMH_PARSE_post_json(), to be cleaned up
 */
void
TMH_PARSE_post_cleanup_callback (void *con_cls)
{
  struct Buffer *r = con_cls;

  if (NULL != r)
  {
    buffer_deinit (r);
    GNUNET_free (r);
  }
}

/**
 * Release all memory allocated for the variable-size fields in
 * the parser specification.
 *
 * @param spec specification to free
 * @param spec_len number of items in @a spec to look at
 */
static void
release_data (struct TMH_PARSE_FieldSpecification *spec,
              unsigned int spec_len)
{
  unsigned int i;

  for (i=0; i < spec_len; i++)
  {
    switch (spec[i].command)
    {
    case TMH_PARSE_JNC_FIELD:
      GNUNET_break (0);
      return;
    case TMH_PARSE_JNC_RET_STRING:
      GNUNET_break (0);
      return;
    case TMH_PARSE_JNC_INDEX:
      GNUNET_break (0);
      return;
    case TMH_PARSE_JNC_RET_DATA:
      break;
    case TMH_PARSE_JNC_RET_DATA_VAR:
      if (NULL != spec[i].destination)
      {
        GNUNET_free (* (void**) spec[i].destination);
        *(void**) spec[i].destination = NULL;
        *spec[i].destination_size_out = 0;
      }
      break;
    case TMH_PARSE_JNC_RET_TYPED_JSON:
      {
        json_t *json;

        json = *(json_t **) spec[i].destination;
        if (NULL != json)
        {
          json_decref (json);
          *(json_t**) spec[i].destination = NULL;
        }
      }
      break;
    case TMH_PARSE_JNC_RET_RSA_PUBLIC_KEY:
      {
        struct TALER_DenominationPublicKey *pk;

        pk = spec[i].destination;
        if (NULL != pk->rsa_public_key)
        {
          GNUNET_CRYPTO_rsa_public_key_free (pk->rsa_public_key);
          pk->rsa_public_key = NULL;
        }
      }
      break;
    case TMH_PARSE_JNC_RET_RSA_SIGNATURE:
      {
        struct TALER_DenominationSignature *sig;

        sig = spec[i].destination;
        if (NULL != sig->rsa_signature)
        {
          GNUNET_CRYPTO_rsa_signature_free (sig->rsa_signature);
          sig->rsa_signature = NULL;
        }
      }
      break;
    case TMH_PARSE_JNC_RET_AMOUNT:
      memset (spec[i].destination,
              0,
              sizeof (struct TALER_Amount));
      break;
    case TMH_PARSE_JNC_RET_TIME_ABSOLUTE:
      break;
    case TMH_PARSE_JNC_RET_UINT64:
      break;
    }
  }
}

/**
 * Process a POST request containing a JSON object.  This function
 * realizes an MHD POST processor that will (incrementally) process
 * JSON data uploaded to the HTTP server.  It will store the required
 * state in the @a con_cls, which must be cleaned up using
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
                     json_t **json)
{
  struct Buffer *r = *con_cls;

  *json = NULL;
  if (NULL == *con_cls)
  {
    /* We are seeing a fresh POST request. */
    r = GNUNET_new (struct Buffer);
    if (GNUNET_OK !=
        buffer_init (r,
                     upload_data,
                     *upload_data_size,
                     REQUEST_BUFFER_INITIAL,
                     REQUEST_BUFFER_MAX))
    {
      *con_cls = NULL;
      buffer_deinit (r);
      GNUNET_free (r);
      return (MHD_NO ==
              TMH_RESPONSE_reply_internal_error (connection,
                                                 "out of memory"))
        ? GNUNET_SYSERR : GNUNET_NO;
    }
    /* everything OK, wait for more POST data */
    *upload_data_size = 0;
    *con_cls = r;
    return GNUNET_YES;
  }
  if (0 != *upload_data_size)
  {
    /* We are seeing an old request with more data available. */

    if (GNUNET_OK !=
        buffer_append (r,
                       upload_data,
                       *upload_data_size,
                       REQUEST_BUFFER_MAX))
    {
      /* Request too long */
      *con_cls = NULL;
      buffer_deinit (r);
      GNUNET_free (r);
      return (MHD_NO ==
              TMH_RESPONSE_reply_request_too_large (connection))
        ? GNUNET_SYSERR : GNUNET_NO;
    }
    /* everything OK, wait for more POST data */
    *upload_data_size = 0;
    return GNUNET_YES;
  }

  /* We have seen the whole request. */

  *json = json_loadb (r->data,
                      r->fill,
                      0,
                      NULL);
  if (NULL == *json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to parse JSON request body\n");
    return (MHD_YES ==
            TMH_RESPONSE_reply_invalid_json (connection))
      ? GNUNET_NO : GNUNET_SYSERR;
  }
  buffer_deinit (r);
  GNUNET_free (r);
  *con_cls = NULL;

  return GNUNET_YES;
}

/**
 * Generate line in parser specification for string. The returned
 * string is already nul-terminated internally by JSON, so no length
 * information is provided. The string will live as long as the containg
 * JSON will, and must not be freed by the user
 * @param field name of the field
 * @param[out] pointer to the string
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_string (const char *field,
                         char **out)
{
  struct TMH_PARSE_FieldSpecification ret =
    {field, (void **) out, 0, NULL, TMH_PARSE_JNC_RET_STRING, 0};
  return ret;
}

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
                         uint64_t *u64)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, (void *) u64, sizeof (uint64_t), NULL, TMH_PARSE_JNC_RET_UINT64, 0 };
  return ret;
}


/**
 * Generate line in parser specification for JSON object value.
 *
 * @param field name of the field
 * @param[out] jsonp address of pointer to JSON to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_object (const char *field,
                         json_t **jsonp)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, jsonp, 0, NULL, TMH_PARSE_JNC_RET_TYPED_JSON, JSON_OBJECT };
  *jsonp = NULL;
  return ret;
}


/**
 * Generate line in parser specification for JSON array value.
 *
 * @param field name of the field
 * @param[out] jsonp address of JSON pointer to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_array (const char *field,
                        json_t **jsonp)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, jsonp, 0, NULL, TMH_PARSE_JNC_RET_TYPED_JSON, JSON_ARRAY };
  *jsonp = NULL;
  return ret;
}


/**
 * Generate line in parser specification for an absolute time.
 *
 * @param field name of the field
 * @param[out] atime time to initialize
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_time_abs (const char *field,
                           struct GNUNET_TIME_Absolute *atime)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, atime, sizeof(struct GNUNET_TIME_Absolute), NULL, TMH_PARSE_JNC_RET_TIME_ABSOLUTE, 0 };
  return ret;
}


/**
 * Generate line in parser specification for RSA public key.
 *
 * @param field name of the field
 * @param[out] pk key to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_denomination_public_key (const char *field,
                                          struct TALER_DenominationPublicKey *pk)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, pk, 0, NULL, TMH_PARSE_JNC_RET_RSA_PUBLIC_KEY, 0 };
  pk->rsa_public_key = NULL;
  return ret;
}


/**
 * Generate line in parser specification for RSA public key.
 *
 * @param field name of the field
 * @param sig the signature to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_denomination_signature (const char *field,
                                         struct TALER_DenominationSignature *sig)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, sig, 0, NULL, TMH_PARSE_JNC_RET_RSA_SIGNATURE, 0 };
  sig->rsa_signature = NULL;
  return ret;
}


/**
 * Generate line in parser specification for an amount.
 *
 * @param field name of the field
 * @param amount a `struct TALER_Amount *` to initialize
 * @return corresponding field spec
 */
struct TMH_PARSE_FieldSpecification
TMH_PARSE_member_amount (const char *field,
                         struct TALER_Amount *amount)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, amount, sizeof(struct TALER_Amount), NULL, TMH_PARSE_JNC_RET_AMOUNT, 0 };
  memset (amount, 0, sizeof (struct TALER_Amount));
  return ret;
}


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
                           size_t *ptr_size)
{
  struct TMH_PARSE_FieldSpecification ret =
    { field, ptr, 0, ptr_size, TMH_PARSE_JNC_RET_DATA_VAR, 0 };
  *ptr = NULL;
  return ret;
}

/**
 * Navigate through a JSON tree.
 *
 * Sends an error response if navigation is impossible (i.e.
 * the JSON object is invalid)
 *
 * @param connection the connection to send an error response to
 * @param root the JSON node to start the navigation at.
 * @param ... navigation specification (see
 * `enum TMH_PARSE_JsonNavigationCommand`)
 * @return
 *    #GNUNET_YES if navigation was successful
 *    #GNUNET_NO if json is malformed, error response was generated
 *    #GNUNET_SYSERR on internal error (no response was generated,
 *                       connection must be closed)
 */
int
TMH_PARSE_navigate_json (struct MHD_Connection *connection,
                         const json_t *root,
                         ...)
{
  va_list argp;
  int ret;
  json_t *path; /* what's our current path from 'root'? */

  path = json_array ();
  va_start (argp, root);
  ret = 2; /* just not any of the valid return values */
  while (2 == ret)
  {
    enum TMH_PARSE_JsonNavigationCommand command
      = va_arg (argp,
                enum TMH_PARSE_JsonNavigationCommand);

    switch (command)
    {
    case TMH_PARSE_JNC_FIELD:
      {
        const char *fname = va_arg(argp, const char *);

        json_array_append_new (path,
                               json_string (fname));
        root = json_object_get (root,
                                fname);
        if (NULL == root)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:s, s:O}",
                                               "error", "missing field in JSON",
                                               "field", fname,
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
      }
      break;

    case TMH_PARSE_JNC_INDEX:
      {
        int fnum = va_arg(argp, int);

        json_array_append_new (path,
                               json_integer (fnum));
        root = json_array_get (root,
                               fnum);
        if (NULL == root)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "missing index in JSON",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
      }
      break;

    case TMH_PARSE_JNC_RET_DATA:
      {
        void *where = va_arg (argp, void *);
        size_t len = va_arg (argp, size_t);
        const char *str;
        int res;

        str = json_string_value (root);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                             MHD_HTTP_BAD_REQUEST,
                                             "{s:s, s:O}",
                                             "error", "string expected",
                                             "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        res = GNUNET_STRINGS_string_to_data (str, strlen (str),
                                             where, len);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "malformed binary data in JSON",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        ret = GNUNET_OK;
      }
      break;

    case TMH_PARSE_JNC_RET_STRING:
      {
        void **where = va_arg (argp, void **);
        *where = (void*) json_string_value (root);
        ret = GNUNET_OK;
      }
      break;
    case TMH_PARSE_JNC_RET_DATA_VAR:
      {
        void **where = va_arg (argp, void **);
        size_t *len = va_arg (argp, size_t *);
        const char *str;
        int res;

        str = json_string_value (root);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_internal_error (connection,
                                                    "json_string_value() failed"))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        *len = (strlen (str) * 5) / 8;
        if (NULL != where)
        {
          *where = GNUNET_malloc (*len);
          res = GNUNET_STRINGS_string_to_data (str,
                                               strlen (str),
                                               *where,
                                               *len);
          if (GNUNET_OK != res)
          {
            GNUNET_break_op (0);
            GNUNET_free (*where);
            *where = NULL;
            *len = 0;
            ret = (MHD_YES ==
                   TMH_RESPONSE_reply_json_pack (connection,
                                                 MHD_HTTP_BAD_REQUEST,
                                                 "{s:s, s:O}",
                                                 "error", "malformed binary data in JSON",
                                                 "path", path))
              ? GNUNET_NO : GNUNET_SYSERR;
            break;
          }
        }
        ret = GNUNET_OK;
      }
      break;

    case TMH_PARSE_JNC_RET_TYPED_JSON:
      {
        int typ = va_arg (argp, int);
        const json_t **r_json = va_arg (argp, const json_t **);

        if ( (NULL == root) ||
             ( (-1 != typ) &&
               (json_typeof (root) != typ)) )
        {
          GNUNET_break_op (0);
          *r_json = NULL;
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:i, s:i, s:O}",
                                               "error", "wrong JSON field type",
                                               "type_expected", typ,
                                               "type_actual", json_typeof (root),
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        *r_json = root;
        json_incref ((json_t *) root);
        ret = GNUNET_OK;
      }
      break;

    case TMH_PARSE_JNC_RET_UINT64:
      {
        uint64_t *r_u64 = va_arg (argp, uint64_t *);

        if (json_typeof (root) != JSON_INTEGER)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:s, s:i, s:O}",
                                               "error", "wrong JSON field type",
                                               "type_expected", "integer",
                                               "type_actual", json_typeof (root),
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        *r_u64 = (uint64_t) json_integer_value (root);
        ret = GNUNET_OK;
      }
      break;

    case TMH_PARSE_JNC_RET_RSA_PUBLIC_KEY:
      {
        struct TALER_DenominationPublicKey *where;
        size_t len;
        const char *str;
        int res;
        void *buf;

        where = va_arg (argp,
                        struct TALER_DenominationPublicKey *);
        str = json_string_value (root);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "string expected",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        len = (strlen (str) * 5) / 8;
        buf = GNUNET_malloc (len);
        res = GNUNET_STRINGS_string_to_data (str,
                                             strlen (str),
                                             buf,
                                             len);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          GNUNET_free (buf);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "malformed binary data in JSON",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        where->rsa_public_key = GNUNET_CRYPTO_rsa_public_key_decode (buf,
                                                                     len);
        GNUNET_free (buf);
        if (NULL == where->rsa_public_key)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "malformed RSA public key in JSON",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        ret = GNUNET_OK;
        break;
      }

    case TMH_PARSE_JNC_RET_RSA_SIGNATURE:
      {
        struct TALER_DenominationSignature *where;
        size_t len;
        const char *str;
        int res;
        void *buf;

        where = va_arg (argp,
                        struct TALER_DenominationSignature *);
        str = json_string_value (root);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "string expected",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        len = (strlen (str) * 5) / 8;
        buf = GNUNET_malloc (len);
        res = GNUNET_STRINGS_string_to_data (str,
                                             strlen (str),
                                             buf,
                                             len);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          GNUNET_free (buf);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "malformed binary data in JSON",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        where->rsa_signature = GNUNET_CRYPTO_rsa_signature_decode (buf,
                                                                   len);
        GNUNET_free (buf);
        if (NULL == where->rsa_signature)
        {
          GNUNET_break_op (0);
          ret = (MHD_YES ==
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "malformed RSA signature in JSON",
                                               "path", path))
            ? GNUNET_NO : GNUNET_SYSERR;
          break;
        }
        ret = GNUNET_OK;
        break;
      }

    case TMH_PARSE_JNC_RET_AMOUNT:
      {
        struct TALER_Amount *where = va_arg (argp, void *);

        if (GNUNET_OK !=
            TALER_json_to_amount ((json_t *) root,
                                  where))
        {
          GNUNET_break_op (0);
          ret = (MHD_YES !=
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O}",
                                               "error", "Bad format",
                                               "path", path))
            ? GNUNET_SYSERR : GNUNET_NO;
          break;
        }
        if (0 != strcmp (where->currency,
                         TMH_merchant_currency_string))
        {
          GNUNET_break_op (0);
          ret = (MHD_YES !=
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:O, s:s}",
                                               "error", "Currency not supported",
                                               "path", path,
                                               "currency", where->currency))
            ? GNUNET_SYSERR : GNUNET_NO;
          memset (where, 0, sizeof (struct TALER_Amount));
          break;
        }
        ret = GNUNET_OK;
        break;
      }

    case TMH_PARSE_JNC_RET_TIME_ABSOLUTE:
      {
        struct GNUNET_TIME_Absolute *where = va_arg (argp, void *);

        if (GNUNET_OK !=
            TALER_json_to_abs ((json_t *) root,
                               where))
        {
          GNUNET_break_op (0);
          ret = (MHD_YES !=
                 TMH_RESPONSE_reply_json_pack (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               "{s:s, s:s, s:O}",
                                               "error", "Bad format",
                                               "hint", "expected absolute time",
                                               "path", path))
            ? GNUNET_SYSERR : GNUNET_NO;
          break;
        }
        ret = GNUNET_OK;
        break;
      }

    default:
      GNUNET_break (0);
      ret = (MHD_YES ==
             TMH_RESPONSE_reply_internal_error (connection,
                                                "unhandled value in switch"))
        ? GNUNET_NO : GNUNET_SYSERR;
      break;
    }
  }
  va_end (argp);
  json_decref (path);
  return ret;
}



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
                     struct TMH_PARSE_FieldSpecification *spec)
{
  unsigned int i;
  int ret;

  ret = GNUNET_YES;
  for (i=0; NULL != spec[i].field_name; i++)
  {
    if (GNUNET_YES != ret)
      break;
    switch (spec[i].command)
    {
    case TMH_PARSE_JNC_FIELD:
      GNUNET_break (0);
      return GNUNET_SYSERR;
    case TMH_PARSE_JNC_INDEX:
      GNUNET_break (0);
      return GNUNET_SYSERR;
    case TMH_PARSE_JNC_RET_DATA:
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_DATA,
                                     spec[i].destination,
                                     spec[i].destination_size_in);
      break;
    case TMH_PARSE_JNC_RET_DATA_VAR:
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_DATA_VAR,
                                     (void **) spec[i].destination,
                                     spec[i].destination_size_out);
      break;
    case TMH_PARSE_JNC_RET_STRING:
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_STRING,
                                     spec[i].destination);
      break;
    case TMH_PARSE_JNC_RET_TYPED_JSON:
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_TYPED_JSON,
                                     spec[i].type,
                                     spec[i].destination);
      break;
    case TMH_PARSE_JNC_RET_RSA_PUBLIC_KEY:
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_RSA_PUBLIC_KEY,
                                     spec[i].destination);
      break;
    case TMH_PARSE_JNC_RET_RSA_SIGNATURE:
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_RSA_SIGNATURE,
                                     spec[i].destination);
      break;
    case TMH_PARSE_JNC_RET_AMOUNT:
      GNUNET_assert (sizeof (struct TALER_Amount) ==
                     spec[i].destination_size_in);
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_AMOUNT,
                                     spec[i].destination);
      break;
    case TMH_PARSE_JNC_RET_TIME_ABSOLUTE:
      GNUNET_assert (sizeof (struct GNUNET_TIME_Absolute) ==
                     spec[i].destination_size_in);
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_TIME_ABSOLUTE,
                                     spec[i].destination);
      break;
    case TMH_PARSE_JNC_RET_UINT64:
      GNUNET_assert (sizeof (uint64_t) ==
                     spec[i].destination_size_in);
      ret = TMH_PARSE_navigate_json (connection,
                                     root,
                                     TMH_PARSE_JNC_FIELD,
                                     spec[i].field_name,
                                     TMH_PARSE_JNC_RET_UINT64,
                                     spec[i].destination);
      break;
    }
  }
  if (GNUNET_YES != ret)
    release_data (spec,
                  i - 1);
  return ret;
}


/* end of taler-mint-httpd_parsing.c */
