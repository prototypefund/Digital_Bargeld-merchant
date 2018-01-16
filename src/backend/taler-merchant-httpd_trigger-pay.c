/*
  This file is part of TALER
  (C) 2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_trigger-pay.c
 * @brief implementation of /trigger-pay handler
 * @author Florian Dold
 */
#include "platform.h"
#include <string.h>
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_trigger-pay.h"


/**
 * Add a header to the response from a query parameter.
 *
 *
 * @param connection connection to take query parameters from
 * @param arg_name name of query parameter
 * @param response response that receives the header
 * @param header_name name of the header to set
 */
void
add_header_from_arg (struct MHD_Connection *connection, const char *arg_name,
                     struct MHD_Response *response, const char *header_name)
{
  const char *arg = MHD_lookup_connection_value (connection,
                                                 MHD_GET_ARGUMENT_KIND,
                                                 arg_name);
  if (NULL == arg)
    return;

  MHD_add_response_header (response, header_name, arg);
}


/**
 * Serves a request to browsers to trigger a payment.
 * Contains all the logic to handle different platforms, so that the frontend
 * does not have to handle that.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_trigger_pay (struct TMH_RequestHandler *rh,
                        struct MHD_Connection *connection,
                        void **connection_cls,
                        const char *upload_data,
                        size_t *upload_data_size)
{
  struct MHD_Response *response;


  // FIXME: Taler wallet detection!
  char *data = "<html><body><p>Processing payment ...</p></body></html>";

  response = MHD_create_response_from_buffer (strlen (data), data, MHD_RESPMEM_PERSISTENT);

  add_header_from_arg (connection, "session_id", response, "X-Taler-Session-Id");
  add_header_from_arg (connection, "contract_url", response, "X-Taler-Contract-Url");
  add_header_from_arg (connection, "h_contract_terms", response, "X-Taler-Contract-Hash");
  add_header_from_arg (connection, "tip_token", response, "X-Taler-Tip");
  add_header_from_arg (connection, "refund_url", response, "X-Taler-Refund-Url");
  add_header_from_arg (connection, "resource_url", response, "X-Taler-Resoure-Url");

  MHD_queue_response (connection, 402, response);
  MHD_destroy_response (response);

  return MHD_YES;
}
