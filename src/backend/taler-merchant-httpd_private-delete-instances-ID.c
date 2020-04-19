/*
  This file is part of TALER
  (C) 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_private-delete-instances-ID.c
 * @brief implement DELETE /instances/$ID
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-delete-instances-ID.h"
#include <taler/taler_json_lib.h>


/**
 * Handle a DELETE "/instances/$ID" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_delete_instances_ID (const struct TMH_RequestHandler *rh,
                                 struct MHD_Connection *connection,
                                 struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  const char *purge;
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_assert (NULL != mi);
  purge = MHD_lookup_connection_value (connection,
                                       MHD_GET_ARGUMENT_KIND,
                                       "purge");
  if ( (NULL != purge) &&
       (0 == strcmp (purge,
                     "yes")) )
    qs = TMH_db->purge_instance (TMH_db->cls,
                                 mi->settings.id);
  else
    qs = TMH_db->delete_instance_private_key (TMH_db->cls,
                                              mi->settings.id);
  switch (qs)
  {
  case GNUNET_DB_STATUS_HARD_ERROR:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_DELETE_INSTANCES_ID_DB_HARD_FAILURE,
                                       "Transaction failed");
  case GNUNET_DB_STATUS_SOFT_ERROR:
    GNUNET_break (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                       "Serialization error for single SQL statement");
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_DELETE_INSTANCES_ID_NO_SUCH_INSTANCE,
                                       ( (NULL != purge) &&
                                         (0 == strcmp (purge,
                                                       "yes")) )
                                       ? "Instance unknown"
                                       : "Private key unknown");
  case GNUNET_DB_STATUS_SUCCESS_ONE_RESULT:
    return TALER_MHD_reply_static (connection,
                                   MHD_HTTP_NO_CONTENT,
                                   NULL,
                                   NULL,
                                   0);
  }
  GNUNET_assert (0);
  return MHD_NO;
}


/* end of taler-merchant-httpd_private-delete-instances-ID.c */
