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
 * @file backend/taler-merchant-httpd_mints.h
 * @brief logic this HTTPD keeps for each mint we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_MINTS_H
#define TALER_MERCHANT_HTTPD_MINTS_H

#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <curl/curl.h>
#include <taler/taler_util.h>
#include <taler/taler_mint_service.h>
#include "taler-merchant-httpd.h"


/**
 * List of our trusted mints in JSON format for inclusion in contracts.
 */
json_t *trusted_mints;


/**
 * Parses "trusted" mints listed in the configuration.
 *
 * @param cfg the configuration
 * @return #GNUNET_OK on success; #GNUNET_SYSERR upon error in
 *          parsing or initialization.
 */
int
TMH_MINTS_init (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Function called to shutdown the mints subsystem.
 */
void
TMH_MINTS_done (void);


/**
 * Function called with the result of a #TMH_MINTS_find_mint()
 * operation.
 *
 * @param cls closure
 * @param mh handle to the mint context
 * @param mint_trusted #GNUNET_YES if this mint is trusted by config
 */
typedef void
(*TMH_MINTS_FindContinuation)(void *cls,
                              struct TALER_MINT_Handle *mh,
                              int mint_trusted);


/**
 * Information we keep for a pending #MMH_MINTS_find_mint() operation.
 */
struct TMH_MINTS_FindOperation;


/**
 * Find a mint that matches @a chosen_mint. If we cannot connect
 * to the mint, or if it is not acceptable, @a fc is called with
 * NULL for the mint.
 *
 * @param chosen_mint URI of the mint we would like to talk to
 * @param fc function to call with the handles for the mint
 * @param fc_cls closure for @a fc
 *
 * FIXME: should probably return a value to *cancel* the
 * operation in case MHD connection goes down and needs to
 * free fc_cls.
 */
struct TMH_MINTS_FindOperation *
TMH_MINTS_find_mint (const char *chosen_mint,
                     TMH_MINTS_FindContinuation fc,
                     void *fc_cls);


/**
 * Abort pending find operation.
 *
 * @param fo handle to operation to abort
 */
void
TMH_MINTS_find_mint_cancel (struct TMH_MINTS_FindOperation *fo);


#endif
