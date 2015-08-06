/*
  This file is part of TALER
  Copyright (C) 2014 GNUnet e.V.

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
 * @file taler-mint-httpd_deposit.h
 * @brief Handle /deposit requests
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_DEPOSIT_H
#define TALER_MINT_HTTPD_DEPOSIT_H

#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-mint-httpd.h"


/**
 * Handle a "/deposit" request.  Parses the JSON in the post to find
 * the "type" (either DIRECT_DEPOSIT or INCREMENTAL_DEPOSIT), and, if
 * successful, passes the JSON data to
 * #parse_and_handle_deposit_request() to further check the details
 * of the operation specified in the "wire" field of the JSON data.
 * If everything checks out, this will ultimately lead to the
 * "/deposit" being executed, or rejected.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_DEPOSIT_handler_deposit (struct TMH_RequestHandler *rh,
                             struct MHD_Connection *connection,
                             void **connection_cls,
                             const char *upload_data,
                             size_t *upload_data_size);

#endif
