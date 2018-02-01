/*
  This file is part of TALER
  Copyright (C) 2014-2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/test_merchant_api.c
 * @brief testcase to test merchant's HTTP API interface
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_bank_service.h>
#include <taler/taler_fakebank_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchantdb_lib.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include <microhttpd.h>

/**
 * URL under which the merchant is reachable during the testcase.
 */
#define MERCHANT_URL "http://localhost:8082"

/**
 * URL under which the exchange is reachable during the testcase.
 */
#define EXCHANGE_URL "http://localhost:8084/"

/**
 * Account number of the exchange at the bank.
 */
#define EXCHANGE_ACCOUNT_NO 2

/**
 * URL of the bank.
 */
#define BANK_URL "http://localhost:8083/"

/**
 * On which port do we run the (fake) bank?
 */
#define BANK_PORT 8083

/**
 * Max size allowed for an order.
 */
#define ORDER_MAX_SIZE 1000

#define RND_BLK(ptr)                                                    \
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK, ptr, sizeof (*ptr))


/**
 * Handle to database.
 */
static struct TALER_MERCHANTDB_Plugin *db;

/**
 * Configuration handle.
 */
struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle to access the exchange.
 */
static struct TALER_EXCHANGE_Handle *exchange;

/**
 * Main execution context for the main loop of the exchange.
 */
static struct GNUNET_CURL_Context *ctx;

/**
 * Array of instances to test against
 */
static char **instances;

/**
 * How many merchant instances this test runs
 */
static unsigned int ninstances = 0;

/**
 * Current instance
 */
static char *instance;

/**
 * Current instance key
 */
static struct GNUNET_CRYPTO_EddsaPrivateKey *instance_priv;

/**
 * Current instance being tested
 */
static unsigned int instance_idx = 0;

/**
 * Task run on timeout.
 */
static struct GNUNET_SCHEDULER_Task *timeout_task;

/**
 * Context for running the #ctx's event loop.
 */
static struct GNUNET_CURL_RescheduleContext *rc;

/**
 * Handle to the fake bank service we run for the
 * aggregator.
 */
static struct TALER_FAKEBANK_Handle *fakebank;

/**
 * Result of the testcases, #GNUNET_OK on success
 */
static int result;


/**
 * Opcodes for the interpreter.
 */
enum OpCode
{
  /**
   * Termination code, stops the interpreter loop (with success).
   */
  OC_END = 0,

  /**
   * Issue a GET /proposal to the backend.
   */
  OC_PROPOSAL_LOOKUP,

  /**
   * Add funds to a reserve by (faking) incoming wire transfer.
   */
  OC_ADMIN_ADD_INCOMING,

  /**
   * Check status of a reserve.
   */
  OC_WITHDRAW_STATUS,

  /**
   * Withdraw a coin from a reserve.
   */
  OC_WITHDRAW_SIGN,

  /**
   * Issue a PUT /proposal to the backend.
   */
  OC_PROPOSAL,

  /**
   * Pay with coins.
   */
  OC_PAY,

  /**
   * Resume pay operation with additional coins.
   */
  OC_PAY_AGAIN,

  /**
   * Abort payment with coins, requesting refund.
   */
  OC_PAY_ABORT,

  /**
   * Abort payment with coins, executing refund.
   */
  OC_PAY_ABORT_REFUND,

  /**
   * Run the aggregator to execute deposits.
   */
  OC_RUN_AGGREGATOR,

  /**
   * Run the wirewatcher to check for incoming transactions.
   */
  OC_RUN_WIREWATCH,

  /**
   * Check that the fakebank has received a certain transaction.
   */
  OC_CHECK_BANK_TRANSFER,

  /**
   * Check that the fakebank has not received any other transactions.
   */
  OC_CHECK_BANK_TRANSFERS_EMPTY,

  /**
   * Retrieve deposit details for a given wire transfer
   */
  OC_TRACK_TRANSFER,

  /**
   * Retrieve wire transfer details for a given transaction
   */
  OC_TRACK_TRANSACTION,

  /**
   * Test getting transactions based on timestamp
   */
  OC_HISTORY,

  /**
   * Test the increase of a order refund
   */
  OC_REFUND_INCREASE,

  /**
   * Test refund lookup
   */
  OC_REFUND_LOOKUP,

  /**
   * Authorize a tip.
   */
  OC_TIP_AUTHORIZE,

  /**
   * Pickup a tip.
   */
  OC_TIP_PICKUP,

  /**
   * Check pay status.
   */
  OC_CHECK_PAYMENT,

  /**
   * Query tip stats.
   */
  OC_TIP_QUERY,

};


/**
 * Structure specifying details about a coin to be melted.
 * Used in a NULL-terminated array as part of command
 * specification.
 */
struct MeltDetails
{

  /**
   * Amount to melt (including fee).
   */
  const char *amount;

  /**
   * Reference to reserve_withdraw operations for coin to
   * be used for the /refresh/melt operation.
   */
  const char *coin_ref;

};


/**
 * State of the interpreter loop.
 */
struct InterpreterState;


/**
 * Internal withdraw handle used when withdrawing tips.
 */
struct WithdrawHandle
{

  /**
   * Withdraw operation this handle represents.
   */
  struct TALER_EXCHANGE_ReserveWithdrawHandle *wsh;

  /**
   * Interpreter state we are part of.
   */
  struct InterpreterState *is;

  /**
   * Offset of this withdraw operation in the current @e is command.
   */
  unsigned int off;

};


/**
 * Details for a exchange operation to execute.
 */
struct Command
{
  /**
   * Opcode of the command.
   */
  enum OpCode oc;

  /**
   * Label for the command, can be NULL.
   */
  const char *label;

  /**
   * Which response code do we expect for this command?
   */
  unsigned int expected_response_code;

  /**
   * Details about the command.
   */
  union
  {

    /**
     * Information for a #OC_ADMIN_ADD_INCOMING command.
     */
    struct
    {

      /**
       * Label to another admin_add_incoming command if we
       * should deposit into an existing reserve, NULL if
       * a fresh reserve should be created.
       */
      const char *reserve_reference;

      /**
       * Instance to use if we are filling a tipping-reserve. In this
       * case, @e reserve_priv is filled from the configuration instead
       * of at random.  Usually NULL (for random @e reserve_priv).
       */
      const char *instance;

      /**
       * String describing the amount to add to the reserve.
       */
      const char *amount;

      /**
       * Wire transfer subject. NULL to use public key corresponding
       * to @e reserve_priv or @e reserve_reference.  Should only be
       * set manually to test invalid wire transfer subjects.
       */
      const char *subject;

      /**
       * Sender (debit) account number.
       */
      uint64_t debit_account_no;

      /**
       * Receiver (credit) account number.
       */
      uint64_t credit_account_no;

      /**
       * Username to use for authentication.
       */
      const char *auth_username;

      /**
       * Password to use for authentication.
       */
      const char *auth_password;

      /**
       * Set (by the interpreter) to the reserve's private key
       * we used to fill the reserve.
       */
      struct TALER_ReservePrivateKeyP reserve_priv;

      /**
       * Set to the API's handle during the operation.
       */
      struct TALER_BANK_AdminAddIncomingHandle *aih;

      /**
       * Set to the wire transfer's unique ID.
       */
      uint64_t serial_id;

    } admin_add_incoming;

    /**
     * Information for OC_PROPOSAL_LOOKUP command.
     */
    struct
    {

      /**
       * Reference to the proposal we want to lookup.
       */
      const char *proposal_reference;

      struct TALER_MERCHANT_ProposalLookupOperation *plo;

    } proposal_lookup;

    /**
     * Information for a #OC_WITHDRAW_STATUS command.
     */
    struct
    {

      /**
       * Label to the #OC_ADMIN_ADD_INCOMING command which
       * created the reserve.
       */
      const char *reserve_reference;

      /**
       * Set to the API's handle during the operation.
       */
      struct TALER_EXCHANGE_ReserveStatusHandle *wsh;

      /**
       * Expected reserve balance.
       */
      const char *expected_balance;

    } reserve_status;

    /**
     * Information for a #OC_WITHDRAW_SIGN command.
     */
    struct
    {

      /**
       * Which reserve should we withdraw from?
       */
      const char *reserve_reference;

      /**
       * String describing the denomination value we should withdraw.
       * A corresponding denomination key must exist in the exchange's
       * offerings.  Can be NULL if @e pk is set instead.
       */
      const char *amount;

      /**
       * If @e amount is NULL, this specifies the denomination key to
       * use.  Otherwise, this will be set (by the interpreter) to the
       * denomination PK matching @e amount.
       */
      const struct TALER_EXCHANGE_DenomPublicKey *pk;

      /**
       * Set (by the interpreter) to the exchange's signature over the
       * coin's public key.
       */
      struct TALER_DenominationSignature sig;

      /**
       * Set (by the interpreter) to the planchet's secrets.
       */
      struct TALER_PlanchetSecretsP ps;

      /**
       * Withdraw handle (while operation is running).
       */
      struct TALER_EXCHANGE_ReserveWithdrawHandle *wsh;

    } reserve_withdraw;

    /**
     * Information for an #OC_PROPOSAL command.
     */
    struct
    {

      /**
       * The order.
       * It's dynamically generated because we need different transaction_id
       * for different merchant instances.
       */
      char order[ORDER_MAX_SIZE];

      /**
       * Handle to the active PUT /proposal operation, or NULL.
       */
      struct TALER_MERCHANT_ProposalOperation *po;

      /**
       * Handle to the active GET /proposal operation, or NULL.
       */
      struct TALER_MERCHANT_ProposalLookupOperation *plo;

      /**
       * Full contract in JSON, set by the /contract operation.
       * FIXME: verify in the code that this bit is actually proposal
       * data and not the whole proposal.
       */
      json_t *contract_terms;

      /**
       * Proposal's signature.
       */
      struct TALER_MerchantSignatureP merchant_sig;

      /**
       * Proposal data's hashcode.
       */
      struct GNUNET_HashCode hash;

      /**
       * The nonce set by the customer looking up the contract
       * the first time.
       */
      struct GNUNET_CRYPTO_EddsaPublicKey nonce;

    } proposal;

    /**
     * Information for a #OC_PAY command.
     */
    struct
    {

      /**
       * Reference to the contract.
       */
      const char *contract_ref;

      /**
       * ";"-separated list of references to withdrawn coins to be used
       * in the payment.  Each reference has the syntax "LABEL[/NUMBER]"
       * where NUMBER refers to a particular coin (in case multiple coins
       * were created in a step).
       */
      char *coin_ref;

      /**
       * Amount to pay (from the coin, including fee).
       */
      const char *amount_with_fee;

      /**
       * Amount to pay (from the coin, excluding fee).  The sum of the
       * deltas between all @e amount_with_fee and the @e
       * amount_without_fee must be less than max_fee, and the sum of
       * the @e amount_with_fee must be larger than the @e
       * total_amount.
       */
      const char *amount_without_fee;

      /**
       * Refund fee to use for each coin (only relevant if we
       * exercise /pay's abort functionality).
       */
      const char *refund_fee;

      /**
       * Pay handle while operation is running.
       */
      struct TALER_MERCHANT_Pay *ph;

      /**
       * Hashcode of the proposal data associated to this payment.
       */
      struct GNUNET_HashCode h_contract_terms;

      /**
       * Merchant's public key
       */
      struct TALER_MerchantPublicKeyP merchant_pub;

    } pay;

    struct {

      /**
       * Reference to the (incomplete) pay operation that is to be
       * resumed.
       */
      char *pay_ref;

      /**
       * ";"-separated list of references to additional withdrawn
       * coins to be used in the payment.  Each reference has the
       * syntax "LABEL[/NUMBER]" where NUMBER refers to a particular
       * coin (in case multiple coins were created in a step).
       */
      char *coin_ref;

      /**
       * Pay handle while operation is running.
       */
      struct TALER_MERCHANT_Pay *ph;

    } pay_again;

    struct {

      /**
       * Reference to the pay operation that is to be aborted.
       */
      char *pay_ref;

      /**
       * Pay handle while operation is running.
       */
      struct TALER_MERCHANT_Pay *ph;

      /**
       * Set in #pay_refund_cb to number of refunds obtained.
       */
      unsigned int num_refunds;

      /**
       * Array of @e num_refund refunds obtained.
       */
      struct TALER_MERCHANT_RefundEntry *res;

      /**
       * Set to the hash of the original contract.
       */
      struct GNUNET_HashCode h_contract;

      /**
       * Set to the merchant's public key.
       */
      struct TALER_MerchantPublicKeyP merchant_pub;

    } pay_abort;

    struct {

      /**
       * Reference to the pay_abort command to be refunded.
       */
      const char *abort_ref;

      /**
       * Number of the coin of @e abort_ref to be refunded.
       */
      unsigned int num_coin;

      /**
       * Refund amount to use.
       */
      const char *refund_amount;

      /**
       * Refund fee to expect.
       */
      const char *refund_fee;

      /**
       * Handle to the refund operation.
       */
      struct TALER_EXCHANGE_RefundHandle *rh;

    } pay_abort_refund;

    struct {

      /**
       * Process for the aggregator.
       */
      struct GNUNET_OS_Process *aggregator_proc;

      /**
       * ID of task called whenever we get a SIGCHILD.
       */
      struct GNUNET_SCHEDULER_Task *child_death_task;

    } run_aggregator;

    struct {

      /**
       * Process for the wirewatcher.
       */
      struct GNUNET_OS_Process *wirewatch_proc;

      /**
       * ID of task called whenever we get a SIGCHILD.
       */
      struct GNUNET_SCHEDULER_Task *child_death_task;

    } run_wirewatch;

    struct {

      /**
       * Which amount do we expect to see transferred?
       */
      const char *amount;

      /**
       * Which account do we expect to be debited?
       */
      uint64_t account_debit;

      /**
       * Which account do we expect to be credited?
       */
      uint64_t account_credit;

      /**
       * Set (!) to the wire transfer subject observed.
       */
      char *subject;

    } check_bank_transfer;

    struct {

      /**
       * #OC_CHECK_BANK_TRANSFER command from which we should grab
       * the WTID.
       */
      char *check_bank_ref;

      /**
       * #OC_PAY command which we expect in the result.
       * Since we are tracking a bank transaction, we want to know
       * which (Taler) deposit is associated with the bank
       * transaction being tracked now.
       */
      char *expected_pay_ref;

      /**
       * Handle to a /track/transfer operation
       */
      struct TALER_MERCHANT_TrackTransferHandle *tdo;

    } track_transfer;

    struct {

      /**
       * #OC_PAY command from which we should grab
       * the WTID.
       */
      char *pay_ref;

      /**
       * #OC_CHECK_BANK_TRANSFER command which we expect in the result.
       */
      char *expected_transfer_ref;

      /**
       * Wire fee we expect to pay for this transaction.
       */
      const char *wire_fee;

      /**
       * Handle to a /track/transaction operation
       */
      struct TALER_MERCHANT_TrackTransactionHandle *tth;

    } track_transaction;

    struct {

      /**
       * Date we want retrieved transactions younger than
       */
      struct GNUNET_TIME_Absolute date;

      /**
       * How many "rows" we expect in the result
       */
      unsigned int nresult;

      /**
       * Handle to /history request
       */
      struct TALER_MERCHANT_HistoryOperation *ho;

      /**
       * The backend will return records with row_id
       * less than this value.
       */
      unsigned int start;

      /**
       * The backend will return at most `nrows` records.
       */
      unsigned int nrows;

    } history;

    struct {
      /**
       * Reference to the order we want reimbursed
       */
      char *order_id;

      /**
       * Handle to a refund increase operation
       */
      struct TALER_MERCHANT_RefundIncreaseOperation *rio;

      /**
       * Amount to refund
       */
      const char *refund_amount;

      /**
       * Reason for refunding
       */
      const char *reason;

      /**
       * Refund fee (MUST match the value given in config)
       */
      const char *refund_fee;

    } refund_increase;

    struct {

      /**
       * Reference to the order whose refund was increased
       */
      char *order_id;

      /**
       * Handle to the operation
       */
      struct TALER_MERCHANT_RefundLookupOperation *rlo;

      /**
       * Used to retrieve the asked refund amount.
       * This information helps the callback to mock a GET /refund
       * response and match it against what the backend actually
       * responded.
       */
      char *increase_ref;

      /**
       * Used to retrieve the number and denomination of coins
       * used to pay for the related contract.
       * This information helps the callback to mock a GET /refund
       * response and match it against what the backend actually
       * responded.
       */
      char *pay_ref;

    } refund_lookup;

    struct {

      /**
       * Specify the instance (to succeed, this must match a prior
       * enable action and the respective wire transfer's instance).
       */
      const char *instance;

