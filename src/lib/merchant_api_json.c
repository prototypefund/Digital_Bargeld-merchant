/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

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
 * @file lib/merchant_api_json.c
 * @brief functions to parse incoming requests (JSON snippets)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include "merchant_api_json.h"

/**
 * Navigate and parse data in a JSON tree.
 *
 * @param root the JSON node to start the navigation at.
 * @param spec parse specification array
 * @return offset in @a spec where parsing failed, -1 on success (!)
 */
static int
parse_json (json_t *root,
            struct MAJ_Specification *spec)
{
  int i;
  json_t *pos; /* what's our current position? */

  pos = root;
  for (i=0;MAJ_CMD_END != spec[i].cmd;i++)
  {
    pos = json_object_get (root,
                           spec[i].field);
    if (NULL == pos)
    {
      GNUNET_break_op (0);
      return i;
    }
    switch (spec[i].cmd)
    {
    case MAJ_CMD_END:
      GNUNET_assert (0);
      return i;
    case MAJ_CMD_AMOUNT:
      if (GNUNET_OK !=
          TALER_json_to_amount (pos,
                                spec[i].details.amount))
      {
        GNUNET_break_op (0);
        return i;
      }
      break;
    case MAJ_CMD_TIME_ABSOLUTE:
      if (GNUNET_OK !=
          TALER_json_to_abs (pos,
                             spec[i].details.abs_time))
      {
        GNUNET_break_op (0);
        return i;
      }
      break;

    case MAJ_CMD_STRING:
      {
        const char *str;

        str = json_string_value (pos);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          return i;
        }
        *spec[i].details.strptr = str;
      }
      break;

    case MAJ_CMD_BINARY_FIXED:
      {
        const char *str;
        int res;

        str = json_string_value (pos);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          return i;
        }
        res = GNUNET_STRINGS_string_to_data (str, strlen (str),
                                             spec[i].details.fixed_data.dest,
                                             spec[i].details.fixed_data.dest_size);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          return i;
        }
      }
      break;

    case MAJ_CMD_BINARY_VARIABLE:
      {
        const char *str;
        size_t size;
        void *data;
        int res;

        str = json_string_value (pos);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          return i;
        }
        size = (strlen (str) * 5) / 8;
        if (size >= 1024)
        {
          GNUNET_break_op (0);
          return i;
        }
        data = GNUNET_malloc (size);
        res = GNUNET_STRINGS_string_to_data (str,
                                             strlen (str),
                                             data,
                                             size);
        if (GNUNET_OK != res)
        {
          GNUNET_break_op (0);
          GNUNET_free (data);
          return i;
        }
        *spec[i].details.variable_data.dest_p = data;
        *spec[i].details.variable_data.dest_size_p = size;
      }
      break;

    case MAJ_CMD_RSA_PUBLIC_KEY:
      {
        size_t size;
        const char *str;
        int res;
        void *buf;

        str = json_string_value (pos);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          return i;
        }
        size = (strlen (str) * 5) / 8;
        buf = GNUNET_malloc (size);
        res = GNUNET_STRINGS_string_to_data (str,
                                             strlen (str),
                                             buf,
                                             size);
        if (GNUNET_OK != res)
        {
          GNUNET_free (buf);
          GNUNET_break_op (0);
          return i;
        }
        *spec[i].details.rsa_public_key
          = GNUNET_CRYPTO_rsa_public_key_decode (buf,
                                                 size);
        GNUNET_free (buf);
        if (NULL == spec[i].details.rsa_public_key)
        {
          GNUNET_break_op (0);
          return i;
        }
      }
      break;

    case MAJ_CMD_RSA_SIGNATURE:
      {
        size_t size;
        const char *str;
        int res;
        void *buf;

        str = json_string_value (pos);
        if (NULL == str)
        {
          GNUNET_break_op (0);
          return i;
        }
        size = (strlen (str) * 5) / 8;
        buf = GNUNET_malloc (size);
        res = GNUNET_STRINGS_string_to_data (str,
                                             strlen (str),
                                             buf,
                                             size);
        if (GNUNET_OK != res)
        {
          GNUNET_free (buf);
          GNUNET_break_op (0);
          return i;
        }
        *spec[i].details.rsa_signature
          = GNUNET_CRYPTO_rsa_signature_decode (buf,
                                                size);
        GNUNET_free (buf);
        if (NULL == spec[i].details.rsa_signature)
          return i;
      }
      break;

    case MAJ_CMD_UINT16:
      {
        json_int_t val;

        if (! json_is_integer (pos))
        {
          GNUNET_break_op (0);
          return i;
        }
        val = json_integer_value (pos);
        if ( (0 > val) || (val > UINT16_MAX) )
        {
          GNUNET_break_op (0);
          return i;
        }
        *spec[i].details.u16 = (uint16_t) val;
      }
      break;

    case MAJ_CMD_JSON_OBJECT:
      {
        if (! (json_is_object (pos) || json_is_array (pos)) )
        {
          GNUNET_break_op (0);
          return i;
        }
        json_incref (pos);
        *spec[i].details.obj = pos;
      }
      break;

    default:
      GNUNET_break (0);
      return i;
    }
  }
  return -1; /* all OK! */
}


