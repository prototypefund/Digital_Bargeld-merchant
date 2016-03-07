/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V. and INRIA

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
 * @file include/taler_merchantdb_lib.h
 * @brief database helper functions used by the merchant backend
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#ifndef TALER_MERCHANTDB_LIB_H
#define TALER_MERCHANTDB_LIB_H

#include <taler/taler_util.h>
#include "taler_merchantdb_plugin.h"

/**
 * Handle to interact with the database.
 */
struct TALER_MERCHANTDB_Plugin;

/**
 * Connect to postgresql database
 *
 * @param cfg the configuration handle
 * @return connection to the database; NULL upon error
 */
struct TALER_MERCHANTDB_Plugin *
TALER_MERCHANTDB_plugin_load (const struct GNUNET_CONFIGURATION_Handle *cfg);


/**
 * Disconnect from the database
 *
 * @param dbh database handle to close
 */
void
TALER_MERCHANTDB_plugin_unload (struct TALER_MERCHANTDB_Plugin *dbh);


#endif  /* MERCHANT_DB_H */

/* end of taler_merchantdb_lib.h */
