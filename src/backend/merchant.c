/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
 * @file merchant/merchant.c
 * @brief Common utility functions for merchant
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "merchant.h"


#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)


/**
 * Parses mints from the configuration.
 *
 * @param cfg the configuration
 * @param mints the array of mints upon successful parsing.  Will be NULL upon
 *          error.
 * @return the number of mints in the above array; GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TALER_MERCHANT_parse_mints (const struct GNUNET_CONFIGURATION_Handle *cfg,
                            struct MERCHANT_Mint **mints)
{
  char *mints_str;
  char *token_nf;               /* do no free (nf) */
  char *mint_section;
  char *mint_hostname;
  struct MERCHANT_Mint *r_mints;
  struct MERCHANT_Mint mint;
  unsigned int cnt;
  int OK;

  OK = 0;
  mints_str = NULL;
  token_nf = NULL;
  mint_section = NULL;
  mint_hostname = NULL;
  r_mints = NULL;
  cnt = 0;
  EXITIF (GNUNET_OK != GNUNET_CONFIGURATION_get_value_string (cfg,
                                                              "merchant",
                                                              "TRUSTED_MINTS",
                                                              &mints_str));
  for (token_nf = strtok (mints_str, " ");
       NULL != token_nf;
       token_nf = strtok (NULL, " "))
  {
    GNUNET_assert (0 < GNUNET_asprintf (&mint_section,
                                        "mint-%s", token_nf));
    EXITIF (GNUNET_OK !=
            GNUNET_CONFIGURATION_get_value_string (cfg,
                                                   mint_section,
                                                   "HOSTNAME",
                                                   &mint_hostname));
    mint.hostname = mint_hostname;
    GNUNET_array_append (r_mints, cnt, mint);
    mint_hostname = NULL;
    GNUNET_free (mint_section);
    mint_section = NULL;
  }
  OK = 1;

 EXITIF_exit:
  GNUNET_free_non_null (mints_str);
  GNUNET_free_non_null (mint_section);
  GNUNET_free_non_null (mint_hostname);
  if (!OK)
  {
    GNUNET_free_non_null (r_mints);
    return GNUNET_SYSERR;
  }

  *mints = r_mints;
  return cnt;
}

/**
 * Parses auditors from the configuration.
 *
 * @param cfg the configuration
 * @param mints the array of auditors upon successful parsing.  Will be NULL upon
 *          error.
 * @return the number of auditors in the above array; GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TALER_MERCHANT_parse_auditors (const struct GNUNET_CONFIGURATION_Handle *cfg,
                               struct MERCHANT_Auditor **auditors)
{
  char *auditors_str;
  char *token_nf;               /* do no free (nf) */
  char *auditor_section;
  char *auditor_name;
  struct MERCHANT_Auditor *r_auditors;
  struct MERCHANT_Auditor auditor;
  unsigned int cnt;
  int OK;

  OK = 0;
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
  OK = 1;

 EXITIF_exit:
  GNUNET_free_non_null (auditors_str);
  GNUNET_free_non_null (auditor_section);
  GNUNET_free_non_null (auditor_name);
  if (!OK)
  {
    GNUNET_free_non_null (r_auditors);
    return GNUNET_SYSERR;
  }

  *auditors = r_auditors;
  return cnt;
}


/**
 * Parse the SEPA information from the configuration.  If any of the required
 * fileds is missing return NULL.
 *
 * @param cfg the configuration
 * @return Sepa details as a structure; NULL upon error
 */
struct MERCHANT_WIREFORMAT_Sepa *
TALER_MERCHANT_parse_wireformat_sepa (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct MERCHANT_WIREFORMAT_Sepa *wf;

  wf = GNUNET_new (struct MERCHANT_WIREFORMAT_Sepa);
  EXITIF (GNUNET_OK != GNUNET_CONFIGURATION_get_value_string (cfg,
                                                              "wire-sepa",
                                                              "IBAN",
                                                              &wf->iban));
  EXITIF (GNUNET_OK != GNUNET_CONFIGURATION_get_value_string (cfg,
                                                              "wire-sepa",
                                                              "NAME",
                                                              &wf->name));
  EXITIF (GNUNET_OK != GNUNET_CONFIGURATION_get_value_string (cfg,
                                                              "wire-sepa",
                                                              "BIC",
                                                              &wf->bic));
  return wf;

 EXITIF_exit:
  GNUNET_free_non_null (wf->iban);
  GNUNET_free_non_null (wf->name);
  GNUNET_free_non_null (wf->bic);
  GNUNET_free (wf);
  return NULL;

}


/**
 * Destroy and free resouces occupied by the wireformat structure
 *
 * @param wf the wireformat structure
 */
void
TALER_MERCHANT_destroy_wireformat_sepa (struct MERCHANT_WIREFORMAT_Sepa *wf)
{
  GNUNET_free_non_null (wf->iban);
  GNUNET_free_non_null (wf->name);
  GNUNET_free_non_null (wf->bic);
  GNUNET_free (wf);
}

/* end of merchant.c */
