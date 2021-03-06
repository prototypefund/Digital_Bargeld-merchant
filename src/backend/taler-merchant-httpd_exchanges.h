/*
  This file is part of TALER
  (C) 2014-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_exchanges.h
 * @brief logic this HTTPD keeps for each exchange we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_EXCHANGES_H
#define TALER_MERCHANT_HTTPD_EXCHANGES_H

#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <curl/curl.h>
#include <taler/taler_util.h>
#include <taler/taler_exchange_service.h>
#include "taler-merchant-httpd.h"


/**
 * List of our trusted exchanges in JSON format for inclusion in contracts.
 */
extern json_t *TMH_trusted_exchanges;


/**
 * Parses "trusted" exchanges listed in the configuration.
 *
 * @param cfg the configuration
 * @return #GNUNET_OK on success; #GNUNET_SYSERR upon error in
 *          parsing or initialization.
 */
int
TMH_EXCHANGES_init (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Function called to shutdown the exchanges subsystem.
 */
void
TMH_EXCHANGES_done (void);


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param eh handle to the exchange context
 * @param wire_fee current applicable wire fee for dealing with @a eh, NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
typedef void
(*TMH_EXCHANGES_FindContinuation)(void *cls,
                                  const struct TALER_EXCHANGE_HttpResponse *hr,
                                  struct TALER_EXCHANGE_Handle *eh,
                                  const struct TALER_Amount *wire_fee,
                                  int exchange_trusted);


/**
 * Information we keep for a pending #MMH_EXCHANGES_find_exchange() operation.
 */
struct TMH_EXCHANGES_FindOperation;


/**
 * Find a exchange that matches @a chosen_exchange. If we cannot connect
 * to the exchange, or if it is not acceptable, @a fc is called with
 * NULL for the exchange.
 *
 * @param chosen_exchange URL of the exchange we would like to talk to
 * @param wire_method the wire method we will use with @a chosen_exchange, NULL for none
 * @param force_reload try to force reloading /keys from the exchange ASAP; note
 *        that IF the forced reload fails, it is possible @a fc won't be called at all
 *        until a /keys download succeeds; only use #GNUNET_YES if a new /keys request
 *        is mandatory. If the force reload request is not allowed due to our rate limiting,
 *        then @a fc will be called immediately with the existing /keys data
 * @param fc function to call with the handles for the exchange
 * @param fc_cls closure for @a fc
 */
struct TMH_EXCHANGES_FindOperation *
TMH_EXCHANGES_find_exchange (const char *chosen_exchange,
                             const char *wire_method,
                             int force_reload,
                             TMH_EXCHANGES_FindContinuation fc,
                             void *fc_cls);


/**
 * Abort pending find operation.
 *
 * @param fo handle to operation to abort
 */
void
TMH_EXCHANGES_find_exchange_cancel (struct TMH_EXCHANGES_FindOperation *fo);


#endif