      /**
       * Reason to use for enabling the tip (required by the API, but not
       * yet really useful as we do not have a way to read back the
       * justifications stored in the merchant's DB).
       */
      const char *justification;

      /**
       * How much should the tip be?
       */
      const char *amount;

      /**
       * Handle for the ongoing operation.
       */
      struct TALER_MERCHANT_TipAuthorizeOperation *tao;

      /**
       * Unique ID for the authorized tip, set by the interpreter.
       */
      struct GNUNET_HashCode tip_id;

      /**
       * When does the authorization expire?
       */
      struct GNUNET_TIME_Absolute tip_expiration;

      /**
       * EC expected for the operation.
       */
      enum TALER_ErrorCode expected_ec;

    } tip_authorize;

    struct {

      /**
       * Reference to operation that authorized the tip. Used
       * to obtain the `tip_id`.
       */
      const char *authorize_ref;

      /**
       * Set to non-NULL to a label of another pick up operation
       * that we should replay.
       */
      const char *replay_ref;

      /**
       * Number of coins we pick up.
       */
      unsigned int num_coins;

      /**
       * Array of @e num_coins denominations of the coins we pick up.
       */
      const char **amounts;

      /**
       * Handle for the ongoing operation.
       */
      struct TALER_MERCHANT_TipPickupOperation *tpo;

      /**
       * Temporary data structure to store the blinding keys while the
       * pickup operation runs.
       */
      struct TALER_PlanchetSecretsP *psa;

      /**
       * Array of denomination keys matching the @e amounts.
       */
      const struct TALER_EXCHANGE_DenomPublicKey **dks;

      /**
       * Temporary data structure of @e num_coins entries for the
       * withdraw operations.
       */
      struct WithdrawHandle *withdraws;

      /**
       * Set (by the interpreter) to an array of @a num_coins signatures
       * created from the (successful) tip operation.
       */
      struct TALER_DenominationSignature *sigs;

      /**
       * EC expected for the operation.
       */
      enum TALER_ErrorCode expected_ec;

    } tip_pickup;

    struct {
      /**
       * Expected available amount (in string format).
       * NULL to skip check.
       */
      char *expected_amount_available;

      /**
       * Expected picked up amount (in string format).
       * NULL to skip check.
       */
      char *expected_amount_picked_up;

      /**
       * Expected authorized amount (in string format).
       * NULL to skip check.
       */
      char *expected_amount_authorized;

      /**
       * Handle for the ongoing operation.
       */
      struct TALER_MERCHANT_TipQueryOperation *tqo;

      /**
       * Merchant instance to use for tipping.
       */
      char *instance;

    } tip_query;

    struct {

      /**
       * Reference for the contract we want to check.
       */
      const char *contract_ref;

      /**
       * Whether to expect the payment to be settled or not.
       */
      int expect_paid;

      /**
       * Operation handle for the /check-payment request,
       * NULL if operation is not running.
       */
      struct TALER_MERCHANT_CheckPaymentOperation *cpo;

    } check_payment;

  } details;

};


/**
 * State of the interpreter loop.
 */
struct InterpreterState
{
  /**
   * Keys from the exchange.
   */
  const struct TALER_EXCHANGE_Keys *keys;

  /**
   * Commands the interpreter will run.
   */
  struct Command *commands;

  /**
   * Interpreter task (if one is scheduled).
   */
  struct GNUNET_SCHEDULER_Task *task;

  /**
   * Instruction pointer.  Tells #interpreter_run() which
   * instruction to run next.
   */
  unsigned int ip;

};


/**
 * Pipe used to communicate child death via signal.
 */
static struct GNUNET_DISK_PipeHandle *sigpipe;

/**
 * Return instance private key from config
 *
 * @param config configuration handle
 * @param instance instance name
 * @return pointer to private key, NULL on error
 */
struct GNUNET_CRYPTO_EddsaPrivateKey *
get_instance_priv (struct GNUNET_CONFIGURATION_Handle *config,
                   const char *instance)
{
  char *config_section;
  char *filename;
  struct GNUNET_CRYPTO_EddsaPrivateKey *ret;

  (void) GNUNET_asprintf (&config_section,
                          "merchant-instance-%s",
                          instance);
  if (GNUNET_OK !=
    GNUNET_CONFIGURATION_get_value_filename (config,
                                             config_section,
                                             "KEYFILE",
                                             &filename))
  {
    GNUNET_break (0);
    GNUNET_free (config_section);
    return NULL;
  }
  GNUNET_free (config_section);
  if (NULL ==
      (ret = GNUNET_CRYPTO_eddsa_key_create_from_file (filename)))
    GNUNET_break (0);
  GNUNET_free (filename);
  return ret;
}

/**
 * The testcase failed, return with an error code.
 *
 * @param is interpreter state to clean up
 */
static void
fail (struct InterpreterState *is)
{
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
          "Interpreter failed at step %s (#%u)\n",
          is->commands[is->ip].label,
          is->ip);
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Find a command by label.
 *
 * @param is interpreter state to search
 * @param label label to look for
 * @return NULL if command was not found
 */
static const struct Command *
find_command (const struct InterpreterState *is,
              const char *label)
{
  const struct Command *cmd;

  if (NULL == label)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Attempt to lookup command for empty label\n");
    return NULL;
  }
  for (unsigned int i=0;
       OC_END != (cmd = &is->commands[i])->oc;
       i++)
    if ( (NULL != cmd->label) &&
       (0 == strcmp (cmd->label,
                     label)) )
      return cmd;
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
          "Command not found: %s\n",
          label);
  return NULL;
}


/**
 * Run the main interpreter loop that performs exchange operations.
 *
 * @param cls contains the `struct InterpreterState`
 */
static void
interpreter_run (void *cls);


/**
 * Run the next command with the interpreter.
 *
 * @param is current interpeter state.
 */
static void
next_command (struct InterpreterState *is)
{
  is->ip++;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Function called upon completion of our /admin/add/incoming request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param serial_id unique ID of the wire transfer
 * @param full_response full response from the exchange (for logging, in case of errors)
 */
static void
add_incoming_cb (void *cls,
                 unsigned int http_status,
		 enum TALER_ErrorCode ec,
                 uint64_t serial_id,
                 const json_t *full_response)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.admin_add_incoming.aih = NULL;
  cmd->details.admin_add_incoming.serial_id = serial_id;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  next_command (is);
}

/**
 * Parse given JSON object to absolute time.
 *
 * @param root the json object representing data
 * @param[out] ret where to write the data
 * @return #GNUNET_OK upon successful parsing; #GNUNET_SYSERR upon error
 */
static int
parse_abs_time (json_t *root,
                struct GNUNET_TIME_Absolute *ret)
{
  const char *val;
  unsigned long long int tval;

  val = json_string_value (root);
  if (NULL == val)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  if ( (0 == strcasecmp (val,
                         "/forever/")) ||
       (0 == strcasecmp (val,
                         "/end of time/")) ||
       (0 == strcasecmp (val,
                         "/never/")) )
  {
    *ret = GNUNET_TIME_UNIT_FOREVER_ABS;
    return GNUNET_OK;
  }
  if (1 != sscanf (val,
                   "/Date(%llu)/",
                   &tval))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  /* Time is in seconds in JSON, but in microseconds in GNUNET_TIME_Absolute */
  ret->abs_value_us = tval * 1000LL * 1000LL;
  if ( (ret->abs_value_us) / 1000LL / 1000LL != tval)
  {
    /* Integer overflow */
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Callback for a /history request. It's up to this function how
 * to render the array containing transactions details (FIXME link to
 * documentation)
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant backend
 * @param ec taler-specific error code
 * @param json actual body containing history
 */
static void
history_cb (void *cls,
            unsigned int http_status,
	    enum TALER_ErrorCode ec,
            const json_t *json)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  unsigned int nresult;
  struct GNUNET_TIME_Absolute last_timestamp;
  struct GNUNET_TIME_Absolute entry_timestamp;

  cmd->details.history.ho = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    fail (is);
    return;
  }
  nresult = json_array_size (json);
  if (nresult != cmd->details.history.nresult)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected number of history entries. Got %d, expected %d\n",
                nresult,
                cmd->details.history.nresult);
    fail (is);
    return;
  }

  last_timestamp = GNUNET_TIME_absolute_get ();
  last_timestamp = GNUNET_TIME_absolute_add (last_timestamp,
                                             GNUNET_TIME_UNIT_DAYS);
  json_t *entry;
  json_t *timestamp;
  size_t index;
  json_array_foreach (json, index, entry)
  {
    timestamp = json_object_get (entry, "timestamp");
    if (GNUNET_OK != parse_abs_time (timestamp, &entry_timestamp))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Bad timestamp given\n");
        fail (is);
        return;
      }
    entry_timestamp = GNUNET_TIME_absolute_max (last_timestamp, entry_timestamp);
    if (last_timestamp.abs_value_us != entry_timestamp.abs_value_us)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "History entries are NOT sorted from younger to older\n");
      fail (is);
      return;
    }
  }

  next_command (is);
}


/**
 * Check if the given historic event @a h corresponds to the given
 * command @a cmd.
 *
 * @param h event in history
 * @param cmd an #OC_ADMIN_ADD_INCOMING command
 * @return #GNUNET_OK if they match, #GNUNET_SYSERR if not
 */
static int
compare_admin_add_incoming_history (const struct TALER_EXCHANGE_ReserveHistory *h,
                                    const struct Command *cmd)
{
  struct TALER_Amount amount;

  if (TALER_EXCHANGE_RTT_DEPOSIT != h->type)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  GNUNET_assert (GNUNET_OK ==
    TALER_string_to_amount (cmd->details.admin_add_incoming.amount,
                            &amount));
  if (0 != TALER_amount_cmp (&amount,
                         &h->amount))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Check if the given historic event @a h corresponds to the given
 * command @a cmd.
 *
 * @param h event in history
 * @param cmd an #OC_WITHDRAW_SIGN command
 * @return #GNUNET_OK if they match, #GNUNET_SYSERR if not
 */
static int
compare_reserve_withdraw_history (const struct TALER_EXCHANGE_ReserveHistory *h,
                                  const struct Command *cmd)
{
  struct TALER_Amount amount;
  struct TALER_Amount amount_with_fee;

  if (TALER_EXCHANGE_RTT_WITHDRAWAL != h->type)
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  GNUNET_assert (GNUNET_OK ==
    TALER_string_to_amount (cmd->details.reserve_withdraw.amount,
                            &amount));
  GNUNET_assert (GNUNET_OK ==
    TALER_amount_add (&amount_with_fee,
                      &amount,
                      &cmd->details.reserve_withdraw.pk->fee_withdraw));
  if (0 != TALER_amount_cmp (&amount_with_fee,
      &h->amount))
  {
    GNUNET_break_op (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Function called with the result of a /reserve/status request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param[in] json original response in JSON format (useful only for diagnostics)
 * @param balance current balance in the reserve, NULL on error
 * @param history_length number of entries in the transaction history, 0 on error
 * @param history detailed transaction history, NULL on error
 */
static void
reserve_status_cb (void *cls,
                   unsigned int http_status,
		   enum TALER_ErrorCode ec,
                   const json_t *json,
                   const struct TALER_Amount *balance,
                   unsigned int history_length,
                   const struct TALER_EXCHANGE_ReserveHistory *history)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  struct Command *rel;
  unsigned int j;
  struct TALER_Amount amount;

  cmd->details.reserve_status.wsh = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    GNUNET_break (0);
    json_dumpf (json, stderr, 0);
    fail (is);
    return;
  }
  switch (http_status)
  {
  case MHD_HTTP_OK:
    j = 0;
    for (unsigned int i=0;i<is->ip;i++)
    {
      switch ((rel = &is->commands[i])->oc)
      {
      case OC_ADMIN_ADD_INCOMING:
        /**
         * If the command being iterated over filled a reserve AND
         * it is the one referenced by the current "history command"
         * ...
         */
        if ( ( (NULL != rel->label) &&
             (0 == strcmp (cmd->details.reserve_status.reserve_reference,
                           rel->label) ) ) ||
           ( (NULL != rel->details.admin_add_incoming.reserve_reference) &&
             (0 == strcmp (cmd->details.reserve_status.reserve_reference,
                         rel->details.admin_add_incoming.reserve_reference) ) ) )
        {
          /**
           * ... then make sure the history element mentions a "deposit
           * operation" on that reserve.
           */
          if (GNUNET_OK != compare_admin_add_incoming_history (&history[j],
                                                               rel))
          {
            GNUNET_break (0);
            fail (is);
            return;
          }
          j++;
        }
        break;
      case OC_WITHDRAW_SIGN:
        /**
         * If the command being iterated over emptied a reserve AND
         * it is the one referenced by the current "history command"
         * ...
         */
        if (0 == strcmp (cmd->details.reserve_status.reserve_reference,
                         rel->details.reserve_withdraw.reserve_reference))
        {
          /**
           * ... then make sure the history element mentions a "withdraw
           * operation" on that reserve.
           */
          if (GNUNET_OK != compare_reserve_withdraw_history (&history[j],
                                                             rel))
          {
            GNUNET_break (0);
            fail (is);
            return;
          }
          j++;
        }
        break;
      default:
      /* unreleated, just skip */
        break;
      }
    }
    if (j != history_length)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    if (NULL != cmd->details.reserve_status.expected_balance)
    {
      GNUNET_assert (GNUNET_OK ==
                     TALER_string_to_amount (cmd->details.reserve_status.expected_balance,
                                             &amount));
      if (0 != TALER_amount_cmp (&amount,
                                 balance))
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
    }
    break;
  default:
    /* Unsupported status code (by test harness) */
    GNUNET_break (0);
    break;
  }
  next_command (is);
}


/**
 * Function called upon completion of our /reserve/withdraw request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code
 * @param sig signature over the coin, NULL on error
 * @param full_response full response from the exchange (for logging, in case of errors)
 */
static void
reserve_withdraw_cb (void *cls,
                     unsigned int http_status,
		     enum TALER_ErrorCode ec,
                     const struct TALER_DenominationSignature *sig,
                     const json_t *full_response)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.reserve_withdraw.wsh = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  switch (http_status)
  {
  case MHD_HTTP_OK:
    if (NULL == sig)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
    /**
     * NOTE: this assert is OK on the second instance run because the
     * interpreter is "cleaned" by cleanup_state()
     */
    GNUNET_assert (NULL == cmd->details.reserve_withdraw.sig.rsa_signature);
    cmd->details.reserve_withdraw.sig.rsa_signature
      = GNUNET_CRYPTO_rsa_signature_dup (sig->rsa_signature);
    break;
  case MHD_HTTP_PAYMENT_REQUIRED:
    /* nothing to check */
    break;
  default:
    /* Unsupported status code (by test harness) */
    GNUNET_break (0);
    break;
  }
  next_command (is);
}


/**
 * Callback for GET /proposal issued at backend.
 * Used to initialize the proposal after it was created.
 *
 * @param cls closure
 * @param http_status HTTP status code we got
 * @param json full response we got
 */
static void
proposal_lookup_initial_cb (void *cls,
                            unsigned int http_status,
                            const json_t *json,
                            const json_t *contract_terms,
                            const struct TALER_MerchantSignatureP *sig,
                            const struct GNUNET_HashCode *hash)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  if (cmd->expected_response_code != http_status)
  {
    fail (is);
  }

  cmd->details.proposal.hash = *hash;
  cmd->details.proposal.merchant_sig = *sig;
  cmd->details.proposal.contract_terms = json_deep_copy (contract_terms);

  cmd->details.proposal.plo = NULL;
  next_command (is);
}


/**
 * Callback that works POST /proposal's output.
 *
 * @param cls closure
 * @param http_status HTTP response code, 200 indicates success;
 *                    0 if the backend's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code
 * @param obj the full received JSON reply, or
 *            error details if the request failed
 * @param contract_terms the order + additional information provided by the
 * backend, NULL on error.
 * @param sig merchant's signature over the contract, NULL on error
 * @param h_contract hash of the contract, NULL on error
 */
static void
proposal_cb (void *cls,
             unsigned int http_status,
	     enum TALER_ErrorCode ec,
             const json_t *obj,
             const char *order_id)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.proposal.po = NULL;
  switch (http_status)
  {
  case MHD_HTTP_OK:
    break;
  default: {
    char *s = json_dumps (obj, JSON_COMPACT);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected status code from /proposal: %u (%d). Step %u, response: %s\n",
                http_status,
                ec,
                is->ip,
                s);
    GNUNET_free_non_null (s);
    fail (is);
    }
    return;
  }

  if (NULL == (cmd->details.proposal.plo
               = TALER_MERCHANT_proposal_lookup (ctx,
                                                 MERCHANT_URL,
                                                 order_id,
                                                 instance,
                                                 &cmd->details.proposal.nonce,
                                                 proposal_lookup_initial_cb,
                                                 is)))
  {
    GNUNET_break (0);
    fail (is);
  }
}


