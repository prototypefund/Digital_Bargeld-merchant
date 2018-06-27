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

/* Error codes.  */
enum PaymentGeneratorError {

  MISSING_MERCHANT_URL = 2,
  FAILED_TO_LAUNCH_MERCHANT,
  MISSING_BANK_URL,
  FAILED_TO_LAUNCH_BANK,
  BAD_CLI_ARG,
  BAD_CONFIG_FILE
};

/* Hard-coded params.  Note, the bank is expected to
 * have the Tor user with account number 3 and password 'x'.
 *
 * This is not a problem _so far_, as the fakebank mocks logins,
 * and the Python bank makes that account by default.  */
#define USER_ACCOUNT_NO 3
#define EXCHANGE_ACCOUNT_NO 2
#define USER_LOGIN_NAME "Tor"
#define USER_LOGIN_PASS "x"
#define EXCHANGE_URL "http://example.com/"

#define FIRST_INSTRUCTION -1
#define TWOCOINS_INSTRUCTION 5

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
 * Merchant process.
 */
static struct GNUNET_OS_Process *merchantd;

/**
 * How many unaggregated payments we want to generate.
 */
static unsigned int unaggregated_number = 1;

/**
 * How many payments that use two coins we want to generate.
 */
static unsigned int twocoins_number = 1;


/**
 * Usually set as ~/.config/taler.net
 */
static const char *default_config_file;

/**
 * Log level used during the run.
 */
static char *loglev;

/**
 * Config filename.
 */
static char *cfg_filename;

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
 * Currency used.
 */
static char *currency;

/**
 * Convenience macros to allocate all the currency-dependant
 * strings;  note that the argument list of the macro is ignored.
 * It is kept as a way to make the macro more auto-descriptive
 * where it is called.
 */

#define ALLOCATE_AMOUNTS(...) \
  GNUNET_asprintf (&CURRENCY_10_02, \
                   "%s:10.02", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_10, \
                   "%s:10", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_9_98, \
                   "%s:9.98", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_5_01, \
                   "%s:5.01", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_5, \
                   "%s:5.01", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_4_99, \
                   "%s:4.99", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_0_02, \
                   "%s:0.02", \
                   currency); \
  GNUNET_asprintf (&CURRENCY_0_01, \
                   "%s:0.01", \
                   currency);

#define ALLOCATE_ORDERS(...) \
  GNUNET_asprintf \
    (&order_worth_5_unaggregated, \
     "{\"max_fee\":\
       {\"currency\":\"%s\",\
        \"value\":0,\
        \"fraction\":50000000},\
       \"refund_deadline\":\"\\/Date(99999999999)\\/\",\
       \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
       \"amount\":\
         {\"currency\":\"%s\",\
          \"value\":5,\
          \"fraction\":0},\
        \"summary\": \"unaggregated deposit!\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"unaggregated cream\",\
                         \"value\":\"{%s:5}\"} ] }", \
     currency, \
     currency, \
     currency); \
  GNUNET_asprintf \
    (&order_worth_10_2coins, \
     "{\"max_fee\":\
       {\"currency\":\"%s\",\
        \"value\":0,\
        \"fraction\":50000000},\
       \"refund_deadline\":\"\\/Date(0)\\/\",\
       \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
       \"amount\":\
         {\"currency\":\"%s\",\
          \"value\":10,\
          \"fraction\":0},\
        \"summary\": \"2-coins payment\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"2-coins payment\",\
                         \"value\":\"{%s:10}\"} ] }", \
     currency, \
     currency, \
     currency);

/**
 * Actual commands collection.
 */
static void
run (void *cls,
     struct TALER_TESTING_Interpreter *is)
{

  /* Currency strings.  */
  char *CURRENCY_10_02;
  char *CURRENCY_10;
  char *CURRENCY_9_98;
  char *CURRENCY_5_01;
  char *CURRENCY_5;
  char *CURRENCY_4_99;
  char *CURRENCY_0_02;
  char *CURRENCY_0_01;

  ALLOCATE_AMOUNTS
    (CURRENCY_10_02,
     CURRENCY_9_98,
     CURRENCY_5_01,
     CURRENCY_5,
     CURRENCY_4_99,
     CURRENCY_0_02,
     CURRENCY_0_01);

  /* Orders.  */
  char *order_worth_5_unaggregated;
  char *order_worth_10_2coins;

  ALLOCATE_ORDERS
    (order_worth_5_unaggregated,
     order_worth_10_2coins);

  /* Will be freed by testing-lib.  */
  GNUNET_assert
    (GNUNET_OK == GNUNET_CURL_append_header
      (is->ctx, APIKEY_SANDBOX));

  struct TALER_TESTING_Command commands[] = {

    CMD_TRANSFER_TO_EXCHANGE
      ("create-reserve-1",
       CURRENCY_5_01),

