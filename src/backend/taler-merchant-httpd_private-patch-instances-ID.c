/*
  This file is part of TALER
  (C) 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation; either version 3,
  or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not,
  see <http://www.gnu.org/licenses/>
*/

/**
 * @file backend/taler-merchant-httpd_private-patch-instances.c
 * @brief implementing PATCH /instances/$ID request handling
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-patch-instances-ID.h"
#include <taler/taler_json_lib.h>


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


/**
 * Free memory used by @a wm
 *
 * @param wm wire method to free
 */
static void
free_wm (struct TMH_WireMethod *wm)
{
  json_decref (wm->j_wire);
  GNUNET_free (wm->wire_method);
  GNUNET_free (wm);
}


/**
 * PATCH configuration of an existing instance, given its configuration.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_patch_instances_ID (const struct TMH_RequestHandler *rh,
                                struct MHD_Connection *connection,
                                struct TMH_HandlerContext *hc)
{
  struct TMH_MerchantInstance *mi = hc->instance;
  struct TALER_MERCHANTDB_InstanceSettings is;
  json_t *payto_uris;
  const char *name;
  struct TMH_WireMethod *wm_head = NULL;
  struct TMH_WireMethod *wm_tail = NULL;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("payto_uris",
                           &payto_uris),
    GNUNET_JSON_spec_string ("name",
                             &name),
    GNUNET_JSON_spec_json ("address",
                           &is.address),
    GNUNET_JSON_spec_json ("jurisdiction",
                           &is.jurisdiction),
    TALER_JSON_spec_amount ("default_max_deposit_fee",
                            &is.default_max_deposit_fee),
    TALER_JSON_spec_amount ("default_max_wire_fee",
                            &is.default_max_wire_fee),
    GNUNET_JSON_spec_uint32 ("default_wire_fee_amortization",
                             &is.default_wire_fee_amortization),
    GNUNET_JSON_spec_relative_time ("default_wire_transfer_delay",
                                    &is.default_wire_transfer_delay),
    GNUNET_JSON_spec_relative_time ("default_pay_delay",
                                    &is.default_pay_delay),
    GNUNET_JSON_spec_end ()
  };
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_assert (NULL != mi);
  {
    enum GNUNET_GenericReturnValue res;

    res = TALER_MHD_parse_json_data (connection,
                                     hc->request_body,
                                     spec);
    /* json is malformed */
    if (GNUNET_NO == res)
    {
      GNUNET_break_op (0);
      return MHD_YES;
    }
    /* other internal errors might have occurred */
    if (GNUNET_SYSERR == res)
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                         "Impossible to parse the order");
  }
  if (! json_is_array (payto_uris))
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PATCH_INSTANCES_BAD_PAYTO_URIS,
                                       "Invalid bank account information");
  for (unsigned int i = 0; i<MAX_RETRIES; i++)
  {
    /* Cleanup after earlier loops */
    {
      struct TMH_WireMethod *wm;

      while (NULL != (wm = wm_head))
      {
        GNUNET_CONTAINER_DLL_remove (wm_head,
                                     wm_tail,
                                     wm);
        free_wm (wm);
      }
    }
    if (GNUNET_OK !=
        TMH_db->start (TMH_db->cls,
                       "PATCH /instances"))
    {
      GNUNET_JSON_parse_free (spec);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_PATCH_INSTANCES_DB_START_ERROR,
                                         "failed to start database transaction");
    }
    /* Check for equality of settings */
    if (! ( (0 == strcmp (mi->settings.name,
                          name)) &&
            (1 == json_equal (mi->settings.address,
                              is.address)) &&
            (1 == json_equal (mi->settings.jurisdiction,
                              is.jurisdiction)) &&
            (0 == TALER_amount_cmp_currency (
               &mi->settings.default_max_deposit_fee,
               &is.default_max_deposit_fee)) &&
            (0 == TALER_amount_cmp (&mi->settings.default_max_deposit_fee,
                                    &is.default_max_deposit_fee)) &&
            (0 == TALER_amount_cmp_currency (&mi->settings.default_max_wire_fee,
                                             &is.default_max_wire_fee)) &&
            (0 == TALER_amount_cmp (&mi->settings.default_max_wire_fee,
                                    &is.default_max_wire_fee)) &&
            (mi->settings.default_wire_fee_amortization ==
             is.default_wire_fee_amortization) &&
            (mi->settings.default_wire_transfer_delay.rel_value_us ==
             is.default_wire_transfer_delay.rel_value_us) &&
            (mi->settings.default_pay_delay.rel_value_us ==
             is.default_pay_delay.rel_value_us) ) )
    {
      qs = TMH_db->update_instance (TMH_db->cls,
                                    &mi->settings);
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
      {
        TMH_db->rollback (TMH_db->cls);
        if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
          goto retry;
        else
          goto giveup;
      }
    }

    /* Check for changes in accounts */
    {
      unsigned int len = json_array_size (payto_uris);
      bool matches[GNUNET_NZL (len)];
      bool matched;

      memset (matches,
              0,
              sizeof (matches));
      for (struct TMH_WireMethod *wm = mi->wm_head;
           NULL != wm;
           wm = wm->next)
      {
        const char *uri = json_string_value (json_object_get (wm->j_wire,
                                                              "payto_uri"));
        GNUNET_assert (NULL != uri);
        matched = false;
        for (unsigned int i = 0; i<len; i++)
        {
          const char *str = json_string_value (json_array_get (payto_uris,
                                                               i));
          if (NULL == str)
          {
            GNUNET_break_op (0);
            TMH_db->rollback (TMH_db->cls);
            GNUNET_JSON_parse_free (spec);
            GNUNET_assert (NULL == wm_head);
            return TALER_MHD_reply_with_error (connection,
                                               MHD_HTTP_BAD_REQUEST,
                                               TALER_EC_POST_INSTANCES_BAD_PAYTO_URIS,
                                               "Invalid bank account information");
          }
          if ( (strcasecmp (uri,
                            str)) )
          {
            if (matches[i])
            {
              GNUNET_break (0);
              TMH_db->rollback (TMH_db->cls);
              GNUNET_JSON_parse_free (spec);
              GNUNET_assert (NULL == wm_head);
              return TALER_MHD_reply_with_error (connection,
                                                 MHD_HTTP_BAD_REQUEST,
                                                 TALER_EC_POST_INSTANCES_BAD_PAYTO_URIS,
                                                 "Invalid bank account information");
            }
            matches[i] = true;
            matched = true;
            break;
          }
        }
        /* delete unmatched (= removed) accounts */
        if (! matched)
        {
          /* Account was REMOVED */
          GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                      "Existing account `%s' not found, inactivating it.\n",
                      uri);
          wm->deleting = true;
          qs = TMH_db->inactivate_account (TMH_db->cls,
                                           &wm->h_wire);
          if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
          {
            TMH_db->rollback (TMH_db->cls);
            if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
              goto retry;
            else
              goto giveup;
          }
        }
      }
      /* Find _new_ accounts */
      for (unsigned int i = 0; i<len; i++)
      {
        struct TALER_MERCHANTDB_AccountDetails ad;
        struct TMH_WireMethod *wm;

        if (matches[i])
          continue; /* account existed */
        ad.payto_uri = json_string_value (json_array_get (payto_uris,
                                                          i));
        GNUNET_assert (NULL != ad.payto_uri);
        GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                    "Adding NEW account `%s'\n",
                    ad.payto_uri);
        GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_NONCE,
                                    &ad.salt,
                                    sizeof (ad.salt));
        wm = GNUNET_new (struct TMH_WireMethod);
        wm->j_wire = json_pack ("{s:s, s:o}",
                                "payto_uri", ad.payto_uri,
                                "salt", GNUNET_JSON_from_data_auto (&ad.salt));
        GNUNET_assert (NULL != wm->j_wire);
        wm->wire_method
          = TALER_payto_get_method (ad.payto_uri);
        GNUNET_assert (NULL != wm->wire_method);
        /* This also tests for things like the IBAN being malformed */
        if (GNUNET_OK !=
            TALER_JSON_merchant_wire_signature_hash (wm->j_wire,
                                                     &wm->h_wire))
        {
          GNUNET_break_op (0);
          free_wm (wm);
          while (NULL != (wm = wm_head))
          {
            GNUNET_CONTAINER_DLL_remove (wm_head,
                                         wm_tail,
                                         wm);
            free_wm (wm);
          }
          TMH_db->rollback (TMH_db->cls);
          GNUNET_JSON_parse_free (spec);
          return TALER_MHD_reply_with_error (connection,
                                             MHD_HTTP_BAD_REQUEST,
                                             TALER_EC_POST_INSTANCES_BAD_PAYTO_URIS,
                                             "Invalid bank account information");
        }
        wm->active = true;
        GNUNET_CONTAINER_DLL_insert (wm_head,
                                     wm_tail,
                                     wm);
        ad.h_wire = wm->h_wire;
        ad.active = true;
        qs = TMH_db->insert_account (TMH_db->cls,
                                     mi->settings.id,
                                     &ad);
        if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
        {
          TMH_db->rollback (TMH_db->cls);
          if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
            goto retry;
          else
            goto giveup;
        }
      }
    }

    qs = TMH_db->commit (TMH_db->cls);
