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
#include "taler-merchant-httpd_tip-query.h"
#include "taler-merchant-httpd_tip-reserve-helper.h"


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


static int
add_instance (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  json_t *ja = cls;
  struct MerchantInstance *mi = value;
  char *url;
  json_t *pta;

  /* Compile array of all unique wire methods supported by this
     instance */
  pta = json_array ();
  GNUNET_assert (NULL != pta);
  for (struct WireMethod *wm = mi->wm_head;
       NULL != wm;
       wm = wm->next)
  {
    int duplicate = GNUNET_NO;

    if (! wm->active)
      break;
    /* Yes, O(n^2), but really how many bank accounts can an
       instance realistically have for this to matter? */
    for (struct WireMethod *pm = mi->wm_head;
         pm != wm;
         pm = pm->next)
      if (0 == strcasecmp (pm->wire_method,
                           wm->wire_method))
      {
        duplicate = GNUNET_YES;
        break;
      }
    if (duplicate)
      continue;
    GNUNET_assert (0 ==
                   json_array_append_new (pta,
                                          json_string (wm->wire_method)));
  }
  GNUNET_asprintf (&url,
                   "/%s/",
                   mi->id);
  GNUNET_assert (0 ==
                 json_array_append_new (
                   ja,
                   json_pack (
                     (NULL != mi->tip_exchange)
                     ? "{s:s, s:s, s:o, s:o, s:s}"
                     : "{s:s, s:s, s:o, s:o}",
                     "name",
                     mi->name,
                     "backend_base_url",
                     url,
                     "merchant_pub",
                     GNUNET_JSON_from_data_auto (&mi->pubkey),
                     "payment_targets",
                     pta,
                     /* optional: */
                     "tipping_exchange_baseurl",
                     mi->tip_exchange)));
  GNUNET_free (url);
  return GNUNET_OK;
}


/**
 * Handle a "/config" request.
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
MH_handler_config (struct TMH_RequestHandler *rh,
                   struct MHD_Connection *connection,
                   void **connection_cls,
                   const char *upload_data,
                   size_t *upload_data_size,
                   struct MerchantInstance *mi)
{
  static struct MHD_Response *response;

  (void) rh;
  (void) connection_cls;
  (void) upload_data;
  (void) upload_data_size;
  (void) mi;
  if (NULL == response)
  {
    json_t *ia;

    ia = json_array ();
    GNUNET_assert (NULL != ia);
    GNUNET_CONTAINER_multihashmap_iterate (by_id_map,
                                           &add_instance,
                                           ia);
    response = TALER_MHD_make_json_pack ("{s:s, s:s, s:o}",
                                         "currency", TMH_currency,
                                         "version", MERCHANT_PROTOCOL_VERSION,
                                         "instances", ia);

  }
  return MHD_queue_response (connection,
                             MHD_HTTP_OK,
                             response);
}


/* end of taler-merchant-httpd_config.c */
