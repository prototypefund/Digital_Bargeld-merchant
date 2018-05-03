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
 * @file lib/testing_api_helpers.c
 * @brief helper functions for test library.
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_testing_lib.h"


/**
 * Start the merchant backend process.  Assume the port
 * is available and the database is clean.  Use the "prepare
 * merchant" function to do such tasks.
 *
 * @param config_filename configuration filename.
 *
 * @return the process, or NULL if the process could not
 *         be started.
 */
struct GNUNET_OS_Process *
TALER_TESTING_run_merchant (const char *config_filename)
{
  struct GNUNET_CONFIGURATION_Handle *cfg;
  struct GNUNET_OS_Process *merchant_proc;
  unsigned int iter;
  unsigned long long port;
  char *wget_cmd;

  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_load (cfg,
                                 config_filename))
    MERCHANT_FAIL ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_number (cfg,
                                             "merchant",
                                             "PORT",
                                             &port))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "PORT");
    GNUNET_CONFIGURATION_destroy (cfg);
    MERCHANT_FAIL ();
  }
  GNUNET_CONFIGURATION_destroy (cfg);

  merchant_proc
    = GNUNET_OS_start_process (GNUNET_NO,
                               GNUNET_OS_INHERIT_STD_ALL,
                               NULL, NULL, NULL,
                               "taler-merchant-httpd",
                               "taler-merchant-httpd",
                               #ifdef CUSTOM_CONFIG
                               "-c", config_filename,
                               #endif
                               NULL);
  if (NULL == merchant_proc)
    MERCHANT_FAIL ();

  GNUNET_asprintf (&wget_cmd,
                   "wget -q -t 1 -T 1 http://127.0.0.1:%llu/"
                   " -o /dev/null -O /dev/null",
                   port);

  /* give child time to start and bind against the socket */
  fprintf (stderr,
           "Waiting for `taler-merchant-httpd' to be ready\n");
  iter = 0;
  do
    {
      if (10 == iter)
      {
	fprintf (stderr,
                 "Failed to launch `taler-merchant-httpd' (or `wget')\n");
	GNUNET_OS_process_kill (merchant_proc,
				SIGTERM);
	GNUNET_OS_process_wait (merchant_proc);
	GNUNET_OS_process_destroy (merchant_proc);
	MERCHANT_FAIL ();
      }
      fprintf (stderr, ".\n");
      sleep (1);
      iter++;
    }
  while (0 != system (wget_cmd));
  GNUNET_free (wget_cmd);
  fprintf (stderr, "\n");

  return merchant_proc;
}


/**
 * Prepare the merchant execution.  Create tables and check if
 * the port is available.
 *
 * @param config_filename configuration filename.
 *
 * @return the base url, or NULL upon errors.  Must be freed
 *         by the caller.
 */
char *
TALER_TESTING_prepare_merchant (const char *config_filename)
{
  struct GNUNET_CONFIGURATION_Handle *cfg;
  unsigned long long port;
  char *base_url;

  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_load (cfg,
                                 config_filename))
    MERCHANT_FAIL ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_number (cfg,
                                             "merchant",
                                             "PORT",
                                             &port))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "PORT");
    GNUNET_CONFIGURATION_destroy (cfg);
    MERCHANT_FAIL ();
  }

  GNUNET_CONFIGURATION_destroy (cfg);

  if (GNUNET_OK !=
      GNUNET_NETWORK_test_port_free (IPPROTO_TCP,
                                     (uint16_t) port))
  {
    fprintf (stderr,
             "Required port %llu not available, skipping.\n",
	     port);
    MERCHANT_FAIL ();
  }

  #ifdef PURGE_DATABASE
  struct GNUNET_OS_Process *dbinit_proc;
  enum GNUNET_OS_ProcessStatusType type;
  unsigned long code;

  /* DB preparation */
  if (NULL == (dbinit_proc = GNUNET_OS_start_process
    (GNUNET_NO,
     GNUNET_OS_INHERIT_STD_ALL,
     NULL, NULL, NULL,
     "taler-merchant-dbinit",
     "taler-merchant-dbinit",
     #ifdef CUSTOM_CONFIG
     "-c", config_filename,
     #endif
     "-r",
     NULL)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to run taler-merchant-dbinit."
                " Check your PATH.\n");
    MERCHANT_FAIL ();
  }

  if (GNUNET_SYSERR ==
      GNUNET_OS_process_wait_status (dbinit_proc,
                                     &type,
                                     &code))
  {
    GNUNET_OS_process_destroy (dbinit_proc);
    MERCHANT_FAIL ();
  }
  if ( (type == GNUNET_OS_PROCESS_EXITED) &&
       (0 != code) )
  {
    fprintf (stderr,
             "Failed to setup database\n");
    MERCHANT_FAIL ();
  }
  if ( (type != GNUNET_OS_PROCESS_EXITED) ||
       (0 != code) )
  {
    fprintf (stderr,
             "Unexpected error running"
             " `taler-merchant-dbinit'!\n");
    MERCHANT_FAIL ();
  }

  GNUNET_OS_process_destroy (dbinit_proc);
  #endif


  GNUNET_asprintf (&base_url,
                   "http://localhost:%llu/",
                   port);
  return base_url;
}
