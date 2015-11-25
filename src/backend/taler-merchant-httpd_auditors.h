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
#include <curl/curl.h>
#include <taler/taler_util.h>
#include <taler/taler_mint_service.h>
#include "taler-merchant-httpd.h"


/**
 * Array of auditors accepted by this mint.
 */
extern json_t *j_auditors;


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
TMH_AUDITORS_init (const struct GNUNET_CONFIGURATION_Handle *cfg);


#endif