/**
 * Process POST /refund (increase) response
 *
 * @param cls closure
 * @param http_status HTTP status code
 * @param ec taler-specific error object
 * @param obj response body; is NULL on success.
 */
static void
refund_increase_cb (void *cls,
                    unsigned int http_status,
                    enum TALER_ErrorCode ec,
                    const json_t *obj)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Refund increase failed\n");
    fail (is);
    return;
  }
  cmd->details.refund_increase.rio = NULL;
  next_command (is);
}


/**
 * Callback that frees all the elements in the hashmap
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct TALER_Amount`
 * @return always #GNUNET_YES (continue to iterate)
 */
static int
hashmap_free (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  struct TALER_Amount *refund_amount = value;

  GNUNET_free (refund_amount);
  return GNUNET_YES;
}


/**
 * Process GET /refund (increase) response.
 *
 * @param cls closure
 * @param http_status HTTP status code
 * @param ec taler-specific error object
 * @param obj response body; is NULL on error.
 */
static void
refund_lookup_cb (void *cls,
                  unsigned int http_status,
                  enum TALER_ErrorCode ec,
                  const json_t *obj)
{
  struct GNUNET_CONTAINER_MultiHashMap *map;
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  size_t index;
  json_t *elem;
  const char *error_name;
  unsigned int error_line;
  struct GNUNET_HashCode h_coin_pub;
  char *icoin_ref;
  char *icoin_refs;
  const struct Command *icoin;
  const struct Command *pay;
  struct TALER_CoinSpendPublicKeyP icoin_pub;
  struct GNUNET_HashCode h_icoin_pub;
  struct TALER_Amount *iamount;
  struct TALER_Amount acc;
  const struct Command *increase;
  struct TALER_Amount refund_amount;
  const json_t *arr;

  cmd->details.refund_lookup.rlo = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Refund lookup failed\n");
    fail (is);
    return;
  }

  map = GNUNET_CONTAINER_multihashmap_create (1, GNUNET_NO);
  arr = json_object_get (obj,
                         "refund_permissions");
  if (NULL == arr)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  json_array_foreach (arr, index, elem)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount *irefund_amount
      = GNUNET_new (struct TALER_Amount);
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("coin_pub", &coin_pub),
      TALER_JSON_spec_amount ("refund_amount", irefund_amount),
      GNUNET_JSON_spec_end ()
    };

    GNUNET_assert (GNUNET_OK ==
		   GNUNET_JSON_parse (elem,
				      spec,
				      &error_name,
				      &error_line));
    GNUNET_CRYPTO_hash (&coin_pub,
			sizeof (struct TALER_CoinSpendPublicKeyP),
			&h_coin_pub);
    GNUNET_assert (GNUNET_OK ==
		   GNUNET_CONTAINER_multihashmap_put (map,
						      &h_coin_pub,
						      irefund_amount,
						      GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY));
  };

  /* Retrieve coins used to pay, from #OC_PAY command */
  GNUNET_assert (NULL != (pay =
			  find_command (is,
					cmd->details.refund_lookup.pay_ref)));
  icoin_refs = GNUNET_strdup (pay->details.pay.coin_ref);
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero ("EUR",
                                        &acc));
  for (icoin_ref = strtok (icoin_refs, ";");
       NULL != icoin_ref;
       icoin_ref = strtok (NULL, ";"))
  {
    GNUNET_assert (NULL != (icoin =
			    find_command (is,
					  icoin_ref)));
    GNUNET_CRYPTO_eddsa_key_get_public (&icoin->details.reserve_withdraw.ps.coin_priv.eddsa_priv,
                                        &icoin_pub.eddsa_pub);
    GNUNET_CRYPTO_hash (&icoin_pub,
                        sizeof (struct TALER_CoinSpendPublicKeyP),
                        &h_icoin_pub);
    /* Can be NULL: not all coins are involved in refund */
    iamount = GNUNET_CONTAINER_multihashmap_get (map,
						 &h_icoin_pub);
    if (NULL == iamount)
      continue;
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_add (&acc,
                                     &acc,
                                     iamount));
  }
  GNUNET_free (icoin_refs);
  /* Check if refund has been 100% covered */
  GNUNET_assert (increase =
                 find_command (is,
                               cmd->details.refund_lookup.increase_ref));
  GNUNET_assert (GNUNET_OK ==
                 TALER_string_to_amount (increase->details.refund_increase.refund_amount,
                                         &refund_amount));
  GNUNET_CONTAINER_multihashmap_iterate (map,
                                         &hashmap_free,
                                         NULL);
  GNUNET_CONTAINER_multihashmap_destroy (map);
  if (0 != TALER_amount_cmp (&acc,
                             &refund_amount))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Incomplete refund: expected '%s', got '%s'\n",
                TALER_amount_to_string (&refund_amount),
                TALER_amount_to_string (&acc));
    fail (is);
    return;
  }
  next_command (is);
}


/**
 * Function called with the result of a /pay operation.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error object
 * @param obj the received JSON reply, should be kept as proof (and, in case of errors,
 *            be forwarded to the customer)
 */
static void
pay_cb (void *cls,
        unsigned int http_status,
        enum TALER_ErrorCode ec,
        const json_t *obj)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  struct GNUNET_CRYPTO_EddsaSignature sig;
  const char *error_name;
  unsigned int error_line;

  cmd->details.pay.ph = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  if (MHD_HTTP_OK == http_status)
  {
    /* Check signature */
    struct PaymentResponsePS mr;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("sig", &sig),
      GNUNET_JSON_spec_fixed_auto ("h_contract_terms",
				   &cmd->details.pay.h_contract_terms),
      GNUNET_JSON_spec_end ()
    };
    GNUNET_assert (GNUNET_OK ==
        GNUNET_JSON_parse (obj,
                           spec,
                           &error_name,
                           &error_line));
    mr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
    mr.purpose.size = htonl (sizeof (mr));
    mr.h_contract_terms = cmd->details.pay.h_contract_terms;
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_PAYMENT_OK,
                                    &mr.purpose,
                                    &sig,
                                    &cmd->details.pay.merchant_pub.eddsa_pub))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Merchant signature given in response to /pay invalid\n");
      fail (is);
      return;
    }
  }
  next_command (is);
}


/**
 * Function called with the result of a /pay again operation.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error object
 * @param obj the received JSON reply, should be kept as proof (and, in case of errors,
 *            be forwarded to the customer)
 */
static void
pay_again_cb (void *cls,
	      unsigned int http_status,
	      enum TALER_ErrorCode ec,
	      const json_t *obj)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  struct GNUNET_CRYPTO_EddsaSignature sig;
  const char *error_name;
  unsigned int error_line;
  const struct Command *pref;

  cmd->details.pay_again.ph = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  GNUNET_assert (NULL != (pref = find_command
			  (is,
			   cmd->details.pay_again.pay_ref)));
  if (MHD_HTTP_OK == http_status)
  {
    struct PaymentResponsePS mr;
    /* Check signature */
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("sig",
				   &sig),
      GNUNET_JSON_spec_fixed_auto ("h_contract_terms",
				   &mr.h_contract_terms),
      GNUNET_JSON_spec_end ()
    };

    GNUNET_assert (GNUNET_OK ==
        GNUNET_JSON_parse (obj,
                           spec,
                           &error_name,
                           &error_line));
    mr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
    mr.purpose.size = htonl (sizeof (mr));
    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_PAYMENT_OK,
                                    &mr.purpose,
                                    &sig,
                                    &pref->details.pay.merchant_pub.eddsa_pub))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Merchant signature given in response to /pay invalid\n");
      fail (is);
      return;
    }
  }
  next_command (is);
}


/**
 * Task triggered whenever we receive a SIGCHLD (child
 * process died).
 *
 * @param cls closure, NULL if we need to self-restart
 */
static void
maint_child_death (void *cls)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  const struct GNUNET_DISK_FileHandle *pr;
  char c[16];

  switch (cmd->oc) {
  case OC_RUN_AGGREGATOR:
    cmd->details.run_aggregator.child_death_task = NULL;
    pr = GNUNET_DISK_pipe_handle (sigpipe,
                                  GNUNET_DISK_PIPE_END_READ);
    GNUNET_break (0 < GNUNET_DISK_file_read (pr,
                                             &c,
                                             sizeof (c)));
    GNUNET_OS_process_wait (cmd->details.run_aggregator.aggregator_proc);
    GNUNET_OS_process_destroy (cmd->details.run_aggregator.aggregator_proc);
    cmd->details.run_aggregator.aggregator_proc = NULL;
    break;
  case OC_RUN_WIREWATCH:
    cmd->details.run_wirewatch.child_death_task = NULL;
    pr = GNUNET_DISK_pipe_handle (sigpipe, GNUNET_DISK_PIPE_END_READ);
    GNUNET_break (0 < GNUNET_DISK_file_read (pr, &c, sizeof (c)));
    GNUNET_OS_process_wait (cmd->details.run_wirewatch.wirewatch_proc);
    GNUNET_OS_process_destroy (cmd->details.run_wirewatch.wirewatch_proc);
    cmd->details.run_wirewatch.wirewatch_proc = NULL;
    break;
  default:
    GNUNET_break (0);
    fail (is);
    return;
  }
  next_command (is);
}


/**
 * Callback for a /track/transfer operation
 *
 * @param cls closure for this function
 * @param http_status HTTP response code returned by the server
 * @param ec taler-specific error code
 * @param sign_key exchange key used to sign @a json, or NULL
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param h_wire hash of the wire transfer address the transfer went to, or NULL on error
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
static void
track_transfer_cb (void *cls,
                   unsigned int http_status,
                   enum TALER_ErrorCode ec,
                   const struct TALER_ExchangePublicKeyP *sign_key,
                   const json_t *json,
                   const struct GNUNET_HashCode *h_wire,
                   const struct TALER_Amount *total_amount,
                   unsigned int details_length,
                   const struct TALER_MERCHANT_TrackTransferDetails *details)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.track_transfer.tdo = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  switch (http_status)
  {
    case MHD_HTTP_OK:
      break;
    default:
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Unhandled HTTP status.\n");
  }
  next_command (is);
}


/**
 * Callback for GET /proposal issued at backend. Just check
 * whether response code is as expected.
 *
 * @param cls closure
 * @param http_status HTTP status code we got
 * @param json full response we got
 */
static void
proposal_lookup_cb (void *cls,
                    unsigned int http_status,
                    const json_t *json,
                    const json_t *contract_terms,
                    const struct TALER_MerchantSignatureP *sig,
                    const struct GNUNET_HashCode *hash)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.proposal_lookup.plo = NULL;
  if (cmd->expected_response_code != http_status)
    fail (is);
  next_command (is);
}


/**
 * Callback for GET /proposal issued at backend. Just check
 * whether response code is as expected.
 *
 * @param cls closure
 * @param http_status HTTP status code we got
 * @param json full response we got
 */
static void
check_payment_cb (void *cls,
                  unsigned int http_status,
                  const json_t *obj,
                  int paid,
                  int refunded,
                  struct TALER_Amount *refund_amount,
                  const char *payment_redirect_url)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "check payment: expected paid: %s: %d\n",
              cmd->label,
              cmd->details.check_payment.expect_paid);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "check payment: paid: %d\n",
              paid);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "check payment: url: %s\n",
              payment_redirect_url);

  cmd->details.check_payment.cpo = NULL;
  if (paid != cmd->details.check_payment.expect_paid)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }

  if (cmd->expected_response_code != http_status)
    fail (is);
  next_command (is);
}


/**
 * Callback to process a GET /tip-query request
 *
 * @param cls closure
 * @param http_status HTTP status code for this request
 * @param ec Taler-specific error code
 * @param raw raw response body
 */
static void
tip_query_cb (void *cls,
              unsigned int http_status,
              enum TALER_ErrorCode ec,
              const json_t *raw,
              struct GNUNET_TIME_Absolute reserve_expiration,
              struct TALER_ReservePublicKeyP *reserve_pub,
              struct TALER_Amount *amount_authorized,
              struct TALER_Amount *amount_available,
              struct TALER_Amount *amount_picked_up)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  struct TALER_Amount a;

  cmd->details.tip_query.tqo = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Tip query callback at command %u/%s(%u)\n",
              is->ip,
              cmd->label,
              cmd->oc);

  GNUNET_assert (NULL != reserve_pub);
  GNUNET_assert (NULL != amount_authorized);
  GNUNET_assert (NULL != amount_available);
  GNUNET_assert (NULL != amount_picked_up);

  if (cmd->details.tip_query.expected_amount_available)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (cmd->details.tip_query.expected_amount_available, &a));
    GNUNET_log (GNUNET_ERROR_TYPE_INFO, "expected available %s, actual %s\n",
                TALER_amount_to_string (&a),
                TALER_amount_to_string (amount_available));
    GNUNET_assert (0 == TALER_amount_cmp (amount_available, &a));
  }

  if (cmd->details.tip_query.expected_amount_authorized)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (cmd->details.tip_query.expected_amount_authorized, &a));
    GNUNET_log (GNUNET_ERROR_TYPE_INFO, "expected authorized %s, actual %s\n",
                TALER_amount_to_string (&a),
                TALER_amount_to_string (amount_authorized));
    GNUNET_assert (0 == TALER_amount_cmp (amount_authorized, &a));
  }

  if (cmd->details.tip_query.expected_amount_picked_up)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (cmd->details.tip_query.expected_amount_picked_up, &a));
    GNUNET_log (GNUNET_ERROR_TYPE_INFO, "expected picked_up %s, actual %s\n",
                TALER_amount_to_string (&a),
                TALER_amount_to_string (amount_picked_up));
    GNUNET_assert (0 == TALER_amount_cmp (amount_picked_up, &a));
  }

  if (cmd->expected_response_code != http_status)
    fail (is);
  next_command (is);
}


/**
 * Function called with detailed wire transfer data.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param ec taler-specific error code
 * @param json original json reply
 */
static void
track_transaction_cb (void *cls,
                      unsigned int http_status,
                      enum TALER_ErrorCode ec,
                      const json_t *json)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.track_transaction.tth = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  if (MHD_HTTP_OK != http_status)
    fail (is);
  next_command (is);
}


/**
 * Callback for a /tip-authorize request.  Returns the result of
 * the operation.
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant backend
 * @param ec taler-specific error code
 * @param tip_id which tip ID should be used to pickup the tip
 * @param tip_expiration when does the tip expire (needs to be picked up before this time)
 * @param exchange_url at what exchange can the tip be picked up
 */
static void
tip_authorize_cb (void *cls,
                  unsigned int http_status,
                  enum TALER_ErrorCode ec,
                  const struct GNUNET_HashCode *tip_id,
                  struct GNUNET_TIME_Absolute tip_expiration,
                  const char *exchange_url)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.tip_authorize.tao = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  if (cmd->details.tip_authorize.expected_ec != ec)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected error code %d (%u) to command %s\n",
                ec,
                http_status,
                cmd->label);
    fail (is);
    return;
  }
  if ( (MHD_HTTP_OK == http_status) &&
       (TALER_EC_NONE == ec) )
  {
    if (0 != strcmp (exchange_url,
                     EXCHANGE_URL))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Unexpected exchange URL %s to command %s\n",
                  exchange_url,
                  cmd->label);
      fail (is);
      return;
    }
    cmd->details.tip_authorize.tip_id = *tip_id;
    cmd->details.tip_authorize.tip_expiration = tip_expiration;
  }
  next_command (is);
}


/**
 * Callbacks of this type are used to serve the result of submitting a
 * withdraw request to a exchange.
 *
 * @param cls closure, a `struct WithdrawHandle *`
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param sig signature over the coin, NULL on error
 * @param full_response full response from the exchange (for logging, in case of errors)
 */
static void
pickup_withdraw_cb (void *cls,
                    unsigned int http_status,
                    enum TALER_ErrorCode ec,
                    const struct TALER_DenominationSignature *sig,
                    const json_t *full_response)
{
  struct WithdrawHandle *wh = cls;
  struct InterpreterState *is = wh->is;
  struct Command *cmd = &is->commands[is->ip];

  wh->wsh = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Withdraw operation %u completed with %u (%d)\n",
              wh->off,
              http_status,
              ec);
  GNUNET_assert (wh->off < cmd->details.tip_pickup.num_coins);
  if ( (MHD_HTTP_OK != http_status) ||
       (TALER_EC_NONE != ec) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s when withdrawing\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  if (NULL == cmd->details.tip_pickup.sigs)
    cmd->details.tip_pickup.sigs = GNUNET_new_array (cmd->details.tip_pickup.num_coins,
                                                     struct TALER_DenominationSignature);
  GNUNET_assert (NULL == cmd->details.tip_pickup.sigs[wh->off].rsa_signature);
  cmd->details.tip_pickup.sigs[wh->off].rsa_signature
    = GNUNET_CRYPTO_rsa_signature_dup (sig->rsa_signature);
  for (unsigned int i=0;i<cmd->details.tip_pickup.num_coins;i++)
    if (NULL != cmd->details.tip_pickup.withdraws[wh->off].wsh)
      return; /* still some ops ongoing */
  GNUNET_free (cmd->details.tip_pickup.withdraws);
  cmd->details.tip_pickup.withdraws = NULL;
  next_command (is);
}


