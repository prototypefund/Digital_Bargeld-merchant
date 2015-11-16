/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/

/**
 * @file merchant/backend/taler-merchant-httpd.c
 * @brief HTTP serving layer intended to perform crypto-work and
 * communication with the mint
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <curl/curl.h>
#include <taler/taler_util.h>
#include <taler/taler_mint_service.h>
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "merchant_db.h"
#include "merchant.h"
#include "taler_merchant_lib.h"
#include "taler-mint-httpd_mhd.h"
#include "taler-merchant-httpd_contract.h"
#include "taler-merchant-httpd_pay.h"

/**
 * Our hostname
 */
static char *hostname;

/**
 * The port we are running on
 */
static long long unsigned port;

/**
 * Merchant's private key
 */
struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;

/**
 * File holding the merchant's private key
 */
char *keyfile;


/**
 * This value tells the mint by which date this merchant would like
 * to receive the funds for a deposited payment
 */
struct GNUNET_TIME_Relative edate_delay;

/**
 * To make 'TMH_PARSE_navigate_json ()' compile
 */
char *TMH_mint_currency_string;

/**
 * Trusted mints
 */
struct MERCHANT_Mint *mints;

/**
 * Active auditors
 */
struct MERCHANT_Auditor *auditors;

/**
 * Shutdown task identifier
 */
static struct GNUNET_SCHEDULER_Task *shutdown_task;

/**
 * Task running the HTTP server.
 */
static struct GNUNET_SCHEDULER_Task *mhd_task;

/**
 * Context "poller" identifier
 */
struct GNUNET_SCHEDULER_Task *poller_task;

/**
 * Our wireformat
 */
struct MERCHANT_WIREFORMAT_Sepa *wire;

/**
 * Salt used to hash the wire object
 */
long long salt;

/**
 * The number of accepted mints
 */
unsigned int nmints;

/**
 * The number of active auditors
 */
unsigned int nauditors;

/**
 * Should we do a dry run where temporary tables are used for storing the data.
 */
static int dry;

/**
 * Global return code
 */
static int result;

/**
 * Connection handle to the our database
 */
