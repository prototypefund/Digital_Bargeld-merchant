/*
  This file is part of TALER
  Copyright (C) 2014-2019 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-merchant-httpd_mhd.c
 * @brief helpers for MHD interaction; these are TALER_EXCHANGE_handler_ functions
 *        that generate simple MHD replies that do not require any real operations
 *        to be performed (error handling, static pages, etc.)
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <jansson.h>
#include "taler-merchant-httpd_mhd.h"


/**
 * Function to call to handle the request by sending
 * back static data from the @a rh.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc handler context (can be updated)
 * @return MHD result code
 */
MHD_RESULT
TMH_MHD_handler_static_response (const struct TMH_RequestHandler *rh,
                                 struct MHD_Connection *connection,
                                 struct TMH_HandlerContext *hc)
{
  (void) hc;
  return TALER_MHD_reply_static (connection,
                                 rh->response_code,
                                 rh->mime_type,
                                 rh->data,
                                 rh->data_size);
}


/**
 * Function to call to handle the request by sending
 * back a redirect to the AGPL source code.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc handler context (can be updated)
 * @return MHD result code
 */
MHD_RESULT
TMH_MHD_handler_agpl_redirect (const struct TMH_RequestHandler *rh,
                               struct MHD_Connection *connection,
                               struct TMH_HandlerContext *hc)
{
  (void) rh;
  (void) hc;
  return TALER_MHD_reply_agpl (connection,
                               "http://www.git.taler.net/?p=merchant.git");
}


/* end of taler-exchange-httpd_mhd.c */