/**
 * Free all elements allocated during a
 * #MAJ_parse_json() operation.
 *
 * @param spec specification of the parse operation
 * @param end number of elements in @a spec to process
 */
static void
parse_free (struct MAJ_Specification *spec,
            int end)
{
  int i;

  for (i=0;i<end;i++)
  {
    switch (spec[i].cmd)
    {
    case MAJ_CMD_END:
      GNUNET_assert (0);
      return;
    case MAJ_CMD_AMOUNT:
      break;
    case MAJ_CMD_TIME_ABSOLUTE:
      break;
    case MAJ_CMD_BINARY_FIXED:
      break;
    case MAJ_CMD_STRING:
      break;
    case MAJ_CMD_BINARY_VARIABLE:
      GNUNET_free (*spec[i].details.variable_data.dest_p);
      *spec[i].details.variable_data.dest_p = NULL;
      *spec[i].details.variable_data.dest_size_p = 0;
      break;
    case MAJ_CMD_RSA_PUBLIC_KEY:
      GNUNET_CRYPTO_rsa_public_key_free (*spec[i].details.rsa_public_key);
      *spec[i].details.rsa_public_key = NULL;
      break;
    case MAJ_CMD_RSA_SIGNATURE:
      GNUNET_CRYPTO_rsa_signature_free (*spec[i].details.rsa_signature);
      *spec[i].details.rsa_signature = NULL;
      break;
    case MAJ_CMD_JSON_OBJECT:
      json_decref (*spec[i].details.obj);
      *spec[i].details.obj = NULL;
      break;
    default:
      GNUNET_break (0);
      break;
    }
  }
}


/**
 * Navigate and parse data in a JSON tree.
 *
 * @param root the JSON node to start the navigation at.
 * @param spec parse specification array
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
int
MAJ_parse_json (const json_t *root,
                struct MAJ_Specification *spec)
{
  int ret;

  ret = parse_json ((json_t *) root,
                    spec);
  if (-1 == ret)
    return GNUNET_OK;
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "JSON field `%s` (%d) had unexpected value\n",
              spec[ret].field,
              ret);
  parse_free (spec, ret);
  return GNUNET_SYSERR;
}


/**
 * Free all elements allocated during a
 * #MAJ_parse_json() operation.
 *
 * @param spec specification of the parse operation
 */
void
MAJ_parse_free (struct MAJ_Specification *spec)
{
  int i;

  for (i=0;MAJ_CMD_END != spec[i].cmd;i++) ;
  parse_free (spec, i);
}


/**
 * The expected field stores a string.
 *
 * @param name name of the JSON field
 * @param strptr where to store a pointer to the field
 */
struct MAJ_Specification
MAJ_spec_string (const char *name,
                 const char **strptr)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_STRING,
      .field = name,
      .details.strptr = strptr
    };
  return ret;
}


/**
 * Specification for parsing an absolute time value.
 *
 * @param name name of the JSON field
 * @param at where to store the absolute time found under @a name
 */
struct MAJ_Specification
MAJ_spec_absolute_time (const char *name,
                        struct GNUNET_TIME_Absolute *at)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_TIME_ABSOLUTE,
      .field = name,
      .details.abs_time = at
    };
  return ret;
}


/**
 * Specification for parsing an amount value.
 *
 * @param name name of the JSON field
 * @param amount where to store the amount found under @a name
 */
struct MAJ_Specification
MAJ_spec_amount (const char *name,
                 struct TALER_Amount *amount)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_AMOUNT,
      .field = name,
      .details.amount = amount
    };
  return ret;
}


/**
 * 16-bit integer.
 *
 * @param name name of the JSON field
 * @param[out] u16 where to store the integer found under @a name
 */
struct MAJ_Specification
MAJ_spec_uint16 (const char *name,
                 uint16_t *u16)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_UINT16,
      .field = name,
      .details.u16 = u16
    };
  return ret;
}


/**
 * JSON object.
 *
 * @param name name of the JSON field
 * @param[out] jsonp where to store the JSON found under @a name
 */
struct MAJ_Specification
MAJ_spec_json (const char *name,
               json_t **jsonp)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_JSON_OBJECT,
      .field = name,
      .details.obj = jsonp
    };
  return ret;
}


/**
 * Specification for parsing an RSA public key.
 *
 * @param name name of the JSON field
 * @param pk where to store the RSA key found under @a name
 */
struct MAJ_Specification
MAJ_spec_rsa_public_key (const char *name,
                         struct GNUNET_CRYPTO_rsa_PublicKey **pk)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_RSA_PUBLIC_KEY,
      .field = name,
      .details.rsa_public_key = pk
    };
  return ret;
}


/**
 * Specification for parsing an RSA signature.
 *
 * @param name name of the JSON field
 * @param sig where to store the RSA signature found under @a name
 */
struct MAJ_Specification
MAJ_spec_rsa_signature (const char *name,
                        struct GNUNET_CRYPTO_rsa_Signature **sig)
{
  struct MAJ_Specification ret =
    {
      .cmd = MAJ_CMD_RSA_SIGNATURE,
      .field = name,
      .details.rsa_signature = sig
    };
  return ret;
}


/* end of merchant_api_json.c */
