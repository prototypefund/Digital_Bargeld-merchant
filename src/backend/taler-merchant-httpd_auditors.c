/*
  This file is part of TALER
  (C) 2014, 2015 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_auditors.c
 * @brief logic this HTTPD keeps for each mint we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
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
   * Auditor's URI.
   */
  char *uri;

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
 * JSON representation of the auditors accepted by this mint.
 */
json_t *j_auditors;


/**
 * Function called on each configuration section. Finds sections
 * about auditors and parses the entries.
 *
 * @param cls closure
 * @param section name of the section
 */
static void
parse_auditors (void *cls,
                const char *section)
{
  char *pks;
  struct Auditor auditor;

  if (0 != strncasecmp (section,
                        "auditor-",
                        strlen ("auditor-")))
    return;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "NAME",
                                             &auditor.name))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "NAME");
    return;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "URI",
                                             &auditor.uri))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "URI");
    GNUNET_free (auditor.name);
    return;
  }
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "PUBLIC_KEY",
                                             &pks))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "PUBLIC_KEY");
    GNUNET_free (auditor.name);
    GNUNET_free (auditor.uri);
    return;
  }
  if (GNUNET_OK !=
      GNUNET_CRYPTO_eddsa_public_key_from_string (pks,
                                                  strlen (pks),
                                                  &auditor.public_key.eddsa_pub))
  {
    GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "PUBLIC_KEY",
                               "valid public key");
    GNUNET_free (auditor.name);
    GNUNET_free (auditor.uri);
    GNUNET_free (pks);
    return;
  }
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
  unsigned int cnt;
  int ok;

  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &parse_auditors,
                                         NULL);

  /* Generate preferred mint(s) array. */
  j_auditors = json_array ();
  for (cnt = 0; cnt < nauditors; cnt++)
    json_array_append_new (j_auditors,
                           json_pack ("{s:s, s:o, s:s}",
                                      "name", auditors[cnt].name,
                                      "auditor_pub", TALER_json_from_data (&auditors[cnt].public_key,
                                                                           sizeof (struct TALER_AuditorPublicKeyP)),
                                      "uri", auditors[cnt].uri));
  return nauditors;
}


/**
 * Release auditor information state.
 */
void
TMH_AUDITORS_done ()
{
  unsigned int i;

  json_decref (j_auditors);
  j_auditors = NULL;
  for (i=0;i<nauditors;i++)
  {
    GNUNET_free (auditors[i].name);
    GNUNET_free (auditors[i].uri);
  }
  GNUNET_free (auditors);
  auditors = NULL;
  nauditors = 0;
}

/* end of taler-merchant-httpd_auditors.c */
