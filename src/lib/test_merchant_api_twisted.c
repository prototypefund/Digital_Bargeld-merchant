/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file exchange/test_merchant_api_new.c
 * @brief testcase to test exchange's HTTP API interface
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
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
#include "taler_merchant_testing_lib.h"
#include <taler/taler_twister_testing_lib.h>
#include <taler/taler_twister_service.h>

/**
 * Configuration file we use.  One (big) configuration is used
 * for the various components for this test.
 */
#define CONFIG_FILE "test_merchant_api_twisted.conf"

/**
 * Exchange base URL.  Could also be taken from config.
 */
#define EXCHANGE_URL "http://localhost:8081/"

/**
 * (real) Twister URL.  Used at startup time to check if it runs.
 */
static char *twister_url;

/**
 * URL of the fakebank.  Obtained from CONFIG_FILE's
 * "exchange-wire-test:BANK_URI" option.
 */
static char *fakebank_url;

/**
 * Merchant base URL.
 */
static char *merchant_url;

/**
 * Exchange base URL.
 */
static char *exchange_url;

/**
 * Merchant process.
 */
static struct GNUNET_OS_Process *merchantd;

/**
 * Twister process.
 */
static struct GNUNET_OS_Process *twisterd;

/**
 * Account number of the exchange at the bank.
 */
#define EXCHANGE_ACCOUNT_NO 2

/**
 * Account number of some user.
 */
#define USER_ACCOUNT_NO 62

/**
 * User name. Never checked by fakebank.
 */
#define USER_LOGIN_NAME "user42"

/**
 * User password. Never checked by fakebank.
 */
#define USER_LOGIN_PASS "pass42"

/**
 * Execute the taler-exchange-wirewatch command with
 * our configuration file.
 *
 * @param label label to use for the command.
 */
#define CMD_EXEC_WIREWATCH(label) \
   TALER_TESTING_cmd_exec_wirewatch (label, CONFIG_FILE)

/**
 * Execute the taler-exchange-aggregator command with
 * our configuration file.
 *
 * @param label label to use for the command.
 */
#define CMD_EXEC_AGGREGATOR(label) \
   TALER_TESTING_cmd_exec_aggregator (label, CONFIG_FILE)

/**
 * Run wire transfer of funds from some user's account to the
 * exchange.
 *
 * @param label label to use for the command.
 * @param amount amount to transfer, i.e. "EUR:1"
 * @param url exchange_url
 */
#define CMD_TRANSFER_TO_EXCHANGE(label,amount) \
   TALER_TESTING_cmd_fakebank_transfer (label, amount, \
     fakebank_url, USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO, \
     USER_LOGIN_NAME, USER_LOGIN_PASS, EXCHANGE_URL)

/**
 * Run wire transfer of funds from some user's account to the
 * exchange.
 *
 * @param label label to use for the command.
 * @param amount amount to transfer, i.e. "EUR:1"
 */
#define CMD_TRANSFER_TO_EXCHANGE_SUBJECT(label,amount,subject) \
   TALER_TESTING_cmd_fakebank_transfer_with_subject \
     (label, amount, fakebank_url, USER_ACCOUNT_NO, \
      EXCHANGE_ACCOUNT_NO, USER_LOGIN_NAME, USER_LOGIN_PASS, \
      subject)

/**
 * Main function that will tell the interpreter what commands to
 * run.
 *
 * @param cls closure
 */
static void
run (void *cls,
     struct TALER_TESTING_Interpreter *is)
{

  struct TALER_TESTING_Command commands[] = {

    /**
     * Move money to the exchange's bank account.
     */
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-1",
                              "EUR:2.02"),
    /**
     * Make a reserve exist, according to the previous
     * transfer.
     */
    CMD_EXEC_WIREWATCH ("wirewatch-1"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-2",
       "http://localhost:8081/",
       "EUR:2.02", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-1",
                                       is->exchange,
                                       "create-reserve-1",
                                       "EUR:1",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-2",
                                       is->exchange,
                                       "create-reserve-1",
                                       "EUR:1",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_status ("withdraw-status",
                              is->exchange,
                              "create-reserve-1",
                              "EUR:0",
                              MHD_HTTP_OK),
    TALER_TESTING_cmd_proposal
      ("create-proposal-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"1\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":2,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:3}\"} ] }",
        NULL),

    TALER_TESTING_cmd_check_payment ("check-payment-1",
                                     merchant_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "create-proposal-1",
                                     GNUNET_NO),

    TALER_TESTING_cmd_pay ("deposit-simple",
                           merchant_url,
                           is->ctx,
                           MHD_HTTP_OK,
                           "create-proposal-1",
                           "withdraw-coin-1;" \
                           "withdraw-coin-2",
                           "EUR:2",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    #if 0
    TALER_TESTING_cmd_check_payment ("check-payment-2",
                                     merchant_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "create-proposal-1",
                                     GNUNET_YES),

    CMD_EXEC_AGGREGATOR ("run-aggregator"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-1",
       exchange_url,
       "EUR:5.98", 2, 62),
    #endif

    /* Should instead change the response body somehow! */
    #if 0
    TALER_TESTING_cmd_empty_object ("hack-1",
                                    CONFIG_FILE,
                                    "deposits.0"),

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-1",
       "deposit-simple",
       "EUR:0.01"), // ignored

    #endif

    #if 0
    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-1",
       "deposit-simple"),
    #endif

    /**
     * End the suite.  Fixme: better to have a label for this
     * too, as it shows a "(null)" token on logs.
     */
    TALER_TESTING_cmd_end ()
  };

  TALER_TESTING_run_with_fakebank (is,
                                   commands,
                                   fakebank_url);
}

/**
 * Kill, wait, and destroy convenience function.
 *
 * @param process process to purge.
 */
static void
purge_process (struct GNUNET_OS_Process *process)
{
  GNUNET_OS_process_kill (process, SIGKILL);
  GNUNET_OS_process_wait (process);
  GNUNET_OS_process_destroy (process);
}

int
main (int argc,
      char * const *argv)
{
  unsigned int ret;
  /* These environment variables get in the way... */
  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("test-merchant-api-new-twisted",
                    "DEBUG", NULL);

  if (NULL == (fakebank_url = TALER_TESTING_prepare_fakebank
      (CONFIG_FILE)))
    return 77;

  if (NULL == (merchant_url = TALER_TESTING_prepare_merchant
      (CONFIG_FILE)))
    return 77;

  if (NULL == (twister_url = TALER_TESTING_prepare_twister
      (CONFIG_FILE)))
    return 77;

  TALER_TESTING_cleanup_files (CONFIG_FILE);

  switch (TALER_TESTING_prepare_exchange (CONFIG_FILE,
                                          &exchange_url))
  {
  case GNUNET_SYSERR:
    GNUNET_break (0);
    return 1;
  case GNUNET_NO:
    return 77;

  case GNUNET_OK:

    if (NULL == (merchantd = TALER_TESTING_run_merchant
        (CONFIG_FILE)))
    {
      purge_process (twisterd);
      return 1; // 1 is fine; after all is merchant test cases.
    }

    if (NULL == (twisterd = TALER_TESTING_run_twister
        (CONFIG_FILE)))
      return 77;

    ret = TALER_TESTING_setup_with_exchange (&run, NULL,
                                             CONFIG_FILE);
    purge_process (merchantd);
    purge_process (twisterd);
    GNUNET_free (merchant_url);
    GNUNET_free (twister_url);

    if (GNUNET_OK != ret)
      return 1;
    break;
  default:
    GNUNET_break (0);
    return 1;
  }
  return 0;
}

/* end of test_merchant_api_new.c */