/**
 * Callback for a /tip-pickup request.  Returns the result of
 * the operation.
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant backend, "200 OK" on success
 * @param ec taler-specific error code
 * @param reserve_pub public key of the reserve that made the @a reserve_sigs, NULL on error
 * @param num_reserve_sigs length of the @a reserve_sigs array, 0 on error
 * @param reserve_sigs array of signatures authorizing withdrawals, NULL on error
 * @param json original json response
 */
static void
pickup_cb (void *cls,
           unsigned int http_status,
           enum TALER_ErrorCode ec,
           const struct TALER_ReservePublicKeyP *reserve_pub,
           unsigned int num_reserve_sigs,
           const struct TALER_ReserveSignatureP *reserve_sigs,
           const json_t *json)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.tip_pickup.tpo = NULL;
  if (http_status != cmd->expected_response_code)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  if (ec != cmd->details.tip_pickup.expected_ec)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }

  if ( (MHD_HTTP_OK != http_status) ||
       (TALER_EC_NONE != ec) )
  {
    next_command (is);
    return;
  }
  if (num_reserve_sigs != cmd->details.tip_pickup.num_coins)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }

  /* pickup successful, now withdraw! */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Obtained %u signatures for withdrawal from picking up a tip\n",
              num_reserve_sigs);
  GNUNET_assert (NULL == cmd->details.tip_pickup.withdraws);
  cmd->details.tip_pickup.withdraws
    = GNUNET_new_array (num_reserve_sigs,
                        struct WithdrawHandle);
  for (unsigned int i=0;i<num_reserve_sigs;i++)
  {
    struct WithdrawHandle *wh = &cmd->details.tip_pickup.withdraws[i];

    wh->off = i;
    wh->is = is;
    GNUNET_assert ( (NULL == wh->wsh) &&
                    ( (NULL == cmd->details.tip_pickup.sigs) ||
                      (NULL == cmd->details.tip_pickup.sigs[wh->off].rsa_signature) ) );
    wh->wsh = TALER_EXCHANGE_reserve_withdraw2 (exchange,
                                                cmd->details.tip_pickup.dks[i],
                                                &reserve_sigs[i],
                                                reserve_pub,
                                                &cmd->details.tip_pickup.psa[i],
                                                &pickup_withdraw_cb,
                                                wh);
    if (NULL == wh->wsh)
    {
      GNUNET_break (0);
      fail (is);
      return;
    }
  }
  if (0 == num_reserve_sigs)
    next_command (is);
}


/**
 * Find denomination key matching the given amount.
 *
 * @param keys array of keys to search
 * @param amount coin value to look for
 * @return NULL if no matching key was found
 */
