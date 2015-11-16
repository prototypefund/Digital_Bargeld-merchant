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
 * @file merchant/backend/taler-merchant-httpd.h
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */

#include "merchant_db.h"

/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 */
void
TM_trigger_daemon (void);


struct TM_HandlerContext;

/**
 * Signature of a function used to clean up the context
 * we keep in the "connection_cls" of MHD when handling
 * a request.
 *
 * @param hc header of the context to clean up.
 */
typedef void
(*TM_ContextCleanup)(struct TM_HandlerContext *hc);


struct TM_HandlerContext
{

  TM_ContextCleanup cc;

};

extern struct MERCHANT_Mint *mints;

extern struct MERCHANT_WIREFORMAT_Sepa *wire;

extern PGconn *db_conn;

extern long long salt;

extern unsigned int nmints;

extern struct GNUNET_TIME_Relative edate_delay;

extern struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;

extern struct GNUNET_SCHEDULER_Task *poller_task;


void
context_task (void *cls,
              const struct GNUNET_SCHEDULER_TaskContext *tc);
