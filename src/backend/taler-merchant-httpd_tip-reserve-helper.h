/*
  This file is part of TALER
  (C) 2018--2019 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_tip-reserve-helper.h
 * @brief helper functions to check the status of a tipping reserve
 * @author Christian Grothoff
 */
#ifndef TALER_MERCHANT_HTTPD_TIP_RESERVE_HELPER_H
#define TALER_MERCHANT_HTTPD_TIP_RESERVE_HELPER_H
#include <jansson.h>
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_tip-reserve-helper.h"


/**
 * Context with input, output and internal state for
 * #TMH_check_tip_reserve() and #TMH_check_tip_reserve_cleanup().
 */
struct CheckTipReserve
{
  /**
   * Input: MHD connection we should resume when finished
   */
  struct MHD_Connection *connection;

  /**
   * Input: private key of the reserve.
   */
  struct TALER_ReservePrivateKeyP reserve_priv;

  /**
   * Output: Set to delay after which the reserve will expire if idle.
   */
  struct GNUNET_TIME_Relative idle_reserve_expiration_time;

  /**
   * Internal: exchange find operation.
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Internal: reserve status operation.
   */
  struct TALER_EXCHANGE_ReservesGetHandle *rsh;

  /**
   * Internal: DLL for resumption on shutdown.
   */
  struct CheckTipReserve *next;

  /**
   * Internal: DLL for resumption on shutdown.
   */
  struct CheckTipReserve *prev;

  /**
   * Output: response object to return (on error only)
   */
  struct MHD_Response *response;

  /**
   * Output: Total amount deposited into the reserve.
   */
  struct TALER_Amount amount_deposited;

  /**
   * Output: total tip amount requested.
   */
  struct TALER_Amount amount_withdrawn;

  /**
   * Input: total amount authorized.
   */
  struct TALER_Amount amount_authorized;

  /**
   * Output: set to the time when the reserve will expire
   */
  struct GNUNET_TIME_Absolute reserve_expiration;

  /**
   * Output: HTTP status code to return (on error only)
   */
  unsigned int response_code;

  /**
   * Input: Set to #GNUNET_NO if no tips were authorized yet.
   * Used to know that @e amount_authorized is not yet initialized
   * and in that case the helper will set it to zero (once we know
   * the currency).
   */
  int none_authorized;

  /**
   * Internal: Is the @e connection currently suspended?
   * #GNUNET_NO if the @e connection was not suspended,
   * #GNUNET_YES if the @e connection was suspended,
   * #GNUNET_SYSERR if @e connection was resumed to as
   * part of #MH_force_pc_resume during shutdown.
   */
  int suspended;

};


/**
 * Check the status of the given reserve at the given exchange.
 * Suspends the MHD connection while this is happening and resumes
 * processing once we know the reserve status (or once an error
 * code has been determined).
 *
 * @param[in,out] ctr context for checking the reserve status
 * @param tip_exchange the URL of the exchange to query
 */
void
TMH_check_tip_reserve (struct CheckTipReserve *ctr,
                       const char *tip_exchange);


/**
 * Clean up any state that might be left in @a ctr.
 *
 * @param[in] context to clean up
 */
void
TMH_check_tip_reserve_cleanup (struct CheckTipReserve *ctr);

/**
 * Force all tip reserve helper contexts to be resumed as we are about to shut
 * down MHD.
 */
void
MH_force_trh_resume (void);


#endif
