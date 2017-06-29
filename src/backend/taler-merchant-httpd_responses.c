/*
  This file is part of TALER
  Copyright (C) 2014-2017 GNUnet e.V.

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
 * @file taler-merchant-httpd_responses.c
 * @brief API for generating the various replies of the exchange; these
 *        functions are called TMH_RESPONSE_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_responses.h"
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include <gnunet/gnunet_util_lib.h>


/**
 * Make JSON response object.
 *
 * @param json the json object
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_json (const json_t *json)
{
  struct MHD_Response *resp;
  char *json_str;

  json_str = json_dumps (json,
			 JSON_INDENT(2));
  GNUNET_assert (NULL != json_str);
  resp = MHD_create_response_from_buffer (strlen (json_str),
					  json_str,
                                          MHD_RESPMEM_MUST_FREE);
  if (NULL == resp)
  {
    free (json_str);
    GNUNET_break (0);
    return NULL;
  }
  (void) MHD_add_response_header (resp,
                                  MHD_HTTP_HEADER_CONTENT_TYPE,
                                  "application/json");
  return resp;
}


/**
 * Send JSON object as response.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json (struct MHD_Connection *connection,
                         const json_t *json,
                         unsigned int response_code)
{
  struct MHD_Response *resp;
  int ret;

  resp = TMH_RESPONSE_make_json (json);
  if (NULL == resp)
    return MHD_NO;
  ret = MHD_queue_response (connection,
                            response_code,
                            resp);
  MHD_destroy_response (resp);
  return ret;
}


/**
 * Make JSON response object.
 *
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_json_pack (const char *fmt,
                             ...)
{
  json_t *json;
  va_list argp;
  struct MHD_Response *ret;
  json_error_t jerror;

  va_start (argp, fmt);
  json = json_vpack_ex (&jerror,
			0,
			fmt,
			argp);
  va_end (argp);
  if (NULL == json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to pack JSON with format `%s': %s\n",
                fmt,
                jerror.text);
    GNUNET_break (0);
    return MHD_NO;
  }
  ret = TMH_RESPONSE_make_json (json);
  json_decref (json);
  return ret;
}


/**
 * Function to call to handle the request by building a JSON
 * reply from a format string and varargs.
 *
 * @param connection the MHD connection to handle
 * @param response_code HTTP response code to use
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json_pack (struct MHD_Connection *connection,
                              unsigned int response_code,
                              const char *fmt,
                              ...)
{
  json_t *json;
  va_list argp;
  int ret;
  json_error_t jerror;

  va_start (argp, fmt);
  json = json_vpack_ex (&jerror,
			0,
			fmt,
			argp);
  va_end (argp);
  if (NULL == json)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to pack JSON with format `%s': %s\n",
                fmt,
                jerror.text);
    GNUNET_break (0);
    return MHD_NO;
  }
  ret = TMH_RESPONSE_reply_json (connection,
                                 json,
                                 response_code);
  json_decref (json);
  return ret;
}


/**
 * Create a response indicating an internal error.
 *
 * @param ec error code to return
 * @param hint hint about the internal error's nature
 * @return a MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_internal_error (enum TALER_ErrorCode ec,
				  const char *hint)
{
  return TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:s}",
                                      "error", "internal error",
				      "code", (json_int_t) ec,
                                      "hint", hint);
}


/**
 * Send a response indicating an internal error.
 *
 * @param connection the MHD connection to use
 * @param ec error code to return
 * @param hint hint about the internal error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_internal_error (struct MHD_Connection *connection,
				   enum TALER_ErrorCode ec,
                                   const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       "{s:s, s:I, s:s}",
                                       "error", "internal error",
				       "code", (json_int_t) ec,
                                       "hint", hint);
}


/**
 * Send a response indicating that the request was too big.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_request_too_large (struct MHD_Connection *connection)
{
  struct MHD_Response *resp;
  int ret;

  resp = MHD_create_response_from_buffer (0,
                                          NULL,
                                          MHD_RESPMEM_PERSISTENT);
  if (NULL == resp)
    return MHD_NO;
  ret = MHD_queue_response (connection,
                            MHD_HTTP_REQUEST_ENTITY_TOO_LARGE,
                            resp);
  MHD_destroy_response (resp);
  return ret;
}


/**
 * Send a response indicating that the JSON was malformed.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_invalid_json (struct MHD_Connection *connection)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:I, s:s}",
				       "code", (json_int_t) TALER_EC_JSON_INVALID,
                                       "error", "invalid json");
}


/**
 * Send a response indicating that we did not find the @a object
 * needed for the reply.
 *
 * @param connection the MHD connection to use
 * @param ec error code to return
 * @param object name of the object we did not find
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_not_found (struct MHD_Connection *connection,
			      enum TALER_ErrorCode ec,
                              const char *object)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       "{s:I, s:s}",
				       "code", (json_int_t) ec,       
                                       "error", object);
}


/**
 * Send a response indicating that the request was malformed.
 *
 * @param connection the MHD connection to use
 * @param ec error code to return
 * @param issue description of what was wrong with the request
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_bad_request (struct MHD_Connection *connection,
				enum TALER_ErrorCode ec,
                                const char *issue)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:I, s:s}",
				       "code", (json_int_t) ec,
                                       "error", issue);
}


/**
 * Add headers we want to return in every response.
 * Useful for testing, like if we want to always close
 * connections.
 *
 * @param response response to modify
 */
void
TMH_RESPONSE_add_global_headers (struct MHD_Response *response)
{
  if (TMH_merchant_connection_close)
    (void) MHD_add_response_header (response,
                                    MHD_HTTP_HEADER_CONNECTION,
                                    "close");
}


/**
 * Send a response indicating an external error.
 *
 * @param connection the MHD connection to use
 * @param ec error code to return
 * @param hint hint about the error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_external_error (struct MHD_Connection *connection,
				   enum TALER_ErrorCode ec,
                                   const char *hint)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s, s:I, s:s}",
                                       "error", "client error",
				       "code", (json_int_t) ec,
                                       "hint", hint);
}


/**
 * Create a response indicating an external error.
 *
 * @param ec error code to return
 * @param hint hint about the internal error's nature
 * @return a MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_external_error (enum TALER_ErrorCode ec,
				  const char *hint)
{
  return TMH_RESPONSE_make_json_pack ("{s:s, s:I, s:s}",
                                      "error", "client error",
				      "code", (json_int_t) ec,
                                      "hint", hint);
}


/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param ec error code to return
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_missing (struct MHD_Connection *connection,
				enum TALER_ErrorCode ec,
                                const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{ s:s, s:I, s:s}",
                                       "error", "missing parameter",
				       "code", (json_int_t) ec,
                                       "parameter", param_name);
}


/**
 * Send a response indicating an invalid argument.
 *
 * @param connection the MHD connection to use
 * @param ec error code to return
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_invalid (struct MHD_Connection *connection,
				enum TALER_ErrorCode ec,
				const char *param_name)
{
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       "{s:s, s:I, s:s}",
                                       "error", "invalid parameter",
				       "code", (json_int_t) ec,
                                       "parameter", param_name);
}


/* end of taler-exchange-httpd_responses.c */
