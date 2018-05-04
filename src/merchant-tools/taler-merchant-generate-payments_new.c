/*
  This file is part of TALER
  (C) 2014-2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it
  under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with TALER; see the file COPYING.  If not,
  see <http://www.gnu.org/licenses/>
*/

/**
 * @file merchant/backend/taler-merchant-httpd.c
 * @brief HTTP serving layer intended to perform crypto-work and
 * communication with the exchange
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_util.h>
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_json_lib.h>
#include <gnunet/gnunet_util_lib.h>
#include <microhttpd.h>
#include <taler/taler_bank_service.h>
#include <taler/taler_fakebank_lib.h>
#include <taler/taler_testing_lib.h>
#include <taler/taler_error_codes.h>
#include "taler_merchant_testing_lib.h"

/**
 * Exit code.
 */
unsigned int result = 1;

/**
 * Merchant process.
 */
static struct GNUNET_OS_Process *merchantd;

/**
 * How many payments we want to generate.
 */
unsigned int payments_number;

/**
 * How many /tracks operation we want to perform.
 */
unsigned int tracks_number;

/**
 * Merchant base URL.
 */
static char *merchant_url;

/**
 * Fakebank URL.
 */
static char *fakebank_url;

/**
 * Actual commands collection.
 */
static void
run_commands (void *cls,
              struct TALER_TESTING_Interpreter *is)
{


  struct TALER_TESTING_Command commands[] = {

    TALER_TESTING_cmd_end ()
  };

  TALER_TESTING_run_with_fakebank (is,
                                   commands,
                                   fakebank_url);
  return;
}

/**
 * Main function that will be run by the scheduler,
 * mainly needed to get the configuration filename to use.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file
 *        used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{
  TALER_LOG_DEBUG ("Using configuration file: %s\n", cfgfile);

  if (NULL == merchant_url)
  {
    TALER_LOG_ERROR ("Option -m is mandatory!\n");
    result = 2;
    return;
  }

  if (NULL == (merchantd = TALER_TESTING_run_merchant
    (cfgfile, merchant_url)))
  {
    TALER_LOG_ERROR ("Failed to launch the merchant\n");
    result = 3;
    return;
  }

  result = 0;

  if (NULL == (fakebank_url = TALER_TESTING_prepare_fakebank
        (cfgfile, "account-1")))
  {
    TALER_LOG_ERROR ("Failed to prepare the fakebank\n");
    result = 4;
    GNUNET_OS_process_kill (merchantd, SIGTERM);
    GNUNET_OS_process_wait (merchantd);
    GNUNET_OS_process_destroy (merchantd);
    return;
  }

  /* Blocks.. */
  result = TALER_TESTING_setup_with_exchange (&run_commands,
                                              NULL,
                                              cfgfile);

  GNUNET_OS_process_kill (merchantd, SIGTERM);
  GNUNET_OS_process_wait (merchantd);
  GNUNET_OS_process_destroy (merchantd);
}


/**
 * The main function of the serve tool
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{

  struct GNUNET_GETOPT_CommandLineOption options[] = {

    GNUNET_GETOPT_option_uint
      ('n',
       "payments-number",
       "PN",
       "will generate PN payments, defaults to 1",
       &payments_number),

    GNUNET_GETOPT_option_uint
      ('t',
       "tracks-number",
       "TN",
       "will perform TN /track operations, defaults to 1",
       &tracks_number),

    /**
     * NOTE: useful when the setup serves merchant
     * backends via unix domain sockets, since there
     * is no way - yet? - to get the merchant base url.
     * Clearly, we could introduce a merchant_base_url
     * value into the configuration.
     */
    GNUNET_GETOPT_option_string
      ('m',
       "merchant-url",
       "MU",
       "merchant base url, mandatory",
       &merchant_url),

    GNUNET_GETOPT_OPTION_END
  };

  GNUNET_PROGRAM_run (argc, argv,
                      "taler-merchant-generate-payments-new",
                      "Populate the database with payments",
                      options, &run, NULL);
  return result;
}
