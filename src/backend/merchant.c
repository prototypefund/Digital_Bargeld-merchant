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
                            struct MERCHANT_MintInfo **mints)
{
  char *mints_str;
  char *token_nf;               /* do no free (nf) */
  char *mint_section;
  char *mint_hostname;
  char *mint_pubkey_enc;
  struct MERCHANT_MintInfo *r_mints;
  struct MERCHANT_MintInfo mint;
  unsigned long long mint_port;
  unsigned int cnt;
  int OK;

  OK = 0;
  mints_str = NULL;
  token_nf = NULL;
  mint_section = NULL;
  mint_hostname = NULL;
  mint_pubkey_enc = NULL;
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
    EXITIF (GNUNET_OK !=
            GNUNET_CONFIGURATION_get_value_number (cfg,
                                                   mint_section,
                                                   "PORT",
                                                   &mint_port));
    EXITIF (GNUNET_OK !=
            GNUNET_CONFIGURATION_get_value_string (cfg,
                                                   mint_section,
                                                   "PUBKEY",
                                                   &mint_pubkey_enc));
    EXITIF (GNUNET_OK !=
            GNUNET_CRYPTO_eddsa_public_key_from_string (mint_pubkey_enc,
                                                        strlen (mint_pubkey_enc),
                                                        &mint.pubkey));
    mint.hostname = mint_hostname;
    mint.port = (uint16_t) mint_port;
    GNUNET_array_append (r_mints, cnt, mint);
    mint_hostname = NULL;
    GNUNET_free (mint_pubkey_enc);
    mint_pubkey_enc = NULL;
    GNUNET_free (mint_section);
    mint_section = NULL;
  }
  OK = 1;

 EXITIF_exit:
  GNUNET_free_non_null (mints_str);
  GNUNET_free_non_null (mint_section);
  GNUNET_free_non_null (mint_hostname);
  GNUNET_free_non_null (mint_pubkey_enc);
  if (!OK)
  {
    GNUNET_free_non_null (r_mints);
    return GNUNET_SYSERR;
  }

  *mints = r_mints;
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
