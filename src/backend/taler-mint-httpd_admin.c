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
 * @file taler-mint-httpd_admin.c
 * @brief Handle /admin/ requests
 * @author Christian Grothoff
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include "taler-mint-httpd_admin.h"
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"


/**
 * Check permissions (we only allow access to /admin/ from loopback).
 *
 * @param connection connection to perform access check for
 * @return #GNUNET_OK if permitted,
 *         #GNUNET_NO if denied and error was queued,
 *         #GNUNET_SYSERR if denied and we failed to report
 */
static int
check_permissions (struct MHD_Connection *connection)
{
  const union MHD_ConnectionInfo *ci;
  const struct sockaddr *addr;
  int res;

  ci = MHD_get_connection_info (connection,
                                MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  if (NULL == ci)
  {
    GNUNET_break (0);
    res = TMH_RESPONSE_reply_internal_error (connection,
                                             "Failed to verify client address");
    return (MHD_YES == res) ? GNUNET_NO : GNUNET_SYSERR;
  }
  addr = ci->client_addr;
  switch (addr->sa_family)
  {
  case AF_INET:
    {
      const struct sockaddr_in *sin = (const struct sockaddr_in *) addr;

      if (INADDR_LOOPBACK != ntohl (sin->sin_addr.s_addr))
      {
        res = TMH_RESPONSE_reply_permission_denied (connection,
                                                    "/admin/ only allowed via loopback");
        return (MHD_YES == res) ? GNUNET_NO : GNUNET_SYSERR;
      }
      break;
    }
  case AF_INET6:
    {
      const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) addr;

      if (! IN6_IS_ADDR_LOOPBACK (&sin6->sin6_addr))
      {
        res = TMH_RESPONSE_reply_permission_denied (connection,
                                                    "/admin/ only allowed via loopback");
        return (MHD_YES == res) ? GNUNET_NO : GNUNET_SYSERR;
      }
      break;
    }
  default:
    GNUNET_break (0);
    res = TMH_RESPONSE_reply_internal_error (connection,
                                             "Unsupported AF");
    return (MHD_YES == res) ? GNUNET_NO : GNUNET_SYSERR;
  }
  return GNUNET_OK;
}



/**
 * Handle a "/admin/add/incoming" request.  Parses the
 * given "reserve_pub", "amount", "transaction" and "h_wire"
 * details and adds the respective transaction to the database.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
  */
int
TMH_ADMIN_handler_admin_add_incoming (struct TMH_RequestHandler *rh,
                                      struct MHD_Connection *connection,
                                      void **connection_cls,
                                      const char *upload_data,
                                      size_t *upload_data_size)
{
  struct TALER_ReservePublicKeyP reserve_pub;
  struct TALER_Amount amount;
  struct GNUNET_TIME_Absolute at;
  json_t *wire;
  json_t *root;
  struct TMH_PARSE_FieldSpecification spec[] = {
    TMH_PARSE_member_fixed ("reserve_pub", &reserve_pub),
    TMH_PARSE_member_amount ("amount", &amount),
    TMH_PARSE_member_time_abs ("execution_date", &at),
    TMH_PARSE_member_object ("wire", &wire),
    TMH_PARSE_MEMBER_END
  };
  int res;

  res = check_permissions (connection);
  if (GNUNET_OK != res)
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  res = TMH_PARSE_post_json (connection,
                             connection_cls,
                             upload_data,
                             upload_data_size,
                             &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  if ( (GNUNET_NO == res) || (NULL == root) )
    return MHD_YES;
  res = TMH_PARSE_json_data (connection,
                             root,
                             spec);
  if (GNUNET_OK != res)
  {
    json_decref (root);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  if (GNUNET_YES !=
      TALER_json_validate_wireformat (TMH_expected_wire_format,
				      wire))
  {
    TMH_PARSE_release_data (spec);
    json_decref (root);
    return TMH_RESPONSE_reply_arg_unknown (connection,
                                           "wire");
  }
  res = TMH_DB_execute_admin_add_incoming (connection,
                                           &reserve_pub,
                                           &amount,
                                           at,
                                           wire);
  TMH_PARSE_release_data (spec);
  json_decref (root);
  return res;
}

/* end of taler-mint-httpd_admin.c */