static const struct TALER_EXCHANGE_DenomPublicKey *
find_pk (const struct TALER_EXCHANGE_Keys *keys,
         const struct TALER_Amount *amount)
{
  struct GNUNET_TIME_Absolute now;
  char *str;

  now = GNUNET_TIME_absolute_get ();
  for (unsigned int i=0;i<keys->num_denom_keys;i++)
  {
    const struct TALER_EXCHANGE_DenomPublicKey *pk;

    pk = &keys->denom_keys[i];
    if ( (0 == TALER_amount_cmp (amount,
                                 &pk->value)) &&
         (now.abs_value_us >= pk->valid_from.abs_value_us) &&
         (now.abs_value_us < pk->withdraw_valid_until.abs_value_us) )
      return pk;
  }
  /* do 2nd pass to check if expiration times are to blame for failure */
  str = TALER_amount_to_string (amount);
  for (unsigned int i=0;i<keys->num_denom_keys;i++)
  {
    const struct TALER_EXCHANGE_DenomPublicKey *pk;

    pk = &keys->denom_keys[i];
    if ( (0 == TALER_amount_cmp (amount,
                                 &pk->value)) &&
         ( (now.abs_value_us < pk->valid_from.abs_value_us) ||
           (now.abs_value_us > pk->withdraw_valid_until.abs_value_us) ) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Have denomination key for `%s', but with wrong expiration range %llu vs [%llu,%llu)\n",
                  str,
                  (unsigned long long) now.abs_value_us,
                  (unsigned long long) pk->valid_from.abs_value_us,
                  (unsigned long long) pk->withdraw_valid_until.abs_value_us);
      GNUNET_free (str);
      return NULL;
    }
  }
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "No denomination key for amount %s found\n",
              str);
  GNUNET_free (str);
  return NULL;
}


/**
 * Reset the interpreter's state.
 *
 * @param is interpreter to reset
 */
static void
cleanup_state (struct InterpreterState *is)
{
  struct Command *cmd;

  for (unsigned int i=0;OC_END != (cmd = &is->commands[i])->oc;i++)
  {
    switch (cmd->oc)
    {
    case OC_END:
      GNUNET_assert (0);
      break;
    case OC_PROPOSAL_LOOKUP:
      if (NULL != cmd->details.proposal_lookup.plo)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_proposal_lookup_cancel (cmd->details.proposal_lookup.plo);
      }
      break;
    case OC_ADMIN_ADD_INCOMING:
      if (NULL != cmd->details.admin_add_incoming.aih)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_BANK_admin_add_incoming_cancel (cmd->details.admin_add_incoming.aih);
        cmd->details.admin_add_incoming.aih = NULL;
      }
      break;
    case OC_WITHDRAW_STATUS:
      if (NULL != cmd->details.reserve_status.wsh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_EXCHANGE_reserve_status_cancel (cmd->details.reserve_status.wsh);
        cmd->details.reserve_status.wsh = NULL;
      }
      break;
    case OC_WITHDRAW_SIGN:
      if (NULL != cmd->details.reserve_withdraw.wsh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_EXCHANGE_reserve_withdraw_cancel (cmd->details.reserve_withdraw.wsh);
        cmd->details.reserve_withdraw.wsh = NULL;
      }
      if (NULL != cmd->details.reserve_withdraw.sig.rsa_signature)
      {
        GNUNET_CRYPTO_rsa_signature_free (cmd->details.reserve_withdraw.sig.rsa_signature);
        cmd->details.reserve_withdraw.sig.rsa_signature = NULL;
      }
      break;
    case OC_PROPOSAL:
      if (NULL != cmd->details.proposal.po)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete (proposal put)\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_proposal_cancel (cmd->details.proposal.po);
        cmd->details.proposal.po = NULL;
      }
      if (NULL != cmd->details.proposal.plo)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete (proposal lookup)\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_proposal_lookup_cancel (cmd->details.proposal.plo);
      }
      if (NULL != cmd->details.proposal.contract_terms)
      {
        json_decref (cmd->details.proposal.contract_terms);
        cmd->details.proposal.contract_terms = NULL;
      }
      break;
    case OC_PAY:
      if (NULL != cmd->details.pay.ph)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_pay_cancel (cmd->details.pay.ph);
        cmd->details.pay.ph = NULL;
      }
      break;
    case OC_PAY_AGAIN:
      if (NULL != cmd->details.pay_again.ph)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_pay_cancel (cmd->details.pay_again.ph);
        cmd->details.pay_again.ph = NULL;
      }
      break;
    case OC_PAY_ABORT:
      if (NULL != cmd->details.pay_abort.ph)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_pay_cancel (cmd->details.pay_abort.ph);
        cmd->details.pay_abort.ph = NULL;
      }
      GNUNET_array_grow (cmd->details.pay_abort.res,
			 cmd->details.pay_abort.num_refunds,
			 0);
      break;
    case OC_PAY_ABORT_REFUND:
      if (NULL != cmd->details.pay_abort_refund.rh)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
	TALER_EXCHANGE_refund_cancel (cmd->details.pay_abort_refund.rh);
	cmd->details.pay_abort_refund.rh = NULL;
      }
      break;
    case OC_RUN_AGGREGATOR:
      if (NULL != cmd->details.run_aggregator.aggregator_proc)
      {
        GNUNET_break (0 ==
                      GNUNET_OS_process_kill (cmd->details.run_aggregator.aggregator_proc,
                                              SIGKILL));
        GNUNET_OS_process_wait (cmd->details.run_aggregator.aggregator_proc);
        GNUNET_OS_process_destroy (cmd->details.run_aggregator.aggregator_proc);
        cmd->details.run_aggregator.aggregator_proc = NULL;
      }
      if (NULL != cmd->details.run_aggregator.child_death_task)
      {
        GNUNET_SCHEDULER_cancel (cmd->details.run_aggregator.child_death_task);
        cmd->details.run_aggregator.child_death_task = NULL;
      }
      break;
    case OC_RUN_WIREWATCH:
      if (NULL != cmd->details.run_wirewatch.wirewatch_proc)
      {
        GNUNET_break (0 ==
                      GNUNET_OS_process_kill (cmd->details.run_wirewatch.wirewatch_proc,
                                              SIGKILL));
        GNUNET_OS_process_wait (cmd->details.run_wirewatch.wirewatch_proc);
        GNUNET_OS_process_destroy (cmd->details.run_wirewatch.wirewatch_proc);
        cmd->details.run_wirewatch.wirewatch_proc = NULL;
      }
      if (NULL != cmd->details.run_wirewatch.child_death_task)
      {
        GNUNET_SCHEDULER_cancel (cmd->details.run_wirewatch.child_death_task);
        cmd->details.run_wirewatch.child_death_task = NULL;
      }
      break;
    case OC_CHECK_BANK_TRANSFER:
      GNUNET_free_non_null (cmd->details.check_bank_transfer.subject);
      cmd->details.check_bank_transfer.subject = NULL;
      break;
    case OC_CHECK_BANK_TRANSFERS_EMPTY:
      break;
    case OC_TRACK_TRANSFER:
      if (NULL != cmd->details.track_transfer.tdo)
      {
        TALER_MERCHANT_track_transfer_cancel (cmd->details.track_transfer.tdo);
        cmd->details.track_transfer.tdo = NULL;
      }
      break;
    case OC_TRACK_TRANSACTION:
      if (NULL != cmd->details.track_transaction.tth)
      {
        TALER_MERCHANT_track_transaction_cancel (cmd->details.track_transaction.tth);
        cmd->details.track_transaction.tth = NULL;
      }
      break;
    case OC_HISTORY:
      if (NULL != cmd->details.history.ho)
      {
        TALER_MERCHANT_history_cancel (cmd->details.history.ho);
        cmd->details.history.ho = NULL;
      }
      break;
    case OC_REFUND_INCREASE:
      if (NULL != cmd->details.refund_increase.rio)
      {
        TALER_MERCHANT_refund_increase_cancel (cmd->details.refund_increase.rio);
        cmd->details.refund_increase.rio = NULL;
      }
      break;
    case OC_REFUND_LOOKUP:
      if (NULL != cmd->details.refund_lookup.rlo)
      {
        TALER_MERCHANT_refund_lookup_cancel (cmd->details.refund_lookup.rlo);
        cmd->details.refund_lookup.rlo = NULL;
      }
      break;
    case OC_TIP_AUTHORIZE:
      if (NULL != cmd->details.tip_authorize.tao)
      {
        TALER_MERCHANT_tip_authorize_cancel (cmd->details.tip_authorize.tao);
        cmd->details.tip_authorize.tao = NULL;
      }
      break;
    case OC_TIP_PICKUP:
      if (NULL != cmd->details.tip_pickup.tpo)
      {
        TALER_MERCHANT_tip_pickup_cancel (cmd->details.tip_pickup.tpo);
        cmd->details.tip_pickup.tpo = NULL;
      }
      if (NULL != cmd->details.tip_pickup.psa)
      {
        GNUNET_free (cmd->details.tip_pickup.psa);
        cmd->details.tip_pickup.psa = NULL;
      }
      if (NULL != cmd->details.tip_pickup.dks)
      {
        GNUNET_free (cmd->details.tip_pickup.dks);
        cmd->details.tip_pickup.dks = NULL;
      }
      if (NULL != cmd->details.tip_pickup.withdraws)
      {
        for (unsigned int j=0;j<cmd->details.tip_pickup.num_coins;j++)
        {
          struct WithdrawHandle *wh = &cmd->details.tip_pickup.withdraws[j];

          if (NULL != wh->wsh)
          {
            TALER_EXCHANGE_reserve_withdraw_cancel (wh->wsh);
            wh->wsh = NULL;
          }
        }
        GNUNET_free (cmd->details.tip_pickup.withdraws);
        cmd->details.tip_pickup.withdraws = NULL;
      }
      if (NULL != cmd->details.tip_pickup.sigs)
      {
        for (unsigned int j=0;j<cmd->details.tip_pickup.num_coins;j++)
        {
          if (NULL != cmd->details.tip_pickup.sigs[j].rsa_signature)
          {
            GNUNET_CRYPTO_rsa_signature_free (cmd->details.tip_pickup.sigs[j].rsa_signature);
            cmd->details.tip_pickup.sigs[j].rsa_signature = NULL;
          }
        }
        GNUNET_free (cmd->details.tip_pickup.sigs);
        cmd->details.tip_pickup.sigs = NULL;
      }
      break;
    case OC_CHECK_PAYMENT:
      if (NULL != cmd->details.check_payment.cpo)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_check_payment_cancel (cmd->details.check_payment.cpo);
        cmd->details.check_payment.cpo = NULL;
      }
      break;
    case OC_TIP_QUERY:
      if (NULL != cmd->details.tip_query.tqo)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Command %u (%s) did not complete\n",
                    i,
                    cmd->label);
        TALER_MERCHANT_tip_query_cancel (cmd->details.tip_query.tqo);
        cmd->details.tip_query.tqo = NULL;
      }
      break;
    default:
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Shutdown: unknown instruction %d at %u (%s)\n",
                  cmd->oc,
                  i,
                  cmd->label);
      break;
    }
  }
}


/**
 * Parse the @a coins specification and grow the @a pc
 * array with the coins found, updating @a npc.
 *
 * @param[in,out] pc pointer to array of coins found
 * @param[in,out] npc length of array at @a pc
 * @param[in] coins string specifying coins to add to @a pc,
 *            clobbered in the process
 * @param is interpreter state
 * @param pref reference to the #OC_PAY command
 * @return #GNUNET_OK on success
 */
static int
build_coins (struct TALER_MERCHANT_PayCoin **pc,
	     unsigned int *npc,
	     char *coins,
	     struct InterpreterState *is,
	     const struct Command *pref)
{
  char *token;

  for (token = strtok (coins, ";");
       NULL != token;
       token = strtok (NULL, ";"))
  {
    const struct Command *coin_ref;
    char *ctok;
    unsigned int ci;
    struct TALER_MERCHANT_PayCoin *icoin;

    /* Token syntax is "LABEL[/NUMBER]" */
    ctok = strchr (token, '/');
    ci = 0;
    if (NULL != ctok)
    {
      *ctok = '\0';
      ctok++;
      if (1 != sscanf (ctok,
		       "%u",
		       &ci))
      {
	GNUNET_break (0);
	return GNUNET_SYSERR;
      }
    }
    GNUNET_assert (coin_ref = find_command (is,
					    token));
    GNUNET_array_grow (*pc,
		       *npc,
		       (*npc) + 1);
    icoin = &(*pc)[(*npc)-1];
    switch (coin_ref->oc)
    {
    case OC_WITHDRAW_SIGN:
      icoin->coin_priv = coin_ref->details.reserve_withdraw.ps.coin_priv;
      icoin->denom_pub = coin_ref->details.reserve_withdraw.pk->key;
      icoin->denom_sig = coin_ref->details.reserve_withdraw.sig;
      icoin->denom_value = coin_ref->details.reserve_withdraw.pk->value;
      icoin->exchange_url = EXCHANGE_URL;
      break;
    case OC_TIP_PICKUP:
      icoin->coin_priv = coin_ref->details.tip_pickup.psa[ci].coin_priv;
      icoin->denom_pub = coin_ref->details.tip_pickup.dks[ci]->key;
      icoin->denom_sig = coin_ref->details.tip_pickup.sigs[ci];
      icoin->denom_value = coin_ref->details.tip_pickup.dks[ci]->value;
      icoin->exchange_url = EXCHANGE_URL;
      break;
    default:
      GNUNET_assert (0);
    }

    GNUNET_assert (GNUNET_OK ==
		   TALER_string_to_amount (pref->details.pay.amount_with_fee,
					   &icoin->amount_with_fee));
    GNUNET_assert (GNUNET_OK ==
		   TALER_string_to_amount (pref->details.pay.amount_without_fee,
					   &icoin->amount_without_fee));
    GNUNET_assert (GNUNET_OK ==
		   TALER_string_to_amount (pref->details.pay.refund_fee,
					   &icoin->refund_fee));
  }
  return GNUNET_OK;
}


/**
 * Callbacks of this type are used to serve the result of submitting a
 * /pay request to a merchant.
 *
 * @param cls closure
 * @param http_status HTTP response code, 200 or 300-level response codes
 *                    can indicate success, depending on whether the interaction
 *                    was with a merchant frontend or backend;
 *                    0 if the merchant's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code
 * @param merchant_pub public key of the merchant
 * @param h_contract hash of the contract
 * @param num_refunds size of the @a merchant_sigs array, 0 on errors
 * @param res merchant signatures refunding coins, NULL on errors
 * @param obj the received JSON reply, with error details if the request failed
 */
static void
pay_refund_cb (void *cls,
	       unsigned int http_status,
	       enum TALER_ErrorCode ec,
	       const struct TALER_MerchantPublicKeyP *merchant_pub,
	       const struct GNUNET_HashCode *h_contract,
	       unsigned int num_refunds,
	       const struct TALER_MERCHANT_RefundEntry *res,
	       const json_t *obj)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.pay_abort.ph = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  if ( (MHD_HTTP_OK == http_status) &&
       (TALER_EC_NONE == ec) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Received %u refunds\n",
                num_refunds);
    cmd->details.pay_abort.num_refunds = num_refunds;
    cmd->details.pay_abort.res
      = GNUNET_new_array (num_refunds,
			  struct TALER_MERCHANT_RefundEntry);
    memcpy (cmd->details.pay_abort.res,
	    res,
	    num_refunds * sizeof (struct TALER_MERCHANT_RefundEntry));
    cmd->details.pay_abort.h_contract = *h_contract;
    cmd->details.pay_abort.merchant_pub = *merchant_pub;
  }
  next_command (is);
}


/**
 * Callbacks of this type are used to serve the result of submitting a
 * refund request to an exchange.
 *
 * @param cls closure
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful deposit;
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param sign_key exchange key used to sign @a obj, or NULL
 * @param obj the received JSON reply, should be kept as proof (and, in particular,
 *            be forwarded to the customer)
 */
static void
abort_refund_cb (void *cls,
		 unsigned int http_status,
		 enum TALER_ErrorCode ec,
		 const struct TALER_ExchangePublicKeyP *sign_key,
		 const json_t *obj)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.pay_abort_refund.rh = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                cmd->label);
    fail (is);
    return;
  }
  next_command (is);
}


/**
 * Run the main interpreter loop that performs exchange operations.
 *
 * @param cls contains the `struct InterpreterState`
 */
static void
interpreter_run (void *cls)
{
  const struct GNUNET_SCHEDULER_TaskContext *tc;
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];
  const struct Command *ref;
  struct TALER_ReservePublicKeyP reserve_pub;
  struct TALER_Amount amount;

  is->task = NULL;
  tc = GNUNET_SCHEDULER_get_task_context ();
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Test aborted by shutdown request\n");
    fail (is);
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Interpreter runs command %u/%s(%u)\n",
              is->ip,
              cmd->label,
              cmd->oc);
  switch (cmd->oc)
  {
  case OC_END:
    result = GNUNET_OK;
    if (instance_idx + 1 == ninstances)
    {
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    cleanup_state (is);
    is->ip = 0;
    instance_idx++;
    instance = instances[instance_idx];
    GNUNET_free_non_null (instance_priv);
    instance_priv = get_instance_priv (cfg, instance);
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Switching instance: `%s'\n",
                instance);
    is->task = GNUNET_SCHEDULER_add_now (interpreter_run,
                                         is);

      break;
  case OC_PROPOSAL_LOOKUP:
    {
      const char *order_id;

      GNUNET_assert (NULL != cmd->details.proposal_lookup.proposal_reference);
      GNUNET_assert (NULL != (ref =
                              find_command (is,
                                            cmd->details.proposal_lookup.proposal_reference)));

      order_id = json_string_value (json_object_get (ref->details.proposal.contract_terms,
                                                     "order_id"));

      if (NULL == (cmd->details.proposal_lookup.plo
                   = TALER_MERCHANT_proposal_lookup (ctx,
                                                     MERCHANT_URL,
                                                     order_id,
                                                     instance,
                                                     &ref->details.proposal.nonce,
                                                     proposal_lookup_cb,
                                                     is)))
      {
        GNUNET_break (0);
        fail (is);
      }
    }
    break;
  case OC_CHECK_PAYMENT:
    {
      const char *order_id;

      GNUNET_assert (NULL != cmd->details.check_payment.contract_ref);
      GNUNET_assert (NULL != (ref =
                              find_command (is,
                                            cmd->details.check_payment.contract_ref)));

      order_id = json_string_value (json_object_get (ref->details.proposal.contract_terms,
                                                     "order_id"));

      GNUNET_assert (NULL != order_id);

      if (NULL == (cmd->details.check_payment.cpo
                   = TALER_MERCHANT_check_payment (ctx,
                                                   MERCHANT_URL,
                                                   instance,
                                                   order_id,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   check_payment_cb,
                                                   is)))
      {
        GNUNET_break (0);
        fail (is);
      }
    }
    break;
  case OC_TIP_QUERY:
    {
      if (instance_idx !=  0)
      {
        // We check /tip-query only for the first instance,
        // since for tipping we use a dedicated instance.
        // On repeated runs, the expected authorized amounts wouldn't
        // match up (they would all accumulate!)
        next_command (is);
        break;
      }
      if (NULL == (cmd->details.tip_query.tqo
                   = TALER_MERCHANT_tip_query (ctx,
                                               MERCHANT_URL,
                                               cmd->details.tip_query.instance,
                                               tip_query_cb,
                                               is)))
      {
        GNUNET_break (0);
        fail (is);
      }
    }
    break;
  case OC_ADMIN_ADD_INCOMING:
    {
      char *subject;
      struct TALER_BANK_AuthenticationData auth;

      if (NULL !=
          cmd->details.admin_add_incoming.subject)
      {
        subject = GNUNET_strdup (cmd->details.admin_add_incoming.subject);
      }
      else
      {
        /* Use reserve public key as subject */
        if (NULL !=
            cmd->details.admin_add_incoming.reserve_reference)
        {
          GNUNET_assert (NULL != (ref
                                  = find_command (is,
                                                  cmd->details.admin_add_incoming.reserve_reference)));
          GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
          cmd->details.admin_add_incoming.reserve_priv
            = ref->details.admin_add_incoming.reserve_priv;
        }
        else if (NULL !=
                 cmd->details.admin_add_incoming.instance)
        {
          char *section;
          char *keys;
          struct GNUNET_CRYPTO_EddsaPrivateKey *pk;

          GNUNET_asprintf (&section,
                           "merchant-instance-%s",
                           cmd->details.admin_add_incoming.instance);
          if (GNUNET_OK !=
              GNUNET_CONFIGURATION_get_value_string (cfg,
                                                     section,
                                                     "TIP_RESERVE_PRIV_FILENAME",
                                                     &keys))
          {
            GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                        "Configuration fails to specify reserve private key filename in section %s\n",
                        section);
            GNUNET_free (section);
            fail (is);
            return;
          }
          pk = GNUNET_CRYPTO_eddsa_key_create_from_file (keys);
          if (NULL == pk)
          {
            GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                       section,
                                       "TIP_RESERVE_PRIV_FILENAME",
                                       "Failed to read private key");
            GNUNET_free (keys);
            GNUNET_free (section);
            fail (is);
            return;
          }
          cmd->details.admin_add_incoming.reserve_priv.eddsa_priv = *pk;
          GNUNET_free (pk);
          GNUNET_free (keys);
          GNUNET_free (section);
        }
        else
        {
          struct GNUNET_CRYPTO_EddsaPrivateKey *priv;

          priv = GNUNET_CRYPTO_eddsa_key_create ();
          cmd->details.admin_add_incoming.reserve_priv.eddsa_priv = *priv;
          GNUNET_free (priv);
        }
        GNUNET_CRYPTO_eddsa_key_get_public (&cmd->details.admin_add_incoming.reserve_priv.eddsa_priv,
                                            &reserve_pub.eddsa_pub);
        subject = GNUNET_STRINGS_data_to_string_alloc (&reserve_pub,
                                                       sizeof (reserve_pub));
      }

      GNUNET_CRYPTO_eddsa_key_get_public (&cmd->details.admin_add_incoming.reserve_priv.eddsa_priv,
                                          &reserve_pub.eddsa_pub);
      GNUNET_assert (GNUNET_OK ==
                     TALER_string_to_amount (cmd->details.admin_add_incoming.amount,
                                             &amount));
      auth.method = TALER_BANK_AUTH_BASIC;
      auth.details.basic.username = (char *) cmd->details.admin_add_incoming.auth_username;
      auth.details.basic.password = (char *) cmd->details.admin_add_incoming.auth_password;
      cmd->details.admin_add_incoming.aih
        = TALER_BANK_admin_add_incoming (ctx,
                                         BANK_URL,
                                         &auth,
                                         EXCHANGE_URL,
                                         subject,
                                         &amount,
                                         cmd->details.admin_add_incoming.debit_account_no,
                                         cmd->details.admin_add_incoming.credit_account_no,
                                         &add_incoming_cb,
                                         is);
      GNUNET_free (subject);
      if (NULL == cmd->details.admin_add_incoming.aih)
      {
        GNUNET_break (0);
        fail (is);
      }
      break;
    }
  case OC_WITHDRAW_STATUS:
    GNUNET_assert (NULL !=
                   cmd->details.reserve_status.reserve_reference);
    GNUNET_assert (NULL != (ref = find_command
      (is,
       cmd->details.reserve_status.reserve_reference)));
    GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
    GNUNET_CRYPTO_eddsa_key_get_public (&ref->details.admin_add_incoming.reserve_priv.eddsa_priv,
                                        &reserve_pub.eddsa_pub);
    if (NULL == (cmd->details.reserve_status.wsh
        = TALER_EXCHANGE_reserve_status (exchange,
                                         &reserve_pub,
                                         &reserve_status_cb,
                                         is)))
    {
      GNUNET_break (0);
      fail (is);
    }
    break;
  case OC_WITHDRAW_SIGN:
    GNUNET_assert (NULL != cmd->details.reserve_withdraw.reserve_reference);
    GNUNET_assert (NULL != (ref = find_command
      (is,
       cmd->details.reserve_withdraw.reserve_reference)));
    GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
    GNUNET_assert (NULL != cmd->details.reserve_withdraw.amount);
    GNUNET_assert (GNUNET_OK ==
      TALER_string_to_amount (cmd->details.reserve_withdraw.amount,
                              &amount));
    GNUNET_assert (NULL != (cmd->details.reserve_withdraw.pk
      = find_pk (is->keys,
                 &amount)));

    TALER_planchet_setup_random (&cmd->details.reserve_withdraw.ps);
    cmd->details.reserve_withdraw.wsh
      = TALER_EXCHANGE_reserve_withdraw
        (exchange,
         cmd->details.reserve_withdraw.pk,
         &ref->details.admin_add_incoming.reserve_priv,
         &cmd->details.reserve_withdraw.ps,
         &reserve_withdraw_cb,
         is);
    if (NULL == cmd->details.reserve_withdraw.wsh)
    {
      GNUNET_break (0);
      fail (is);
    }
    break;
  case OC_PROPOSAL:
    {
      json_t *order;
      json_error_t error;

      GNUNET_assert (NULL != (order = json_loads (cmd->details.proposal.order,
                                                  JSON_REJECT_DUPLICATES,
                                                  &error)));
      GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                  &cmd->details.proposal.nonce,
                                  sizeof (struct GNUNET_CRYPTO_EddsaPublicKey));

      if (NULL != instance)
      {
        json_t *merchant;

        merchant = json_object ();
        json_object_set_new (merchant,
                             "instance",
                             json_string (instance));
        json_object_set_new (order,
                             "merchant",
                             merchant);
      }
      cmd->details.proposal.po = TALER_MERCHANT_order_put (ctx,
                                                           MERCHANT_URL,
                                                           order,
                                                           &proposal_cb,
                                                           is);
      json_decref (order);
      if (NULL == cmd->details.proposal.po)
        {
          GNUNET_break (0);
          fail (is);
        }
      break;
    }
  case OC_PAY:
    {
      struct TALER_MERCHANT_PayCoin *pc;
      unsigned int npc;
      char *coins;
      const char *order_id;
      struct GNUNET_TIME_Absolute refund_deadline;
      struct GNUNET_TIME_Absolute pay_deadline;
      struct GNUNET_TIME_Absolute timestamp;
      struct GNUNET_HashCode h_wire;
      struct TALER_MerchantPublicKeyP merchant_pub;
      struct TALER_MerchantSignatureP merchant_sig;
      struct TALER_Amount total_amount;
      struct TALER_Amount max_fee;
      const char *error_name;
      unsigned int error_line;

      /* get proposal */
      GNUNET_assert (NULL != (ref = find_command
                              (is,
                               cmd->details.pay.contract_ref)));
      merchant_sig = ref->details.proposal.merchant_sig;
      GNUNET_assert (NULL != ref->details.proposal.contract_terms);
      {
        /* Get information that needs to be replied in the deposit permission */
        struct GNUNET_JSON_Specification spec[] = {
          GNUNET_JSON_spec_string ("order_id",
				   &order_id),
          GNUNET_JSON_spec_absolute_time ("refund_deadline",
					  &refund_deadline),
          GNUNET_JSON_spec_absolute_time ("pay_deadline",
					  &pay_deadline),
          GNUNET_JSON_spec_absolute_time ("timestamp",
					  &timestamp),
          GNUNET_JSON_spec_fixed_auto ("merchant_pub",
				       &merchant_pub),
          GNUNET_JSON_spec_fixed_auto ("H_wire",
				       &h_wire),
          TALER_JSON_spec_amount ("amount",
				  &total_amount),
          TALER_JSON_spec_amount ("max_fee",
				  &max_fee),
          GNUNET_JSON_spec_end()
        };

        if (GNUNET_OK !=
            GNUNET_JSON_parse (ref->details.proposal.contract_terms,
                               spec,
                               &error_name,
                               &error_line))
        {
          GNUNET_break_op (0);
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Parser failed on %s:%u\n",
                      error_name,
                      error_line);
          /**
           * Let's use fail() here, as the proposal might be broken
           * because of backend's fault.
           */
          fail (is);
          return;
        }
        cmd->details.pay.merchant_pub = merchant_pub;
      }
      /* strtok loop here */
      coins = GNUNET_strdup (cmd->details.pay.coin_ref);
      pc = NULL;
      npc = 0;
      if (GNUNET_OK !=
	  build_coins (&pc,
		       &npc,
		       coins,
		       is,
		       cmd))
      {
	fail (is);
	GNUNET_array_grow (pc,
			   npc,
			   0);
	GNUNET_free (coins);
	return;
      }
      GNUNET_free (coins);

      cmd->details.pay.ph = TALER_MERCHANT_pay_wallet
        (ctx,
         MERCHANT_URL,
         instance,
         &ref->details.proposal.hash,
         &total_amount,
         &max_fee,
         &merchant_pub,
         &merchant_sig,
         timestamp,
         refund_deadline,
         pay_deadline,
         &h_wire,
         order_id,
         npc /* num_coins */,
         pc /* coins */,
         &pay_cb,
         is);
      GNUNET_array_grow (pc,
                         npc,
                         0);
    }
    if (NULL == cmd->details.pay.ph)
    {
      GNUNET_break (0);
      fail (is);
    }
    break;

  case OC_PAY_AGAIN:
    {
      struct TALER_MERCHANT_PayCoin *pc;
      const struct Command *pref;
      unsigned int npc;
      char *coins;
      const char *order_id;
      struct GNUNET_TIME_Absolute refund_deadline;
      struct GNUNET_TIME_Absolute pay_deadline;
      struct GNUNET_TIME_Absolute timestamp;
      struct GNUNET_HashCode h_wire;
      struct TALER_MerchantPublicKeyP merchant_pub;
      struct TALER_MerchantSignatureP merchant_sig;
      struct TALER_Amount total_amount;
      struct TALER_Amount max_fee;
      const char *error_name;
      unsigned int error_line;

      /* Get original /pay command */
      GNUNET_assert (NULL != (pref = find_command
                              (is,
                               cmd->details.pay_again.pay_ref)));
      /* get proposal */
      GNUNET_assert (NULL != (ref = find_command
                              (is,
                               pref->details.pay.contract_ref)));
      merchant_sig = ref->details.proposal.merchant_sig;
      GNUNET_assert (NULL != ref->details.proposal.contract_terms);
      {
        /* Get information that needs to be replied in the deposit permission */
        struct GNUNET_JSON_Specification spec[] = {
          GNUNET_JSON_spec_string ("order_id",
				   &order_id),
          GNUNET_JSON_spec_absolute_time ("refund_deadline",
					  &refund_deadline),
          GNUNET_JSON_spec_absolute_time ("pay_deadline",
					  &pay_deadline),
          GNUNET_JSON_spec_absolute_time ("timestamp",
					  &timestamp),
          GNUNET_JSON_spec_fixed_auto ("merchant_pub",
				       &merchant_pub),
          GNUNET_JSON_spec_fixed_auto ("H_wire",
				       &h_wire),
          TALER_JSON_spec_amount ("amount",
				  &total_amount),
          TALER_JSON_spec_amount ("max_fee",
				  &max_fee),
          GNUNET_JSON_spec_end()
        };

        if (GNUNET_OK !=
            GNUNET_JSON_parse (ref->details.proposal.contract_terms,
                               spec,
                               &error_name,
                               &error_line))
        {
          GNUNET_break_op (0);
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Parser failed on %s:%u\n",
                      error_name,
                      error_line);
          /**
           * Let's use fail() here, as the proposal might be broken
           * because of backend's fault.
           */
          fail (is);
          return;
        }
      }
      pc = NULL;
      npc = 0;
      /* Loop over coins from pay again */
      coins = GNUNET_strdup (cmd->details.pay_again.coin_ref);
      if (GNUNET_OK !=
	  build_coins (&pc,
		       &npc,
		       coins,
		       is,
		       pref))
      {
	fail (is);
	GNUNET_array_grow (pc,
			   npc,
			   0);
	GNUNET_free (coins);
	return;
      }
      GNUNET_free (coins);
      /* then repeat payment attempt */
      cmd->details.pay_again.ph = TALER_MERCHANT_pay_wallet
        (ctx,
         MERCHANT_URL,
         instance,
         &ref->details.proposal.hash,
         &total_amount,
         &max_fee,
         &merchant_pub,
         &merchant_sig,
         timestamp,
         refund_deadline,
         pay_deadline,
         &h_wire,
         order_id,
         npc /* num_coins */,
         pc /* coins */,
         &pay_again_cb,
         is);
      GNUNET_array_grow (pc,
                         npc,
                         0);
    }
    if (NULL == cmd->details.pay_again.ph)
    {
      GNUNET_break (0);
      fail (is);
    }
    break;
  case OC_PAY_ABORT:
    {
      struct TALER_MERCHANT_PayCoin *pc;
      const struct Command *pref;
      unsigned int npc;
      char *coins;
      const char *order_id;
      struct GNUNET_TIME_Absolute refund_deadline;
      struct GNUNET_TIME_Absolute pay_deadline;
      struct GNUNET_TIME_Absolute timestamp;
      struct GNUNET_HashCode h_wire;
      struct TALER_MerchantPublicKeyP merchant_pub;
      struct TALER_MerchantSignatureP merchant_sig;
      struct TALER_Amount total_amount;
      struct TALER_Amount max_fee;
      const char *error_name;
      unsigned int error_line;

      /* Get original /pay command */
      GNUNET_assert (NULL != (pref = find_command
                              (is,
                               cmd->details.pay_abort.pay_ref)));
      /* get proposal */
      GNUNET_assert (NULL != (ref = find_command
                              (is,
                               pref->details.pay.contract_ref)));
      merchant_sig = ref->details.proposal.merchant_sig;
      GNUNET_assert (NULL != ref->details.proposal.contract_terms);
      {
        /* Get information that needs to be replied in the deposit permission */
        struct GNUNET_JSON_Specification spec[] = {
          GNUNET_JSON_spec_string ("order_id",
				   &order_id),
          GNUNET_JSON_spec_absolute_time ("refund_deadline",
					  &refund_deadline),
          GNUNET_JSON_spec_absolute_time ("pay_deadline",
					  &pay_deadline),
          GNUNET_JSON_spec_absolute_time ("timestamp",
					  &timestamp),
          GNUNET_JSON_spec_fixed_auto ("merchant_pub",
				       &merchant_pub),
          GNUNET_JSON_spec_fixed_auto ("H_wire",
				       &h_wire),
          TALER_JSON_spec_amount ("amount",
				  &total_amount),
          TALER_JSON_spec_amount ("max_fee",
				  &max_fee),
          GNUNET_JSON_spec_end()
        };

        if (GNUNET_OK !=
            GNUNET_JSON_parse (ref->details.proposal.contract_terms,
                               spec,
                               &error_name,
                               &error_line))
        {
          GNUNET_break_op (0);
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Parser failed on %s:%u\n",
                      error_name,
                      error_line);
          /**
           * Let's use fail() here, as the proposal might be broken
           * because of backend's fault.
           */
          fail (is);
          return;
        }
      }
      /* strtok loop over original coins here */
      pc = NULL;
      npc = 0;
      coins = GNUNET_strdup (pref->details.pay.coin_ref);
      if (GNUNET_OK !=
	  build_coins (&pc,
		       &npc,
		       coins,
		       is,
		       pref))
      {
	fail (is);
	GNUNET_array_grow (pc,
			   npc,
			   0);
	GNUNET_free (coins);
	return;
      }
      GNUNET_free (coins);
      /* then trigger abort-refund operation */
      cmd->details.pay_abort.ph = TALER_MERCHANT_pay_abort
        (ctx,
         MERCHANT_URL,
         instance,
         &ref->details.proposal.hash,
         &total_amount,
         &max_fee,
         &merchant_pub,
         &merchant_sig,
         timestamp,
         refund_deadline,
         pay_deadline,
         &h_wire,
         order_id,
         npc /* num_coins */,
         pc /* coins */,
         &pay_refund_cb,
         is);
      GNUNET_array_grow (pc,
                         npc,
                         0);
    }
    if (NULL == cmd->details.pay_abort.ph)
    {
      GNUNET_break (0);
      fail (is);
    }
    break;
  case OC_PAY_ABORT_REFUND:
    {
      struct TALER_Amount refund_fee;
      const struct TALER_MERCHANT_RefundEntry *re;

      /* Get original /pay command */
      GNUNET_assert (NULL != (ref = find_command
                              (is,
                               cmd->details.pay_abort_refund.abort_ref)));
      GNUNET_assert (OC_PAY_ABORT == ref->oc);
      GNUNET_assert (ref->details.pay_abort.num_refunds >
		     cmd->details.pay_abort_refund.num_coin);
      re = &ref->details.pay_abort.res[cmd->details.pay_abort_refund.num_coin];

      GNUNET_assert (GNUNET_OK ==
		     TALER_string_to_amount
		     (cmd->details.pay_abort_refund.refund_amount,
		      &amount));
      GNUNET_assert (GNUNET_OK ==
		     TALER_string_to_amount
		     (cmd->details.pay_abort_refund.refund_fee,
		      &refund_fee));
      cmd->details.pay_abort_refund.rh
	= TALER_EXCHANGE_refund2 (exchange,
				  &amount,
				  &refund_fee,
				  &ref->details.pay_abort.h_contract,
				  &re->coin_pub,
				  re->rtransaction_id,
				  &ref->details.pay_abort.merchant_pub,
				  &re->merchant_sig,
				  &abort_refund_cb,
				  is);
      if (NULL == cmd->details.pay_abort_refund.rh)
      {
	GNUNET_break (0);
	fail (is);
      }
    }
    break;
  case OC_RUN_AGGREGATOR:
    {
      const struct GNUNET_DISK_FileHandle *pr;

      GNUNET_assert (NULL != (cmd->details.run_aggregator.aggregator_proc
        = GNUNET_OS_start_process (GNUNET_NO,
                                   GNUNET_OS_INHERIT_STD_ALL,
                                   NULL, NULL, NULL,
                                   "taler-exchange-aggregator",
                                   "taler-exchange-aggregator",
                                   "-c", "test_merchant_api.conf",
                                   "-t", /* exit when done */
                                   NULL)));
      pr = GNUNET_DISK_pipe_handle (sigpipe, GNUNET_DISK_PIPE_END_READ);
      cmd->details.run_aggregator.child_death_task
        = GNUNET_SCHEDULER_add_read_file (GNUNET_TIME_UNIT_FOREVER_REL,
                                          pr,
                                          &maint_child_death, is);
    }
    break;
  case OC_RUN_WIREWATCH:
    {
      const struct GNUNET_DISK_FileHandle *pr;
      static int once;

      cmd->details.run_wirewatch.wirewatch_proc
        = GNUNET_OS_start_process (GNUNET_NO,
                                   GNUNET_OS_INHERIT_STD_ALL,
                                   NULL, NULL, NULL,
                                   "taler-exchange-wirewatch",
                                   "taler-exchange-wirewatch",
                                   "-c", "test_merchant_api.conf",
                                   "-t", "test", /* use Taler's bank/fakebank */
                                   "-T", /* exit when done */
                                   (0 == once ? "-r" : NULL),
                                   NULL);
      if (NULL == cmd->details.run_wirewatch.wirewatch_proc)
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      once = 1;
      pr = GNUNET_DISK_pipe_handle (sigpipe,
                                    GNUNET_DISK_PIPE_END_READ);
      cmd->details.run_wirewatch.child_death_task
        = GNUNET_SCHEDULER_add_read_file (GNUNET_TIME_UNIT_FOREVER_REL,
                                          pr,
                                          &maint_child_death, is);
      return;
    }
  case OC_CHECK_BANK_TRANSFER:
    {
      GNUNET_assert (GNUNET_OK ==
		     TALER_string_to_amount
		     (cmd->details.check_bank_transfer.amount,
		      &amount));
      if (GNUNET_OK != TALER_FAKEBANK_check
           (fakebank,
            &amount,
            cmd->details.check_bank_transfer.account_debit,
            cmd->details.check_bank_transfer.account_credit,
            EXCHANGE_URL,
            &cmd->details.check_bank_transfer.subject))
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      next_command (is);
      return;
    }
  case OC_CHECK_BANK_TRANSFERS_EMPTY:
    {
      if (GNUNET_OK !=
          TALER_FAKEBANK_check_empty (fakebank))
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      next_command (is);
      return;
    }
  case OC_TRACK_TRANSFER:
    {
      struct TALER_WireTransferIdentifierRawP wtid;
      const char *subject;

      GNUNET_assert (NULL != ( ref = find_command
        (is,
         cmd->details.track_transfer.check_bank_ref)));
      subject = ref->details.check_bank_transfer.subject;
      GNUNET_assert (GNUNET_OK ==
		     GNUNET_STRINGS_string_to_data (subject,
						    strlen (subject),
						    &wtid,
						    sizeof (wtid)));
      if (NULL == (cmd->details.track_transfer.tdo
          = TALER_MERCHANT_track_transfer (ctx,
                                           MERCHANT_URL,
                                           instance,
					   "test",
                                           &wtid,
                                           EXCHANGE_URL,
                                           &track_transfer_cb,
                                           is)))
      {
        GNUNET_break (0);
        fail (is);
      }
    }
    return;
  case OC_TRACK_TRANSACTION:
  {
    const struct Command *proposal_ref;
    const char *order_id;

    GNUNET_assert(NULL != (ref = find_command
      (is,
       cmd->details.track_transaction.pay_ref)));
    GNUNET_assert (NULL != (proposal_ref = find_command
      (is,
       ref->details.pay.contract_ref)));
    order_id = json_string_value
      (json_object_get (proposal_ref->details.proposal.contract_terms,
                        "order_id"));

    if (NULL == (cmd->details.track_transaction.tth
        = TALER_MERCHANT_track_transaction (ctx,
                                            MERCHANT_URL,
                                            instance,
                                            order_id,
                                            &track_transaction_cb,
                                            is)))
    {
      GNUNET_break (0);
      fail (is);
    }
    return;
  }
  case OC_HISTORY:
    if (0 == cmd->details.history.date.abs_value_us)
    {
      cmd->details.history.date = GNUNET_TIME_absolute_add
        (GNUNET_TIME_absolute_get (),
         GNUNET_TIME_UNIT_HOURS);
      GNUNET_TIME_round_abs (&cmd->details.history.date);
    }
    if (NULL == (cmd->details.history.ho
        = TALER_MERCHANT_history (ctx,
    	                          MERCHANT_URL,
                                  instance,
                                  cmd->details.history.start,
                                  cmd->details.history.nrows,
    	                          cmd->details.history.date,
    	                          &history_cb,
    				  is)))
    {
      GNUNET_break (0);
      fail (is);
    }
    break;
  case OC_REFUND_INCREASE:
    {
      struct TALER_Amount refund_amount;

      GNUNET_assert (GNUNET_OK ==
                     TALER_string_to_amount
                     (cmd->details.refund_increase.refund_amount,
                      &refund_amount));
      if (NULL == (cmd->details.refund_increase.rio
                   = TALER_MERCHANT_refund_increase
                   (ctx,
                    MERCHANT_URL,
                    cmd->details.refund_increase.order_id,
                    &refund_amount,
                    cmd->details.refund_increase.reason,
                    instance,
                    refund_increase_cb,
                    is)))
        {
          GNUNET_break (0);
          fail (is);
        }
      break;
    }
  case OC_REFUND_LOOKUP:
    {
      if (NULL == (cmd->details.refund_lookup.rlo
                   = TALER_MERCHANT_refund_lookup
                   (ctx,
                    MERCHANT_URL,
                    cmd->details.refund_lookup.order_id,
                    instance,
                    refund_lookup_cb,
                    is)))
      {
        GNUNET_break (0);
        fail (is);
      }
      break;
    }
  case OC_TIP_AUTHORIZE:
    {
      GNUNET_assert (NULL != cmd->details.tip_authorize.amount);
      GNUNET_assert (GNUNET_OK ==
                     TALER_string_to_amount (cmd->details.tip_authorize.amount,
                                             &amount));
      if (NULL == (cmd->details.tip_authorize.tao
                   = TALER_MERCHANT_tip_authorize
                   (ctx,
                    MERCHANT_URL,
                    "http://merchant.com/pickup",
                    "http://merchant.com/continue",
                    &amount,
                    cmd->details.tip_authorize.instance,
                    cmd->details.tip_authorize.justification,
                    &tip_authorize_cb,
                    is)))
      {
        GNUNET_break (0);
        fail (is);
      }
      break;
    }
  case OC_TIP_PICKUP:
    {
      unsigned int num_planchets;
      const struct Command *rr;

      if (NULL == cmd->details.tip_pickup.replay_ref)
      {
        rr = NULL;
        for (num_planchets=0;
             NULL != cmd->details.tip_pickup.amounts[num_planchets];
             num_planchets++);
        ref = find_command (is,
                            cmd->details.tip_pickup.authorize_ref);
        GNUNET_assert (NULL != ref);
        GNUNET_assert (OC_TIP_AUTHORIZE == ref->oc);
      }
      else
      {
        rr = find_command (is,
                           cmd->details.tip_pickup.replay_ref);
        GNUNET_assert (NULL != rr);
        GNUNET_assert (OC_TIP_PICKUP == rr->oc);
        num_planchets = rr->details.tip_pickup.num_coins;
        ref = find_command (is,
                            rr->details.tip_pickup.authorize_ref);
        GNUNET_assert (NULL != ref);
        GNUNET_assert (OC_TIP_AUTHORIZE == ref->oc);
      }
      cmd->details.tip_pickup.num_coins = num_planchets;
      {
        struct TALER_PlanchetDetail planchets[num_planchets];

        cmd->details.tip_pickup.psa = GNUNET_new_array (num_planchets,
                                                        struct TALER_PlanchetSecretsP);
        cmd->details.tip_pickup.dks = GNUNET_new_array (num_planchets,
                                                        const struct TALER_EXCHANGE_DenomPublicKey *);
        for (unsigned int i=0;i<num_planchets;i++)
        {
          if (NULL == rr)
          {
            GNUNET_assert (GNUNET_OK ==
                           TALER_string_to_amount (cmd->details.tip_pickup.amounts[i],
                                                   &amount));
            cmd->details.tip_pickup.dks[i]
              = find_pk (is->keys,
                         &amount);
            if (NULL == cmd->details.tip_pickup.dks[i])
            {
              GNUNET_break (0);
              fail (is);
              return;
            }
            TALER_planchet_setup_random (&cmd->details.tip_pickup.psa[i]);
          }
          else
          {
            cmd->details.tip_pickup.dks[i]
              = rr->details.tip_pickup.dks[i];
            cmd->details.tip_pickup.psa[i]
              = rr->details.tip_pickup.psa[i];
          }
          if (GNUNET_OK !=
              TALER_planchet_prepare (&cmd->details.tip_pickup.dks[i]->key,
                                      &cmd->details.tip_pickup.psa[i],
                                      &planchets[i]))
          {
            GNUNET_break (0);
            fail (is);
            return;
          }
        }
        if (NULL == (cmd->details.tip_pickup.tpo
                     = TALER_MERCHANT_tip_pickup (ctx,
                                                  MERCHANT_URL,
                                                  &ref->details.tip_authorize.tip_id,
                                                  num_planchets,
                                                  planchets,
                                                  &pickup_cb,
                                                  is)))
        {
          GNUNET_break (0);
          fail (is);
        }
        for (unsigned int i=0;i<num_planchets;i++)
          GNUNET_free (planchets[i].coin_ev);
      }
      break;
    }
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown instruction %d at %u (%s)\n",
                cmd->oc,
                is->ip,
                cmd->label);
    fail (is);
    return;
  }
}


