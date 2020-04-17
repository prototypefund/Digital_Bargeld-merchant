/*
  This file is part of TALER
  (C) 2019, 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_config.h
 * @brief headers for /config handler
 * @author Florian Dold
 */
#ifndef TALER_MERCHANT_HTTPD_CONFIG_H
#define TALER_MERCHANT_HTTPD_CONFIG_H
#include <microhttpd.h>
#include "taler-merchant-httpd.h"

/**
 * Manages a /config call.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc handler context (can be updated)
 * @return MHD result code
 */
MHD_RESULT
MH_handler_config (const struct TMH_RequestHandler *rh,
                   struct MHD_Connection *connection,
                   struct TMH_HandlerContext *hc);

#endif
