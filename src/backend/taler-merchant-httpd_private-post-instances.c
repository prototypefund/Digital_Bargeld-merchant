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
 * @file backend/taler-merchant-httpd_private-post-instances.c
 * @brief implementing POST /instances request handling
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-post-instances.h"
#include <taler/taler_json_lib.h>


/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


/**
 * Check if the array of @a payto_uris contains exactly the same
 * URIs as those already in @a mi (possibly in a different order).
 *
 * @param mi a merchant instance with accounts
 * @param payto_uris a JSON array with accounts (presumably)
 * @return true if they are 'equal', false if not or of payto_uris is not an array
 */
static bool
accounts_equal (const struct TMH_MerchantInstance *mi,
                json_t *payto_uris)
{
  if (! json_is_array (payto_uris))
    return false;
  {
    unsigned int len = json_array_size (payto_uris);
    bool matches[GNUNET_NZL (len)];
    struct TMH_WireMethod *wm;

    memset (matches,
            0,
            sizeof (matches));
    for (wm = mi->wm_head;
         NULL != wm;
         wm = wm->next)
    {
      const char *uri = json_string_value (json_object_get (wm->j_wire,
                                                            "payto_uri"));

      GNUNET_assert (NULL != uri);
      for (unsigned int i = 0; i<len; i++)
      {
        const char *str = json_string_value (json_array_get (payto_uris,
                                                             i));
        if (NULL == str)
          return false;
        if ( (strcasecmp (uri,
                          str)) )
        {
          if (matches[i])
          {
            GNUNET_break (0);
            return false; /* duplicate entry!? */
          }
          matches[i] = true;
          break;
        }
      }
    }
    for (unsigned int i = 0; i<len; i++)
      if (! matches[i])
        return false;
  }
  return true;
}


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
 * Free memory used by @a mi.
 *
 * @param mi instance to free
 */
static void
free_mi (struct TMH_MerchantInstance *mi)
{
  struct TMH_WireMethod *wm;

  while (NULL != (wm = mi->wm_head))
  {
    GNUNET_CONTAINER_DLL_remove (mi->wm_head,
                                 mi->wm_tail,
                                 wm);
    free_wm (wm);
  }
  GNUNET_free (mi->settings.id);
  GNUNET_free (mi->settings.name);
  GNUNET_free (mi);
}


