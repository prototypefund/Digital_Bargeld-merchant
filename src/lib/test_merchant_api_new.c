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

/**
 * Configuration file we use.  One (big) configuration is used
 * for the various components for this test.
 */
#define CONFIG_FILE "test_merchant_api.conf"

/**
 * Exchange base URL.  Could also be taken from config.
 */
#define EXCHANGE_URL "http://localhost:8081/"

static const char *pickup_amounts_1[] = {"EUR:5", NULL};

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
 * Merchant process.
 */
static struct GNUNET_OS_Process *merchantd;

/**
 * Exchange base URL.
 */
static char *exchange_url;

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
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Merchant serves at `%s'\n",
              merchant_url);

  struct TALER_TESTING_Command commands[] = {

    /**
     * Move money to the exchange's bank account.
     */
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-1",
                              "EUR:10.02"),
    /**
     * Make a reserve exist, according to the previous
     * transfer.
     */
    CMD_EXEC_WIREWATCH ("wirewatch-1"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-2",
       "http://localhost:8081/",
       "EUR:10.02", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-1",
                                       is->exchange,
                                       "create-reserve-1",
                                       "EUR:5",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-2",
                                       is->exchange,
                                       "create-reserve-1",
                                       "EUR:5",
                                       MHD_HTTP_OK),
    /**
     * Check the reserve is depleted.
     */
    TALER_TESTING_cmd_status ("withdraw-status-1",
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
           \"value\":5,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:5}\"} ] }",
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
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_check_payment ("check-payment-2",
                                     merchant_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "create-proposal-1",
                                     GNUNET_YES),

    TALER_TESTING_cmd_pay_abort ("pay-abort-2",
                                 merchant_url,
                                 "deposit-simple",
                                 is->ctx,
                                 MHD_HTTP_FORBIDDEN),

    TALER_TESTING_cmd_pay ("replay-simple",
                           merchant_url,
                           is->ctx,
                           MHD_HTTP_OK,
                           "create-proposal-1",
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_proposal
      ("create-proposal-2",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"2\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":5,\
           \"fraction\":0},\
        \"summary\": \"useful product\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:5}\"} ] }",
        NULL),

    TALER_TESTING_cmd_pay ("deposit-double-2",
                           merchant_url,
                           is->ctx,
                           MHD_HTTP_FORBIDDEN,
                           "create-proposal-2",
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_history ("history-0",
                               merchant_url,
                               is->ctx,
                               MHD_HTTP_OK,
      /**
       * all records to be returned; setting date as 0 lets the
       * interpreter set it as 'now' + one hour delta, just to
       * make sure it surpasses the proposal's timestamp.
       */
                               GNUNET_TIME_UNIT_ZERO_ABS,
      /**
       * We only expect ONE result (create-proposal-1) to be
       * included in /history response, because create-proposal-3
       * did NOT go through because of double spending.
       */
                               1, // nresult
                               10, // start
                               10), // nrows

    TALER_TESTING_cmd_fakebank_transfer ("create-reserve-2",
                                         "EUR:1",
                                         fakebank_url,
                                         63, 2,
                                         "user63",
                                         "pass63",
                                         EXCHANGE_URL),

    TALER_TESTING_cmd_fakebank_transfer_with_ref
      ("create-reserve-2b",
       "EUR:4.01",
       fakebank_url,
       63, 2,
       "user63",
       "pass63",
       "create-reserve-2",
       EXCHANGE_URL),
    CMD_EXEC_WIREWATCH ("wirewatch-2"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-2",
       "http://localhost:8081/",
       "EUR:1", 63, 2),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-2",
       "http://localhost:8081/",
       "EUR:4.01", 63, 2),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-2",
                                       is->exchange,
                                       "create-reserve-2",
                                       "EUR:5",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal_lookup ("fetch-proposal-2",
                                       is->ctx,
                                       merchant_url,
                                       MHD_HTTP_OK,
                                       "create-proposal-2",
                                       NULL),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty-1"),

    CMD_EXEC_AGGREGATOR ("run-aggregator"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-498c",
       "http://localhost:8081/",
       "EUR:4.98", 2, 62),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty-2"),

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c",
       "deposit-simple",
       "EUR:0.01"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c",
       "deposit-simple"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-again",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c",
       "deposit-simple"),

    TALER_TESTING_cmd_pay ("deposit-simple-2",
                           merchant_url,
                           is->ctx,
                           MHD_HTTP_OK,
                           "create-proposal-2",
                           "withdraw-coin-2",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    CMD_EXEC_AGGREGATOR ("run-aggregator-2"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-498c-2",
       "http://localhost:8081/",
       "EUR:4.98",
       EXCHANGE_ACCOUNT_NO,
       USER_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-2",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c-2",
       "deposit-simple-2"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-2-again",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c-2",
       "deposit-simple-2"),

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-2",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c-2",
       "deposit-simple-2",
       "EUR:0.01"),

    TALER_TESTING_cmd_history ("history-1",
                               merchant_url,
                               is->ctx,
                               MHD_HTTP_OK,
                               GNUNET_TIME_UNIT_ZERO_ABS,
      /**
       * Now we expect BOTH contracts (create-proposal-{1,2})
       * to be included in /history response, because
       * create-proposal-2 has now been correctly paid.
       */
                               2,
                               10,
                               10),

    TALER_TESTING_cmd_history
      ("history-2",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       GNUNET_TIME_absolute_add (GNUNET_TIME_UNIT_ZERO_ABS,
                                 GNUNET_TIME_UNIT_MICROSECONDS),
       /* zero results expected, time too ancient. */
       0,
       10,
       10),

    TALER_TESTING_cmd_refund_increase ("refund-increase-1",
                                       merchant_url,
                                       is->ctx,
                                       "refund test",
                                       "1",
                                       "EUR:0.1",
                                       "EUR:0.01"),

    TALER_TESTING_cmd_refund_lookup ("refund-lookup-1",
                                     merchant_url,
                                     is->ctx,
                                     "refund-increase-1",
                                     "deposit-simple",
                                     "1"),

    /* Test tipping.  */
    TALER_TESTING_cmd_fakebank_transfer_with_instance
      ("create-reserve-tip-1",
       "EUR:20.04",
       fakebank_url,
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO,
       USER_LOGIN_NAME,
       USER_LOGIN_PASS,
       "tip",
       EXCHANGE_URL,
       CONFIG_FILE),


    CMD_EXEC_WIREWATCH ("wirewatch-11"),

    TALER_TESTING_cmd_tip_authorize ("authorize-tip-1",
                                     merchant_url,
                                     exchange_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "tip",
                                     "tip 1",
                                     "EUR:5.01"),

    TALER_TESTING_cmd_tip_authorize ("authorize-tip-2",
                                     merchant_url,
                                     exchange_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "tip",
                                     "tip 2",
                                     "EUR:5.01"),

    TALER_TESTING_cmd_tip_query ("query-tip-1",
                                 merchant_url,
                                 is->ctx,
                                 MHD_HTTP_OK,
                                 "tip"),

    TALER_TESTING_cmd_tip_query_with_amounts ("query-tip-2",
                                              merchant_url,
                                              is->ctx,
                                              MHD_HTTP_OK,
                                              "tip",
                                              "EUR:0.0", // picked
                                              "EUR:10.02", // auth
                                              "EUR:20.04"),// ava

    TALER_TESTING_cmd_tip_pickup ("pickup-tip-1",
                                  merchant_url,
                                  is->ctx,
                                  MHD_HTTP_OK,
                                  "authorize-tip-1",
                                  pickup_amounts_1,
                                  is->exchange),

    TALER_TESTING_cmd_tip_query_with_amounts ("query-tip-3",
                                              merchant_url,
                                              is->ctx,
                                              MHD_HTTP_OK,
                                              "tip",
                                              "EUR:5.01", // picked
                                              NULL, // auth
                                              "EUR:15.03"),// ava
    /* Will fail here until all new
     * transfers have not been checked.  I.e.,
     * there is now a 20.04 euro "pending" transfer.  */

    /* pay again logic.  */
    TALER_TESTING_cmd_fakebank_transfer
      ("create-reserve-10",
       "EUR:10.02",
       fakebank_url,
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO,
       USER_LOGIN_NAME,
       USER_LOGIN_PASS,
       EXCHANGE_URL),


    CMD_EXEC_WIREWATCH ("wirewatch-10"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-10",
       "http://localhost:8081/",
       "EUR:10.02", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-10a",
                                       is->exchange,
                                       "create-reserve-10",
                                       "EUR:5",
                                       MHD_HTTP_OK),
    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-10b",
                                       is->exchange,
                                       "create-reserve-10",
                                       "EUR:5",
                                       MHD_HTTP_OK),


    TALER_TESTING_cmd_status ("withdraw-status-10",
                              is->exchange,
                              "create-reserve-10",
                              "EUR:0",
                              MHD_HTTP_OK),


    TALER_TESTING_cmd_proposal
      ("create-proposal-10",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"10\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":10,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:10}\"} ] }",
        NULL),

    TALER_TESTING_cmd_pay ("pay-fail-partial-double-10",
                           merchant_url,
                           is->ctx,
                           MHD_HTTP_FORBIDDEN,
                           "create-proposal-10",
                           "withdraw-coin-10a;withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_pay_again
      ("pay-again-10",
       merchant_url,
       "pay-fail-partial-double-10",
       "withdraw-coin-10a;withdraw-coin-10b",
       "EUR:0.01",
       is->ctx,
       MHD_HTTP_OK),

    CMD_EXEC_AGGREGATOR ("run-aggregator-10"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-9.97-10",
       "http://localhost:8081/",
       "EUR:9.97",
       EXCHANGE_ACCOUNT_NO,
       USER_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty-10"),

    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-11",
                              "EUR:10.02"),

    CMD_EXEC_WIREWATCH ("wirewatch-11"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-11",
       "http://localhost:8081/",
       "EUR:10.02", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-11a",
                                       is->exchange,
                                       "create-reserve-11",
                                       "EUR:5",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-11b",
                                       is->exchange,
                                       "create-reserve-11",
                                       "EUR:5",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_status ("withdraw-status-11",
                              is->exchange,
                              "create-reserve-11",
                              "EUR:0",
                              MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-11",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"11\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":10,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:10}\"} ] }",
        NULL),

    TALER_TESTING_cmd_pay ("pay-fail-partial-double-11",
                           merchant_url,
                           is->ctx,
                           MHD_HTTP_FORBIDDEN,
                           "create-proposal-11",
                           "withdraw-coin-11a;withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-11",
                                 merchant_url,
                                 "pay-fail-partial-double-11",
                                 is->ctx,
                                 MHD_HTTP_OK),

    TALER_TESTING_cmd_pay_abort_refund ("pay-abort-refund-11",
                                        is->exchange,
                                        /* abort reference */
                                        "pay-abort-11",
                                        0,
                                        "EUR:5",
                                        "EUR:0.01",
                                        MHD_HTTP_OK),

    CMD_EXEC_AGGREGATOR ("run-aggregator-11"),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty-11"),

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

int
main (int argc,
      char * const *argv)
{
  unsigned int ret;
  /* These environment variables get in the way... */
  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("test-merchant-api-new",
                    "DEBUG",
                    NULL);

  if (NULL ==
    (fakebank_url = TALER_TESTING_prepare_fakebank (CONFIG_FILE)))
    return 77;

  if (NULL ==
    (merchant_url = TALER_TESTING_prepare_merchant (CONFIG_FILE)))
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

    if (NULL == (merchantd =
        TALER_TESTING_run_merchant (CONFIG_FILE)))
      return 1;

    ret = TALER_TESTING_setup_with_exchange (&run,
                                             NULL,
                                             CONFIG_FILE);

    GNUNET_OS_process_kill (merchantd, SIGTERM); 
    GNUNET_OS_process_wait (merchantd); 
    GNUNET_OS_process_destroy (merchantd); 
    GNUNET_free (merchant_url);

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
