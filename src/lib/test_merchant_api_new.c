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
#include <taler/taler_error_codes.h>
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
 * URL of the fakebank.
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
 * Auditor base URL; only used to fix FTBFS.
 */
static char *auditor_url;

/**
 * Account number of the exchange at the bank.
 */
#define EXCHANGE_ACCOUNT_NO 2

/**
 * Account number of some user.
 */
#define USER_ACCOUNT_NO 62

/**
 * Account number used by the merchant
 */
#define MERCHANT_ACCOUNT_NO 3

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
  struct TALER_TESTING_Command pay[] = {
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
       EXCHANGE_URL,
       "EUR:10.02",
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-1",
       is->exchange,
       "create-reserve-1",
       "EUR:5",
       MHD_HTTP_OK),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-2",
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
                                     MHD_HTTP_OK,
                                     "create-proposal-1",
                                     GNUNET_NO),
    TALER_TESTING_cmd_pay ("deposit-simple",
                           merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-1",
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_check_payment ("check-payment-2",
                                     merchant_url,
                                     MHD_HTTP_OK,
                                     "create-proposal-1",
                                     GNUNET_YES),

    TALER_TESTING_cmd_pay_abort ("pay-abort-2",
                                 merchant_url,
                                 "deposit-simple",
                                 MHD_HTTP_FORBIDDEN),

    TALER_TESTING_cmd_pay ("replay-simple",
                           merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-1",
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_check_bank_empty
      ("check_bank_empty-1"),

    CMD_EXEC_AGGREGATOR ("run-aggregator"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-498c",
       EXCHANGE_URL,
       "EUR:4.98",
       EXCHANGE_ACCOUNT_NO,
       MERCHANT_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty-2"),

    TALER_TESTING_cmd_end ()
  };


  struct TALER_TESTING_Command double_spending[] = {

    TALER_TESTING_cmd_proposal
      ("create-proposal-2",
       merchant_url,
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

    TALER_TESTING_cmd_proposal_lookup ("fetch-proposal-2",
                                       merchant_url,
                                       MHD_HTTP_OK,
                                       "create-proposal-2",
                                       NULL),

    TALER_TESTING_cmd_pay ("deposit-double-2",
                           merchant_url,
                           MHD_HTTP_FORBIDDEN,
                           "create-proposal-2",
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_history ("history-0",
                               merchant_url,
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
                               -10), // nrows


    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command track[] = {

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "deposit-simple"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-1",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-again",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c"),

    TALER_TESTING_cmd_fakebank_transfer
      ("create-reserve-2",
       "EUR:1",
       fakebank_url,
       USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO,
       "user62",
       "pass62",
       EXCHANGE_URL),

    TALER_TESTING_cmd_fakebank_transfer_with_ref
      ("create-reserve-2b",
       "EUR:4.01",
       fakebank_url,
       USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO,
       "user62",
       "pass62",
       "create-reserve-2",
       EXCHANGE_URL),

    CMD_EXEC_WIREWATCH ("wirewatch-2"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-2a",
       EXCHANGE_URL,
       "EUR:1",
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-2b",
       EXCHANGE_URL,
       "EUR:4.01",
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount ("withdraw-coin-2",
                                       is->exchange,
                                       "create-reserve-2",
                                       "EUR:5",
                                       MHD_HTTP_OK),

    TALER_TESTING_cmd_pay ("deposit-simple-2",
                           merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-2",
                           "withdraw-coin-2",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    CMD_EXEC_AGGREGATOR ("run-aggregator-2"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-498c-2",
       EXCHANGE_URL,
       "EUR:4.98",
       EXCHANGE_ACCOUNT_NO,
       MERCHANT_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-2",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c-2"),

    TALER_TESTING_cmd_merchant_track_transfer
      ("track-transfer-2-again",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "check_bank_transfer-498c-2"),

    TALER_TESTING_cmd_merchant_track_transaction
      ("track-transaction-2",
       merchant_url,
       is->ctx,
       MHD_HTTP_OK,
       "deposit-simple-2"),

    TALER_TESTING_cmd_history ("history-1",
                               merchant_url,
                               MHD_HTTP_OK,
                               GNUNET_TIME_UNIT_ZERO_ABS,
      /**
       * Now we expect BOTH contracts (create-proposal-{1,2})
       * to be included in /history response, because
       * create-proposal-2 has now been correctly paid.
       */
                               2,
                               10,
                               -10),
    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command refund[] = {

    TALER_TESTING_cmd_refund_increase
      ("refund-increase-1",
       merchant_url,
       "refund test",
       "1",
       "EUR:0.1",
       "EUR:0.01",
       MHD_HTTP_OK),

    /* Ordinary refund.  */
    TALER_TESTING_cmd_refund_lookup ("refund-lookup-1",
                                     merchant_url,
                                     "refund-increase-1",
                                     "deposit-simple",
                                     "1",
                                     MHD_HTTP_OK),

    /* Trying to pick up refund from non existent proposal.  */
    TALER_TESTING_cmd_refund_lookup ("refund-lookup-non-existent",
                                     merchant_url,
                                     "refund-increase-1",
                                     "deposit-simple",
                                     "non-existend-id",
                                     MHD_HTTP_NOT_FOUND),

    /* Test /refund on a contract that was never paid.  */

    TALER_TESTING_cmd_proposal
      ("create-proposal-not-to-be-paid",
       merchant_url,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"1-unpaid\",\
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

    /* Try to increase a non paid proposal.  */
    TALER_TESTING_cmd_refund_increase
      ("refund-increase-unpaid-proposal",
       merchant_url,
       "refund test",
       "1-unpaid",
       "EUR:0.1",
       "EUR:0.01",
       MHD_HTTP_BAD_REQUEST),

    /* Try to increase a non existent proposal.  */
    TALER_TESTING_cmd_refund_increase
      ("refund-increase-unpaid-proposal",
       merchant_url,
       "refund test",
       "non-existent-id",
       "EUR:0.1",
       "EUR:0.01",
       MHD_HTTP_NOT_FOUND),

    /**
     * The following block will (1) create a new
     * reserve, then (2) a proposal, then (3) pay for
     * it, and finally (4) attempt to pick up a refund 
     * from it without any increasing taking place
     * in the first place.
     **/
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-unincreased-refund",
                              "EUR:5.01"),

    CMD_EXEC_WIREWATCH ("wirewatch-unincreased-refund"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-unincreased-refund",
       EXCHANGE_URL,
       "EUR:5.01",
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_withdraw_amount
      ("withdraw-coin-unincreased-refund",
       is->exchange,
       "create-reserve-unincreased-refund",
       "EUR:5",
       MHD_HTTP_OK),

    TALER_TESTING_cmd_proposal
      ("create-proposal-unincreased-refund",
       merchant_url,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"unincreased-proposal\",\
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

    TALER_TESTING_cmd_pay ("pay-unincreased-proposal",
                           merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-unincreased-refund",
                           "withdraw-coin-unincreased-refund",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    CMD_EXEC_AGGREGATOR ("run-aggregator-unincreased-refund"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-unincreased-refund",
       EXCHANGE_URL,
       "EUR:4.98",
       EXCHANGE_ACCOUNT_NO,
       MERCHANT_ACCOUNT_NO),

    /* Actually try to pick up the refund from the
     * "unincreased proposal".  */
    TALER_TESTING_cmd_refund_lookup_with_amount
      ("refund-lookup-unincreased",
       merchant_url,
       NULL,
       "pay-unincreased-proposal",
       "unincreased-proposal",
       MHD_HTTP_OK,
       /* If a lookup is attempted on an unincreased proposal,
        * the backend will simply respond with a empty refunded
        * coin "set", but the HTTP response code is 200 OK.  */
       "EUR:0"),

    TALER_TESTING_cmd_end ()
  };


  struct TALER_TESTING_Command tip[] = {

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

    CMD_EXEC_WIREWATCH ("wirewatch-3"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-tip-1",
       EXCHANGE_URL,
       "EUR:20.04", USER_ACCOUNT_NO, EXCHANGE_ACCOUNT_NO),

    TALER_TESTING_cmd_tip_authorize ("authorize-tip-1",
                                     merchant_url,
                                     exchange_url,
                                     MHD_HTTP_OK,
                                     "tip",
                                     "tip 1",
                                     "EUR:5.01"),

    TALER_TESTING_cmd_tip_authorize ("authorize-tip-2",
                                     merchant_url,
                                     exchange_url,
                                     MHD_HTTP_OK,
                                     "tip",
                                     "tip 2",
                                     "EUR:5.01"),

    /* This command tests the authorization of tip
     * against a reserve that does not exist.  This is
     * implemented by passing a "tip instance" that
     * specifies a reserve key that was never used to
     * actually create a reserve.  */
    TALER_TESTING_cmd_tip_authorize_with_ec
      ("authorize-tip-null",
       merchant_url,
       exchange_url,
       MHD_HTTP_NOT_FOUND,
       "nulltip",
       "tip 2",
       "EUR:5.01",
       TALER_EC_TIP_AUTHORIZE_RESERVE_UNKNOWN),

    TALER_TESTING_cmd_tip_query ("query-tip-1",
                                 merchant_url,
                                 MHD_HTTP_OK,
                                 "tip"),

    TALER_TESTING_cmd_tip_query_with_amounts ("query-tip-2",
                                              merchant_url,
                                              MHD_HTTP_OK,
                                              "tip",
                                              "EUR:0.0", // picked
                                              "EUR:10.02", // auth
                                              "EUR:20.04"),// ava

    TALER_TESTING_cmd_tip_pickup ("pickup-tip-1",
                                  merchant_url,
                                  MHD_HTTP_OK,
                                  "authorize-tip-1",
                                  pickup_amounts_1),

    TALER_TESTING_cmd_tip_query_with_amounts ("query-tip-3",
                                              merchant_url,
                                              MHD_HTTP_OK,
                                              "tip",
                                              "EUR:5.01", // picked
                                              NULL, // auth
                                              "EUR:15.03"),// ava

    TALER_TESTING_cmd_tip_pickup ("pickup-tip-2",
                                  merchant_url,
                                  MHD_HTTP_OK,
                                  "authorize-tip-2",
                                  pickup_amounts_1),

    TALER_TESTING_cmd_tip_query_with_amounts ("query-tip-4",
                                              merchant_url,
                                              MHD_HTTP_OK,
                                              "tip",
                                              "EUR:10.02", // pick
                                              "EUR:10.02", // auth
                                              "EUR:10.02"), // ava

    TALER_TESTING_cmd_fakebank_transfer_with_instance
      ("create-reserve-insufficient-funds",
       "EUR:1.01",
       fakebank_url,
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO,
       USER_LOGIN_NAME,
       USER_LOGIN_PASS,
       "dtip",
       EXCHANGE_URL,
       CONFIG_FILE),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-insufficient-tip-funds",
       EXCHANGE_URL,
       "EUR:1.01",
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO),

    CMD_EXEC_WIREWATCH
      ("wirewatch-insufficient-tip-funds"),

    TALER_TESTING_cmd_tip_authorize_with_ec
      ("authorize-tip-3-insufficient-funds",
       merchant_url,
       exchange_url,
       MHD_HTTP_PRECONDITION_FAILED,
       "dtip",
       "tip 3",
       "EUR:2.02",
       TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS),

    TALER_TESTING_cmd_tip_authorize_with_ec
      ("authorize-tip-4-unknown-instance",
       merchant_url,
       exchange_url,
       MHD_HTTP_NOT_FOUND,
       "unknown",
       "tip 4",
       "EUR:5.01",
       TALER_EC_TIP_AUTHORIZE_INSTANCE_UNKNOWN),

    TALER_TESTING_cmd_tip_authorize_with_ec
      ("authorize-tip-5-notip-instance",
       merchant_url,
       exchange_url,
       MHD_HTTP_NOT_FOUND,
       "default",
       "tip 5",
       "EUR:5.01",
       TALER_EC_TIP_AUTHORIZE_INSTANCE_DOES_NOT_TIP),

    TALER_TESTING_cmd_tip_pickup_with_ec
      ("pickup-tip-3-too-much",
       merchant_url,
       MHD_HTTP_CONFLICT,
       "authorize-tip-1",
       pickup_amounts_1,
       TALER_EC_TIP_PICKUP_NO_FUNDS),

    TALER_TESTING_cmd_tip_authorize_fake
      ("fake-tip-authorization"),

    TALER_TESTING_cmd_tip_pickup_with_ec
      ("pickup-non-existent-id",
       merchant_url,
       MHD_HTTP_NOT_FOUND,
       "fake-tip-authorization",
       pickup_amounts_1,
       TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN),

    TALER_TESTING_cmd_proposal
      ("create-proposal-tip-1",
       merchant_url,
       MHD_HTTP_OK,
       "{\"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"1-tip\",\
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

    TALER_TESTING_cmd_pay ("deposit-tip-simple",
                           merchant_url,
                           MHD_HTTP_OK,
                           "create-proposal-tip-1",
                           "pickup-tip-1",
                           "EUR:5", // amount + fee
                           "EUR:4.99", // amount - fee
                           "EUR:0.01"), // refund fee

    CMD_EXEC_AGGREGATOR ("aggregator-tip-1"),
    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-tip-498c",
       EXCHANGE_URL,
       "EUR:4.98",
       EXCHANGE_ACCOUNT_NO,
       MERCHANT_ACCOUNT_NO),
    TALER_TESTING_cmd_check_bank_empty
    ("check_bank_empty-at-tips"),

    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command pay_again[] = {

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
       EXCHANGE_URL,
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
       MHD_HTTP_OK),

    CMD_EXEC_AGGREGATOR ("run-aggregator-10"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-9.97-10",
       EXCHANGE_URL,
       "EUR:9.97",
       EXCHANGE_ACCOUNT_NO,
       MERCHANT_ACCOUNT_NO),

    TALER_TESTING_cmd_check_bank_empty ("check_bank_empty-10"),

    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command pay_abort[] = {
    CMD_TRANSFER_TO_EXCHANGE ("create-reserve-11",
                              "EUR:10.02"),

    CMD_EXEC_WIREWATCH ("wirewatch-11"),

    TALER_TESTING_cmd_check_bank_transfer
      ("check_bank_transfer-11",
       EXCHANGE_URL,
       "EUR:10.02",
       USER_ACCOUNT_NO,
       EXCHANGE_ACCOUNT_NO),

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

    TALER_TESTING_cmd_pay ("pay-fail-partial-double-11-good",
                           merchant_url,
                           MHD_HTTP_NOT_ACCEPTABLE,
                           "create-proposal-11",
                           "withdraw-coin-11a",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),
    TALER_TESTING_cmd_pay ("pay-fail-partial-double-11-bad",
                           merchant_url,
                           MHD_HTTP_FORBIDDEN,
                           "create-proposal-11",
                           "withdraw-coin-1",
                           "EUR:5",
                           "EUR:4.99",
                           "EUR:0.01"),

    TALER_TESTING_cmd_pay_abort ("pay-abort-11",
                                 merchant_url,
                                 "pay-fail-partial-double-11-good",
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

    TALER_TESTING_cmd_end ()
  };

  struct TALER_TESTING_Command commands[] = {

    TALER_TESTING_cmd_batch ("pay",
                             pay),

    TALER_TESTING_cmd_batch ("double-spending",
                             double_spending),

    TALER_TESTING_cmd_batch ("track",
                             track),
    TALER_TESTING_cmd_history
      ("history-2",
       merchant_url,
       MHD_HTTP_OK,
       GNUNET_TIME_absolute_add (GNUNET_TIME_UNIT_ZERO_ABS,
                                 GNUNET_TIME_UNIT_MICROSECONDS),
       /* zero results expected, there isn't any row with id
        * bigger than 10. */
       0,
       10,
       10),

    TALER_TESTING_cmd_batch ("refund",
                             refund),

    TALER_TESTING_cmd_batch ("tip",
                             tip),

    TALER_TESTING_cmd_batch ("pay-again",
                             pay_again),

    TALER_TESTING_cmd_batch ("pay-abort",
                             pay_abort),

    TALER_TESTING_cmd_history_default_start
      ("history-default-start",
       merchant_url,
       MHD_HTTP_OK,
       GNUNET_TIME_UNIT_ZERO_ABS,
       5, /* Expected number of records */
       -100), /* Delta */
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
      (fakebank_url = TALER_TESTING_prepare_fakebank
        (CONFIG_FILE,
         "account-exchange")))
    return 77;
  if (NULL ==
      (merchant_url = TALER_TESTING_prepare_merchant (CONFIG_FILE)))
    return 77;

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

    if (NULL == (merchantd =
        TALER_TESTING_run_merchant (CONFIG_FILE, merchant_url)))
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