/**
 * Function run when the test times out.
 *
 * @param cls NULL
 */
static void
do_timeout (void *cls)
{
  timeout_task = NULL;
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Function run when the test terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls the interpreter state.
 */
static void
do_shutdown (void *cls)
{
  struct InterpreterState *is = cls;

  if (NULL != timeout_task)
  {
    GNUNET_SCHEDULER_cancel (timeout_task);
    timeout_task = NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Shutdown executing\n");
  cleanup_state (is);
  if (NULL != instance_priv)
  {
    GNUNET_free (instance_priv);
    instance_priv = NULL;
  }
  if (NULL != is->task)
  {
    GNUNET_SCHEDULER_cancel (is->task);
    is->task = NULL;
  }
  GNUNET_free (is);
  for (unsigned int i=0;i<ninstances;i++)
    GNUNET_free (instances[i]);
  GNUNET_free_non_null (instances);
  if (NULL != exchange)
  {
    TALER_EXCHANGE_disconnect (exchange);
    exchange = NULL;
  }
  if (NULL != ctx)
  {
    GNUNET_CURL_fini (ctx);
    ctx = NULL;
  }
  if (NULL != rc)
  {
    GNUNET_CURL_gnunet_rc_destroy (rc);
    rc = NULL;
  }
  TALER_FAKEBANK_stop (fakebank);
  fakebank = NULL;
  db->drop_tables (db->cls);
  TALER_MERCHANTDB_plugin_unload (db);
  GNUNET_CONFIGURATION_destroy (cfg);
}


/**
 * Functions of this type are called to provide the retrieved signing and
 * denomination keys of the exchange.  No TALER_EXCHANGE_*() functions should be called
 * in this callback.
 *
 * @param cls closure
 * @param keys information about keys of the exchange
 * @param vc compatibility information
 */
static void
cert_cb (void *cls,
         const struct TALER_EXCHANGE_Keys *keys,
	 enum TALER_EXCHANGE_VersionCompatibility vc)
{
  struct InterpreterState *is = cls;

  /* check that keys is OK */
#define ERR(cond) do { if(!(cond)) break; GNUNET_break (0); GNUNET_SCHEDULER_shutdown(); return; } while (0)
  ERR (NULL == keys);
  ERR (0 == keys->num_sign_keys);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
          "Read %u signing keys\n",
          keys->num_sign_keys);
  ERR (0 == keys->num_denom_keys);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
          "Read %u denomination keys\n",
          keys->num_denom_keys);
#undef ERR

  /* run actual tests via interpreter-loop */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Certificate callback invoked, starting interpreter\n");
  is->keys = keys;
  is->task = GNUNET_SCHEDULER_add_now (&interpreter_run,
                                       is);
}


