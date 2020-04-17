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
 * @file backend/taler-merchant-httpd_private-get-instances.c
 * @brief implement GET /instances
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-get-instances.h"

/**
 * Add merchant instance to our JSON array.
 *
 * @param cls a `json_t *` JSON array to build
 * @param key unused
 * @param value a `struct TMH_MerchantInstance *`
 * @return #GNUNET_OK (continue to iterate)
 */
static int
add_instance (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  json_t *ja = cls;
  struct TMH_MerchantInstance *mi = value;
  json_t *pta;

  (void) key;
  /* Compile array of all unique wire methods supported by this
     instance */
  pta = json_array ();
  GNUNET_assert (NULL != pta);
  for (struct TMH_WireMethod *wm = mi->wm_head;
       NULL != wm;
       wm = wm->next)
  {
    bool duplicate = false;

    if (! wm->active)
      break;
    /* Yes, O(n^2), but really how many bank accounts can an
       instance realistically have for this to matter? */
    for (struct TMH_WireMethod *pm = mi->wm_head;
         pm != wm;
         pm = pm->next)
      if (0 == strcasecmp (pm->wire_method,
                           wm->wire_method))
      {
        duplicate = true;
        break;
      }
    if (duplicate)
      continue;
    GNUNET_assert (0 ==
                   json_array_append_new (pta,
                                          json_string (wm->wire_method)));
  }
  GNUNET_assert (0 ==
                 json_array_append_new (
                   ja,
                   json_pack (
                     "{s:s, s:s, s:o, s:o}",
                     "name",
                     mi->name,
                     "instance",
                     mi->id,
                     "merchant_pub",
                     GNUNET_JSON_from_data_auto (&mi->pubkey),
                     "payment_targets",
                     pta)));
  return GNUNET_OK;
}


/**
 * Handle a GET "/instances" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_get_instances (const struct TMH_RequestHandler *rh,
                           struct MHD_Connection *connection,
                           struct TMH_HandlerContext *hc)
{
  struct MHD_Response *response;
  json_t *ia;

  (void) hc;
  ia = json_array ();
  GNUNET_assert (NULL != ia);
  GNUNET_CONTAINER_multihashmap_iterate (TMH_by_id_map,
                                         &add_instance,
                                         ia);
  response = TALER_MHD_make_json_pack ("{s:o}",
                                       "instances", ia);
  GNUNET_assert (NULL != response);
  {
    MHD_RESULT ret;

    ret = MHD_queue_response (connection,
                              MHD_HTTP_OK,
                              response);
    MHD_destroy_response (response);
    return ret;
  }
}


/* end of taler-merchant-httpd_private-get-instances.c */
