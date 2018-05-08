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
#include <taler/taler_testing_bank_lib.h>
#include <taler/taler_error_codes.h>
#include "taler_merchant_testing_lib.h"

#define MISSING_MERCHANT_URL 2
#define FAILED_TO_LAUNCH_MERCHANT 3
#define MISSING_BANK_URL 4
#define FAILED_TO_LAUNCH_BANK 5

/**
 * Exit code.
 */
unsigned int result = 1;

/**
 * Merchant process.
 */
static struct GNUNET_OS_Process *merchantd;

/**
 * Bank process.
 */
static struct GNUNET_OS_Process *bankd;

/**
 * How many payments we want to generate.
 */
unsigned int payments_number;

/**
 * How many /tracks operation we want to perform.
 */
unsigned int tracks_number;

/**
 * Bank base URL.
 */
static char *bank_url;

/**
 * Log file.
 */
static char *logfile;

/**
 * Merchant base URL.
 */
static char *merchant_url;

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

  TALER_TESTING_run (is, commands);
}

/**
 * Send SIGTERM and wait for process termination.
 *
 * @param process process to terminate.
 */
void
terminate_process (struct GNUNET_OS_Process *process)
{
  GNUNET_OS_process_kill (process, SIGTERM);
  GNUNET_OS_process_wait (process);
  GNUNET_OS_process_destroy (process);
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
    GNUNET_OS_process_kill (merchantd, SIGTERM);
    GNUNET_OS_process_wait (merchantd);
    GNUNET_OS_process_destroy (merchantd);
    return;
  }

  if (NULL == bank_url)
  {
    TALER_LOG_ERROR ("Option -b is mandatory!\n");
    result = 5;
    return;
  }
  if (NULL == (bankd = TALER_TESTING_run_bank (cfgfile,
                                               bank_url)))
  {
    TALER_LOG_ERROR ("Failed to run the bank\n");
    result = 4;
    GNUNET_OS_process_kill (merchantd, SIGTERM);
    GNUNET_OS_process_wait (merchantd);
    GNUNET_OS_process_destroy (merchantd);
    return;
  }

  GNUNET_OS_process_kill (merchantd, SIGTERM);
  GNUNET_OS_process_wait (merchantd);
  GNUNET_OS_process_destroy (merchantd);
  GNUNET_OS_process_kill (bankd, SIGTERM);
  GNUNET_OS_process_wait (bankd);
  GNUNET_OS_process_destroy (bankd);
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

  const char *default_config_file;

  default_config_file = GNUNET_OS_project_data_get
    ()->user_config_file;

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

    GNUNET_GETOPT_option_string
      ('b',
       "bank-url",
       "BU",
       "bank base url, mandatory",
       &bank_url),

    GNUNET_GETOPT_option_string
      ('l',
       "logfile",
       "LF",
       "will log to file LF",
       &logfile),

    GNUNET_GETOPT_OPTION_END
  };

  GNUNET_assert (GNUNET_SYSERR != GNUNET_GETOPT_run
    ("taler-merchant-generate-payments-new",
     options,
     argc,
     argv));

  GNUNET_log_setup ("taler-merchant-generate-payments-new",
                    "INFO",
                    logfile);

  if (NULL == merchant_url)
  {
    TALER_LOG_ERROR ("Option -m is mandatory!\n");
    return MISSING_MERCHANT_URL;
  }

  if (NULL == (merchantd = TALER_TESTING_run_merchant
    (default_config_file, merchant_url)))
  {
    TALER_LOG_ERROR ("Failed to launch the merchant\n");
    terminate_process (merchantd);
    return FAILED_TO_LAUNCH_MERCHANT;
  }

  if (NULL == bank_url)
  {
    TALER_LOG_ERROR ("Option -b is mandatory!\n");
    return MISSING_BANK_URL;
  }

  if (NULL == (bankd = TALER_TESTING_run_bank
    (default_config_file,
     bank_url)))
  {
    TALER_LOG_ERROR ("Failed to run the bank\n");
    terminate_process (bankd);
    terminate_process (merchantd);
    return FAILED_TO_LAUNCH_BANK;
  }

  result = TALER_TESTING_setup_with_exchange
    (run_commands,
     options,
     default_config_file);
  return result;
}
