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
 * @file taler-mint-httpd_withdraw.h
 * @brief Handle /withdraw/ requests
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_MINT_HTTPD_WITHDRAW_H
#define TALER_MINT_HTTPD_WITHDRAW_H

#include <microhttpd.h>
#include "taler-mint-httpd.h"

/**
 * Handle a "/withdraw/status" request.  Parses the
 * given "reserve_pub" argument (which should contain the
 * EdDSA public key of a reserve) and then respond with the
 * status of the reserve.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_WITHDRAW_handler_withdraw_status (struct TMH_RequestHandler *rh,
                                      struct MHD_Connection *connection,
                                      void **connection_cls,
                                      const char *upload_data,
                                      size_t *upload_data_size);


/**
 * Handle a "/withdraw/sign" request.  Parses the "reserve_pub"
 * EdDSA key of the reserve and the requested "denom_pub" which
 * specifies the key/value of the coin to be withdrawn, and checks
 * that the signature "reserve_sig" makes this a valid withdrawl
 * request from the specified reserve.  If so, the envelope
 * with the blinded coin "coin_ev" is passed down to execute the
 * withdrawl operation.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_WITHDRAW_handler_withdraw_sign (struct TMH_RequestHandler *rh,
                                    struct MHD_Connection *connection,
                                    void **connection_cls,
                                    const char *upload_data,
                                    size_t *upload_data_size);

#endif
