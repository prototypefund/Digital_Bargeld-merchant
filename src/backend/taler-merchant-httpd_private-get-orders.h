/*
  This file is part of TALER
  (C) 2019, 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_private-get-orders.h
 * @brief implement GET /orders
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_PRIVATE_GET_ORDERS_H
#define TALER_MERCHANT_HTTPD_PRIVATE_GET_ORDERS_H

#include "taler-merchant-httpd.h"


/**
 * Handle a GET "/orders" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_get_orders (const struct TMH_RequestHandler *rh,
                        struct MHD_Connection *connection,
                        struct TMH_HandlerContext *hc);


/**
 * There has been a change or addition of a new @a order_id.  Wake up
 * long-polling clients that may have been waiting for this event.
 *
 * @param instance_id the instance where the order changed
 * @param order_id the order that changed
 * @param paid is the order paid by the customer?
 * @param refunded was the order refunded?
 * @param wire was the merchant paid via wire transfer?
 * @param data execution date of the order
 * @param order_serial_id serial ID of the order in the database
 */
void
TMH_notify_order_change (const char *instance_id,
                         const char *order_id,
                         bool paid,
                         bool refunded,
                         bool wired,
                         struct GNUNET_TIME_Absolute date,
                         uint64_t order_serial_id);


/**
 * We are shutting down, force resume of all GET /orders requests.
 */
void
TMH_force_get_orders_resume (void);


/* end of taler-merchant-httpd_private-get-orders.h */
#endif
