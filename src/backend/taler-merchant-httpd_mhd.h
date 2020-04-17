/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V. and INRIA

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
 * @file taler-merchant-httpd_mhd.h
 * @brief helpers for MHD interaction, used to generate simple responses
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_EXCHANGE_HTTPD_MHD_H
#define TALER_EXCHANGE_HTTPD_MHD_H
#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-merchant-httpd.h"


/**
 * Function to call to handle the request by sending
 * back static data from the @a rh.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc handler context (can be updated)
 * @return MHD result code
 */
MHD_RESULT
TMH_MHD_handler_static_response (const struct TMH_RequestHandler *rh,
                                 struct MHD_Connection *connection,
                                 struct TMH_HandlerContext *hc);


/**
 * Function to call to handle the request by sending
 * back a redirect to the AGPL source code.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc handler context (can be updated)
 * @return MHD result code
 */
MHD_RESULT
TMH_MHD_handler_agpl_redirect (const struct TMH_RequestHandler *rh,
                               struct MHD_Connection *connection,
                               struct TMH_HandlerContext *hc);


/**
 * Function to call to handle the request by building a JSON
 * reply from varargs.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param response_code HTTP response code to use
 * @param do_cache can the response be cached? (0: no, 1: yes)
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD result code
 */
MHD_RESULT
TMH_MHD_helper_send_json_pack (struct TMH_RequestHandler *rh,
                               struct MHD_Connection *connection,
                               void *connection_cls,
                               int response_code,
                               int do_cache,
                               const char *fmt,
                               ...);


/**
 * Function to call to handle the request by building a JSON
 * reply with an error message from @a rh.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
MHD_RESULT
TMH_MHD_handler_send_json_pack_error (struct TMH_RequestHandler *rh,
                                      struct MHD_Connection *connection,
                                      void **connection_cls,
                                      const char *upload_data,
                                      size_t *upload_data_size,
                                      struct TMH_MerchantInstance *mi);


#endif
