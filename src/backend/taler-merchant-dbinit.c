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
 * @file merchant/taler_merchant_dbinit.c
 * @brief Program to initialise merchant database
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "merchant_db.h"


/**
 * Global execution result
 */
static int result;


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{
  PGconn *conn;

  conn = MERCHANT_DB_connect (config);
  if (NULL == conn)
    return;
  if (GNUNET_OK == MERCHANT_DB_initialize (conn, GNUNET_NO))
    result = GNUNET_OK;
  MERCHANT_DB_disconnect (conn);
}


/**
 * The main function
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };

  result = GNUNET_SYSERR;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv, "taler-merchant-dbinit",
                          gettext_noop
                          ("Initialise Taler Merchant's database"),
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;
}