retry:
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      continue;
    break;
  } /* for(... MAX_RETRIES) */
giveup:
  if (0 > qs)
  {
    struct TMH_WireMethod *wm;

    while (NULL != (wm = wm_head))
    {
      GNUNET_CONTAINER_DLL_remove (wm_head,
                                   wm_tail,
                                   wm);
      free_wm (wm);
    }
    GNUNET_JSON_parse_free (spec);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_PATCH_INSTANCES_DB_COMMIT_ERROR,
                                       "failed to add instance to database");
  }
  /* Deactivate existing wire methods that were removed above */
  for (struct TMH_WireMethod *wm = mi->wm_head;
       NULL != wm;
       wm = wm->next)
  {
    /* We did not flip the 'active' bits earlier because the
       DB transaction could still fail. Now it is time to update our
       runtime state. */
    if (wm->deleting)
      wm->active = false;
  }

  /* Update our 'settings' */
  GNUNET_free (mi->settings.name);
  json_decref (mi->settings.address);
  json_decref (mi->settings.jurisdiction);
  is.id = mi->settings.id;
  mi->settings = is;
  mi->settings.address = json_incref (mi->settings.address);
  mi->settings.jurisdiction = json_incref (mi->settings.jurisdiction);
  mi->settings.name = GNUNET_strdup (name);

  /* Add 'new' wire methods to our list */
  {
    struct TMH_WireMethod *wm;

    while (NULL != (wm = wm_head))
    {
      GNUNET_CONTAINER_DLL_remove (wm_head,
                                   wm_tail,
                                   wm);
      GNUNET_CONTAINER_DLL_insert (mi->wm_head,
                                   mi->wm_tail,
                                   wm);
    }
  }

  GNUNET_JSON_parse_free (spec);
  return TALER_MHD_reply_static (connection,
                                 MHD_HTTP_NO_CONTENT,
                                 NULL,
                                 NULL,
                                 0);
}


/* end of taler-merchant-httpd_private-patch-instances-ID.c */
