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
 * @file backend/taler-merchant-httpd_tip-enable.h
 * @brief headers for /tip-enable handler
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_TIP_ENABLE_H
#define TALER_MERCHANT_HTTPD_TIP_ENABLE_H
#include <microhttpd.h>
#include "taler-merchant-httpd.h"

/**
 * Manages a /tip-enable call, storing information about the
 * reserve.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_tip_enable (struct TMH_RequestHandler *rh,
                       struct MHD_Connection *connection,
                       void **connection_cls,
                       const char *upload_data,
                       size_t *upload_data_size);

#endif
