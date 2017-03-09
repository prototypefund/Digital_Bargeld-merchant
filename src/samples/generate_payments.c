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

#define EXCHANGE_URI "http://localhost:8081/"

#define MERCHANT_URI "http://localhost:8082"

#define BANK_URI "http://localhost:8083/"

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
 * Configuration handle.
 */
struct GNUNET_CONFIGURATION_Handle *cfg;

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
                                             "http://localhost:18080/",
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
                                             "http://localhost:18080/",
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

  GNUNET_CONFIGURATION_destroy (cfg);
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
      .details.admin_add_incoming.sender_details = "{ \"type\":\"test\", \"bank_uri\":\"" BANK_URI "\", \"account_number\":62, \"uuid\":1 }",
      .details.admin_add_incoming.transfer_details = "{ \"uuid\": 1}",
      .details.admin_add_incoming.amount = "EUR:5.01" },
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
main ()
{
  struct GNUNET_OS_Process *proc;
  struct GNUNET_OS_Process *exchanged;
  struct GNUNET_OS_Process *merchantd;
  unsigned int cnt;
  struct GNUNET_SIGNAL_Context *shc_chld;


  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("merchant-create-payments",
                    "DEBUG",
                    NULL);
  cfg = GNUNET_CONFIGURATION_create ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONFIGURATION_load (cfg,
                                            "merchant_generate_payments.conf"));


  proc = GNUNET_OS_start_process (GNUNET_NO,
                                  GNUNET_OS_INHERIT_STD_ALL,
                                  NULL, NULL, NULL,
                                  "taler-exchange-keyup",
                                  "taler-exchange-keyup",
                                  "-c", "merchant_generate_payments.conf",
                                  NULL);
  if (NULL == proc)
  {
    fprintf (stderr,
             "Failed to run taler-exchange-keyup. Check your PATH.\n");
    return 77;
  }


  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);

  proc = GNUNET_OS_start_process (GNUNET_NO,
                                  GNUNET_OS_INHERIT_STD_ALL,
                                  NULL, NULL, NULL,
                                  "taler-exchange-dbinit",
                                  "taler-exchange-dbinit",
                                  "-c", "merchant_generate_payments.conf",
                                  "-r",
                                  NULL);
  if (NULL == proc)
  {
    fprintf (stderr,
             "Failed to run taler-exchange-dbinit. Check your PATH.\n");
    return 77;
  }
  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "About to launch the exchange.\n");

  exchanged = GNUNET_OS_start_process (GNUNET_NO,
                                       GNUNET_OS_INHERIT_STD_ALL,
                                       NULL, NULL, NULL,
                                       "taler-exchange-httpd",
                                       "taler-exchange-httpd",
                                       "-c", "merchant_generate_payments.conf",
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
                                       "-c", "merchant_generate_payments.conf",
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