PGconn *db_conn;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
 * A client has requested the given url using the given method
 * (#MHD_HTTP_METHOD_GET, #MHD_HTTP_METHOD_PUT,
 * #MHD_HTTP_METHOD_DELETE, #MHD_HTTP_METHOD_POST, etc).  The callback
 * must call MHD callbacks to provide content to give back to the
 * client and return an HTTP status code (i.e. #MHD_HTTP_OK,
 * #MHD_HTTP_NOT_FOUND, etc.).
 *
 * @param cls argument given together with the function
 *        pointer when the handler was registered with MHD
 * @param url the requested url
 * @param method the HTTP method used (#MHD_HTTP_METHOD_GET,
 *        #MHD_HTTP_METHOD_PUT, etc.)
 * @param version the HTTP version string (i.e.
 *        #MHD_HTTP_VERSION_1_1)
 * @param upload_data the data being uploaded (excluding HEADERS,
 *        for a POST that fits into memory and that is encoded
 *        with a supported encoding, the POST data will NOT be
 *        given in upload_data and is instead available as
 *        part of #MHD_get_connection_values; very large POST
 *        data *will* be made available incrementally in
 *        @a upload_data)
 * @param upload_data_size set initially to the size of the
 *        @a upload_data provided; the method must update this
 *        value to the number of bytes NOT processed;
 * @param con_cls pointer that the callback can set to some
 *        address and that will be preserved by MHD for future
 *        calls for this request; since the access handler may
 *        be called many times (i.e., for a PUT/POST operation
 *        with plenty of upload data) this allows the application
 *        to easily associate some request-specific state.
 *        If necessary, this state can be cleaned up in the
 *        global #MHD_RequestCompletedCallback (which
 *        can be set with the #MHD_OPTION_NOTIFY_COMPLETED).
 *        Initially, `*con_cls` will be NULL.
 * @return #MHD_YES if the connection was handled successfully,
 *         #MHD_NO if the socket must be closed due to a serios
 *         error while handling the request
 */
static int
url_handler (void *cls,
             struct MHD_Connection *connection,
             const char *url,
             const char *method,
             const char *version,
             const char *upload_data,
             size_t *upload_data_size,
             void **con_cls)
{
  static struct TMH_RequestHandler handlers[] =
    {
      /* Landing page, tell humans to go away. */
      { "/", MHD_HTTP_METHOD_GET, "text/plain",
        "Hello, I'm a merchant's Taler backend. This HTTP server is not for humans.\n", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK },

      /* Further test page */
      { "/hello", MHD_HTTP_METHOD_GET, "text/plain",
        "Hello, Customer.\n", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK },

      { "/contract", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_contract, MHD_HTTP_OK },

      { "/contract", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      { "/pay", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_pay, MHD_HTTP_OK },

      { "/pay", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },


      {NULL, NULL, NULL, NULL, 0, 0 }
    };

  static struct TMH_RequestHandler h404 =
    {
      "", NULL, "text/html",
      "<html><title>404: not found</title></html>", 0,
      &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
    };

  /* Compiler complains about non returning a value in a non-void
    declared function: the FIX is to return what the handler for
    a particular URL returns */

  struct TMH_RequestHandler *rh;
  unsigned int i;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Handling request for URL '%s'\n",
              url);

  for (i=0;NULL != handlers[i].url;i++)
  {
    rh = &handlers[i];
    if ( (0 == strcasecmp (url,
                           rh->url)) &&
         ( (NULL == rh->method) ||
           (0 == strcasecmp (method,
                             rh->method)) ) )
      return rh->handler (rh,
                          connection,
                          con_cls,
                          upload_data,
                          upload_data_size);
  }
  return TMH_MHD_handler_static_response (&h404,
                                          connection,
                                          con_cls,
                                          upload_data,
                                          upload_data_size);

}

/**
 * Function called with information about who is auditing
 * a particular mint and what key the mint is using.
 *
 * @param cls closure, will be 'struct MERCHANT_Mint' so that
 * when this function gets called, it will change the flag 'pending'
 * to 'false'. Note: 'keys' is automatically saved inside the mint's
 * handle, which is contained inside 'struct MERCHANT_Mint', when
 * this callback is called. Thus, once 'pending' turns 'false',
 * it is safe to call 'TALER_MINT_get_keys()' on the mint's handle,
 * in order to get the "good" keys.
 *
 * @param keys information about the various keys used
 *        by the mint
 */
static void
keys_mgmt_cb (void *cls, const struct TALER_MINT_Keys *keys)
{
  /* HOT UPDATE: the merchants need the denomination keys!
    Because it wants to (firstly) verify the deposit confirmation
    sent by the mint, and the signed blob depends (among the
    other things) on the coin's deposit fee. That information
    is never communicated by the wallet to the merchant.
    Again, the merchant needs it because it wants to verify that
    the wallet didn't exceede the limit imposed by the merchant
    on the total deposit fee for a purchase */

  if (NULL != keys)
  {
    ((struct MERCHANT_Mint *) cls)->pending = 0;
  }
  else
    printf ("no keys gotten\n");

  printf ("cb: poller_task [%p]\n", poller_task);
}


/**
 * Shutdown task (magically invoked when the application is being
 * quit)
 *
 * @param cls NULL
 * @param tc scheduler task context
 */
static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  unsigned int cnt;

  for (cnt = 0; cnt < nmints; cnt++)
  {
    if (NULL != mints[cnt].conn)
      TALER_MINT_disconnect (mints[cnt].conn);

  }
  if (NULL != poller_task)
  {
    GNUNET_SCHEDULER_cancel (poller_task);
    poller_task = NULL;
  }
  if (NULL != mhd_task)
  {
    GNUNET_SCHEDULER_cancel (mhd_task);
    mhd_task = NULL;
  }
  if (NULL != mhd)
  {
    MHD_stop_daemon (mhd);
    mhd = NULL;
  }
  if (NULL != db_conn)
  {
    MERCHANT_DB_disconnect (db_conn);
    db_conn = NULL;
  }
  if (NULL != keyfile)
    GNUNET_free (privkey);
}


/**
 * Task that runs the context's event loop using the GNUnet scheduler.
 *
 * @param cls mint context
 * @param tc scheduler context (unused)
 */
void
context_task (void *cls,
              const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  long timeout;
  int max_fd;
  fd_set read_fd_set;
  fd_set write_fd_set;
  fd_set except_fd_set;
  struct GNUNET_NETWORK_FDSet *rs;
  struct GNUNET_NETWORK_FDSet *ws;
  struct GNUNET_TIME_Relative delay;
  struct TALER_MINT_Context *ctx;

  ctx = (struct TALER_MINT_Context *) cls;
  poller_task = NULL;
  TALER_MINT_perform (ctx);
  max_fd = -1;
  timeout = -1;
  FD_ZERO (&read_fd_set);
  FD_ZERO (&write_fd_set);
  FD_ZERO (&except_fd_set);
  TALER_MINT_get_select_info (ctx,
                              &read_fd_set,
                              &write_fd_set,
                              &except_fd_set,
                              &max_fd,
                              &timeout);
  if (timeout >= 0)
    delay =
    GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MILLISECONDS,
                                   timeout);
  else
    delay = GNUNET_TIME_UNIT_FOREVER_REL;
  rs = GNUNET_NETWORK_fdset_create ();
  GNUNET_NETWORK_fdset_copy_native (rs,
                                    &read_fd_set,
                                    max_fd + 1);
  ws = GNUNET_NETWORK_fdset_create ();
  GNUNET_NETWORK_fdset_copy_native (ws,
                                    &write_fd_set,
                                    max_fd + 1);
  poller_task =
  GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_DEFAULT,
                               delay,
                               rs,
                               ws,
                               &context_task,
                               cls);
  printf ("scheduling poller_task [%p]\n", poller_task);
  GNUNET_NETWORK_fdset_destroy (rs);
  GNUNET_NETWORK_fdset_destroy (ws);
}


/**
 * Function called whenever MHD is done with a request.  If the
 * request was a POST, we may have stored a `struct Buffer *` in the
 * @a con_cls that might still need to be cleaned up.  Call the
 * respective function to free the memory.
 *
 * @param cls client-defined closure
 * @param connection connection handle
 * @param con_cls value as set by the last call to
 *        the #MHD_AccessHandlerCallback
 * @param toe reason for request termination
 * @see #MHD_OPTION_NOTIFY_COMPLETED
 * @ingroup request
 */
static void
handle_mhd_completion_callback (void *cls,
                                struct MHD_Connection *connection,
                                void **con_cls,
                                enum MHD_RequestTerminationCode toe)
{
  if (NULL == *con_cls)
    return;
  TMH_PARSE_post_cleanup_callback (*con_cls);
  *con_cls = NULL;
}


/**
 * Function that queries MHD's select sets and
 * starts the task waiting for them.
 */
static struct GNUNET_SCHEDULER_Task *
prepare_daemon (void);


/**
 * Call MHD to process pending requests and then go back
 * and schedule the next run.
 *
 * @param cls the `struct MHD_Daemon` of the HTTP server to run
 * @param tc scheduler context
 */
static void
run_daemon (void *cls,
            const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  mhd_task = NULL;
  GNUNET_assert (MHD_YES == MHD_run (mhd));
  mhd_task = prepare_daemon ();
}


/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 */
void
trigger_daemon ()
{
  GNUNET_SCHEDULER_cancel (mhd_task);
  run_daemon (NULL, NULL);
}


/**
 * Function that queries MHD's select sets and
 * starts the task waiting for them.
 *
 * @param daemon_handle HTTP server to prepare to run
 */
static struct GNUNET_SCHEDULER_Task *
prepare_daemon ()
{
  struct GNUNET_SCHEDULER_Task * ret;
  fd_set rs;
  fd_set ws;
  fd_set es;
  struct GNUNET_NETWORK_FDSet *wrs;
  struct GNUNET_NETWORK_FDSet *wws;
  int max;
  MHD_UNSIGNED_LONG_LONG timeout;
  int haveto;
  struct GNUNET_TIME_Relative tv;

  FD_ZERO (&rs);
  FD_ZERO (&ws);
  FD_ZERO (&es);
  wrs = GNUNET_NETWORK_fdset_create ();
  wws = GNUNET_NETWORK_fdset_create ();
  max = -1;
  GNUNET_assert (MHD_YES ==
                 MHD_get_fdset (mhd,
                                &rs,
                                &ws,
                                &es,
                                &max));
  haveto = MHD_get_timeout (mhd, &timeout);
  if (haveto == MHD_YES)
    tv.rel_value_us = (uint64_t) timeout * 1000LL;
  else
    tv = GNUNET_TIME_UNIT_FOREVER_REL;
  GNUNET_NETWORK_fdset_copy_native (wrs, &rs, max + 1);
  GNUNET_NETWORK_fdset_copy_native (wws, &ws, max + 1);
  ret =
      GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_HIGH,
				   tv, wrs, wws,
                                   &run_daemon,
                                   NULL);
  GNUNET_NETWORK_fdset_destroy (wrs);
  GNUNET_NETWORK_fdset_destroy (wws);
  return ret;
}



