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


#define APIKEY_SANDBOX "Authorization: ApiKey sandbox"


/* Error codes.  */
enum PaymentGeneratorError
{

  MISSING_MERCHANT_URL = 2,
  FAILED_TO_LAUNCH_MERCHANT,
  MISSING_BANK_URL,
  FAILED_TO_LAUNCH_BANK,
  BAD_CLI_ARG,
  MISSING_CURRENCY
};

/* Hard-coded params.  Note, the bank is expected to
 * have the Tor user with account number 3 and password 'x'.
 */
#define PAYER_URL "FIXME/3"
#define EXCHANGE_ACCOUNT_NO 2
#define USER_LOGIN_NAME "Tor"
#define USER_LOGIN_PASS "x"
#define EXCHANGE_URL "http://example.com/"
#define FIRST_INSTRUCTION -1
#define TRACKS_INSTRUCTION 9
#define TWOCOINS_INSTRUCTION 5

/**
 * Help string shown if NO subcommand is given on command line.
 */
static int root_help;

/**
 * Witnesses if the ordinary cases payment suite should be run.
 */
static unsigned int ordinary;

/**
 * Witnesses if the corner cases payment suite should be run.
 */
static unsigned int corner;

/**
 * Root help string.
 */
static const char *root_help_str = \
  "taler-merchant-benchmark\nPopulates production database"
  " with fake payments.\nMust be used with either 'ordinary'"
  " or 'corner' sub-commands.\n";

/**
 * Alternative non default instance.
 */
static char *alt_instance_id;

/**
 * Base URL of the alternative non default instance.
 */
static char *alt_instance_url;

/**
 * How many unaggregated payments we want to generate.
 */
static unsigned int unaggregated_number = 1;

/**
 * How many payments that use two coins we want to generate.
 */
static unsigned int twocoins_number = 1;

/**
 * Exit code.
 */
static int result;

/**
 * Bank process.
 */
static struct GNUNET_OS_Process *bankd;

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
static unsigned int tracks_number = 1;


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
 * Authentication data to use. FIXME: init !
 */
static struct TALER_BANK_AuthenticationData auth;

static char *exchange_payto;
static struct TALER_WireTransferIdentifierRawP wtid;

/**
 * Convenience macros to allocate all the currency-dependant
 * strings;  note that the argument list of the macro is ignored.
 * It is kept as a way to make the macro more auto-descriptive
 * where it is called.
 */