/**
 * Signal handler called for SIGCHLD.  Triggers the
 * respective handler by writing to the trigger pipe.
 */
static void
sighandler_child_death ()
{
  static char c;
  int old_errno = errno;	/* back-up errno */

  GNUNET_break (1 == GNUNET_DISK_file_write
    (GNUNET_DISK_pipe_handle (sigpipe,
                              GNUNET_DISK_PIPE_END_WRITE),
                              &c,
                              sizeof (c)));
  errno = old_errno; /* restore errno */
}

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 */
static void
run (void *cls)
{
  struct InterpreterState *is;
  static const char *pickup_amounts_1[] = {
    "EUR:5",
    NULL
  };
  static struct Command commands[] =
  {
    /* Fill reserve with EUR:5.01, as withdraw fee is 1 ct per
       config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.debit_account_no = 62,
      .details.admin_add_incoming.credit_account_no = EXCHANGE_ACCOUNT_NO,
      .details.admin_add_incoming.auth_username = "user62",
      .details.admin_add_incoming.auth_password = "pass62",
      .details.admin_add_incoming.amount = "EUR:10.02" },
    /* Run wirewatch to observe /admin/add/incoming */
    { .oc = OC_RUN_WIREWATCH,
      .label = "wirewatch-1" },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-1",
      .details.check_bank_transfer.amount = "EUR:10.02",
      .details.check_bank_transfer.account_debit = 62,
      .details.check_bank_transfer.account_credit = EXCHANGE_ACCOUNT_NO
    },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-1",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-1",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Check that deposit and withdraw operation are in history,
       and that the balance is now at zero */
    { .oc = OC_WITHDRAW_STATUS,
      .label = "withdraw-status-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_status.reserve_reference
        = "create-reserve-1",
      .details.reserve_status.expected_balance = "EUR:0" },
    /* Create proposal */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
        \"max_fee\":\
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
        \"products\":\
          [ {\"description\":\"ice cream\",\
             \"value\":\"{EUR:5}\"} ] }"},

    /* check payment before we pay */
    { .oc = OC_CHECK_PAYMENT,
      .label = "check-payment-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.check_payment.contract_ref = "create-proposal-1",
      .details.check_payment.expect_paid = GNUNET_NO },

    /* execute simple payment */
    { .oc = OC_PAY,
      .label = "deposit-simple",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-1",
      .details.pay.coin_ref = "withdraw-coin-1",
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },

    /* check payment after we've paid */
    { .oc = OC_CHECK_PAYMENT,
      .label = "check-payment-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.check_payment.contract_ref = "create-proposal-1",
      .details.check_payment.expect_paid = GNUNET_YES },

    /* Test for #5262: abort after full payment */
    { .oc = OC_PAY_ABORT,
      .label = "pay-abort-2",
      .expected_response_code = MHD_HTTP_FORBIDDEN,
      .details.pay_abort.pay_ref = "deposit-simple",
    },

    /* Try to replay payment reusing coin */
    { .oc = OC_PAY,
      .label = "replay-simple",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-1",
      .details.pay.coin_ref = "withdraw-coin-1",
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },

    /* Create another contract */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
        \"max_fee\":\
          {\"currency\":\"EUR\",\
           \"value\":0,\
           \"fraction\":50000000},\
        \"order_id\":\"2\",\
        \"refund_deadline\":\"\\/Date(0)\\/\",\
        \"pay_deadline\":\"\\/Date(9999999999)\\/\",\
        \"amount\":\
          {\"currency\":\"EUR\",\
           \"value\":5,\
           \"fraction\":0},\
        \"summary\":\"useful product\",\
        \"products\":\
          [ {\"description\":\"ice cream\",\
             \"value\":\"{EUR:5}\"} ] }" },
    /**
     * Try to double-spend the 5 EUR coin at the same
     * merchant (but different transaction ID)
     */
    { .oc = OC_PAY,
      .label = "deposit-double-2",
      .expected_response_code = MHD_HTTP_FORBIDDEN,
      .details.pay.contract_ref = "create-proposal-2",
      .details.pay.coin_ref = "withdraw-coin-1",
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },

    { .oc = OC_HISTORY,
      .label = "history-0",
      .expected_response_code = MHD_HTTP_OK,
      /**
       * all records to be returned; setting date as 0 lets the
       * interpreter set it as 'now' + one hour delta, just to
       * make sure it surpasses the proposal's timestamp.
       */
      .details.history.date.abs_value_us = 0,
      /**
       * We only expect ONE result (create-proposal-1) to be
       * included in /history response, because create-proposal-3
       * did NOT go through because of double spending.
       */
      .details.history.nresult = 1,
      .details.history.start = 10,
      .details.history.nrows = 10
    },

    /* Fill second reserve with EUR:1 */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.debit_account_no = 63,
      .details.admin_add_incoming.credit_account_no = EXCHANGE_ACCOUNT_NO,
      .details.admin_add_incoming.auth_username = "user63",
      .details.admin_add_incoming.auth_password = "pass63",
      .details.admin_add_incoming.amount = "EUR:1" },

    /* Add another 4.01 EUR to reserve #2 */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-2b",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.reserve_reference = "create-reserve-2",
      .details.admin_add_incoming.debit_account_no = 63,
      .details.admin_add_incoming.credit_account_no = EXCHANGE_ACCOUNT_NO,
      .details.admin_add_incoming.auth_username = "user63",
      .details.admin_add_incoming.auth_password = "pass63",
      .details.admin_add_incoming.amount = "EUR:4.01" },
    /* Run wirewatch to observe /admin/add/incoming */
    { .oc = OC_RUN_WIREWATCH,
      .label = "wirewatch-2" },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-2",
      .details.check_bank_transfer.amount = "EUR:1",
      .details.check_bank_transfer.account_debit = 63,
      .details.check_bank_transfer.account_credit = EXCHANGE_ACCOUNT_NO
    },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-2",
      .details.check_bank_transfer.amount = "EUR:4.01",
      .details.check_bank_transfer.account_debit = 63,
      .details.check_bank_transfer.account_credit = EXCHANGE_ACCOUNT_NO
    },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-2",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Proposal lookup */
    {
      .oc = OC_PROPOSAL_LOOKUP,
      .label = "fetch-proposal-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal_lookup.proposal_reference
        = "create-proposal-2" },

    /* Check nothing happened on the bank side so far */
    { .oc = OC_CHECK_BANK_TRANSFERS_EMPTY,
      .label = "check_bank_empty" },

    /* Run transfers. */
    { .oc = OC_RUN_AGGREGATOR,
      .label = "run-aggregator" },

    /* Obtain WTID of the transfer generated by "deposit-simple" */
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-498c",
      .details.check_bank_transfer.amount = "EUR:4.98",
      /* exchange-outgoing */
      .details.check_bank_transfer.account_debit = 2,
      /* merchant */
      .details.check_bank_transfer.account_credit = 62
    },

    /* Check that there are no other unusual transfers */
    { .oc = OC_CHECK_BANK_TRANSFERS_EMPTY,
      .label = "check_bank_empty" },

    { .oc = OC_TRACK_TRANSACTION,
      .label = "track-transaction-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.track_transaction.expected_transfer_ref
        = "check_bank_transfer-498c",
      .details.track_transaction.pay_ref = "deposit-simple",
      .details.track_transaction.wire_fee = "EUR:0.01"
    },

    /* Trace the WTID back to the original transaction */
    { .oc = OC_TRACK_TRANSFER,
      .label = "track-transfer-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.track_transfer.check_bank_ref
        = "check_bank_transfer-498c",
      .details.track_transfer.expected_pay_ref = "deposit-simple"
    },
    { .oc = OC_TRACK_TRANSFER,
      .label = "track-transfer-1-again",
      .expected_response_code = MHD_HTTP_OK,
      .details.track_transfer.check_bank_ref
        = "check_bank_transfer-498c",
      .details.track_transfer.expected_pay_ref = "deposit-simple"
    },

    /* Pay again successfully on 2nd contract */
    { .oc = OC_PAY,
      .label = "deposit-simple-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-2",
      .details.pay.coin_ref = "withdraw-coin-2",
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },

    /* Run transfers. */
    { .oc = OC_RUN_AGGREGATOR,
      .label = "run-aggregator-2" },

    /* Obtain WTID of the transfer */
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-498c-2",
      .details.check_bank_transfer.amount = "EUR:4.98",
      /* exchange-outgoing */
      .details.check_bank_transfer.account_debit = 2,
      /* merchant */
      .details.check_bank_transfer.account_credit = 62
    },

    /* Check that there are no other unusual transfers */
    { .oc = OC_CHECK_BANK_TRANSFERS_EMPTY,
      .label = "check_bank_empty" },

    /* Trace the WTID back to the original transaction */
    { .oc = OC_TRACK_TRANSFER,
      .label = "track-transfer-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.track_transfer.check_bank_ref
        = "check_bank_transfer-498c-2",
      .details.track_transfer.expected_pay_ref
        = "deposit-simple-2"
    },
    { .oc = OC_TRACK_TRANSFER,
      .label = "track-transfer-2-again",
      .expected_response_code = MHD_HTTP_OK,
      .details.track_transfer.check_bank_ref
        = "check_bank_transfer-498c-2",
      .details.track_transfer.expected_pay_ref
        = "deposit-simple-2"
    },

    { .oc = OC_TRACK_TRANSACTION,
      .label = "track-transaction-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.track_transaction.expected_transfer_ref
        = "check_bank_transfer-498c-2",
      .details.track_transaction.wire_fee = "EUR:0.01",
      .details.track_transaction.pay_ref = "deposit-simple-2"
    },

    { .oc = OC_HISTORY,
      .label = "history-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.history.date.abs_value_us = 0,
      /**
       * Now we expect BOTH contracts (create-proposal-{1,2})
       * to be included in /history response, because
       * create-proposal-2 has now been correctly paid.
       */
      .details.history.nresult = 2,
      .details.history.start = 10,
      .details.history.nrows = 10
    },

    { .oc = OC_HISTORY,
      .label = "history-2",
      .expected_response_code = MHD_HTTP_OK,
      /*no records returned, time limit too ancient*/
      .details.history.date.abs_value_us = 1,
      .details.history.nresult = 0,
      .details.history.start = 10,
      .details.history.nrows = 10
    },

    { .oc = OC_REFUND_INCREASE,
      .label = "refund-increase-1",
      .details.refund_increase.refund_amount = "EUR:0.1",
      .details.refund_increase.refund_fee = "EUR:0.01",
      .details.refund_increase.reason = "refund test",
      .details.refund_increase.order_id = "1"
    },
    { .oc = OC_REFUND_LOOKUP,
      .label = "refund-lookup-1",
      .details.refund_lookup.order_id = "1",
      .details.refund_lookup.increase_ref = "refund-increase-1",
      .details.refund_lookup.pay_ref = "deposit-simple"
    },

    /* Test tipping */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-tip-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.instance = "tip",
      .details.admin_add_incoming.debit_account_no = 62,
      .details.admin_add_incoming.credit_account_no = EXCHANGE_ACCOUNT_NO,
      .details.admin_add_incoming.auth_username = "user62",
      .details.admin_add_incoming.auth_password = "pass62",
      /* we run *two* instances, but only this first call will
         actually fill the reserve, as the second one will be seen as
         a duplicate. Hence fill with twice the require amount per
         round. */
      .details.admin_add_incoming.amount = "EUR:20.04" },
    /* Run wirewatch to observe /admin/add/incoming */
    { .oc = OC_RUN_WIREWATCH,
      .label = "wirewatch-3" },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-tip-1",
      .details.check_bank_transfer.amount = "EUR:20.04",
      .details.check_bank_transfer.account_debit = 62,
      .details.check_bank_transfer.account_credit = EXCHANGE_ACCOUNT_NO
    },
    /* Authorize two tips */
    { .oc = OC_TIP_AUTHORIZE,
      .label = "authorize-tip-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_authorize.instance = "tip",
      .details.tip_authorize.justification = "tip 1",
      .details.tip_authorize.amount = "EUR:5.01" },
    { .oc = OC_TIP_AUTHORIZE,
      .label = "authorize-tip-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_authorize.instance = "tip",
      .details.tip_authorize.justification = "tip 2",
      .details.tip_authorize.amount = "EUR:5.01" },
    /* Check tip status */
    { .oc = OC_TIP_QUERY,
      .label = "query-tip-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_query.instance = "tip" },
    { .oc = OC_TIP_QUERY,
      .label = "query-tip-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_query.instance = "tip",
      .details.tip_query.expected_amount_authorized = "EUR:10.02",
      .details.tip_query.expected_amount_picked_up = "EUR:0.0",
      .details.tip_query.expected_amount_available = "EUR:20.04" },
    /* Withdraw tip */
    { .oc = OC_TIP_PICKUP,
      .label = "pickup-tip-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_pickup.authorize_ref = "authorize-tip-1",
      .details.tip_pickup.amounts = pickup_amounts_1 },
    /* Check tip status again */
    { .oc = OC_TIP_QUERY,
      .label = "query-tip-3",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_query.instance = "tip",
      .details.tip_query.expected_amount_picked_up = "EUR:5.01",
      .details.tip_query.expected_amount_available = "EUR:15.03" },
    { .oc = OC_TIP_PICKUP,
      .label = "pickup-tip-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_pickup.authorize_ref = "authorize-tip-2",
      .details.tip_pickup.amounts = pickup_amounts_1 },
    { .oc = OC_TIP_QUERY,
      .label = "query-tip-4",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_query.instance = "tip",
      .details.tip_query.expected_amount_picked_up = "EUR:10.02",
      .details.tip_query.expected_amount_available = "EUR:10.02",
      .details.tip_query.expected_amount_authorized = "EUR:10.02" },
    { .oc = OC_TIP_PICKUP,
      .label = "pickup-tip-2b",
      .expected_response_code = MHD_HTTP_OK,
      .details.tip_pickup.replay_ref = "pickup-tip-2",
      .details.tip_pickup.authorize_ref = "authorize-tip-2",
      .details.tip_pickup.amounts = pickup_amounts_1 },
    /* Test authorization failure modes */
    { .oc = OC_TIP_AUTHORIZE,
      .label = "authorize-tip-3-insufficient-funds",
      .expected_response_code = MHD_HTTP_PRECONDITION_FAILED,
      .details.tip_authorize.instance = "dtip",
      .details.tip_authorize.justification = "tip 3",
      .details.tip_authorize.amount = "EUR:5.01",
      .details.tip_authorize.expected_ec = TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS },
    { .oc = OC_TIP_AUTHORIZE,
      .label = "authorize-tip-4-unknown-instance",
      .expected_response_code = MHD_HTTP_NOT_FOUND,
      .details.tip_authorize.instance = "unknown",
      .details.tip_authorize.justification = "tip 4",
      .details.tip_authorize.amount = "EUR:5.01",
      .details.tip_authorize.expected_ec = TALER_EC_TIP_AUTHORIZE_INSTANCE_UNKNOWN },
    { .oc = OC_TIP_AUTHORIZE,
      .label = "authorize-tip-5-notip-instance",
      .expected_response_code = MHD_HTTP_NOT_FOUND,
      .details.tip_authorize.instance = "default",
      .details.tip_authorize.justification = "tip 5",
      .details.tip_authorize.amount = "EUR:5.01",
      .details.tip_authorize.expected_ec = TALER_EC_TIP_AUTHORIZE_INSTANCE_DOES_NOT_TIP },
    { .oc = OC_TIP_PICKUP,
      .label = "pickup-tip-3-too-much",
      .expected_response_code = MHD_HTTP_CONFLICT,
      .details.tip_pickup.expected_ec = TALER_EC_TIP_PICKUP_NO_FUNDS,
      .details.tip_pickup.authorize_ref = "authorize-tip-1",
      .details.tip_pickup.amounts = pickup_amounts_1 },
    /* Spend tip (just to be sure...) */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-tip-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
        \"max_fee\":\
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
    	\"summary\": \"merchant-lib testcase\",\
        \"products\":\
          [ {\"description\":\"ice cream tip\",\
             \"value\":\"{EUR:5}\"} ] }"},
    { .oc = OC_PAY,
      .label = "deposit-tip-simple",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-tip-1",
      .details.pay.coin_ref = "pickup-tip-1",
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },
    /* Run transfers. */
    { .oc = OC_RUN_AGGREGATOR,
      .label = "run-aggregator-tip-1" },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-tip-498c",
      .details.check_bank_transfer.amount = "EUR:4.98",
      /* exchange-outgoing */
      .details.check_bank_transfer.account_debit = 2,
      /* merchant */
      .details.check_bank_transfer.account_credit = 62
    },

    /* Check that there are no other unusual transfers */
    { .oc = OC_CHECK_BANK_TRANSFERS_EMPTY,
      .label = "check_bank_empty" },


    /* ****************** /pay again logic ************* */

    /* Fill reserve with EUR:10.02, as withdraw fee is 1 ct per
       config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-10",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.debit_account_no = 62,
      .details.admin_add_incoming.credit_account_no = EXCHANGE_ACCOUNT_NO,
      .details.admin_add_incoming.auth_username = "user62",
      .details.admin_add_incoming.auth_password = "pass62",
      .details.admin_add_incoming.amount = "EUR:10.02" },
    /* Run wirewatch to observe /admin/add/incoming */
    { .oc = OC_RUN_WIREWATCH,
      .label = "wirewatch-10" },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-10",
      .details.check_bank_transfer.amount = "EUR:10.02",
      .details.check_bank_transfer.account_debit = 62,
      .details.check_bank_transfer.account_credit = EXCHANGE_ACCOUNT_NO
    },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-10a",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-10",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-10b",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-10",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Check that deposit and withdraw operation are in history,
       and that the balance is now at zero */
    { .oc = OC_WITHDRAW_STATUS,
      .label = "withdraw-status-10",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_status.reserve_reference
        = "create-reserve-10",
      .details.reserve_status.expected_balance = "EUR:0" },

    /* Create proposal */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-10",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
        \"max_fee\":\
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
        \"products\":\
          [ {\"description\":\"ice cream\",\
             \"value\":\"{EUR:10}\"} ] }"},

    /* execute simple payment, re-using one ancient coin */
    { .oc = OC_PAY,
      .label = "pay-fail-partial-double-10",
      .expected_response_code = MHD_HTTP_FORBIDDEN,
      .details.pay.contract_ref = "create-proposal-10",
      .details.pay.coin_ref = "withdraw-coin-10a;withdraw-coin-1",
      /* These amounts are given per coin! */
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },

    /* Try to replay payment reusing coin */
    { .oc = OC_PAY_AGAIN,
      .label = "pay-again-10",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay_again.pay_ref = "pay-fail-partial-double-10",
      .details.pay_again.coin_ref = "withdraw-coin-10a;withdraw-coin-10b" },

    /* Run transfers. */
    { .oc = OC_RUN_AGGREGATOR,
      .label = "run-aggregator-10" },

    /* Obtain WTID of the transfer */
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-9.97-10",
      .details.check_bank_transfer.amount = "EUR:9.97",
      /* exchange-outgoing */
      .details.check_bank_transfer.account_debit = 2,
      /* merchant */
      .details.check_bank_transfer.account_credit = 62
    },

    /* Check that there are no other unusual transfers */
    { .oc = OC_CHECK_BANK_TRANSFERS_EMPTY,
      .label = "check_bank_empty-10" },


    /* ****************** /pay abort logic ************* */

    /* Fill reserve with EUR:10.02, as withdraw fee is 1 ct per
       config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-11",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.debit_account_no = 62,
      .details.admin_add_incoming.credit_account_no = EXCHANGE_ACCOUNT_NO,
      .details.admin_add_incoming.auth_username = "user62",
      .details.admin_add_incoming.auth_password = "pass62",
      .details.admin_add_incoming.amount = "EUR:10.02" },
    /* Run wirewatch to observe /admin/add/incoming */
    { .oc = OC_RUN_WIREWATCH,
      .label = "wirewatch-11" },
    { .oc = OC_CHECK_BANK_TRANSFER,
      .label = "check_bank_transfer-11",
      .details.check_bank_transfer.amount = "EUR:10.02",
      .details.check_bank_transfer.account_debit = 62,
      .details.check_bank_transfer.account_credit = EXCHANGE_ACCOUNT_NO
    },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-11a",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-11",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-11b",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference
        = "create-reserve-11",
      .details.reserve_withdraw.amount = "EUR:5" },

    /* Check that deposit and withdraw operation are in history,
       and that the balance is now at zero */
    { .oc = OC_WITHDRAW_STATUS,
      .label = "withdraw-status-11",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_status.reserve_reference
        = "create-reserve-11",
      .details.reserve_status.expected_balance = "EUR:0" },

    /* Create proposal */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-11",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
        \"max_fee\":\
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
        \"products\":\
          [ {\"description\":\"ice cream\",\
             \"value\":\"{EUR:10}\"} ] }"},

    /* execute simple payment, re-using one ancient coin */
    { .oc = OC_PAY,
      .label = "pay-fail-partial-double-11",
      .expected_response_code = MHD_HTTP_FORBIDDEN,
      .details.pay.contract_ref = "create-proposal-11",
      .details.pay.coin_ref = "withdraw-coin-11a;withdraw-coin-1",
      /* These amounts are given per coin! */
      .details.pay.refund_fee = "EUR:0.01",
      .details.pay.amount_with_fee = "EUR:5",
      .details.pay.amount_without_fee = "EUR:4.99" },

    /* Try to replay payment reusing coin */
    { .oc = OC_PAY_ABORT,
      .label = "pay-abort-11",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay_abort.pay_ref = "pay-fail-partial-double-11",
    },

    { .oc = OC_PAY_ABORT_REFUND,
      .label = "pay-abort-refund-11",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay_abort_refund.abort_ref = "pay-abort-11",
      .details.pay_abort_refund.num_coin = 0,
      .details.pay_abort_refund.refund_amount = "EUR:5",
      .details.pay_abort_refund.refund_fee = "EUR:0.01" },

    /* Run transfers. */
    { .oc = OC_RUN_AGGREGATOR,
      .label = "run-aggregator-11" },
    /* Check that there are no other unusual transfers */
    { .oc = OC_CHECK_BANK_TRANSFERS_EMPTY,
      .label = "check_bank_empty-11" },
    /* end of testcase */
    { .oc = OC_END }
  };
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Interpreter initializing\n");
  fakebank = TALER_FAKEBANK_start (BANK_PORT);
  if (NULL == fakebank)
  {
    fprintf (stderr,
             "\nFailed to start fake bank service\n");
    result = 77;
    return;
  }

  is = GNUNET_new (struct InterpreterState);
  is->commands = commands;

  GNUNET_assert (ctx = GNUNET_CURL_init
    (&GNUNET_CURL_gnunet_scheduler_reschedule,
     &rc));
  rc = GNUNET_CURL_gnunet_rc_create (ctx);
  GNUNET_assert (NULL != (exchange
    = TALER_EXCHANGE_connect (ctx,
                              EXCHANGE_URL,
                              &cert_cb, is,
                              TALER_EXCHANGE_OPTION_END)));
  timeout_task
    = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                   (GNUNET_TIME_UNIT_SECONDS, 150),
                                    &do_timeout, NULL);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown, is);
}


