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
 * @file backend/taler-merchant-httpd_config.c
 * @brief implement API for querying configuration data of the backend
 * @author Florian Dold
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_exchanges.h"


/**
 * Taler protocol version in the format CURRENT:REVISION:AGE
 * as used by GNU libtool.  See
 * https://www.gnu.org/software/libtool/manual/html_node/Libtool-versioning.html
 *
 * Please be very careful when updating and follow
 * https://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html#Updating-version-info
 * precisely.  Note that this version has NOTHING to do with the
 * release version, and the format is NOT the same that semantic
 * versioning uses either.
 *
 * When changing this version, you likely want to also update
 * #MERCHANT_PROTOCOL_CURRENT and #MERCHANT_PROTOCOL_AGE in
 * merchant_api_config.c!
 */
#define MERCHANT_PROTOCOL_VERSION "0:0:0"


/**
 * Handle a "/config" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc handler context (can be updated)
 * @return MHD result code
 */
MHD_RESULT
MH_handler_config (struct TMH_RequestHandler *rh,
                   struct MHD_Connection *connection,
                   struct TMH_HandlerContext *hc)
{
  static struct MHD_Response *response;

  (void) rh;
  (void) hc;
  if (NULL == response)
  {
    response = TALER_MHD_make_json_pack ("{s:s, s:s }",
                                         "currency", TMH_currency,
                                         "version", MERCHANT_PROTOCOL_VERSION);
  }
  return MHD_queue_response (connection,
                             MHD_HTTP_OK,
                             response);
}


/* end of taler-merchant-httpd_config.c */