#define ALLOCATE_AMOUNTS(...) \
  char *CURRENCY_10_02; \
  char *CURRENCY_10; \
  char *CURRENCY_9_98; \
  char *CURRENCY_5_01; \
  char *CURRENCY_5; \
  char *CURRENCY_4_99; \
  char *CURRENCY_0_02; \
  char *CURRENCY_0_01; \
  \
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
                   "%s:5", \
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
  char *order_worth_5; \
  char *order_worth_5_track; \
  char *order_worth_5_unaggregated; \
  char *order_worth_10_2coins; \
  \
  GNUNET_asprintf \
    (&order_worth_5, \
    "{\"max_fee\":\
       {\"currency\":\"%s\",\
        \"value\":0,\
        \"fraction\":50000000},\
       \"refund_deadline\":\"\\/Date(0)\\/\",\
       \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
       \"amount\":\
         {\"currency\":\"%s\",\
          \"value\":5,\
          \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{%s:5}\"} ] }", \
    currency, \
    currency, \
    currency); \
  GNUNET_asprintf \
    (&order_worth_5_track, \
    "{\"max_fee\":\
       {\"currency\":\"%s\",\
        \"value\":0,\
        \"fraction\":50000000},\
       \"refund_deadline\":\"\\/Date(0)\\/\",\
       \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
       \"amount\":\
         {\"currency\":\"%s\",\
          \"value\":5,\
          \"fraction\":0},\
        \"summary\": \"ice track cream!\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"ice track cream\",\
                         \"value\":\"{%s:5}\"} ] }", \
    currency, \
    currency, \
    currency); \
  GNUNET_asprintf \
    (&order_worth_5_unaggregated, \
    "{\"max_fee\":\
       {\"currency\":\"%s\",\
        \"value\":0,\
        \"fraction\":50000000},\
       \"wire_transfer_delay\":\"\\/Delay(30000)\\/\",\
       \"refund_deadline\":\"\\/Date(22)\\/\",\
       \"pay_deadline\":\"\\/Date(1)\\/\",\
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

  /* Will be freed by testing-lib.  */
  GNUNET_assert
    (GNUNET_OK == GNUNET_CURL_append_header
      (is->ctx, APIKEY_SANDBOX));

  ALLOCATE_AMOUNTS
    (CURRENCY_10_02,
    CURRENCY_9_98,
    CURRENCY_5_01,
    CURRENCY_5,
    CURRENCY_4_99,
    CURRENCY_0_02,
    CURRENCY_0_01);

  ALLOCATE_ORDERS
    (order_worth_5,
    order_worth_5_track,
    order_worth_5_unaggregated,
    order_worth_10_2coins);

  struct TALER_TESTING_Command ordinary_commands[] = {

    TALER_TESTING_cmd_transfer
      ("create-reserve-1",
      CURRENCY_10_02,
      PAYER_URL,  // bank base URL + path to the payer account.
      &auth,
      exchange_payto,
      &wtid,
      EXCHANGE_URL),

    TALER_TESTING_cmd_exec_wirewatch
      ("wirewatch-1",
      cfg_filename),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-1",
      "create-reserve-1",
      CURRENCY_5,
      MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-2",
      "create-reserve-1",
      CURRENCY_5,
      MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-1",
      merchant_url,
      MHD_HTTP_OK,
      order_worth_5),

    TALER_TESTING_cmd_pay
      ("deposit-simple",
      merchant_url,
      MHD_HTTP_OK,
      "create-proposal-1",
      "withdraw-coin-1",
      CURRENCY_5,
      CURRENCY_4_99,
      CURRENCY_0_01),

    TALER_TESTING_cmd_rewind_ip
      ("rewind-payments",
      FIRST_INSTRUCTION,
      &payments_number),

    /* Next proposal-pay cycle will be used by /track CMDs
     * and so it will not have to be looped over, only /track
     * CMDs will have to.  */

    TALER_TESTING_cmd_proposal
      ("create-proposal-2",
      merchant_url,
      MHD_HTTP_OK,
      order_worth_5_track),

    TALER_TESTING_cmd_pay
      ("deposit-simple-2",
      merchant_url,
      MHD_HTTP_OK,
      "create-proposal-2",
      "withdraw-coin-2",
      CURRENCY_5,
      CURRENCY_4_99,
      CURRENCY_0_01),

    /* /track/transaction over deposit-simple-2 */

    TALER_TESTING_cmd_exec_aggregator
      ("aggregate-1",
      cfg_filename),

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-1",
      merchant_url,
      MHD_HTTP_OK,
      "deposit-simple-2"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-1",
      merchant_url,
      MHD_HTTP_OK,
      "track-transaction-1"),

    TALER_TESTING_cmd_rewind_ip
      ("rewind-tracks",
      TRACKS_INSTRUCTION,
      &tracks_number),

    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command corner_commands[] = {

    TALER_TESTING_cmd_transfer
      ("create-reserve-1",
      CURRENCY_5_01,
      PAYER_URL,
      &auth,
      exchange_payto,
      &wtid,
      EXCHANGE_URL),

    TALER_TESTING_cmd_exec_wirewatch
      ("wirewatch-1",
      cfg_filename),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-1",
      "create-reserve-1",
      CURRENCY_5,
      MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-unaggregated-proposal",
      alt_instance_url,
      MHD_HTTP_OK,
      order_worth_5_unaggregated),

    TALER_TESTING_cmd_pay
      ("deposit-unaggregated",
      merchant_url,
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

    TALER_TESTING_cmd_transfer
      ("create-reserve-2",
      CURRENCY_10_02,
      PAYER_URL,
      &auth,
      exchange_payto,
      &wtid,
      EXCHANGE_URL),

    TALER_TESTING_cmd_exec_wirewatch
      ("wirewatch-2",
      cfg_filename),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-2",
      "create-reserve-2",
      CURRENCY_5,
      MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-3",
      "create-reserve-2",
      CURRENCY_5,
      MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-twocoins-proposal",
      merchant_url,
      MHD_HTTP_OK,
      order_worth_10_2coins),

    TALER_TESTING_cmd_pay
      ("deposit-twocoins",
      merchant_url,
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

  if (GNUNET_OK == ordinary)
  {
    TALER_TESTING_run (is,
                       ordinary_commands);
    return;
  }

  if (GNUNET_OK == corner)
  {
    TALER_TESTING_run (is,
                       corner_commands);
    return;
  }

  /* Should never get here, as the control on subcommands
   * happens earlier at launch time.  */
  fprintf (stderr,
           "None of 'ordinary' or 'corner'"
           " subcommands were given\n");
  result = 1;
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
  struct GNUNET_GETOPT_CommandLineOption *options;
  struct GNUNET_GETOPT_CommandLineOption root_options[] = {
    GNUNET_GETOPT_option_cfgfile
      (&cfg_filename),
    GNUNET_GETOPT_option_version
      (PACKAGE_VERSION " " VCS_VERSION),
    GNUNET_GETOPT_option_flag
      ('h',
      "help",
      NULL,
      &root_help),
    GNUNET_GETOPT_OPTION_END
  };

  struct GNUNET_GETOPT_CommandLineOption corner_options[] = {
    GNUNET_GETOPT_option_help
      ("Populate databases with corner case payments"),
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
     */GNUNET_GETOPT_option_string
      ('m',
      "merchant-url",
      "MU",
      "merchant base url, mandatory",
      &merchant_url),
    GNUNET_GETOPT_option_string
      ('k',
      "currency",
      "K",
      "Used currency, mandatory",
      &currency),
    GNUNET_GETOPT_option_string
      ('i',
      "alt-instance",
      "AI",
      "alternative (non default) instance,"
      " used to provide fresh wire details to"
      " make unaggregated transactions stay so."
      " Note, this instance will be given far"
      " future wire deadline, and so it should"
      " never author now-deadlined transactions,"
      " as they would get those far future ones"
      " aggregated too.",
      &alt_instance_id),
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

  struct GNUNET_GETOPT_CommandLineOption ordinary_options[] = {
    GNUNET_GETOPT_option_cfgfile
      (&cfg_filename),
    GNUNET_GETOPT_option_version
      (PACKAGE_VERSION " " VCS_VERSION),
    GNUNET_GETOPT_option_help
      ("Generate Taler ordinary payments"
      " to populate the databases"),
    GNUNET_GETOPT_option_loglevel
      (&loglev),
    GNUNET_GETOPT_option_uint
      ('p',
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
     */GNUNET_GETOPT_option_string
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

  default_config_file = GNUNET_OS_project_data_get
                          ()->user_config_file;

  loglev = NULL;
  GNUNET_log_setup ("taler-merchant-benchmark",
                    loglev,
                    logfile);
  options = root_options;
  if (NULL != argv[1])
  {
    if (0 == strcmp ("ordinary", argv[1]))
    {
      ordinary = GNUNET_YES;
      options = ordinary_options;
    }
    if (0 == strcmp ("corner", argv[1]))
    {
      corner = GNUNET_YES;
      options = corner_options;
    }
  }

  if (GNUNET_SYSERR !=
      (result = GNUNET_GETOPT_run
                  ("taler-merchant-benchmark",
                  options,
                  argc,
                  argv)))
  {

    if (GNUNET_YES == root_help)
    {
      fprintf (stdout,
               "%s",
               root_help_str);
      return 0;
    }

    /* --help was given.  */
    if (0 == result)
      return 0;
  }
  if (-1 == result)
    return 1;
  if ( (GNUNET_YES != ordinary) &&
       (GNUNET_YES != corner) )
  {
    fprintf (stderr,
             "Please use 'ordinary' or 'corner' subcommands.\n");
    return 1;
  }

  if ( (GNUNET_YES == corner) &&
       (NULL == alt_instance_id) )
  {
    fprintf (stderr,
             "option '-i' is mandatory with sub-command 'corner'!\n");
    return 1;
  }
  if (NULL == cfg_filename)
    cfg_filename = (char *) default_config_file;
  if (NULL == currency)
  {
    TALER_LOG_ERROR ("Option -k is mandatory!\n");
    return MISSING_CURRENCY;
  }
  if (NULL == merchant_url)
  {
    TALER_LOG_ERROR ("Option -m is mandatory!\n");
    return MISSING_MERCHANT_URL;
  }

  if (NULL != alt_instance_id)
  {
    GNUNET_assert (0 < GNUNET_asprintf (&alt_instance_url,
                                        "%s/instances/%s/",
                                        merchant_url,
                                        &alt_instance_id));
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
    terminate_process (merchantd);
    return MISSING_BANK_URL;
  }

  if (NULL == (bankd = TALER_TESTING_run_bank
                         (cfg_filename,
                         bank_url)))
  {
    TALER_LOG_ERROR ("Failed to run the bank\n");
    terminate_process (merchantd);
    return FAILED_TO_LAUNCH_BANK;
  }

  /**
   * FIXME: Need to retrieve the bank base URL!
   */
  exchange_payto = TALER_payto_xtalerbank_make ("FIXME-BANK-HOSTNAME:PORT",
                                                "/2");

  result = TALER_TESTING_setup_with_exchange
             (run,
             NULL,
             cfg_filename);

  terminate_process (merchantd);
  terminate_process (bankd);

  return (GNUNET_OK == result) ? 0 : result;
}
