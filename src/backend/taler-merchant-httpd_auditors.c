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
 * Active auditors
 */
static struct MERCHANT_Auditor *auditors;

/**
 * The number of active auditors
 */
static unsigned int nauditors;

/**
 * Array of auditors accepted by this mint.
 */
json_t *j_auditors;


/**
 * Parses auditors from the configuration.
 *
 * @param cfg the configuration
 * @param mints the array of auditors upon successful parsing.  Will be NULL upon
 *          error.
 * @return the number of auditors in the above array; #GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TMH_AUDITORS_init (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  char *auditors_str;
  char *token_nf;               /* do no free (nf) */
  char *auditor_section;
  char *auditor_name;
  struct MERCHANT_Auditor *r_auditors;
  struct MERCHANT_Auditor auditor;
  unsigned int cnt;
  int ok;

  ok = 0;
  auditors_str = NULL;
  token_nf = NULL;
  auditor_section = NULL;
  auditor_name = NULL;
  r_auditors = NULL;
  cnt = 0;
  EXITIF (GNUNET_OK !=
          GNUNET_CONFIGURATION_get_value_string (cfg,
                                                 "merchant",
                                                 "AUDITORS",
                                                 &auditors_str));
  for (token_nf = strtok (auditors_str, " ");
       NULL != token_nf;
       token_nf = strtok (NULL, " "))
  {
    GNUNET_assert (0 < GNUNET_asprintf (&auditor_section,
                                        "auditor-%s", token_nf));
    EXITIF (GNUNET_OK !=
            GNUNET_CONFIGURATION_get_value_string (cfg,
                                                   auditor_section,
                                                   "NAME",
                                                   &auditor_name));
    auditor.name = auditor_name;
    GNUNET_array_append (r_auditors, cnt, auditor);
    auditor_name = NULL;
    GNUNET_free (auditor_section);
    auditor_section = NULL;
  }
  ok = 1;

 EXITIF_exit:
  GNUNET_free_non_null (auditors_str);
  GNUNET_free_non_null (auditor_section);
  GNUNET_free_non_null (auditor_name);
  if (! ok)
  {
    GNUNET_free_non_null (r_auditors);
    return GNUNET_SYSERR;
  }

  auditors = r_auditors;
  nauditors = cnt;

  /* Generate preferred mint(s) array. */
  j_auditors = json_array ();
  for (cnt = 0; cnt < nauditors; cnt++)
  {
    json_array_append_new (j_auditors,
                           json_pack ("{s:s}",
                                      "name", auditors[cnt].name));
  }

  return nauditors;
}