/**
 * Main function for the testcase for the exchange API.
 *
 * @param argc expected to be 1
 * @param argv expected to only contain the program name
 */
int
main (int argc,
      char * const *argv)
{
  char *_instances;
  char *token;
  struct GNUNET_OS_Process *proc;
  struct GNUNET_OS_Process *exchanged;
  struct GNUNET_OS_Process *merchantd;
  unsigned int cnt;
  struct GNUNET_SIGNAL_Context *shc_chld;

  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("test-merchant-api",
                    "DEBUG",
                    NULL);
  cfg = GNUNET_CONFIGURATION_create ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONFIGURATION_load (cfg,
                                            "test_merchant_api.conf"));
  GNUNET_assert (GNUNET_OK ==
    GNUNET_CONFIGURATION_get_value_string (cfg,
                                           "merchant",
                                           "INSTANCES",
                                           &_instances));

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Found instances `%s'\n",
              _instances);
  GNUNET_break (NULL != (token = strtok (_instances, " ")));
  GNUNET_array_append (instances,
                       ninstances,
                       GNUNET_strdup (token));
  while (NULL != (token = strtok (NULL, " ")))
    GNUNET_array_append (instances,
                         ninstances,
                         GNUNET_strdup (token));
  GNUNET_free (_instances);
  instance = instances[instance_idx];
  instance_priv = get_instance_priv (cfg, instance);
  db = TALER_MERCHANTDB_plugin_load (cfg);
  if (NULL == db)
  {
    GNUNET_CONFIGURATION_destroy (cfg);
    return 77;
  }
  (void) db->drop_tables (db->cls);
  if (GNUNET_OK != db->initialize (db->cls))
  {
    TALER_MERCHANTDB_plugin_unload (db);
    GNUNET_CONFIGURATION_destroy (cfg);
    return 77;
  }
  if (NULL == (proc = GNUNET_OS_start_process
       (GNUNET_NO,
        GNUNET_OS_INHERIT_STD_ALL,
        NULL, NULL, NULL,
        "taler-exchange-keyup",
        "taler-exchange-keyup",
        "-c", "test_merchant_api.conf",
        NULL)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to run taler-exchange-keyup. Check your PATH.\n");
    return 77;
  }
  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);
  if (NULL == (proc = GNUNET_OS_start_process
       (GNUNET_NO,
        GNUNET_OS_INHERIT_STD_ALL,
        NULL, NULL, NULL,
        "taler-exchange-dbinit",
        "taler-exchange-dbinit",
        "-c", "test_merchant_api.conf",
        "-r",
        NULL)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to run taler-exchange-dbinit. Check your PATH.\n");
    return 77;
  }
  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);
  if (NULL == (exchanged = GNUNET_OS_start_process
       (GNUNET_NO,
        GNUNET_OS_INHERIT_STD_ALL,
        NULL, NULL, NULL,
        "taler-exchange-httpd",
        "taler-exchange-httpd",
        "-c", "test_merchant_api.conf",
        NULL)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to run taler-exchange-httpd. Check your PATH.\n");
    return 77;
  }
  /* give child time to start and bind against the socket */
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Waiting for taler-exchange-httpd to be ready\n");
  cnt = 0;
  do
  {
    fprintf (stderr, ".");
    sleep (1);
    cnt++;
    if (cnt > 60)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "\nFailed to start taler-exchange-httpd\n");
      GNUNET_OS_process_kill (exchanged,
                              SIGKILL);
      GNUNET_OS_process_wait (exchanged);
      GNUNET_OS_process_destroy (exchanged);
      return 77;
    }
  }
  while (0 != system ("wget -q -t 1 -T 1 " EXCHANGE_URL "keys -o /dev/null -O /dev/null"));
  fprintf (stderr, "\n");
  if (NULL == (merchantd = GNUNET_OS_start_process
       (GNUNET_NO,
        GNUNET_OS_INHERIT_STD_ALL,
        NULL, NULL, NULL,
        "taler-merchant-httpd",
        "taler-merchant-httpd",
        "-c", "test_merchant_api.conf",
        "-L", "DEBUG",
        NULL)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to run taler-merchant-httpd. Check your PATH.\n");
    GNUNET_OS_process_kill (exchanged,
                            SIGKILL);
    GNUNET_OS_process_wait (exchanged);
    GNUNET_OS_process_destroy (exchanged);
    return 77;
  }
  /* give child time to start and bind against the socket */
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Waiting for taler-merchant-httpd to be ready\n");
  cnt = 0;
  do
  {
    fprintf (stderr, ".");
    sleep (1);
    cnt++;
    if (cnt > 60)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "\nFailed to start taler-merchant-httpd\n");
      GNUNET_OS_process_kill (merchantd,
                              SIGKILL);
      GNUNET_OS_process_wait (merchantd);
      GNUNET_OS_process_destroy (merchantd);
      GNUNET_OS_process_kill (exchanged,
                              SIGKILL);
      GNUNET_OS_process_wait (exchanged);
      GNUNET_OS_process_destroy (exchanged);
      return 77;
    }
  }
  while (0 != system ("wget -q -t 1 -T 1 " MERCHANT_URL " -o /dev/null -O /dev/null"));
  fprintf (stderr, "\n");

  result = GNUNET_SYSERR;
  GNUNET_assert (NULL != (sigpipe = GNUNET_DISK_pipe
    (GNUNET_NO, GNUNET_NO, GNUNET_NO, GNUNET_NO)));
  shc_chld = GNUNET_SIGNAL_handler_install
    (GNUNET_SIGCHLD,
     &sighandler_child_death);
  GNUNET_SCHEDULER_run (&run, NULL);
  GNUNET_SIGNAL_handler_uninstall (shc_chld);
  shc_chld = NULL;
  GNUNET_DISK_pipe_close (sigpipe);
  GNUNET_OS_process_kill (merchantd,
                          SIGTERM);
  GNUNET_OS_process_wait (merchantd);
  GNUNET_OS_process_destroy (merchantd);
  GNUNET_OS_process_kill (exchanged,
                          SIGTERM);
  GNUNET_OS_process_wait (exchanged);
  GNUNET_OS_process_destroy (exchanged);
  if (77 == result)
    return 77;
  return (GNUNET_OK == result) ? 0 : 1;
}
