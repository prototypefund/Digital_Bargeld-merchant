/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation; either version 3,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public
  License along with TALER; see the file COPYING.  If not,
  see <http://www.gnu.org/licenses/>
*/

/**
 * @file taler-exchange-httpd_parsing.c
 * @brief functions to parse incoming requests
 *        (MHD arguments and JSON snippets)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_responses.h"

/* FIXME: de-duplicate code with taler-exchange-httpd_parsing.c
   and taler-exchange-httpd_response.c */

/**
 * Initial size for POST request buffer.
 */
#define REQUEST_BUFFER_INITIAL (2 * 1024)

/**
 * Maximum POST request size.
 */
#define REQUEST_BUFFER_MAX (1024 * 1024)


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
 * Free the data in a buffer.  Does *not* free
 * the buffer object itself.
 *
 * @param buf buffer to de-initialize
 */
static void
buffer_deinit (struct Buffer *buf)
{
  GNUNET_free_non_null (buf->data);
  buf->data = NULL;
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
 * Process a POST request containing a JSON object.  This function
 * realizes an MHD POST processor that will (incrementally) process
 * JSON data uploaded to the HTTP server.  It will store the
 * required state in the @a con_cls, which must be cleaned up
 * using #TMH_PARSE_post_cleanup_callback().
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
 *    #GNUNET_NO if request is incomplete or invalid
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
  enum GNUNET_JSON_PostResult pr;

  pr = GNUNET_JSON_post_parser (REQUEST_BUFFER_MAX,
                                connection,
                                con_cls,
                                upload_data,
                                upload_data_size,
                                json);
  switch (pr)
  {

  case GNUNET_JSON_PR_OUT_OF_MEMORY:
    return (MHD_NO == TMH_RESPONSE_reply_internal_error
              (connection,
              TALER_EC_PARSER_OUT_OF_MEMORY,
              "out of memory")) ? GNUNET_SYSERR : GNUNET_NO;

  case GNUNET_JSON_PR_CONTINUE:
    return GNUNET_YES;

  case GNUNET_JSON_PR_REQUEST_TOO_LARGE:
    return (MHD_NO == TMH_RESPONSE_reply_request_too_large
              (connection)) ? GNUNET_SYSERR : GNUNET_NO;

  case GNUNET_JSON_PR_JSON_INVALID:
    return (MHD_YES ==
            TMH_RESPONSE_reply_invalid_json (connection))
           ? GNUNET_NO : GNUNET_SYSERR;
  case GNUNET_JSON_PR_SUCCESS:
    GNUNET_break (NULL != *json);
    return GNUNET_YES;
  }
  /* this should never happen */
  GNUNET_break (0);
  return GNUNET_SYSERR;
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
 *                #GNUNET_JSON_parse_free() when done)
 *    #GNUNET_NO if json is malformed, error response was generated
 *    #GNUNET_SYSERR on internal error
 */
int
TMH_PARSE_json_data (struct MHD_Connection *connection,
                     const json_t *root,
                     struct GNUNET_JSON_Specification *spec)
{
  int ret;
  const char *error_json_name;
  unsigned int error_line;

  ret = GNUNET_JSON_parse (root,
                           spec,
                           &error_json_name,
                           &error_line);
  if (GNUNET_SYSERR == ret)
  {
    if (NULL == error_json_name)
      error_json_name = "<no field>";
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Parsing failed due to field '%s'\n",
                error_json_name);
    ret = (MHD_YES ==
           TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_BAD_REQUEST,
                                         "{s:s, s:s, s:I}",
                                         "error", "parse error",
                                         "field", error_json_name,
                                         "line", (json_int_t) error_line))
          ? GNUNET_NO : GNUNET_SYSERR;
    return ret;
  }
  return GNUNET_YES;
}



/**
 * Extract base32crockford encoded data from request.
 *
 * Queues an error response to the connection if the parameter is
 * missing or invalid.
 *
 * @param connection the MHD connection
 * @param param_name the name of the parameter with the key
 * @param[out] out_data pointer to store the result
 * @param out_size expected size of data
 * @return
 *   #GNUNET_YES if the the argument is present
 *   #GNUNET_NO if the argument is absent or malformed
 *   #GNUNET_SYSERR on internal error (error response could not be sent)
 */
int
TMH_PARSE_mhd_request_arg_data (struct MHD_Connection *connection,
                                const char *param_name,
                                void *out_data,
                                size_t out_size)
{
  const char *str;

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     param_name);
  if (NULL == str)
  {
    return (MHD_NO ==
            TMH_RESPONSE_reply_arg_missing (connection,
                                            TALER_EC_PARAMETER_MISSING,
                                            param_name))
           ? GNUNET_SYSERR : GNUNET_NO;
  }
  if (GNUNET_OK !=
      GNUNET_STRINGS_string_to_data (str,
                                     strlen (str),
                                     out_data,
                                     out_size))
    return (MHD_NO ==
            TMH_RESPONSE_reply_arg_invalid (connection,
                                            TALER_EC_PARAMETER_MALFORMED,
                                            param_name))
           ? GNUNET_SYSERR : GNUNET_NO;
  return GNUNET_OK;
}


/* end of taler-merchant-httpd_parsing.c */
