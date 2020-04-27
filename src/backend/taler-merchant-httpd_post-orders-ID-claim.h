/*
  This file is part of TALER
  (C) 2014, 2015, 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_post-orders-ID-claim.h
 * @brief headers for POST /orders/$ID/claim handler
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_POST_ORDERS_ID_CLAIM_H
#define TALER_MERCHANT_HTTPD_POST_ORDERS_ID_CLAIM_H
#include <microhttpd.h>
#include "taler-merchant-httpd.h"

/**
 * Manage a POST /orders/$ID/claim request.  Allows the client to
 * claim the order (unless already claims) and creates the respective
 * contract.  Returns the contract terms.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_post_orders_ID_claim (const struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          struct TMH_HandlerContext *hc);

#endif
