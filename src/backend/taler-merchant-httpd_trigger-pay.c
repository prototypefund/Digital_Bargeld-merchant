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
 * @file backend/taler-merchant-httpd_check-payment.c
 * @brief implementation of /check-payment handler
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

  const char *contract_url;
  const char *h_contract_terms_str;
  const char *confirm_url;
  const char *session_id;

  session_id = MHD_lookup_connection_value (connection,
                                            MHD_GET_ARGUMENT_KIND,
                                            "session_id");

  confirm_url = MHD_lookup_connection_value (connection,
                                             MHD_GET_ARGUMENT_KIND,
                                             "confirm_url");

  contract_url = MHD_lookup_connection_value (connection,
                                              MHD_GET_ARGUMENT_KIND,
                                              "contract_url");

  h_contract_terms_str = MHD_lookup_connection_value (connection,
                                                      MHD_GET_ARGUMENT_KIND,
                                                      "h_contract_terms");


  // FIXME: Taler wallet detection!
  char *data = "<html><body><p>Processing payment ...</p></body></html>";

  response = MHD_create_response_from_buffer (strlen (data), data, MHD_RESPMEM_PERSISTENT);
  if (NULL != session_id)
    MHD_add_response_header (response, "X-Taler-Session-Id", session_id);
  if (NULL != contract_url)
    MHD_add_response_header (response, "X-Taler-Contract-Url", contract_url);
  if (NULL != h_contract_terms_str)
    MHD_add_response_header (response, "X-Taler-Contract-Hash", h_contract_terms_str);
  if (NULL != confirm_url)
    MHD_add_response_header (response, "X-Taler-Confirm-Url", confirm_url);
  MHD_queue_response (connection, 402, response);
  MHD_destroy_response (response);

  return MHD_YES;
}
