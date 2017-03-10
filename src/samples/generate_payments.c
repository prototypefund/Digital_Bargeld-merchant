/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, see <http://www.gnu.org/licenses/>
*/

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_fakebank_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchantdb_lib.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include <microhttpd.h>

#define EXCHANGE_URI "http://localexchange/"
#define MERCHANT_URI "http://localshop/"
#define BANK_URI "http://localbank/"
#define INSTANCE "FSF"
#define CURRENCY "EUR"

#define ORDER_MAX_SIZE 1000

/**
 * Task run on timeout.
 */
static struct GNUNET_SCHEDULER_Task *timeout_task;

/**
 * Handle to access the exchange.
 */
static struct TALER_EXCHANGE_Handle *exchange;

/**
 * Main execution context for the main loop of the exchange.
 */
static struct GNUNET_CURL_Context *ctx;

/**
 * Context for running the #ctx's event loop.
 */
static struct GNUNET_CURL_RescheduleContext *rc;

/**
 * Result of the testcases, #GNUNET_OK on success.
 */
static int result;

/**
 * Pipe used to communicate child death via signal.
 */
static struct GNUNET_DISK_PipeHandle *sigpipe;

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
  OC_PAY

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
       * Set (by the interpreter) to the coin's private key.
       */
      struct TALER_CoinSpendPrivateKeyP coin_priv;

      /**
       * Blinding key used for the operation.
       */
      struct TALER_DenominationBlindingKeyP blinding_key;

      /**
       * Withdraw handle (while operation is running).
       */
      struct TALER_EXCHANGE_ReserveWithdrawHandle *wsh;

    } reserve_withdraw;

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
       * String describing the amount to add to the reserve.
       */
      const char *amount;

      /**
       * Sender's bank account details (JSON).
       */
      const char *sender_details;

      /**
       * Transfer details (JSON)
       */
       const char *transfer_details;

      /**
       * Set (by the interpreter) to the reserve's private key
       * we used to fill the reserve.
       */
      struct TALER_ReservePrivateKeyP reserve_priv;

      /**
       * Set to the API's handle during the operation.
       */
      struct TALER_EXCHANGE_AdminAddIncomingHandle *aih;

    } admin_add_incoming;

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
       * Full contract in JSON, set by the /contract operation.
       * FIXME: verify in the code that this bit is actually proposal
       * data and not the whole proposal.
       */
      json_t *proposal_data;

      /**
       * Proposal's signature.
       */
      struct TALER_MerchantSignatureP merchant_sig;

      /**
       * Proposal data's hashcode.
       */
      struct GNUNET_HashCode hash;

    } proposal;

    /**
     * Information for a #OC_PAY command.
     * FIXME: support tests where we pay with multiple coins at once.
     */
    struct
    {

      /**
       * Reference to the contract.
       */
      const char *contract_ref;

      /**
       * Reference to a reserve_withdraw operation for a coin to
       * be used for the /deposit operation.
       */
      const char *coin_ref;

      /**
       * If this @e coin_ref refers to an operation that generated
       * an array of coins, this value determines which coin to use.
       */
      unsigned int coin_idx;

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
       * Deposit handle while operation is running.
       */
      struct TALER_MERCHANT_Pay *ph;

      /**
       * Hashcode of the proposal data associated to this payment.
       */
      struct GNUNET_HashCode h_proposal_data;

      /**
       * Merchant's public key
       */
      struct TALER_MerchantPublicKeyP merchant_pub;

    } pay;



  } details;

};

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
 * The generator failed, return with an error code.
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
 * Callback that works PUT /proposal's output.
 *
 * @param cls closure
 * @param http_status HTTP response code, 200 indicates success;
 *                    0 if the backend's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code
 * @param obj the full received JSON reply, or
 *            error details if the request failed
 * @param proposal_data the order + additional information provided by the
 * backend, NULL on error.
 * @param sig merchant's signature over the contract, NULL on error
 * @param h_contract hash of the contract, NULL on error
 */
