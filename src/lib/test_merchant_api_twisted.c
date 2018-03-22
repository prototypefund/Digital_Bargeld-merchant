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
 * Configuration file for the proxy between merchant and
 * exchange.  Not used directly here in the code (instead
 * used in the merchant config), but kept around for consistency.
 */
#define PROXY_EXCHANGE_CONFIG_FILE \
  "test_merchant_api_proxy_exchange.conf"

/**
 * Configuration file for the proxy between "lib" and merchant.
 */
#define PROXY_MERCHANT_CONFIG_FILE \
  "test_merchant_api_proxy_merchant.conf"

/**
 * Exchange base URL.  Could also be taken from config.
 */
#define EXCHANGE_URL "http://localhost:8081/"

/**
 * Twister URL that proxies the exchange.
 */
static char *twister_exchange_url;

/**
 * Twister URL that proxies the merchant.
 */
static char *twister_merchant_url;

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
 * Twister process that proxies the exchange.
 */
static struct GNUNET_OS_Process *twisterexchanged;

/**
 * Twister process that proxies the merchant.
 */
static struct GNUNET_OS_Process *twistermerchantd;

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


  /**** Covering /proposal lib ****/

  /**
   * Make the merchant return a 400 Bad Request response
   * due to uploaded body malformation.
   */
  TALER_TESTING_cmd_malform_request
    ("malform-order",
     PROXY_MERCHANT_CONFIG_FILE),

  TALER_TESTING_cmd_proposal
    ("create-proposal-0",
     twister_merchant_url,
     is->ctx,
     MHD_HTTP_BAD_REQUEST,
     /* giving a valid JSON to not make it fail before
      * data reaches the merchant.  */
     "{\"not\": \"used\"}",
     NULL),

    TALER_TESTING_cmd_hack_response_code
      ("proposal-500",
       PROXY_MERCHANT_CONFIG_FILE,
       MHD_HTTP_INTERNAL_SERVER_ERROR),

  TALER_TESTING_cmd_proposal
    ("create-proposal-1",
     twister_merchant_url,
     is->ctx,
     /* This status code == 0 is gotten via a 500 Internal Server
      * Error handed to the library.  */
     MHD_HTTP_INTERNAL_SERVER_ERROR,
     /* giving a valid JSON to not make it fail before
      * data reaches the merchant.  */
     "{\"not\": \"used\"}",
     NULL),

  /**
   * Cause the PUT /proposal callback to be called
   * with a response code == 0.  We achieve this by malforming
   * the response body.
   */

    TALER_TESTING_cmd_malform_response
      ("malform-proposal",
       PROXY_MERCHANT_CONFIG_FILE),

    TALER_TESTING_cmd_proposal
      ("create-proposal-2",
       twister_merchant_url,
       is->ctx,
       0,
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
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:5}\"} ] }",
        NULL),


    /**
     * Cause proposal to be invalid: this is achieved
     * by deleting the "order_id" field of it.
     */
    TALER_TESTING_cmd_delete_object ("remove-order-id",
                                     PROXY_MERCHANT_CONFIG_FILE,
                                     "order_id"),
    TALER_TESTING_cmd_proposal
      ("create-proposal-3",
       twister_merchant_url,
       is->ctx,
       0,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
         \"fulfillment_url\": \"https://example.com/\",\
         \"order_id\":\"2\",\
         \"refund_deadline\":\"\\/Date(0)\\/\",\
         \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
         \"amount\":\
           {\"currency\":\"EUR\",\
            \"value\":5,\
            \"fraction\":0},\
         \"summary\": \"merchant-lib testcase\",\
         \"products\": [ {\"description\":\"ice cream\",\
                          \"value\":\"{EUR:5}\"} ] }",
         NULL),
    /**
     * Cause a 404 Not Found response code,
     * due to a non existing merchant instance.
     */
    TALER_TESTING_cmd_proposal
      ("create-proposal-4",
       twister_merchant_url,
       is->ctx,
       MHD_HTTP_NOT_FOUND,
       "{\"amount\":\"EUR:5\",\
         \"fulfillment_url\": \"https://example.com/\",\
         \"summary\": \"merchant-lib testcase\"}",
       "non-existent-instance"),

    /* Cause a 404 Not Found from /proposal/lookup,
     * due to a non existing order id being queried.  */
    TALER_TESTING_cmd_proposal_lookup ("lookup-0",
                                       is->ctx,
                                       twister_merchant_url,
                                       MHD_HTTP_NOT_FOUND,
                                       NULL,
                                       "does-not-exist"),
    /* Cause a unparsable response to be returned.  */
    TALER_TESTING_cmd_malform_response
      ("malform-proposal-lookup",
       PROXY_MERCHANT_CONFIG_FILE),
    /* To be short, we'll make a _error_ response to be
     * unparsable.  */
    TALER_TESTING_cmd_proposal_lookup ("lookup-1",
                                       is->ctx,
                                       twister_merchant_url,
                                       0, // response code.
                                       NULL,
                                       "does-not-exist"),

    /* Generating a proposal-lookup response which doesn't pass
     * validation, by removing a field that is expected by the
     * library.  The library will call the callback with a status
     * code of 0.  */

    /* First step is to create a _valid_ proposal, so that
     * we can lookup for it later.  */
    TALER_TESTING_cmd_proposal
      ("create-proposal-5",
       twister_merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"5\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":5,\
           \"fraction\":0},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:5}\"} ] }",
        NULL),

    /* Remove expected field.  */
    TALER_TESTING_cmd_delete_object ("remove-contract-terms",
                                     PROXY_MERCHANT_CONFIG_FILE,
                                     "contract_terms"),

    /* lookup!  */
    TALER_TESTING_cmd_proposal_lookup ("lookup-5",
                                       is->ctx,
                                       twister_merchant_url,
                                       // expected response code.
                                       0,
                                       "create-proposal-5",
                                       NULL),
    /**** Covering /history lib ****/

    /**
     * Changing the response code to a unexpected
     * one.  NOTE: this is unexpected to the *lib*
     * code, that is then expected to trigger some
     * emergency behaviour, like setting the response
     * code to zero before calling the callback.
     */
    TALER_TESTING_cmd_hack_response_code
      ("twist-history",
       PROXY_MERCHANT_CONFIG_FILE,
       MHD_HTTP_GONE),

    TALER_TESTING_cmd_history ("history-0",
                               twister_merchant_url,
                               is->ctx,
                               0,
                               GNUNET_TIME_UNIT_ZERO_ABS,
                               1, // nresult
                               10, // start
                               10), // nrows
    /**
     * Making the returned response malformed, in order
     * to make the JSON downloader+parser fail and call
     * the lib passing a response code as zero.
     */
    TALER_TESTING_cmd_malform_response
      ("malform-history",
       PROXY_MERCHANT_CONFIG_FILE),

    TALER_TESTING_cmd_history ("history-1",
                               twister_merchant_url,
                               is->ctx,
                               0, // also works with MHD_HTTP_GONE
                               GNUNET_TIME_UNIT_ZERO_ABS,
                               1, // nresult
                               10, // start
                               10), // nrows


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
      ("create-proposal-6",
       twister_merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"11\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":2,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:3}\"} ] }",
        NULL),

    TALER_TESTING_cmd_check_payment ("check-payment-1",
                                     twister_merchant_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "create-proposal-6",
                                     GNUNET_NO),

    TALER_TESTING_cmd_pay ("deposit-simple",
                           twister_merchant_url,
                           is->ctx,
                           MHD_HTTP_OK,
                           "create-proposal-6",
                           "withdraw-coin-1;" \
                           "withdraw-coin-2",
                           "EUR:2",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    TALER_TESTING_cmd_check_payment ("check-payment-2",
                                     twister_merchant_url,
                                     is->ctx,
                                     MHD_HTTP_OK,
                                     "create-proposal-6",
                                     GNUNET_YES),

    CMD_EXEC_AGGREGATOR ("run-aggregator"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-1",
       twister_exchange_url,
       /* paid,         1.97 =
          brutto        2.00 -
          deposit fee   0.01 * 2 -
          wire fee      0.01
       */
       "EUR:1.97", 2, 62),

    /* Should instead change the response body somehow! */
    TALER_TESTING_cmd_modify_object ("hack-0",
                                     PROXY_EXCHANGE_CONFIG_FILE,
                                     "total",
                                     "EUR:0.98"),

    TALER_TESTING_cmd_delete_object ("hack-1",
                                     PROXY_EXCHANGE_CONFIG_FILE,
                                     "deposits.0"),

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-1",
       twister_merchant_url,
       is->ctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       "check_bank_transfer-1",
       "deposit-simple",
       "EUR:0.01"), // ignored

    /****** Covering /pay *******/

    /**
     * Move money to the exchange's bank account.
     */
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-abort-1",
                              "EUR:1.01"),

    /**
     * Make a reserve exist, according to the previous
     * transfer.
     */
    CMD_EXEC_WIREWATCH ("wirewatch-abort-1"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-abort-1",
       "http://localhost:8081/",
       "EUR:1.01", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-abort-1",
                                       is->exchange,
                                       "create-reserve-abort-1",
                                       "EUR:1",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_status ("withdraw-status-abort-1",
                              is->exchange,
                              "create-reserve-abort-1",
                              "EUR:0",
                              MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-abort-1",
       twister_merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"abort-one\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(99999999999)\\/\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":3,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:3}\"} ] }",
        NULL),

    TALER_TESTING_cmd_pay ("deposit-simple-for-abort",
                           twister_merchant_url,
                           is->ctx,
                           0,
                           "create-proposal-abort-1",
                           "withdraw-coin-abort-1",
                           "EUR:1",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    TALER_TESTING_cmd_delete_object ("hack-abort",
                                     PROXY_MERCHANT_CONFIG_FILE,
                                     "merchant_pub"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-1",
                                 twister_merchant_url,
                                 "deposit-simple-for-abort",
                                 is->ctx,
                                 MHD_HTTP_OK),
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
  GNUNET_OS_process_kill (process, SIGINT);
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

  if (NULL == (twister_exchange_url = TALER_TESTING_prepare_twister
      (PROXY_EXCHANGE_CONFIG_FILE)))
    return 77;

  if (NULL == (twister_merchant_url = TALER_TESTING_prepare_twister
      (PROXY_MERCHANT_CONFIG_FILE)))
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
      // 1 is fine; after all this is merchant test cases.
      return 1;

    if (NULL == (twisterexchanged = TALER_TESTING_run_twister
        (PROXY_EXCHANGE_CONFIG_FILE)))
      return 77;

    if (NULL == (twistermerchantd = TALER_TESTING_run_twister
        (PROXY_MERCHANT_CONFIG_FILE)))
      return 77;

    /* Run the exchange and schedule 'run()' */
    ret = TALER_TESTING_setup_with_exchange (&run, NULL,
                                             CONFIG_FILE);
    purge_process (merchantd);
    purge_process (twisterexchanged);
    purge_process (twistermerchantd);
    GNUNET_free (fakebank_url);
    GNUNET_free (merchant_url);
    GNUNET_free (twister_exchange_url);
    GNUNET_free (twister_merchant_url);

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
