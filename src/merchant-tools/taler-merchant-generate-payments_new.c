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

#define USER_ACCOUNT_NO 3
#define EXCHANGE_ACCOUNT_NO 2
#define USER_LOGIN_NAME "Tor"
#define USER_LOGIN_PASS "x"
#define EXCHANGE_URL "http://example.com/"

#define CMD_TRANSFER_TO_EXCHANGE(label,amount) \
   TALER_TESTING_cmd_fakebank_transfer (label, amount, \
     bank_url, USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO, \
     USER_LOGIN_NAME, USER_LOGIN_PASS, EXCHANGE_URL)

/**
 * Exit code.
 */
static unsigned int result;

/**
 * Bank process.
 */
static struct GNUNET_OS_Process *bankd;

/**
 * Indicates whether we'll use the Python bank (GNUNET_YES),
 * or the fakebank.
 */
static int with_pybank;

/**
 * Merchant process.
 */
static struct GNUNET_OS_Process *merchantd;

/**
 * How many payments we want to generate.
 */
static unsigned int payments_number = 1;

/**
 * How many /tracks operation we want to perform.
 */
static unsigned int tracks_number;


static const char *default_config_file;

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
run (void *cls,
     struct TALER_TESTING_Interpreter *is)
{

  struct TALER_TESTING_Command commands[] = {
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-1",
                              "USD:10.02"),
    TALER_TESTING_cmd_exec_wirewatch ("wirewatch-1",
                                      default_config_file),
    TALER_TESTING_cmd_rewind_ip ("rew-payments", -1,
                                 &payments_number),
    TALER_TESTING_cmd_end ()
  };

  if (GNUNET_YES == with_pybank)
  {
    TALER_TESTING_run (is,
                       commands);
    return; 
  }

  TALER_TESTING_run_with_fakebank
    (is,
     commands,
     bank_url);
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

    GNUNET_GETOPT_option_flag
      ('p',
       "with-pybank",
       "Use the Python bank, if given",
       &with_pybank),

    GNUNET_GETOPT_OPTION_END
  };

  GNUNET_assert (GNUNET_SYSERR != GNUNET_GETOPT_run
    ("taler-merchant-generate-payments-new",
     options,
     argc,
     argv));

  GNUNET_log_setup ("taler-merchant-generate-payments-new",
                    "DEBUG",
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

  if ((GNUNET_YES == with_pybank)
      && NULL == (bankd = TALER_TESTING_run_bank
        (default_config_file,
         bank_url)))
  {
    TALER_LOG_ERROR ("Failed to run the bank\n");
    terminate_process (bankd);
    terminate_process (merchantd);
    return FAILED_TO_LAUNCH_BANK;
  }

  result = TALER_TESTING_setup_with_exchange
    (run,
     NULL,
     default_config_file);

  terminate_process (merchantd);
  terminate_process (bankd);

  return (GNUNET_OK == result) ? 0 : result;
}