/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be
 * NULL!)
 * @param config configuration
 */
void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{

  unsigned int cnt;
  mints = NULL;
  keyfile = NULL;
  result = GNUNET_SYSERR;
  shutdown_task =
  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
                                &do_shutdown,
				NULL);

  GNUNET_log (GNUNET_ERROR_TYPE_INFO, "merchant launched\n");

  EXITIF (GNUNET_SYSERR ==
         (nmints =
	 TALER_MERCHANT_parse_mints (config,
                                     &mints)));
  EXITIF (GNUNET_SYSERR ==
         (nauditors =
	 TALER_MERCHANT_parse_auditors (config,
                                        &auditors)));
  EXITIF (NULL ==
         (wire =
	 TALER_MERCHANT_parse_wireformat_sepa (config)));
  EXITIF (GNUNET_OK !=
         GNUNET_CONFIGURATION_get_value_filename (config,
                                                  "merchant",
                                                  "KEYFILE",
                                                  &keyfile));
  EXITIF (NULL ==
         (privkey =
	 GNUNET_CRYPTO_eddsa_key_create_from_file (keyfile)));
  EXITIF (NULL ==
         (db_conn = MERCHANT_DB_connect (config)));
  EXITIF (GNUNET_OK !=
          MERCHANT_DB_initialize (db_conn, dry));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_number (config,
                                                 "merchant",
                                                 "PORT",
                                                 &port));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_string (config,
                                                 "merchant",
                                                 "HOSTNAME",
                                                 &hostname));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_string (config,
                                                 "merchant",
                                                 "CURRENCY",
                                                 &TMH_mint_currency_string));

  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_time (config,
                                                 "merchant",
                                                 "EDATE",
                                                 &edate_delay));

  salt = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE,
                                   UINT64_MAX);

  for (cnt = 0; cnt < nmints; cnt++)
  {
    EXITIF (NULL == (mints[cnt].ctx = TALER_MINT_init ()));
    mints[cnt].pending = 1;
    mints[cnt].conn = TALER_MINT_connect (mints[cnt].ctx,
                                          mints[cnt].hostname,
                                          &keys_mgmt_cb,
                                          &mints[cnt],
					  TALER_MINT_OPTION_END); 
    EXITIF (NULL == mints[cnt].conn);
    poller_task =
    GNUNET_SCHEDULER_add_now (&context_task, mints[cnt].ctx);
  }

  mhd = MHD_start_daemon (MHD_USE_SUSPEND_RESUME,
                          port,
                          NULL, NULL,
                          &url_handler, NULL,
			  MHD_OPTION_NOTIFY_COMPLETED,
			  &handle_mhd_completion_callback, NULL,
                          MHD_OPTION_END);
  EXITIF (NULL == mhd);
  result = GNUNET_OK;
  mhd_task = prepare_daemon ();

  EXITIF_exit:
    if (GNUNET_OK != result)
      GNUNET_SCHEDULER_shutdown ();
  GNUNET_free_non_null (keyfile);
    if (GNUNET_OK != result)
      GNUNET_SCHEDULER_shutdown ();

}

/**
 * The main function of the serve tool
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{

   static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'t', "temp", NULL,
     gettext_noop ("Use temporary database tables"), GNUNET_NO,
     &GNUNET_GETOPT_set_one, &dry},
     GNUNET_GETOPT_OPTION_END
    };


  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-merchant-http",
                          "Serve merchant's HTTP interface",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;

}
