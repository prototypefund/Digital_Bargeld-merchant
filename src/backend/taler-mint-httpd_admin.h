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
 * @file taler-mint-httpd_admin.h
 * @brief Handle /admin/ requests
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_ADMIN_H
#define TALER_MINT_HTTPD_ADMIN_H

#include <microhttpd.h>
#include "taler-mint-httpd.h"

/**
 * Handle a "/admin/add/incoming" request.  Parses the
 * given "reserve_pub", "amount", "transaction" and "h_wire"
 * details and adds the respective transaction to the database.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_ADMIN_handler_admin_add_incoming (struct TMH_RequestHandler *rh,
                                      struct MHD_Connection *connection,
                                      void **connection_cls,
                                      const char *upload_data,
                                      size_t *upload_data_size);

#endif