/**
 * Generate an instance, given its configuration.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_post_instances (const struct TMH_RequestHandler *rh,
                            struct MHD_Connection *connection,
                            struct TMH_HandlerContext *hc)
{
  struct TALER_MERCHANTDB_InstanceSettings is;
  json_t *payto_uris;
  const char *id;
  const char *name;
  struct TMH_WireMethod *wm_head = NULL;
  struct TMH_WireMethod *wm_tail = NULL;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_json ("payto_uris",
                           &payto_uris),
    GNUNET_JSON_spec_string ("instance",
                             &id),
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

  {
    /* Test if an instance of this id is known */
    struct TMH_MerchantInstance *mi;

    mi = TMH_lookup_instance (is.id);
    if (NULL != mi)
    {
      /* Check for idempotency */
      if ( (0 == strcmp (mi->settings.id,
                         id)) &&
           (0 == strcmp (mi->settings.name,
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
            is.default_pay_delay.rel_value_us) &&
           (accounts_equal (mi,
                            payto_uris)) )
      {
        GNUNET_JSON_parse_free (spec);
        return TALER_MHD_reply_static (connection,
                                       MHD_HTTP_NO_CONTENT,
                                       NULL,
                                       NULL,
                                       0);
      }
      else
      {
        GNUNET_JSON_parse_free (spec);
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_CONFLICT,
                                           TALER_EC_POST_INSTANCES_ALREADY_EXISTS,
                                           "An instance using this identifier already exists");
      }
    }
  }

  {
    bool payto_ok = true;
    unsigned int len;

    if (! json_is_array (payto_uris))
    {
      payto_ok = false;
      len = 0;
    }
    else
    {
      len = json_array_size (payto_uris);
    }
    for (unsigned int i = 0; i<len; i++)
    {
      json_t *payto_uri = json_array_get (payto_uris,
                                          i);

      if (! json_is_string (payto_uri))
      {
        payto_ok = false;
        break;
      }
      /* Test for the same payto:// URI being given twice */
      for (unsigned int j = 0; j<i; j++)
      {
        json_t *old_uri = json_array_get (payto_uris,
                                          j);
        if (json_equal (payto_uri,
                        old_uri))
        {
          payto_ok = false;
          break;
        }
      }
      if (! payto_ok)
        break;

      {
        struct TMH_WireMethod *wm;
        struct GNUNET_HashCode salt;

        GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_NONCE,
                                    &salt,
                                    sizeof (salt));
        wm = GNUNET_new (struct TMH_WireMethod);
        wm->j_wire = json_pack ("{s:O, s:s}",
                                "payto_uri", payto_uri,
                                "salt", GNUNET_JSON_from_data_auto (&salt));
        GNUNET_assert (NULL != wm->j_wire);
        /* This also tests for things like the IBAN being malformed */
        if (GNUNET_OK !=
            TALER_JSON_merchant_wire_signature_hash (wm->j_wire,
                                                     &wm->h_wire))
        {
          payto_ok = false;
          GNUNET_free (wm);
          break;
        }
        wm->wire_method
          = TALER_payto_get_method (json_string_value (payto_uri));
        GNUNET_assert (NULL != wm->wire_method);
        wm->active = true;
        GNUNET_CONTAINER_DLL_insert (wm_head,
                                     wm_tail,
                                     wm);
      }
    }
    if (! payto_ok)
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
                                         MHD_HTTP_BAD_REQUEST,
                                         TALER_EC_POST_INSTANCES_BAD_PAYTO_URIS,
                                         "Invalid bank account information");
    }
  }

  {
    struct TMH_MerchantInstance *mi;
    enum GNUNET_DB_QueryStatus qs;

    mi = GNUNET_new (struct TMH_MerchantInstance);
    mi->wm_head = wm_head;
    mi->wm_tail = wm_tail;
    mi->settings = is;
    mi->settings.id = GNUNET_strdup (id);
    mi->settings.name = GNUNET_strdup (name);
    GNUNET_CRYPTO_eddsa_key_create (&mi->merchant_priv.eddsa_priv);
    GNUNET_CRYPTO_eddsa_key_get_public (&mi->merchant_priv.eddsa_priv,
                                        &mi->merchant_pub.eddsa_pub);

    for (unsigned int i = 0; i<MAX_RETRIES; i++)
    {
      if (GNUNET_OK !=
          TMH_db->start (TMH_db->cls,
                         "post /instances"))
      {
        GNUNET_JSON_parse_free (spec);
        free_mi (mi);
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           TALER_EC_POST_INSTANCES_DB_START_ERROR,
                                           "failed to start database transaction");
      }
      qs = TMH_db->insert_instance (TMH_db->cls,
                                    &mi->merchant_pub,
                                    &mi->merchant_priv,
                                    &mi->settings);
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
      {
        TMH_db->rollback (TMH_db->cls);
        // TODO: only on soft error do:
        continue;
      }
      for (struct TMH_WireMethod *wm = wm_head;
           NULL != wm;
           wm = wm->next)
      {
        struct TALER_MERCHANTDB_AccountDetails ad;
        struct GNUNET_JSON_Specification spec[] = {
          GNUNET_JSON_spec_string ("payto_uri",
                                   &ad.payto_uri),
          GNUNET_JSON_spec_fixed_auto ("salt",
                                       &ad.salt)
        };

        GNUNET_assert (GNUNET_OK ==
                       TALER_MHD_parse_json_data (NULL,
                                                  wm->j_wire,
                                                  spec));
        ad.h_wire = wm->h_wire;
        ad.active = wm->active;
        qs = TMH_db->insert_account (TMH_db->cls,
                                     mi->settings.id,
                                     &ad);
        if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
          break;
      }
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
      {
        TMH_db->rollback (TMH_db->cls);
        // TODO: only on soft error do:
        continue;
      }
      qs = TMH_db->commit (TMH_db->cls);
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
        break; /* success! */
    }
    if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
    {
      GNUNET_JSON_parse_free (spec);
      free_mi (mi);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_POST_INSTANCES_DB_COMMIT_ERROR,
                                         "failed to add instance to database");
    }
    /* Finally, also update our running process */
    GNUNET_assert (GNUNET_OK ==
                   TMH_add_instance (mi));
  }
  GNUNET_JSON_parse_free (spec);
  return TALER_MHD_reply_static (connection,
                                 MHD_HTTP_NO_CONTENT,
                                 NULL,
                                 NULL,
                                 0);
}


/* end of taler-merchant-httpd_private-post-instances.c */
