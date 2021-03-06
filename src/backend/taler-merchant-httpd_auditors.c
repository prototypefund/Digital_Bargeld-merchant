/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2018 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_auditors.c
 * @brief logic this HTTPD keeps for each exchange we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd_auditors.h"

/**
 * Our representation of an auditor.
 */
struct Auditor
{
  /**
   * Auditor's legal name.
   */
  char *name;

  /**
   * Auditor's URL.
   */
  char *url;

  /**
   * Public key of the auditor.
   */
  struct TALER_AuditorPublicKeyP public_key;

};


/**
 * Array of the auditors this merchant is willing to accept.
 */
static struct Auditor *auditors;

/**
 * The length of the #auditors array.
 */
static unsigned int nauditors;

/**
 * JSON representation of the auditors accepted by this exchange.
 */
json_t *j_auditors;


/**
 * Check if the given @a dk issued by exchange @a mh is audited by
 * an auditor that is acceptable for this merchant. (And if the
 * denomination is not yet expired or something silly like that.)
 *
 * @param mh exchange issuing @a dk
 * @param dk a denomination issued by @a mh
 * @param exchange_trusted #GNUNET_YES if the exchange of @a dk is trusted by config
 * @param[out] hc HTTP status code to return (on error)
 * @param[out] ec Taler error code to return (on error)
 * @return #GNUNET_OK
 */
int
TMH_AUDITORS_check_dk (struct TALER_EXCHANGE_Handle *mh,
                       const struct TALER_EXCHANGE_DenomPublicKey *dk,
                       int exchange_trusted,
                       unsigned int *hc,
                       enum TALER_ErrorCode *ec)
{
  const struct TALER_EXCHANGE_Keys *keys;
  const struct TALER_EXCHANGE_AuditorInformation *ai;

  if (0 == GNUNET_TIME_absolute_get_remaining (dk->expire_deposit).rel_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Denomination key offered by client has expired for deposits\n");
    *hc = MHD_HTTP_GONE;
    *ec = TALER_EC_PAY_DENOMINATION_DEPOSIT_EXPIRED;
    return GNUNET_SYSERR; /* expired */
  }
  if (GNUNET_YES == exchange_trusted)
  {
    *ec = TALER_EC_NONE;
    *hc = MHD_HTTP_OK;
    return GNUNET_OK;
  }
  keys = TALER_EXCHANGE_get_keys (mh);
  if (NULL == keys)
  {
    /* this should never happen, keys should have been successfully
       obtained before we even got into this function */
    *ec = TALER_EC_PAY_EXCHANGE_HAS_NO_KEYS;
    *hc = MHD_HTTP_FAILED_DEPENDENCY;
    return GNUNET_SYSERR;
  }
  for (unsigned int i = 0; i<keys->num_auditors; i++)
  {
    ai = &keys->auditors[i];
    for (unsigned int j = 0; j<nauditors; j++)
    {
      if (0 == GNUNET_memcmp (&ai->auditor_pub,
                              &auditors[j].public_key))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                    "Found supported auditor `%s' (%s)\n",
                    auditors[j].name,
                    TALER_B2S (&auditors[j].public_key));
      }
      for (unsigned int k = 0; k<ai->num_denom_keys; k++)
        if (&keys->denom_keys[k] == dk)
        {
          *ec = TALER_EC_NONE;
          *hc = MHD_HTTP_OK;
          return GNUNET_OK;
        }
    }
  }
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "Denomination key %s offered by client not audited by any accepted auditor\n",
              GNUNET_h2s (&dk->h_key));
  *hc = MHD_HTTP_BAD_REQUEST;
  *ec = TALER_EC_PAY_DENOMINATION_KEY_AUDITOR_FAILURE;
  return GNUNET_NO;
}


/**
 * Function called on each configuration section. Finds sections
 * about auditors and parses the entries.
 *
 * @param cls closure, with a `const struct GNUNET_CONFIGURATION_Handle *`
 * @param section name of the section
 */
static void
parse_auditors (void *cls,
                const char *section)
{
  const struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  char *pks;
  struct Auditor auditor;
  char *currency;

  if (0 != strncasecmp (section,
                        "merchant-auditor-",
                        strlen ("merchant-auditor-")))
    return;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "CURRENCY",
                                             &currency))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "CURRENCY");
    return;
  }
  if (0 != strcasecmp (currency,
                       TMH_currency))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Auditor given in section `%s' is for another currency. Skipping.\n",
                section);
    GNUNET_free (currency);
    return;
  }
  GNUNET_free (currency);
  auditor.name = GNUNET_strdup (&section[strlen ("merchant-auditor-")]);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "AUDITOR_BASE_URL",
                                             &auditor.url))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "URL");
    GNUNET_free (auditor.name);
    return;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "AUDITOR_KEY",
                                             &pks))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "AUDITOR_KEY");
    GNUNET_free (auditor.name);
    GNUNET_free (auditor.url);
    return;
  }
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_public_key_from_string (pks,
                                                  strlen (pks),
                                                  &auditor.public_key.eddsa_pub))
  {
    GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "AUDITOR_KEY",
                               "need a valid EdDSA public key");
    GNUNET_free (auditor.name);
    GNUNET_free (auditor.url);
    GNUNET_free (pks);
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Loaded key data of auditor `%s' (%s)\n",
              auditor.name,
              TALER_B2S (&auditor.public_key));
  GNUNET_free (pks);
  GNUNET_array_append (auditors,
                       nauditors,
                       auditor);
}


/**
 * Parses auditor information from the configuration.
 *
 * @param cfg the configuration
 * @return the number of auditors found; #GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TMH_AUDITORS_init (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &parse_auditors,
                                         (void *) cfg);

  /* Generate preferred exchange(s) array. */
  j_auditors = json_array ();
  for (unsigned int cnt = 0; cnt < nauditors; cnt++)
    GNUNET_assert (0 ==
                   json_array_append_new (j_auditors,
                                          json_pack ("{s:s, s:o, s:s}",
                                                     "name", auditors[cnt].name,
                                                     "auditor_pub",
                                                     GNUNET_JSON_from_data_auto (
                                                       &auditors[cnt].public_key),
                                                     "url",
                                                     auditors[cnt].url)));
  return nauditors;
}


/**
 * Release auditor information state.
 */
void
TMH_AUDITORS_done ()
{
  json_decref (j_auditors);
  j_auditors = NULL;
  for (unsigned int i = 0; i<nauditors; i++)
  {
    GNUNET_free (auditors[i].name);
    GNUNET_free (auditors[i].url);
  }
  GNUNET_free_non_null (auditors);
  auditors = NULL;
  nauditors = 0;
}


/* end of taler-merchant-httpd_auditors.c */