static void
proposal_cb (void *cls,
             unsigned int http_status,
	     enum TALER_ErrorCode ec,
             const json_t *obj,
             const json_t *proposal_data,
             const struct TALER_MerchantSignatureP *sig,
             const struct GNUNET_HashCode *hash)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.proposal.po = NULL;
  switch (http_status)
  {
  case MHD_HTTP_OK:
    cmd->details.proposal.proposal_data = json_incref ((json_t *) proposal_data);
    cmd->details.proposal.merchant_sig = *sig;
    cmd->details.proposal.hash = *hash;
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Hashed proposal, '%s'\n",
                GNUNET_h2s (hash));
    break;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "unexpected status code from /proposal: %u. Step %u\n",
                http_status,
                is->ip);
    json_dumpf (obj, stderr, 0);
    GNUNET_break (0);
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
 * @param ec taler-specific error code
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
  struct PaymentResponsePS mr;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  struct GNUNET_HashCode h_proposal_data;
  const char *error_name;
  unsigned int error_line;

  cmd->details.pay.ph = NULL;
  if (cmd->expected_response_code != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u to command %s\n",
                http_status,
                cmd->label);
    json_dumpf (obj, stderr, 0);
    fail (is);
    return;
  }
  if (MHD_HTTP_OK == http_status)
  {
    /* Check signature */
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("sig", &sig),
      GNUNET_JSON_spec_fixed_auto ("h_proposal_data", &h_proposal_data),
      GNUNET_JSON_spec_end ()
    };
    if (GNUNET_OK !=
        GNUNET_JSON_parse (obj,
                           spec,
                           &error_name,
                           &error_line))
    {
      GNUNET_break_op (0);
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Parser failed on %s:%u\n",
                  error_name,
                  error_line);
      fail (is);
      return;
    }
    mr.purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
    mr.purpose.size = htonl (sizeof (mr));
    mr.h_proposal_data = h_proposal_data;
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
 * Function called upon completion of our /admin/add/incoming request.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200) for successful status request
 *                    0 if the exchange's reply is bogus (fails to follow the protocol)
 * @param ec taler-specific error code, #TALER_EC_NONE on success
 * @param full_response full response from the exchange (for logging, in case of errors)
 */
