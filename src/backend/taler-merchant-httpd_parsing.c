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
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/

/**
 * @file taler-exchange-httpd_parsing.c
 * @brief functions to parse incoming requests (MHD arguments and JSON snippets)
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
 * Initialize a buffer and copy first chunk of data in it.
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
  if (0 == alloc_size)
  {
    buf->data = NULL;
    return GNUNET_OK;
  }
  buf->data = GNUNET_malloc (alloc_size);
  GNUNET_memcpy (buf->data,
                 data,
                 data_size);
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
  GNUNET_free_non_null (buf->data);
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
    size_t new_size = buf->alloc ? buf->alloc : 1;
    
    while (new_size < buf->fill + data_size)
      new_size *= 2;
    if (new_size > max_size)
      return GNUNET_NO;
    new_buf = GNUNET_malloc (new_size);
    memcpy (new_buf, buf->data, buf->fill);
    GNUNET_free (buf->data);
    buf->data = new_buf;
    buf->alloc = new_size;
  }
  memcpy (buf->data + buf->fill,
	  data,
	  data_size);
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
  struct Buffer *r = *con_cls;
  
  TALER_LOG_DEBUG ("Will parse: %.*s\n",
                   (int) *upload_data_size,
                   upload_data);
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
      /* return GNUNET_SYSERR if this isn't even
       * able to generate proper error response.  */
      return (MHD_NO == TMH_RESPONSE_reply_internal_error
        (connection,
         TALER_EC_PARSER_OUT_OF_MEMORY,
         "out of memory")) ? GNUNET_SYSERR : GNUNET_NO;
    }
    /* everything OK, wait for more POST data */
    *upload_data_size = 0;
    *con_cls = r;
    return GNUNET_YES;
  }

  /* When zero, upload is over.  */
  if (0 != *upload_data_size)
  {
    TALER_LOG_INFO ("Parser asking for more data"
                    ", current data size is %zu\n",
                    *upload_data_size);

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
      return (MHD_NO == TMH_RESPONSE_reply_request_too_large
        (connection)) ? GNUNET_SYSERR : GNUNET_NO;
    }

    /* everything OK, wait for more POST data */
    *upload_data_size = 0;
    return GNUNET_YES;
  }

  TALER_LOG_DEBUG ("About to parse: %.*s\n",
                   (int) r->fill,
                   r->data);
  /* We have seen the whole request. */
  *json = json_loadb (r->data,
                      r->fill,
                      0,
                      NULL);
  if (NULL == *json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to parse JSON request body\n");
    return (MHD_YES == TMH_RESPONSE_reply_invalid_json
      (connection)) ? GNUNET_NO : GNUNET_SYSERR;
  }
  buffer_deinit (r);
  GNUNET_free (r);
  *con_cls = NULL;

  return GNUNET_YES;
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
