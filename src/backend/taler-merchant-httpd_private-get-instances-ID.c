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
 * @file backend/taler-merchant-httpd_private-get-instances-ID.c
 * @brief implement GET /instances/$ID
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-get-instances-ID.h"
#include <taler/taler_json_lib.h>


/**
 * Handle a GET "/instances/$ID" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_get_instances_ID (const struct TMH_RequestHandler *rh,
                              struct MHD_Connection *connection,
                              struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  json_t *ja;

  GNUNET_assert (NULL != mi);
  ja = json_array ();
  GNUNET_assert (NULL != ja);
  for (struct TMH_WireMethod *wm = mi->wm_head;
       NULL != wm;
       wm = wm->next)
  {
    GNUNET_assert (
      0 ==
      json_array_append_new (
        ja,
        json_pack (
          "{s:O, s:o, s:O, s:o}",
          "payto_uri",
          json_object_get (wm->j_wire,
                           "payto_uri"),
          "h_wire",
          GNUNET_JSON_from_data_auto (&wm->h_wire),
          "salt",
          json_object_get (wm->j_wire,
                           "salt"),
          "active",
          (wm->active) ? json_true () : json_false ())));
  }

  return TALER_MHD_reply_json_pack (
    connection,
    MHD_HTTP_OK,
    "{s:o, s:s, s:o, s:O, s:O,"
    " s:o, s:o, s:I, s:o, s:o}",
    "accounts",
    ja,
    "name",
    mi->settings.name,
    "merchant_pub",
    GNUNET_JSON_from_data_auto (
      &mi->merchant_pub),
    "address",
    mi->settings.address,
    "jurisdiction",
    mi->settings.jurisdiction,
    /* end of first group of 5 */
    "default_max_wire_fee",
    TALER_JSON_from_amount (
      &mi->settings.default_max_wire_fee),
    "default_max_deposit_fee",
    TALER_JSON_from_amount (
      &mi->settings.default_max_wire_fee),
    "default_wire_fee_amortization",
    (json_int_t)
    mi->settings.default_wire_fee_amortization,
    "default_wire_transfer_delay",
    GNUNET_JSON_from_time_rel (
      mi->settings.default_wire_transfer_delay),
    "default_pay_delay",
    GNUNET_JSON_from_time_rel (
      mi->settings.default_pay_delay));
}


/* end of taler-merchant-httpd_private-get-instances-ID.c */