static void
add_incoming_cb (void *cls,
                 unsigned int http_status,
		 enum TALER_ErrorCode ec,
                 const json_t *full_response)
{
  struct InterpreterState *is = cls;
  struct Command *cmd = &is->commands[is->ip];

  cmd->details.admin_add_incoming.aih = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break (0);
    fail (is);
    return;
  }
  next_command (is);
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
  unsigned int i;
  const struct Command *cmd;

  if (NULL == label)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Attempt to lookup command for empty label\n");
    return NULL;
  }
  for (i=0;OC_END != (cmd = &is->commands[i])->oc;i++)
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
                "Unexpected response code %u to command %s\n",
                http_status,
                cmd->label);
    json_dumpf (full_response, stderr, 0);
    GNUNET_break (0);
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
  unsigned int i;
  struct GNUNET_TIME_Absolute now;
  struct TALER_EXCHANGE_DenomPublicKey *pk;
  char *str;

  now = GNUNET_TIME_absolute_get ();
  for (i=0;i<keys->num_denom_keys;i++)
  {
    pk = &keys->denom_keys[i];
    if ( (0 == TALER_amount_cmp (amount,
                                 &pk->value)) &&
         (now.abs_value_us >= pk->valid_from.abs_value_us) &&
         (now.abs_value_us < pk->withdraw_valid_until.abs_value_us) )
      return pk;
  }
  /* do 2nd pass to check if expiration times are to blame for failure */
  str = TALER_amount_to_string (amount);
  for (i=0;i<keys->num_denom_keys;i++)
  {
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
  struct TALER_CoinSpendPublicKeyP coin_pub;
  struct TALER_Amount amount;
  struct GNUNET_TIME_Absolute execution_date;
  json_t *sender_details;
  json_t *transfer_details;

  is->task = NULL;
  tc = GNUNET_SCHEDULER_get_task_context ();
  if (0 != (tc->reason & GNUNET_SCHEDULER_REASON_SHUTDOWN))
  {
    fprintf (stderr,
             "Test aborted by shutdown request\n");
    fail (is);
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
	      "Interpreter runs command %u/%s(%u)\n",
	      is->ip,
	      cmd->label,
	      cmd->oc);

  switch (cmd->oc)
  {
    case OC_END:
      result = GNUNET_OK;
      GNUNET_SCHEDULER_shutdown ();
      return;

    case OC_PAY:
      {
        struct TALER_MERCHANT_PayCoin pc;
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
        ref = find_command (is,
                            cmd->details.pay.contract_ref);
        GNUNET_assert (NULL != ref);
        merchant_sig = ref->details.proposal.merchant_sig;
        GNUNET_assert (NULL != ref->details.proposal.proposal_data);
        {
          /* Get information that need to be replied in the deposit permission */
          struct GNUNET_JSON_Specification spec[] = {
            GNUNET_JSON_spec_string ("order_id", &order_id),
            GNUNET_JSON_spec_absolute_time ("refund_deadline", &refund_deadline),
            GNUNET_JSON_spec_absolute_time ("pay_deadline", &pay_deadline),
            GNUNET_JSON_spec_absolute_time ("timestamp", &timestamp),
            GNUNET_JSON_spec_fixed_auto ("merchant_pub", &merchant_pub),
            GNUNET_JSON_spec_fixed_auto ("H_wire", &h_wire),
            TALER_JSON_spec_amount ("amount", &total_amount),
            TALER_JSON_spec_amount ("max_fee", &max_fee),
            GNUNET_JSON_spec_end()
          };
  
          if (GNUNET_OK !=
              GNUNET_JSON_parse (ref->details.proposal.proposal_data,
                                 spec,
                                 &error_name,
                                 &error_line))
          {
            GNUNET_break_op (0);
            GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                        "Parser failed on %s:%u\n",
                        error_name,
                        error_line);
            fail (is);
            return;
          }
          cmd->details.pay.merchant_pub = merchant_pub;
        }
  
        {
          const struct Command *coin_ref;
  	memset (&pc, 0, sizeof (pc));
  	coin_ref = find_command (is,
  	                         cmd->details.pay.coin_ref);
  	GNUNET_assert (NULL != ref);
  	switch (coin_ref->oc)
  	{
  	case OC_WITHDRAW_SIGN:
  	  pc.coin_priv = coin_ref->details.reserve_withdraw.coin_priv;
  	  pc.denom_pub = coin_ref->details.reserve_withdraw.pk->key;
  	  pc.denom_sig = coin_ref->details.reserve_withdraw.sig;
            pc.denom_value = coin_ref->details.reserve_withdraw.pk->value;
  	  break;
  	default:
  	  GNUNET_assert (0);
  	}
  
  	if (GNUNET_OK !=
  	    TALER_string_to_amount (cmd->details.pay.amount_without_fee,
  				    &pc.amount_without_fee))
  	{
  	  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
  		      "Failed to parse amount `%s' at %u\n",
  		      cmd->details.pay.amount_without_fee,
  		      is->ip);
  	  fail (is);
  	  return;
  	}
  
  	if (GNUNET_OK !=
  	    TALER_string_to_amount (cmd->details.pay.amount_with_fee,
  				    &pc.amount_with_fee))
  	{
  	  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
  		      "Failed to parse amount `%s' at %u\n",
  		      cmd->details.pay.amount_with_fee,
  		      is->ip);
  	  fail (is);
  	  return;
  	}
        }
  
        cmd->details.pay.ph
  	= TALER_MERCHANT_pay_wallet (ctx,
  				     MERCHANT_URI,
                                     INSTANCE,
  				     &ref->details.proposal.hash,
  				     &total_amount,
  				     &max_fee,
  				     &merchant_pub,
                                       &merchant_sig,
  				     timestamp,
  				     refund_deadline,
  				     pay_deadline,
  				     &h_wire,
  				     EXCHANGE_URI,
                                     order_id,
  				     1 /* num_coins */,
  				     &pc /* coins */,
  				     &pay_cb,
  				     is);
      }
      if (NULL == cmd->details.pay.ph)
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      return;


    case OC_PROPOSAL:
      {
        json_t *order;
        json_error_t error;
  
        order = json_loads (cmd->details.proposal.order,
                            JSON_REJECT_DUPLICATES,
                            &error);
        if (NULL == order)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Failed to parse the order `%s' at command #%u: %s at %u\n",
                      cmd->details.proposal.order,
                      is->ip,
                      error.text,
                      (unsigned int) error.column);
          fail (is);
          return;
        }
        
        cmd->details.proposal.po
          = TALER_MERCHANT_order_put (ctx,
                                      MERCHANT_URI,
                                      order,
                                      &proposal_cb,
                                      is);
        json_decref (order);
        if (NULL == cmd->details.proposal.po)
        {
          GNUNET_break (0);
          fail (is);
          return;
        }
        return;
      }

    case OC_ADMIN_ADD_INCOMING:
      if (NULL !=
          cmd->details.admin_add_incoming.reserve_reference)
      {
        ref = find_command (is,
                            cmd->details.admin_add_incoming.reserve_reference);
        GNUNET_assert (NULL != ref);
        GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
        cmd->details.admin_add_incoming.reserve_priv
          = ref->details.admin_add_incoming.reserve_priv;
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
      if (GNUNET_OK !=
          TALER_string_to_amount (cmd->details.admin_add_incoming.amount,
                                  &amount))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse amount `%s' at %u\n",
                    cmd->details.admin_add_incoming.amount,
                    is->ip);
        fail (is);
        return;
      }
  
      execution_date = GNUNET_TIME_absolute_get ();
      GNUNET_TIME_round_abs (&execution_date);
      sender_details = json_loads (cmd->details.admin_add_incoming.sender_details,
                                   JSON_REJECT_DUPLICATES,
                                   NULL);
      if (NULL == sender_details)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse sender details `%s' at %u\n",
                    cmd->details.admin_add_incoming.sender_details,
                    is->ip);
        fail (is);
        return;
      }
      transfer_details = json_loads (cmd->details.admin_add_incoming.transfer_details,
                                     JSON_REJECT_DUPLICATES,
                                     NULL);
  
      if (NULL == transfer_details)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse transfer details `%s' at %u\n",
                    cmd->details.admin_add_incoming.transfer_details,
                    is->ip);
        fail (is);
        return;
      }
      cmd->details.admin_add_incoming.aih
        = TALER_EXCHANGE_admin_add_incoming (exchange,
                                             EXCHANGE_URI,
                                             &reserve_pub,
                                             &amount,
                                             execution_date,
                                             sender_details,
                                             transfer_details,
                                             &add_incoming_cb,
                                             is);
      json_decref (sender_details);
      json_decref (transfer_details);
      if (NULL == cmd->details.admin_add_incoming.aih)
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      return;

    case OC_WITHDRAW_SIGN:
      GNUNET_assert (NULL !=
                     cmd->details.reserve_withdraw.reserve_reference);
      ref = find_command (is,
                          cmd->details.reserve_withdraw.reserve_reference);
      GNUNET_assert (NULL != ref);
      GNUNET_assert (OC_ADMIN_ADD_INCOMING == ref->oc);
      if (NULL != cmd->details.reserve_withdraw.amount)
      {
        if (GNUNET_OK !=
            TALER_string_to_amount (cmd->details.reserve_withdraw.amount,
                                    &amount))
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Failed to parse amount `%s' at %u\n",
                      cmd->details.reserve_withdraw.amount,
                      is->ip);
          fail (is);
          return;
        }
        cmd->details.reserve_withdraw.pk = find_pk (is->keys,
                                                    &amount);
      }
      if (NULL == cmd->details.reserve_withdraw.pk)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to determine denomination key at %u\n",
                    is->ip);
        fail (is);
        return;
      }
  
      /* create coin's private key */
      {
        struct GNUNET_CRYPTO_EddsaPrivateKey *priv;
  
        priv = GNUNET_CRYPTO_eddsa_key_create ();
        cmd->details.reserve_withdraw.coin_priv.eddsa_priv = *priv;
        GNUNET_free (priv);
      }
      GNUNET_CRYPTO_eddsa_key_get_public (&cmd->details.reserve_withdraw.coin_priv.eddsa_priv,
                                          &coin_pub.eddsa_pub);
      GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                                  &cmd->details.reserve_withdraw.blinding_key,
                                  sizeof (cmd->details.reserve_withdraw.blinding_key));
  
      cmd->details.reserve_withdraw.wsh
        = TALER_EXCHANGE_reserve_withdraw (exchange,
                                           cmd->details.reserve_withdraw.pk,
                                           &ref->details.admin_add_incoming.reserve_priv,
                                           &cmd->details.reserve_withdraw.coin_priv,
                                           &cmd->details.reserve_withdraw.blinding_key,
                                           &reserve_withdraw_cb,
                                           is);
      if (NULL == cmd->details.reserve_withdraw.wsh)
      {
        GNUNET_break (0);
        fail (is);
        return;
      }
      return;

    default:
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Unknown command, OC: %d, label: %s.\n",
                  cmd->oc,
                  cmd->label);
      fail (is);
  }
}

/**
 * Functions of this type are called to provide the retrieved signing and
 * denomination keys of the exchange.  No TALER_EXCHANGE_*() functions should
 * be called in this callback.
 *
 * @param cls closure
 * @param keys information about keys of the exchange
 */
static void
cert_cb (void *cls,
         const struct TALER_EXCHANGE_Keys *keys)
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

  GNUNET_break (1 ==
		GNUNET_DISK_file_write (GNUNET_DISK_pipe_handle
					(sigpipe, GNUNET_DISK_PIPE_END_WRITE),
					&c, sizeof (c)));
  errno = old_errno;		/* restore errno */
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
  struct Command *cmd;
  unsigned int i;

  if (NULL != timeout_task)
  {
    GNUNET_SCHEDULER_cancel (timeout_task);
    timeout_task = NULL;
  }

  for (i=0;OC_END != (cmd = &is->commands[i])->oc;i++)
    switch (cmd->oc)
    {
      case OC_END:
        GNUNET_assert (0);
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
  
      case OC_PROPOSAL:
        if (NULL != cmd->details.proposal.po)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                      "Command %u (%s) did not complete\n",
                      i,
                      cmd->label);
          TALER_MERCHANT_proposal_cancel (cmd->details.proposal.po);
          cmd->details.proposal.po = NULL;
        }
        if (NULL != cmd->details.proposal.proposal_data)
        {
          json_decref (cmd->details.proposal.proposal_data);
          cmd->details.proposal.proposal_data = NULL;
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
  
      case OC_ADMIN_ADD_INCOMING:
        if (NULL != cmd->details.admin_add_incoming.aih)
        {
          GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                      "Command %u (%s) did not complete\n",
                      i,
                      cmd->label);
          TALER_EXCHANGE_admin_add_incoming_cancel (cmd->details.admin_add_incoming.aih);
          cmd->details.admin_add_incoming.aih = NULL;
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

  if (NULL != is->task)
  {
    GNUNET_SCHEDULER_cancel (is->task);
    is->task = NULL;
  }
  GNUNET_free (is);
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
  static struct Command commands[] =
  {
    /* Fill reserve with EUR:5.01, as withdraw fee is 1 ct per config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.sender_details = "{ \"bank_uri\":\"" BANK_URI "\", \"type\":\"test\", \"account_number\":62, \"uuid\":1 }",
      .details.admin_add_incoming.transfer_details = "{ \"uuid\": 1}",
      .details.admin_add_incoming.amount = CURRENCY ":5.01" },
    /* Fill reserve with EUR:5.01, as withdraw fee is 1 ct per config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.sender_details = "{ \"bank_uri\":\"" BANK_URI "\", \"type\":\"test\", \"account_number\":62, \"uuid\":1 }",
      .details.admin_add_incoming.transfer_details = "{ \"uuid\": 1}",
      .details.admin_add_incoming.amount = CURRENCY ":5.01" },
    /* Fill reserve with EUR:5.01, as withdraw fee is 1 ct per config */
    { .oc = OC_ADMIN_ADD_INCOMING,
      .label = "create-reserve-3",
      .expected_response_code = MHD_HTTP_OK,
      .details.admin_add_incoming.sender_details = "{ \"bank_uri\":\"" BANK_URI "\", \"type\":\"test\", \"account_number\":62, \"uuid\":1 }",
      .details.admin_add_incoming.transfer_details = "{ \"uuid\": 1}",
      .details.admin_add_incoming.amount = CURRENCY ":5.01" },
    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference = "create-reserve-1",
      .details.reserve_withdraw.amount = CURRENCY ":5" },
    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference = "create-reserve-2",
      .details.reserve_withdraw.amount = CURRENCY ":5" },
    /* Withdraw a 5 EUR coin, at fee of 1 ct */
    { .oc = OC_WITHDRAW_SIGN,
      .label = "withdraw-coin-3",
      .expected_response_code = MHD_HTTP_OK,
      .details.reserve_withdraw.reserve_reference = "create-reserve-3",
      .details.reserve_withdraw.amount = CURRENCY ":5" },

    /* Create proposal */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-1",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
                  \"max_fee\":\
                     {\"currency\":\"" CURRENCY "\", \"value\":0, \"fraction\":50000000},\
                  \"order_id\":\"1\",\
                  \"timestamp\":\"\\/Date(42)\\/\",\
                  \"refund_deadline\":\"\\/Date(0)\\/\",\
                  \"pay_deadline\":\"\\/Date(9999999999)\\/\",\
                  \"amount\":{\"currency\":\"" CURRENCY "\", \"value\":5, \"fraction\":0},\
                  \"merchant\":{\"instance\":\"" INSTANCE "\"},\
		  \"summary\": \"merchant-lib testcase\",\
                  \"products\":\
                     [ {\"description\":\"ice cream\", \"value\":\"{" CURRENCY ":5}\"} ] }"},

    /* Create proposal */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-2",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
                  \"max_fee\":\
                     {\"currency\":\"" CURRENCY "\", \"value\":0, \"fraction\":50000000},\
                  \"order_id\":\"2\",\
                  \"timestamp\":\"\\/Date(42)\\/\",\
                  \"refund_deadline\":\"\\/Date(0)\\/\",\
                  \"pay_deadline\":\"\\/Date(9999999999)\\/\",\
                  \"amount\":{\"currency\":\"" CURRENCY "\", \"value\":5, \"fraction\":0},\
                  \"merchant\":{\"instance\":\"" INSTANCE "\"},\
		  \"summary\": \"merchant-lib testcase\",\
                  \"products\":\
                     [ {\"description\":\"ice cream\", \"value\":\"{" CURRENCY ":5}\"} ] }"},

    /* Create proposal */
    { .oc = OC_PROPOSAL,
      .label = "create-proposal-3",
      .expected_response_code = MHD_HTTP_OK,
      .details.proposal.order = "{\
                  \"max_fee\":\
                     {\"currency\":\"" CURRENCY "\", \"value\":0, \"fraction\":50000000},\
                  \"order_id\":\"3\",\
                  \"timestamp\":\"\\/Date(42)\\/\",\
                  \"refund_deadline\":\"\\/Date(0)\\/\",\
                  \"pay_deadline\":\"\\/Date(9999999999)\\/\",\
                  \"amount\":{\"currency\":\"" CURRENCY "\", \"value\":5, \"fraction\":0},\
                  \"merchant\":{\"instance\":\"" INSTANCE "\"},\
		  \"summary\": \"merchant-lib testcase\",\
                  \"products\":\
                     [ {\"description\":\"ice cream\", \"value\":\"{" CURRENCY ":5}\"} ] }"},

    { .oc = OC_PAY,
      .label = "deposit-simple",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-1",
      .details.pay.coin_ref = "withdraw-coin-1",
      .details.pay.amount_with_fee = CURRENCY ":5",
      .details.pay.amount_without_fee = CURRENCY ":4.99" },

    { .oc = OC_PAY,
      .label = "deposit-simple",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-2",
      .details.pay.coin_ref = "withdraw-coin-2",
      .details.pay.amount_with_fee = CURRENCY ":5",
      .details.pay.amount_without_fee = CURRENCY ":4.99" },

    { .oc = OC_PAY,
      .label = "deposit-simple",
      .expected_response_code = MHD_HTTP_OK,
      .details.pay.contract_ref = "create-proposal-3",
      .details.pay.coin_ref = "withdraw-coin-3",
      .details.pay.amount_with_fee = CURRENCY ":5",
      .details.pay.amount_without_fee = CURRENCY ":4.99" },

    { .oc = OC_END,
      .label = "end-of-commands"}
  };

  is = GNUNET_new (struct InterpreterState);
  is->commands = commands;

  ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                          &rc);
  GNUNET_assert (NULL != ctx);
  rc = GNUNET_CURL_gnunet_rc_create (ctx);
  exchange = TALER_EXCHANGE_connect (ctx,
                                     EXCHANGE_URI,
                                     &cert_cb, is,
                                     TALER_EXCHANGE_OPTION_END);
  GNUNET_assert (NULL != exchange);
  timeout_task
    = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                    (GNUNET_TIME_UNIT_SECONDS, 150),
                                    &do_timeout, NULL);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown, is);


}

int
main (int argc,
      char *argv[])
{
  struct GNUNET_OS_Process *exchanged;
  struct GNUNET_OS_Process *merchantd;
  unsigned int cnt;
  struct GNUNET_SIGNAL_Context *shc_chld;

  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");

  GNUNET_log_setup ("merchant-create-payments",
                    "DEBUG",
                    NULL);

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "About to launch the exchange.\n");

  exchanged = GNUNET_OS_start_process (GNUNET_NO,
                                       GNUNET_OS_INHERIT_STD_ALL,
                                       NULL, NULL, NULL,
                                       "taler-exchange-httpd",
                                       "taler-exchange-httpd",
                                       NULL);
  if (NULL == exchanged)
  {
    fprintf (stderr,
             "Failed to run taler-exchange-httpd. Check your PATH.\n");
    return 77;
  }

  fprintf (stderr,
           "Waiting for taler-exchange-httpd to be ready\n");
  cnt = 0;
  do
    {
      fprintf (stderr, ".");
      sleep (1);
      cnt++;
      if (cnt > 60)
      {
        fprintf (stderr,
                 "\nFailed to start taler-exchange-httpd\n");
        GNUNET_OS_process_kill (exchanged,
                                SIGKILL);
        GNUNET_OS_process_wait (exchanged);
        GNUNET_OS_process_destroy (exchanged);
        return 77;
      }
    }
  while (0 != system ("wget -q -t 1 -T 1 " EXCHANGE_URI "keys -o /dev/null -O /dev/null"));
  fprintf (stderr, "\n");

  merchantd = GNUNET_OS_start_process (GNUNET_NO,
                                       GNUNET_OS_INHERIT_STD_ALL,
                                       NULL, NULL, NULL,
                                       "taler-merchant-httpd",
                                       "taler-merchant-httpd",
                                       "-L", "DEBUG",
                                       NULL);
  if (NULL == merchantd)
  {
    fprintf (stderr,
             "Failed to run taler-merchant-httpd. Check your PATH.\n");
    GNUNET_OS_process_kill (exchanged,
                            SIGKILL);
    GNUNET_OS_process_wait (exchanged);
    GNUNET_OS_process_destroy (exchanged);
    return 77;
  }
  /* give child time to start and bind against the socket */
  fprintf (stderr,
           "Waiting for taler-merchant-httpd to be ready\n");
  cnt = 0;
  do
    {
      fprintf (stderr, ".");
      sleep (1);
      cnt++;
      if (cnt > 60)
      {
        fprintf (stderr,
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
  while (0 != system ("wget -q -t 1 -T 1 " MERCHANT_URI " -o /dev/null -O /dev/null"));
  fprintf (stderr, "\n");

  result = GNUNET_SYSERR;
  sigpipe = GNUNET_DISK_pipe (GNUNET_NO, GNUNET_NO, GNUNET_NO, GNUNET_NO);
  GNUNET_assert (NULL != sigpipe);
  shc_chld = GNUNET_SIGNAL_handler_install (GNUNET_SIGCHLD,
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
