/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

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
 * @file backend/taler-merchant-httpd_history.c
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */

#ifndef TALER_MERCHANT_HTTPD_HISTORY_H
#define TALER_MERCHANT_HTTPD_HISTORY_H
#include <microhttpd.h>
#include "taler-merchant-httpd.h"


/**
 * Manage a /history request. Query the db and returns transactions
 * younger than the date given as parameter
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
MH_handler_history (struct TMH_RequestHandler *rh,
                    struct MHD_Connection *connection,
                    void **connection_cls,
                    const char *upload_data,
                    size_t *upload_data_size,
                    struct MerchantInstance *mi);

/* end of taler-merchant-httpd_history.c */
#endif
