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
 * @file taler-mint-httpd_refresh.h
 * @brief Handle /refresh/ requests
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_REFRESH_H
#define TALER_MINT_HTTPD_REFRESH_H

#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include "taler-mint-httpd.h"


/**
 * Handle a "/refresh/melt" request.  Parses the request into the JSON
 * components and then hands things of to #handle_refresh_melt_json()
 * to validate the melted coins, the signature and execute the melt
 * using TMH_DB_execute_refresh_melt().
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
TMH_REFRESH_handler_refresh_melt (struct TMH_RequestHandler *rh,
                                  struct MHD_Connection *connection,
                                  void **connection_cls,
                                  const char *upload_data,
                                  size_t *upload_data_size);


/**
 * Handle a "/refresh/reveal" request.  This time, the client reveals
 * the private transfer keys except for the cut-and-choose value
 * returned from "/refresh/commit".  This function parses the revealed
 * keys and secrets and ultimately passes everything to
 * #TMH_DB_execute_refresh_reveal() which will verify that the
 * revealed information is valid then returns the signed refreshed
 * coins.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_REFRESH_handler_refresh_reveal (struct TMH_RequestHandler *rh,
                                    struct MHD_Connection *connection,
                                    void **connection_cls,
                                    const char *upload_data,
                                    size_t *upload_data_size);


/**
 * Handle a "/refresh/link" request
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_REFRESH_handler_refresh_link (struct TMH_RequestHandler *rh,
                                  struct MHD_Connection *connection,
                                  void **connection_cls,
                                  const char *upload_data,
                                  size_t *upload_data_size);


#endif
