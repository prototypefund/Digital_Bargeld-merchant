/*
  This file is part of TALER
  Copyright (C) 2017 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant-tools/taler-merchant-tip-enable.c
 * @brief enable tips by telling the backend that a reserve was charged
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_util.h>
#include <gnunet/gnunet_util_lib.h>
#include "taler_merchant_service.h"
#include <microhttpd.h> /* just for HTTP status code, no need to link against */

/**
 * Return value from main().
 */
static int global_ret;

/**
 * -a option: how much money was deposited into the reserve
 */
static struct TALER_Amount amount;

/**
 * For which instance did we charge the reserve?
 */
static char *instance;

/**
 * Under which URI does the backend run?
 */
static char *backend_uri;

/**
 * UUID of the wire transfer.
 */
static char *credit_uuid;

/**
 * Expiration time for the reserve.
 */
static struct GNUNET_TIME_Absolute expiration;

/**
 * Main execution context for the main loop of the exchange.
 */
static struct GNUNET_CURL_Context *ctx;

/**
 * Context for running the #ctx's event loop.
 */
static struct GNUNET_CURL_RescheduleContext *rc;

/**
 * Handle for the /tip-enable operation.
 */
static struct TALER_MERCHANT_TipEnableOperation *teo;


/**
 * Function run when the test terminates (good or bad).
 * Cleans up our state.
 *
 * @param cls the interpreter state.
 */
static void
do_shutdown (void *cls)
{
  if (NULL != teo)
  {
    TALER_MERCHANT_tip_enable_cancel (teo);
    teo = NULL;
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
 * Callback for a /tip-enable request.  Returns the result of
 * the operation.
 *
 * @param cls closure
 * @param http_status HTTP status returned by the merchant backend
 * @param ec taler-specific error code
 */
static void
enable_cb (void *cls,
           unsigned int http_status,
           enum TALER_ErrorCode ec)
{
  teo = NULL;
  GNUNET_SCHEDULER_shutdown ();
  if ( (MHD_HTTP_OK == http_status) &&
       (TALER_EC_NONE == ec) )
  {
    global_ret = 0;
    return;
  }
  fprintf (stderr,
           "Failed with HTTP status %u and error code %u\n",
           http_status,
           ec);
  global_ret = 3;
}


/**
 * Main function that will be run.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param cfg configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct TALER_ReservePrivateKeyP reserve_priv;
  char *section;
  char *res_str;
  struct GNUNET_HashCode hcredit_uuid;
  struct GNUNET_CURL_Context *ctx;

  GNUNET_asprintf (&section,
                   "merchant-instance-%s",
                   instance);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "TIP_RESERVE_PRIV",
                                             &res_str))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "TIP_RESERVE_PRIV");
    GNUNET_free (section);
    global_ret = 1;
    return;
  }
  if (GNUNET_OK !=
      GNUNET_STRINGS_string_to_data (res_str,
                                     strlen (res_str),
                                     &reserve_priv,
                                     sizeof (struct TALER_ReservePrivateKeyP)))
  {
    GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "TIP_RESERVE_PRIV",
                               "Must decode to private EdDSA key");
    GNUNET_free (section);
    GNUNET_free (res_str);
    global_ret = 1;
    return;
  }
  GNUNET_free (res_str);
  GNUNET_free (section);

  GNUNET_CRYPTO_hash (credit_uuid,
                      strlen (credit_uuid),
                      &hcredit_uuid);

  ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                          &rc);
  if (NULL == ctx)
  {
    GNUNET_break (0);
    global_ret = 1;
    return;
  }
  rc = GNUNET_CURL_gnunet_rc_create (ctx);
  teo = TALER_MERCHANT_tip_enable (ctx,
                                   backend_uri,
                                   &amount,
                                   expiration,
                                   &reserve_priv,
                                   &hcredit_uuid,
                                   &enable_cb,
                                   NULL);
  GNUNET_assert (NULL != teo);
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 NULL);
}


/**
 * The main function of the database initialization tool.
 * Used to initialize the Taler Exchange's database.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc,
      char *const *argv)
{
  struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_option_mandatory
    (TALER_getopt_get_amount ('a',
                              "amount",
                              "VALUE",
                              "value that was added to the reserve",
                              &amount)),
    GNUNET_GETOPT_option_mandatory
    (GNUNET_GETOPT_option_string ('b',
                                  "backend",
                                  "URI",
                                  "URI of the backend to use",
                                  &backend_uri)),
    GNUNET_GETOPT_option_mandatory
    (GNUNET_GETOPT_option_string ('C',
                                  "credit-uuid",
                                  "UUID",
                                  "unique identifier of the wire transfer (to detect duplicate invocations)",
                                  &credit_uuid)),
    GNUNET_GETOPT_option_mandatory
    (GNUNET_GETOPT_option_absolute_time ('e',
                                         "expiration",
                                         "TIMESTAMP",
                                         "when does the reserve expire",
                                         &expiration)),
    GNUNET_GETOPT_option_mandatory
    (GNUNET_GETOPT_option_string ('i',
                                  "instance",
                                  "NAME",
                                  "name of the instance of which the reserve was charged",
                                  &instance)),
    GNUNET_GETOPT_OPTION_END
  };

  /* force linker to link against libtalerutil; if we do
     not do this, the linker may "optimize" libtalerutil
     away and skip #TALER_OS_init(), which we do need */
  (void) TALER_project_data_default ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-merchant-tip-enable",
                                   "INFO",
                                   NULL));
  global_ret = 2;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-merchant-tip-enable",
			  "Enable tipping by telling the backend that a reserve was charged",
			  options,
			  &run,
                          NULL))
    return 1;
  return global_ret;
}


/* end of taler-exchange-tip-enable.c */