    TALER_TESTING_cmd_exec_wirewatch
      ("wirewatch-1",
       cfg_filename),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-1",
       is->exchange,
       "create-reserve-1",
       CURRENCY_5,
       MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-unaggregated-proposal",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       order_worth_5_unaggregated,
       NULL),

    TALER_TESTING_cmd_pay
      ("deposit-unaggregated",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "create-unaggregated-proposal",
       "withdraw-coin-1",
       CURRENCY_5,
       CURRENCY_4_99,
       CURRENCY_0_01),

    TALER_TESTING_cmd_rewind_ip
      ("rewind-unaggregated",
       FIRST_INSTRUCTION,
       &unaggregated_number),

    CMD_TRANSFER_TO_EXCHANGE
      ("create-reserve-2",
       CURRENCY_10_02),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-2",
       is->exchange,
       "create-reserve-1",
       CURRENCY_5,
       MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-3",
       is->exchange,
       "create-reserve-1",
       CURRENCY_5,
       MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-twocoins-proposal",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       order_worth_10_2coins,
       NULL),

    TALER_TESTING_cmd_pay
      ("deposit-twocoins",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "create-twocoins-proposal",
       "withdraw-coin-2;withdraw-coin-3",
       CURRENCY_10,
       CURRENCY_9_98,
       CURRENCY_0_02),

    TALER_TESTING_cmd_exec_aggregator
      ("aggregate-twocoins",
       cfg_filename),

    TALER_TESTING_cmd_rewind_ip
      ("rewind-twocoins",
       TWOCOINS_INSTRUCTION,
       &twocoins_number),

    TALER_TESTING_cmd_end ()
  };

  TALER_TESTING_run (is,
                     commands);
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
 * @return 0 ok, or `enum PaymentGeneratorError` on error
 */
int
main (int argc,
      char *const *argv)
{
  struct GNUNET_CONFIGURATION_Handle *cfg;

  default_config_file = GNUNET_OS_project_data_get
    ()->user_config_file;

  struct GNUNET_GETOPT_CommandLineOption options[] = {

    GNUNET_GETOPT_option_cfgfile
      (&cfg_filename),

    GNUNET_GETOPT_option_version
      (PACKAGE_VERSION " " VCS_VERSION),

    GNUNET_GETOPT_option_help
      ("Generate Taler payments to populate the database(s)"),

    GNUNET_GETOPT_option_loglevel
      (&loglev),

    GNUNET_GETOPT_option_uint
      ('u',
       "unaggregated-number",
       "UN",
       "will generate UN unaggregated payments, defaults to 1",
       &unaggregated_number),

    GNUNET_GETOPT_option_uint
      ('t',
       "two-coins",
       "TC",
       "will perform TC 2-coins payments, defaults to 1",
       &twocoins_number),

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

  result = GNUNET_GETOPT_run
    ("taler-merchant-generate-payments-alt",
     options,
     argc,
     argv);

  if (GNUNET_NO == result)
  {
    /* --help or --version were given, just return.  */ 
    return 0;
  }

  GNUNET_assert (GNUNET_SYSERR != result);
  loglev = NULL;
  GNUNET_log_setup ("taler-merchant-generate-payments-alt",
                    loglev,
                    logfile);

  if (NULL == cfg_filename)
    cfg_filename = (char *) default_config_file;

  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_OK != GNUNET_CONFIGURATION_load
      (cfg,
       cfg_filename))
  {
    TALER_LOG_ERROR ("Could not parse configuration\n");
    return BAD_CONFIG_FILE;
  }
  if (GNUNET_OK != GNUNET_CONFIGURATION_get_value_string
      (cfg,
       "taler",
       "currency",
       &currency))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "taler",
                               "currency");
    GNUNET_CONFIGURATION_destroy (cfg);
    return BAD_CONFIG_FILE;
  }
  GNUNET_CONFIGURATION_destroy (cfg);

  if (NULL == merchant_url)
  {
    TALER_LOG_ERROR ("Option -m is mandatory!\n");
    return MISSING_MERCHANT_URL;
  }

  if (NULL == (merchantd = TALER_TESTING_run_merchant
    (cfg_filename, merchant_url)))
  {
    TALER_LOG_ERROR ("Failed to launch the merchant\n");
    return FAILED_TO_LAUNCH_MERCHANT;
  }

  if (NULL == bank_url)
  {
    TALER_LOG_ERROR ("Option -b is mandatory!\n");
    return MISSING_BANK_URL;
  }

  if ( NULL == (bankd = TALER_TESTING_run_bank
    (cfg_filename,
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
     cfg_filename);

  terminate_process (merchantd);
  terminate_process (bankd);

  return (GNUNET_OK == result) ? 0 : result;
}
