/**
 * This file is part of TALER
 * Copyright (C) 2014-2018 Taler Systems SA
 *
 * TALER is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3, or
 * (at your option) any later version.
 *
 * TALER is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with TALER; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>
 */

/**
 * @file exchange/test_merchant_api_twisted.c
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
 * Twister URL that proxies the merchant.
 */
static char *twister_merchant_url_instance_nonexistent;

/**
 * Twister URL that proxies the merchant.
 */
static char *twister_merchant_url_instance_tor;

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
 * Auditor URL; fixes FTBFS.
 */
static char *auditor_url;

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
 * Account number of the merchant at the bank.
 */
#define MERCHANT_ACCOUNT_NO 3

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
                                       fakebank_url, USER_ACCOUNT_NO, \
                                       EXCHANGE_ACCOUNT_NO, \
                                       USER_LOGIN_NAME, USER_LOGIN_PASS, \
                                       EXCHANGE_URL)

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

  /**** Triggering #5719 ****/
  struct TALER_TESTING_Command bug_5719[] = {

    /**
     * Move money to the exchange's bank account.
     */
    CMD_TRANSFER_TO_EXCHANGE ("5719-create-reserve",
                              "EUR:1.01"),
    /**
     * Make a reserve exist, according to the previous
     * transfer.
     */
    CMD_EXEC_WIREWATCH ("5719-wirewatch"),

    TALER_TESTING_cmd_check_bank_transfer
      ("5719-check-transfer",
      EXCHANGE_URL,
      "EUR:1.01",
      USER_ACCOUNT_NO,
      EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("5719-withdraw",
                                       "5719-create-reserve",
                                       "EUR:1",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_status ("5719-reserve-status",
                              "5719-create-reserve",
                              "EUR:0",
                              MHD_HTTP_OK),
    TALER_TESTING_cmd_proposal
      ("5719-create-proposal",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"5719TRIGGER\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":1,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"triggering bug 5719\",\
                         \"value\":\"{EUR:1}\"} ] }"),

    /**
     * Instruct the Twister to malform the response given by
     * the exchange to the merchant.  This way, the parser will
     * not manage to pass the callback a valid JSON and will
     * instead pass a NULL pointer.  This should trigger the path
     * mentioned in the bug report #5719.
     */TALER_TESTING_cmd_malform_response
      ("5719-malform-exchange-resp",
      PROXY_EXCHANGE_CONFIG_FILE),

    TALER_TESTING_cmd_pay ("5719-deposit",
                           twister_merchant_url,
                           MHD_HTTP_SERVICE_UNAVAILABLE,
                           "5719-create-proposal",
                           "5719-withdraw",
                           "EUR:1",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now
    TALER_TESTING_cmd_end ()
  };


  /**** Covering /check-payment ****/
  struct TALER_TESTING_Command check_payment[] = {

    TALER_TESTING_cmd_proposal
      ("proposal-for-check-payment",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"fail-check-payment-1\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":2,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:3}\"} ] }"),

    /* Need any response code != 200.  */
    TALER_TESTING_cmd_hack_response_code
      ("non-200-response-code",
      PROXY_MERCHANT_CONFIG_FILE,
      MHD_HTTP_MULTIPLE_CHOICES),

    TALER_TESTING_cmd_check_payment
      ("check-payment-fail",
      twister_merchant_url,
      MHD_HTTP_MULTIPLE_CHOICES,
      "proposal-for-check-payment",
      GNUNET_SYSERR),  // any response != 200 gives "syserr"

    TALER_TESTING_cmd_delete_object ("hack-check-payment-0",
                                     PROXY_MERCHANT_CONFIG_FILE,
                                     "taler_pay_uri"),
    TALER_TESTING_cmd_check_payment
      ("check-payment-fail-invalid",
      twister_merchant_url,
      0,
      "proposal-for-check-payment",
      GNUNET_SYSERR),

    TALER_TESTING_cmd_modify_object_dl
      ("paid-true-for-unpaid",
      PROXY_MERCHANT_CONFIG_FILE,
      "paid",
      "true"),

    TALER_TESTING_cmd_check_payment
      ("check-payment-fail-invalid-0",
      twister_merchant_url,
      0,
      "proposal-for-check-payment",
      GNUNET_SYSERR),

    TALER_TESTING_cmd_end ()
  };

  /**** Covering /proposal lib ****/
  struct TALER_TESTING_Command proposal[] = {

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
      MHD_HTTP_BAD_REQUEST,
      /* giving a valid JSON to not make it fail before
       * data reaches the merchant.  */
      "{\"not\": \"used\"}"),

    TALER_TESTING_cmd_hack_response_code
      ("proposal-500",
      PROXY_MERCHANT_CONFIG_FILE,
      MHD_HTTP_INTERNAL_SERVER_ERROR),

    TALER_TESTING_cmd_proposal
      ("create-proposal-1",
      twister_merchant_url,
      /* This status code == 0 is gotten via a 500 Internal Server
       * Error handed to the library.  */
      MHD_HTTP_INTERNAL_SERVER_ERROR,
      /* giving a valid JSON to not make it fail before
       * data reaches the merchant.  */
      "{\"not\": \"used\"}"),

    /**
     * Cause the PUT /proposal callback to be called
     * with a response code == 0.  We achieve this by malforming
     * the response body.
     */TALER_TESTING_cmd_malform_response
      ("malform-proposal",
      PROXY_MERCHANT_CONFIG_FILE),

    TALER_TESTING_cmd_proposal
      ("create-proposal-2",
      twister_merchant_url,
      0,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"1\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":5,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:5}\"} ] }"),
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
      0,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
         \"fulfillment_url\": \"https://example.com/\",\
         \"order_id\":\"2\",\
         \"refund_deadline\":{\"t_ms\":0},\
         \"pay_deadline\":{\"t_ms\":99999999999},\
         \"amount\":\
           {\"currency\":\"EUR\",\
            \"value\":5,\
            \"fraction\":0},\
         \"summary\": \"merchant-lib testcase\",\
         \"products\": [ {\"description\":\"ice cream\",\
                          \"value\":\"{EUR:5}\"} ] }"),
    /**
     * Cause a 404 Not Found response code,
     * due to a non existing merchant instance.
     */
    TALER_TESTING_cmd_proposal
      ("create-proposal-4",
      twister_merchant_url_instance_nonexistent,
      MHD_HTTP_NOT_FOUND,
      "{\"amount\":\"EUR:5\",\
         \"fulfillment_url\": \"https://example.com/\",\
         \"summary\": \"merchant-lib testcase\"}"),

    /* Cause a 404 Not Found from /proposal/lookup,
     * due to a non existing order id being queried.  */
    TALER_TESTING_cmd_proposal_lookup ("lookup-0",
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
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"5\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":5,\
           \"fraction\":0},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:5}\"} ] }"),

    /* Remove expected field.  */
    TALER_TESTING_cmd_delete_object ("remove-contract-terms",
                                     PROXY_MERCHANT_CONFIG_FILE,
                                     "contract_terms"),

    /* lookup!  */
    TALER_TESTING_cmd_proposal_lookup ("lookup-5",
                                       twister_merchant_url,
                                       // expected response code.
                                       0,
                                       "create-proposal-5",
                                       NULL),
    TALER_TESTING_cmd_end ()
  };

  /**** Covering /history lib ****/
  struct TALER_TESTING_Command history[] = {

    /**
     * Changing the response code to a unexpected
     * one.  NOTE: this is unexpected to the *lib*
     * code, that is then expected to trigger some
     * emergency behaviour, like setting the response
     * code to zero before calling the callback.
     */TALER_TESTING_cmd_hack_response_code
      ("twist-history",
      PROXY_MERCHANT_CONFIG_FILE,
      MHD_HTTP_GONE),

    TALER_TESTING_cmd_history ("history-0",
                               twister_merchant_url,
                               0,
                               GNUNET_TIME_UNIT_ZERO_ABS,
                               1, // nresult
                               10, // start
                               10), // nrows
    /**
     * Making the returned response malformed, in order
     * to make the JSON downloader+parser fail and call
     * the lib passing a response code as zero.
     */TALER_TESTING_cmd_malform_response
      ("malform-history",
      PROXY_MERCHANT_CONFIG_FILE),

    TALER_TESTING_cmd_history ("history-1",
                               twister_merchant_url,
                               0, // also works with MHD_HTTP_GONE
                               GNUNET_TIME_UNIT_ZERO_ABS,
                               1, // nresult
                               10, // start
                               10), // nrows


    TALER_TESTING_cmd_end ()
  };

  /**
   * This block tests whether a refund_deadline and/or
   * wire_transfer_deadline very far in the future do NOT
   * result in any wire transfer from the aggregator (#5366).
   */struct TALER_TESTING_Command unaggregation[] = {

    CMD_TRANSFER_TO_EXCHANGE
      ("create-reserve-unaggregation",
      "EUR:5.01"),

    CMD_EXEC_WIREWATCH
      ("wirewatch-unaggregation"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-unaggregation",
      EXCHANGE_URL,
      "EUR:5.01",
      USER_ACCOUNT_NO,
      EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty
      ("check_bank_unaggregated-a"),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-unaggregation",
      "create-reserve-unaggregation",
      "EUR:5",
      MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-unaggregation",
      /* Need a fresh instance in order to associate this
       * proposal with a fresh h_wire;  this way, this proposal
       * won't get hooked by the aggregator gathering same-H_wire'd
       * transactions.  */
      twister_merchant_url_instance_tor,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"refund_deadline\":{\"t_ms\":2},\
        \"pay_deadline\":{\"t_ms\":1},\
        \"wire_transfer_deadline\":{\"t_ms\":2366841600},\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":5,\
           \"fraction\":0},\
        \"summary\": \"unaggregated product\",\
        \"fulfillment_url\": \"https://example.com/\",\
        \"products\": [ {\"description\":\"unaggregated cream\",\
                         \"value\":\"{EUR:5}\"} ] }"),

    TALER_TESTING_cmd_pay
      ("pay-unaggregation",
      twister_merchant_url_instance_tor,
      MHD_HTTP_OK,
      "create-proposal-unaggregation",
      "withdraw-coin-unaggregation",
      "EUR:5",  // amount + fee
      "EUR:4.99",  // amount - fee
      "EUR:0.01"),  // refund fee

    CMD_EXEC_AGGREGATOR
      ("aggregation-attempt"),

    /* Make sure NO aggregation took place.  */
    TALER_TESTING_cmd_check_bank_empty
      ("check_bank_unaggregated-b"),

    TALER_TESTING_cmd_end ()
  };

  /***** Test #5383 *****/
  struct TALER_TESTING_Command track_5383[] = {
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-5383",
                              "EUR:2.02"),
    CMD_EXEC_WIREWATCH ("wirewatch-5383"),
    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-5383",
      EXCHANGE_URL,
      "EUR:2.02",
      USER_ACCOUNT_NO,
      EXCHANGE_ACCOUNT_NO),
    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-5383a",
      "create-reserve-5383",
      "EUR:1",
      MHD_HTTP_OK),
    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-5383b",
      "create-reserve-5383",
      "EUR:1",
      MHD_HTTP_OK),
    TALER_TESTING_cmd_proposal
      ("create-proposal-5383",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"5383\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":2,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:2}\"} ] }"),
    TALER_TESTING_cmd_pay ("deposit-simple-5383",
                           twister_merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-5383",
                           "withdraw-coin-5383a;" \
                           "withdraw-coin-5383b",
                           "EUR:2",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now
    CMD_EXEC_AGGREGATOR ("run-aggregator-5383"),
    TALER_TESTING_cmd_check_bank_transfer
      ("check_aggregation_transfer-5383",
      twister_exchange_url,  /* has the 8888-port thing.  */
      /* paid,         1.97 =
         brutto        2.00 -
         deposit fee   0.01 * 2 -
         wire fee      0.01
      */"EUR:1.97",
      EXCHANGE_ACCOUNT_NO,
      MERCHANT_ACCOUNT_NO),
    TALER_TESTING_cmd_modify_object_dl
      ("hack-5383",
      PROXY_EXCHANGE_CONFIG_FILE,
      "total",
      "EUR:0.98"),
    TALER_TESTING_cmd_merchant_track_transfer
      ("track-5383",
      twister_merchant_url,
      MHD_HTTP_FAILED_DEPENDENCY,
      "check_aggregation_transfer-5383"),

    TALER_TESTING_cmd_end ()
  };


  /***** Test transactions tracking *****/
  struct TALER_TESTING_Command track[] = {

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
      EXCHANGE_URL,
      "EUR:2.02", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty
      ("track_chunk_check_empty-a"),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-1",
                                       "create-reserve-1",
                                       "EUR:1",
                                       MHD_HTTP_OK),
    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-2",
                                       "create-reserve-1",
                                       "EUR:1",
                                       MHD_HTTP_OK),
    TALER_TESTING_cmd_status ("withdraw-status",
                              "create-reserve-1",
                              "EUR:0",
                              MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-6",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"11\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":2,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:3}\"} ] }"),

    TALER_TESTING_cmd_check_payment ("check-payment-1",
                                     twister_merchant_url,
                                     MHD_HTTP_OK,
                                     "create-proposal-6",
                                     GNUNET_NO),

    TALER_TESTING_cmd_pay ("deposit-simple",
                           twister_merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-6",
                           "withdraw-coin-1;" \
                           "withdraw-coin-2",
                           "EUR:2",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    TALER_TESTING_cmd_check_payment ("check-payment-2",
                                     twister_merchant_url,
                                     MHD_HTTP_OK,
                                     "create-proposal-6",
                                     GNUNET_YES),
    CMD_EXEC_AGGREGATOR ("run-aggregator"),
    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-1",
      twister_exchange_url,  /* has the 8888-port thing.  */
      /* paid,         1.97 =
         brutto        2.00 -
         deposit fee   0.01 * 2 -
         wire fee      0.01
      */"EUR:1.97",
      EXCHANGE_ACCOUNT_NO,
      MERCHANT_ACCOUNT_NO),

    /**
     * Fake total to include only one coin.  Math: each 1-EUR
     * coin contributes 0.99 to the final wire transfer.  The
     * wire transfer itself drains 0.01-EUR from the total amount.
     * Therefore, wire transferring 1-EUR coin results in a net
     * of: 0.99 - 0.01 = 0.98. *//**
     * NOTE: the following two hacks aim at modifying the
     * communication between the merchant and the exchange.
     * In particular, they are supposed to modify the call
     * to /track/transfer issued from the merchant to the
     * exchange that happens _before_ the call to /track/transaction
     * issued below by the test case (to the merchant backend.) */TALER_TESTING_cmd_modify_object_dl
      ("hack-0",
      PROXY_EXCHANGE_CONFIG_FILE,
      "total",
      "EUR:0.98"),
    TALER_TESTING_cmd_delete_object
      ("hack-1",
      PROXY_EXCHANGE_CONFIG_FILE,
      "deposits.0"),
    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-1",
      twister_merchant_url,
      MHD_HTTP_FAILED_DEPENDENCY,
      "deposit-simple"),

    TALER_TESTING_cmd_end ()
  };


  /****** Covering /pay *******/
  struct TALER_TESTING_Command pay[] = {

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
      EXCHANGE_URL,
      "EUR:1.01", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-abort-1",
                                       "create-reserve-abort-1",
                                       "EUR:1",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_status ("withdraw-status-abort-1",
                              "create-reserve-abort-1",
                              "EUR:0",
                              MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-abort-1",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"abort-one\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":3,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\":\"ice cream\",\
                         \"value\":\"{EUR:3}\"} ] }"),

    /* Will only pay _half_ the supposed price,
     * so we'll then have the right to abort.  */
    TALER_TESTING_cmd_pay ("deposit-simple-for-abort",
                           twister_merchant_url,
                           MHD_HTTP_NOT_ACCEPTABLE,
                           "create-proposal-abort-1",
                           "withdraw-coin-abort-1",
                           "EUR:1",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    TALER_TESTING_cmd_delete_object ("hack-abort-1",
                                     PROXY_MERCHANT_CONFIG_FILE,
                                     "merchant_pub"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-1",
                                 twister_merchant_url,
                                 "deposit-simple-for-abort",
                                 0),

    TALER_TESTING_cmd_delete_object
      ("hack-abort-2",
      PROXY_MERCHANT_CONFIG_FILE,
      "refund_permissions.0.rtransaction_id"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-2",
                                 twister_merchant_url,
                                 "deposit-simple-for-abort",
                                 0),

    TALER_TESTING_cmd_modify_object_dl
      ("hack-abort-3",
      PROXY_MERCHANT_CONFIG_FILE,
      "refund_permissions.0.coin_pub",
      /* dummy coin.  */
      "8YX10E41ZWHX0X2RK4XFAXB2D3M05M1HNG14ZFZZB8M7SA4QCKCG"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-3",
                                 twister_merchant_url,
                                 "deposit-simple-for-abort",
                                 0),

    TALER_TESTING_cmd_flip_download
      ("hack-abort-4",
      PROXY_MERCHANT_CONFIG_FILE,
      "refund_permissions.0.merchant_sig"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-4",
                                 twister_merchant_url,
                                 "deposit-simple-for-abort",
                                 0),
    /* just malforming the response.  */
    TALER_TESTING_cmd_malform_response
      ("malform-abortion",
      PROXY_MERCHANT_CONFIG_FILE),

    TALER_TESTING_cmd_pay_abort ("pay-abort-5",
                                 twister_merchant_url,
                                 "deposit-simple-for-abort",
                                 0),

    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-double-spend",
                              "EUR:1.01"),

    CMD_EXEC_WIREWATCH ("wirewatch-double-spend"),

    TALER_TESTING_cmd_proposal
      ("create-proposal-double-spend",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"DS-1\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":1,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\": \"will succeed\"}] }"),

    TALER_TESTING_cmd_proposal
      ("create-proposal-double-spend-1",
      twister_merchant_url,
      MHD_HTTP_OK,
      "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"DS-2\",\
        \"refund_deadline\":{\"t_ms\":0},\
        \"pay_deadline\":{\"t_ms\":99999999999},\
        \"fulfillment_url\": \"https://example.com/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":1,\
           \"fraction\":0},\
        \"summary\": \"merchant-lib testcase\",\
        \"products\": [ {\"description\": \"will fail\"}] }"),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-double-spend",
      "create-reserve-double-spend",
      "EUR:1",
      MHD_HTTP_OK),

    TALER_TESTING_cmd_pay ("deposit-simple-ok",
                           twister_merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-double-spend",
                           "withdraw-coin-double-spend",
                           "EUR:1",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    TALER_TESTING_cmd_flip_download
      ("hack-coin-history",
      PROXY_MERCHANT_CONFIG_FILE,
      "history.0.coin_sig"),

    /* Coin history check will fail,
     * due to coin's bad signature.  */
    TALER_TESTING_cmd_pay ("deposit-simple-fail",
                           twister_merchant_url,
                           0,
                           "create-proposal-double-spend-1",
                           "withdraw-coin-double-spend",
                           "EUR:1",
                           "EUR:1.99", // no sense now
                           "EUR:0.01"), // no sense now

    /* max uint64 number: 9223372036854775807; try to overflow! */

    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command commands[] = {

    TALER_TESTING_cmd_batch ("check-payment",
                             check_payment),

    TALER_TESTING_cmd_batch ("proposal",
                             proposal),

    TALER_TESTING_cmd_batch ("history",
                             history),

    TALER_TESTING_cmd_batch ("unaggregation",
                             unaggregation),

    TALER_TESTING_cmd_batch ("track",
                             track),

    TALER_TESTING_cmd_batch ("track-5383",
                             track_5383),

    TALER_TESTING_cmd_batch ("pay",
                             pay),

    TALER_TESTING_cmd_batch ("bug-5719",
                             bug_5719),
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
      char *const *argv)
{
  unsigned int ret;
  /* These environment variables get in the way... */
  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("test-merchant-api-twisted",
                    "DEBUG", NULL);

  if (NULL == (fakebank_url = TALER_TESTING_prepare_fakebank
                                (CONFIG_FILE,
                                "account-exchange")))
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

  twister_merchant_url_instance_nonexistent = TALER_url_join (
    twister_merchant_url, "instances/foo/", NULL);
  twister_merchant_url_instance_tor = TALER_url_join (
    twister_merchant_url, "instances/tor/", NULL);

  TALER_TESTING_cleanup_files (CONFIG_FILE);

  switch (TALER_TESTING_prepare_exchange (CONFIG_FILE,
                                          &auditor_url,
                                          &exchange_url))
  {
  case GNUNET_SYSERR:
    GNUNET_break (0);
    return 1;
  case GNUNET_NO:
    return 77;

  case GNUNET_OK:

    if (NULL == (merchantd = TALER_TESTING_run_merchant
                               (CONFIG_FILE, merchant_url)))
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


/* end of test_merchant_api_twisted.c */
