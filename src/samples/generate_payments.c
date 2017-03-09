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

  };

};


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
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 */
static void
run (void *cls)
{
  while(1);
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
