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
 * @file backend/taler-merchant-httpd_auditors.h
 * @brief logic this HTTPD keeps for each mint we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_AUDITORS_H
#define TALER_MERCHANT_HTTPD_AUDITORS_H

#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_mint_service.h>
#include "taler-merchant-httpd.h"


/**
 * JSON representation of the auditors accepted by this mint.
 */
extern json_t *j_auditors;


/**
 * Parses auditor information from the configuration.
 *
 * @param cfg the configuration
 * @return the number of auditors found; #GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TMH_AUDITORS_init (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Check if the given @a dk issued by mint @a mh is audited by
 * an auditor that is acceptable for this merchant. (And if the
 * denomination is not yet expired or something silly like that.)
 *
 * @param mh mint issuing @a dk
 * @param dk a denomination issued by @a mh
 * @return #GNUNET_OK if we accept this denomination
 */
int
TMH_AUDITORS_check_dk (struct TALER_MINT_Handle *mh,
                       const struct TALER_MINT_DenomPublicKey *dk);


/**
 * Release auditor information state.
 */
void
TMH_AUDITORS_done (void);




#endif
