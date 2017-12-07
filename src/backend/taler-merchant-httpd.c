/*
  This file is part of TALER
  (C) 2014-2017 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/backend/taler-merchant-httpd.c
 * @brief HTTP serving layer intended to perform crypto-work and
 * communication with the exchange
 * @author Marcello Stanisci
 * @author Christian Grothoff
 * @author Florian Dold
 */
#include "platform.h"
#include <microhttpd.h>
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_wire_plugin.h>
#include "taler-merchant-httpd_responses.h"
#include "taler_merchantdb_lib.h"
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_proposal.h"
#include "taler-merchant-httpd_pay.h"
#include "taler-merchant-httpd_track-transaction.h"
#include "taler-merchant-httpd_track-transfer.h"
#include "taler-merchant-httpd_tip-authorize.h"
#include "taler-merchant-httpd_tip-enable.h"
#include "taler-merchant-httpd_tip-pickup.h"
#include "taler-merchant-httpd_tip-query.h"
#include "taler-merchant-httpd_history.h"
#include "taler-merchant-httpd_refund.h"

/**
 * Backlog for listen operation on unix-domain sockets.
 */
#define UNIX_BACKLOG 500

/**
 * Hashmap pointing at merchant instances by 'id'. An 'id' is
 * just a string that identifies a merchant instance. When a frontend
 * needs to specify an instance to the backend, it does so by 'id'
 */
struct GNUNET_CONTAINER_MultiHashMap *by_id_map;

/**
 * Hashmap pointing at merchant instances by public key. This map
 * is mainly used to check whether there is more than one instance
 * using the same key
 */
struct GNUNET_CONTAINER_MultiHashMap *by_kpub_map;

/**
 * The port we are running on
 */
static long long unsigned port;

/**
 * This value tells the exchange by which date this merchant would like
 * to receive the funds for a deposited payment
 */
struct GNUNET_TIME_Relative wire_transfer_delay;

/**
 * If the frontend does NOT specify a payment deadline, how long should
 * offers we make be valid by default?
 */
struct GNUNET_TIME_Relative default_pay_deadline;

/**
 * Default maximum wire fee to assume, unless stated differently in the proposal
 * already.
 */
struct TALER_Amount default_max_wire_fee;

/**
 * Default max deposit fee that the merchant is willing to
 * pay; if deposit costs more, then the customer will cover
 * the difference.
 */
struct TALER_Amount default_max_deposit_fee;

/**
 * Default factor for wire fee amortization.
 */
unsigned long long default_wire_fee_amortization;

/**
 * Should a "Connection: close" header be added to each HTTP response?
 */
int TMH_merchant_connection_close;

/**
 * Task running the HTTP server.
 */
static struct GNUNET_SCHEDULER_Task *mhd_task;

/**
 * Global return code
 */
static int result;

/**
 * Connection handle to the our database
 */
struct TALER_MERCHANTDB_Plugin *db;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
 * Path for the unix domain-socket
 * to run the daemon on.
 */
static char *serve_unixpath;

/**
 * File mode for unix-domain socket.
 */
static mode_t unixpath_mode;


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
      { "/pay", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_pay, MHD_HTTP_OK },
      { "/pay", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },
      { "/track/transfer", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &MH_handler_track_transfer, MHD_HTTP_OK},
      { "/track/transfer", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK},
      { "/track/transaction", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &MH_handler_track_transaction, MHD_HTTP_OK},
      { "/track/transaction", NULL, "text/plain",
        "Only GET is allowed", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK},
      { "/history", MHD_HTTP_METHOD_GET, "text/plain",
        "Only GET is allowed", 0,
        &MH_handler_history, MHD_HTTP_OK},
      { "/proposal", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_proposal_put, MHD_HTTP_OK },
      { "/proposal", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_proposal_lookup, MHD_HTTP_OK},
      { "/proposal", NULL, "text/plain",
        "Only GET/POST are allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },
      { "/refund", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_refund_increase, MHD_HTTP_OK},
      { "/refund", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_refund_lookup, MHD_HTTP_OK},
      { "/refund", NULL, "application/json",
        "Only POST/GET are allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED},
      { "/tip-authorize", MHD_HTTP_METHOD_POST, "text/plain",
        NULL, 0,
        &MH_handler_tip_authorize, MHD_HTTP_OK},
      { "/tip-authorize", NULL, "application/json",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED},
      { "/tip-pickup", MHD_HTTP_METHOD_POST, "text/plain",
        NULL, 0,
        &MH_handler_tip_pickup, MHD_HTTP_OK},
      { "/tip-pickup", NULL, "application/json",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED},
      { "/tip-enable", MHD_HTTP_METHOD_POST, "text/plain",
        NULL, 0,
        &MH_handler_tip_enable, MHD_HTTP_OK},
      { "/tip-enable", NULL, "application/json",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED},
      { "/tip-query", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_tip_query, MHD_HTTP_OK},
      {NULL, NULL, NULL, NULL, 0, 0 }
    };
  static struct TMH_RequestHandler h404 =
    {
      "", NULL, "text/html",
      "<html><title>404: not found</title></html>", 0,
      &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
    };
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Handling request for URL `%s'\n",
              url);
  for (unsigned int i=0;NULL != handlers[i].url;i++)
  {
    struct TMH_RequestHandler *rh = &handlers[i];

    if ( (0 == strcasecmp (url,
                           rh->url)) &&
         ( (NULL == rh->method) ||
           (0 == strcasecmp (method,
                             rh->method)) ) )
    {
      struct TM_HandlerContext *hc;
      int ret;

      ret = rh->handler (rh,
			 connection,
			 con_cls,
			 upload_data,
			 upload_data_size);
      hc = *con_cls;
      if (NULL != hc)
        hc->rh = rh;
      return ret;
    }
  }
  return TMH_MHD_handler_static_response (&h404,
                                          connection,
                                          con_cls,
                                          upload_data,
                                          upload_data_size);
}


/**
 * Callback that frees all the elements in the hashmap
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct MerchantInstance`
 */
static int
hashmap_free (void *cls,
              const struct GNUNET_HashCode *key,
              void *value)
{
  struct MerchantInstance *mi = value;

  json_decref (mi->j_wire);
  GNUNET_free (mi->id);
  GNUNET_free (mi->keyfile);
  GNUNET_free_non_null (mi->tip_exchange);
  GNUNET_free (mi);
  return GNUNET_YES;
}


/**
 * Shutdown task (magically invoked when the application is being
 * quit)
 *
 * @param cls NULL
 */
static void
do_shutdown (void *cls)
{
  if (NULL != mhd_task)
  {
    GNUNET_SCHEDULER_cancel (mhd_task);
    mhd_task = NULL;
  }
  MH_force_pc_resume ();
  if (NULL != mhd)
  {
    MHD_stop_daemon (mhd);
    mhd = NULL;
  }
  if (NULL != db)
  {
    TALER_MERCHANTDB_plugin_unload (db);
    db = NULL;
  }
  TMH_EXCHANGES_done ();
  TMH_AUDITORS_done ();

  GNUNET_CONTAINER_multihashmap_iterate (by_id_map,
                                         &hashmap_free,
                                         NULL);
  if (NULL != by_id_map)
  {
    GNUNET_CONTAINER_multihashmap_destroy (by_id_map);
    by_id_map = NULL;
  }
  if (NULL != by_kpub_map)
  {
    GNUNET_CONTAINER_multihashmap_destroy (by_kpub_map);
    by_kpub_map = NULL;
  }
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
  struct TM_HandlerContext *hc = *con_cls;

  if (NULL == hc)
    return;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finished handling request for `%s' with status %d\n",
              hc->rh->url,
              (int) toe);
  hc->cc (hc);
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
 */
static void
run_daemon (void *cls)
{
  mhd_task = NULL;
  GNUNET_assert (MHD_YES == MHD_run (mhd));
  mhd_task = prepare_daemon ();
}


/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 * Basically, we need to explicitly resume MHD's event loop whenever
 * we made progress serving a request.  This function re-schedules
 * the task processing MHD's activities to run immediately.
 */
void
TMH_trigger_daemon ()
{
  GNUNET_SCHEDULER_cancel (mhd_task);
  run_daemon (NULL);
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
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Adding run_daemon select task\n");
  ret = GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_HIGH,
				     tv,
				     wrs,
				     wws,
				     &run_daemon,
				     NULL);
  GNUNET_NETWORK_fdset_destroy (wrs);
  GNUNET_NETWORK_fdset_destroy (wws);
  return ret;
}


/**
 * Callback that looks for 'merchant-instance-*' sections,
 * and populates accordingly each instance's data
 *
 * @param cls closure
 * @section section name this callback gets
 */
static void
instances_iterator_cb (void *cls,
                       const char *section)
{
  char *substr;
  char *token;
  char *instance_wiresection;
  struct MerchantInstance *mi;
  struct IterateInstancesCls *iic;
  struct GNUNET_CRYPTO_EddsaPrivateKey *pk;
  /* used as hashmap keys */
  struct GNUNET_HashCode h_pk;
  struct GNUNET_HashCode h_id;
  json_t *type;
  char *emsg;

  iic = cls;
  substr = strstr (section, "merchant-instance-");

  if ( (NULL == substr) ||
       (NULL != strstr (section,
                        "merchant-instance-wireformat-")) )
    return;

  if (substr != section)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to specify a merchant instance\n");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  /** Get id **/
  token = strrchr (section, '-');
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Extracted token: %s\n",
              token + 1);
  mi = GNUNET_new (struct MerchantInstance);

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (iic->config,
                                               section,
                                               "KEYFILE",
                                               &mi->keyfile))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "KEYFILE");
    GNUNET_free (mi);
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_OK ==
      GNUNET_CONFIGURATION_get_value_string (iic->config,
                                             section,
                                             "TIP_EXCHANGE",
                                             &mi->tip_exchange))
  {
    char *tip_reserves;
    struct GNUNET_CRYPTO_EddsaPrivateKey *pk;

    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_get_value_string (iic->config,
                                               section,
                                               "TIP_RESERVE_PRIV_FILENAME",
                                               &tip_reserves))
    {
      GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "TIP_RESERVE_PRIV_FILENAME");
      GNUNET_free (mi);
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    pk = GNUNET_CRYPTO_eddsa_key_create_from_file (tip_reserves);
    if (NULL == pk)
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "TIP_RESERVE_PRIV_FILENAME",
                                 "Failed to read private key");
      GNUNET_free (tip_reserves);
      GNUNET_free (mi);
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
    mi->tip_reserve.eddsa_priv = *pk;
    GNUNET_free (pk);
    GNUNET_free (tip_reserves);
  }

  if (GNUNET_YES !=
      GNUNET_DISK_file_test (mi->keyfile))
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Merchant private key `%s' does not exist yet, creating it!\n",
                mi->keyfile);
  if (NULL ==
       (pk = GNUNET_CRYPTO_eddsa_key_create_from_file (mi->keyfile)))
  {
    GNUNET_break (0);
    GNUNET_free (mi->keyfile);
    GNUNET_free (mi);
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  mi->privkey.eddsa_priv = *pk;
  GNUNET_CRYPTO_eddsa_key_get_public (pk,
                                      &mi->pubkey.eddsa_pub);
  GNUNET_free (pk);

  mi->id = GNUNET_strdup (token + 1);
  if (0 == strcmp ("default", mi->id))
    iic->default_instance = GNUNET_YES;

  GNUNET_asprintf (&instance_wiresection,
                   "merchant-instance-wireformat-%s",
                   mi->id);
  mi->j_wire = iic->plugin->get_wire_details (iic->plugin->cls,
                                              iic->config,
                                              instance_wiresection);
  GNUNET_free (instance_wiresection);
  if ( (NULL == (type = json_object_get (mi->j_wire,
                                         "type"))) ||
       (! json_is_string (type)) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Malformed wireformat: lacks type\n");
    iic->ret |= GNUNET_SYSERR;
  }
  mi->wire_method = json_string_value (type);

  if (TALER_EC_NONE !=
      iic->plugin->wire_validate (iic->plugin->cls,
                                  mi->j_wire,
                                  NULL,
                                  &emsg))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Malformed wireformat: %s\n",
                emsg);
    GNUNET_free (emsg);
    iic->ret |= GNUNET_SYSERR;
  }

  if (GNUNET_OK !=
      TALER_JSON_hash (mi->j_wire,
                       &mi->h_wire))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to hash wireformat\n");
    iic->ret |= GNUNET_SYSERR;
  }
#define EXTRADEBUG
#ifdef EXTRADEBUGG
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Found wireformat instance:\n");
              json_dumpf (mi->j_wire, stdout, 0);
              printf ("\n");
#endif

  GNUNET_CRYPTO_hash (mi->id,
                      strlen (mi->id),
                      &h_id);
  GNUNET_CRYPTO_hash (&mi->pubkey.eddsa_pub,
                      sizeof (struct GNUNET_CRYPTO_EddsaPublicKey),
                      &h_pk);
  if (GNUNET_OK !=
      GNUNET_CONTAINER_multihashmap_put (by_id_map,
                                         &h_id,
                                         mi,
                                         GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to put an entry into the 'by_id' hashmap\n");
    iic->ret |= GNUNET_SYSERR;
  }
  if (GNUNET_OK !=
      GNUNET_CONTAINER_multihashmap_put (by_kpub_map,
                                         &h_pk,
                                         mi,
                                         GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to put an entry into the 'by_kpub_map' hashmap\n");
    iic->ret |= GNUNET_SYSERR;
  }
}


/**
 * Lookup a merchant instance by its name.
 *
 * @param name name of the instance to resolve
 * @return NULL if that instance is unknown to us
 */
struct MerchantInstance *
TMH_lookup_instance (const char *name)
{
  struct GNUNET_HashCode h_instance;

  GNUNET_CRYPTO_hash (name,
                      strlen (name),
                      &h_instance);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Looking for by-id key %s of '%s' in hashmap\n",
              GNUNET_h2s (&h_instance),
              name);
  /* We're fine if that returns NULL, the calling routine knows how
     to handle that */
  return GNUNET_CONTAINER_multihashmap_get (by_id_map,
                                            &h_instance);
}


/**
 * Extract merchant instance from the given JSON
 *
 * @param json the JSON to inspect; it is not required to
 * comply with any particular format. It will only be checked
 * if the field "instance" is there.
 * @return a pointer to a #struct MerchantInstance. This will be
 * the 'default' merchant if the frontend did not specif any
 * "instance" field. The user should not care to free the returned
 * value, as it is taken from a global array that will be freed
 * by the general shutdown routine. NULL if the frontend specified
 * a wrong instance
 */
struct MerchantInstance *
TMH_lookup_instance_json (struct json_t *json)
{
  struct json_t *instance;
  const char *instance_str;

  if (NULL == (instance = json_object_get (json, "instance")))
    instance_str = "default";
  else
    instance_str = json_string_value (instance);
  return TMH_lookup_instance (instance_str);
}


/**
 * Iterate over each merchant instance, in order to populate
 * each instance's own data
 *
 * @param config configuration handle
 * @param allowed which wire format is allowed/expected?
 * @return #GNUNET_OK if successful, #GNUNET_SYSERR upon errors
 * (for example, if no "default" instance is defined)
 */
static int
iterate_instances (const struct GNUNET_CONFIGURATION_Handle *config,
                   const char *allowed)
{
  struct IterateInstancesCls *iic;
  char *lib_name;

  (void) GNUNET_asprintf (&lib_name,
                          "libtaler_plugin_wire_%s",
                          allowed);
  iic = GNUNET_new (struct IterateInstancesCls);
  iic->current_index = 0;
  iic->config = config;
  iic->default_instance = GNUNET_NO;
  iic->plugin = GNUNET_PLUGIN_load (lib_name,
                                    NULL);
  if (NULL == iic->plugin)
  {
    GNUNET_free (lib_name);
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Wire transfer method `%s' not supported\n",
                allowed);
    return GNUNET_SYSERR;
  }
  iic->plugin->library_name = lib_name;
  GNUNET_CONFIGURATION_iterate_sections (config,
                                         &instances_iterator_cb,
                                         (void *) iic);

  if (GNUNET_NO == iic->default_instance)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No default merchant instance found\n");
    goto fail;
  }
  else
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Congratulations, you have a default instance\n");

  if (0 != (GNUNET_SYSERR & iic->ret))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
               "At least one instance has not been successfully parsed\n");
    goto fail;
  }

  GNUNET_PLUGIN_unload (lib_name,
                        iic->plugin);
  GNUNET_free (lib_name);
  GNUNET_free (iic);
  return GNUNET_OK;

 fail:
  GNUNET_PLUGIN_unload (lib_name,
			iic->plugin);
  GNUNET_free (lib_name);
  GNUNET_free (iic);
  return GNUNET_SYSERR;
}

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be
 *        NULL!)
 * @param config configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{
  char *wireformat;
  int fh;

  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 NULL);
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-merchant-httpd",
                                   "WARNING",
                                   NULL));
  if (GNUNET_SYSERR ==
      TMH_EXCHANGES_init (config))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_SYSERR ==
      TMH_AUDITORS_init (config))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (NULL ==
     (by_id_map = GNUNET_CONTAINER_multihashmap_create(1, GNUNET_NO)))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (NULL ==
     (by_kpub_map = GNUNET_CONTAINER_multihashmap_create(1, GNUNET_NO)))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (GNUNET_SYSERR ==
      GNUNET_CONFIGURATION_get_value_time (config,
                                           "merchant",
                                           "WIRE_TRANSFER_DELAY",
                                           &wire_transfer_delay))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "WIRE_TRANSFER_DELAY");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (GNUNET_SYSERR ==
      GNUNET_CONFIGURATION_get_value_time (config,
                                           "merchant",
                                           "DEFAULT_PAY_DEADLINE",
                                           &default_pay_deadline))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "DEFAULT_PAY_DEADLINE");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (GNUNET_OK !=
      TALER_config_get_denom (config,
                              "merchant",
                              "DEFAULT_MAX_WIRE_FEE",
                              &default_max_wire_fee))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "DEFAULT_MAX_WIRE_FEE");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (GNUNET_OK !=
      TALER_config_get_denom (config,
                              "merchant",
                              "DEFAULT_MAX_DEPOSIT_FEE",
                              &default_max_deposit_fee))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "DEFAULT_MAX_DEPOSIT_FEE");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_number (config,
                                             "merchant",
                                             "DEFAULT_WIRE_FEE_AMORTIZATION",
                                             &default_wire_fee_amortization))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "DEFAULT_WIRE_FEE_AMORTIZATION");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  wireformat = NULL;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (config,
                                             "merchant",
                                             "WIREFORMAT",
                                             &wireformat))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchant",
                               "WIREFORMAT");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_OK !=
      iterate_instances (config,
			 wireformat))
  {
    GNUNET_free (wireformat);
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  GNUNET_free (wireformat);

  if (NULL ==
      (db = TALER_MERCHANTDB_plugin_load (config)))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_OK !=
      db->initialize (db->cls))
  {
    GNUNET_break (0);
    GNUNET_SCHEDULER_shutdown ();
    return;
  }


  {
    const char *choices[] = {"tcp", "unix"};
    const char *serve_type;

    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_get_value_choice (config,
                                               "merchant",
                                               "serve",
                                               choices,
                                               &serve_type))
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 "merchant",
                                 "serve",
                                 "serve type required");
      GNUNET_SCHEDULER_shutdown ();
      return;
    }

    if (0 == strcmp (serve_type, "unix"))
    {
      struct sockaddr_un *un;
      char *mode;
      struct GNUNET_NETWORK_Handle *nh;

      if (GNUNET_OK !=
          GNUNET_CONFIGURATION_get_value_filename (config,
                                                   "merchant",
                                                   "unixpath",
                                                   &serve_unixpath))
      {
        GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                   "merchant",
                                   "unixpath",
                                   "unixpath required");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }

      if (strlen (serve_unixpath) >= sizeof (un->sun_path))
      {
        fprintf (stderr,
                 "Invalid configuration: unix path too long\n");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }

      if (GNUNET_OK !=
          GNUNET_CONFIGURATION_get_value_string (config,
                                                 "merchant",
                                                 "unixpath_mode",
                                                 &mode))
      {
        GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                   "merchant",
                                   "unixpath_mode",
                                   "unixpath_mode required");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }
      errno = 0;
      unixpath_mode = (mode_t) strtoul (mode, NULL, 8);
      if (0 != errno)
      {
        GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                   "merchant",
                                   "unixpath_mode",
                                   "unixpath_mode must be octal number");
        GNUNET_free (mode);
        GNUNET_SCHEDULER_shutdown ();
        return;
      }
      GNUNET_free (mode);

      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Creating listen socket '%s' with mode %o\n",
                  serve_unixpath, unixpath_mode);

      if (GNUNET_OK != GNUNET_DISK_directory_create_for_file (serve_unixpath))
      {
        GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
                                  "mkdir",
                                  serve_unixpath);
      }

      un = GNUNET_new (struct sockaddr_un);
      un->sun_family = AF_UNIX;
      strncpy (un->sun_path,
               serve_unixpath,
               sizeof (un->sun_path) - 1);

      GNUNET_NETWORK_unix_precheck (un);

      if (NULL == (nh = GNUNET_NETWORK_socket_create (AF_UNIX,
                                                      SOCK_STREAM,
                                                      0)))
      {
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "socket(AF_UNIX)");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }
      if (GNUNET_OK !=
          GNUNET_NETWORK_socket_bind (nh,
                                      (void *) un,
                                      sizeof (struct sockaddr_un)))
      {
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "bind(AF_UNIX)");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }
      if (GNUNET_OK !=
          GNUNET_NETWORK_socket_listen (nh,
                                        UNIX_BACKLOG))
      {
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "listen(AF_UNIX)");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }

      fh = GNUNET_NETWORK_get_fd (nh);
      GNUNET_NETWORK_socket_free_memory_only_ (nh);
      if (0 != chmod (serve_unixpath,
                      unixpath_mode))
      {
        GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                             "chmod");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }
      port = 0;
    }
    else if (0 == strcmp (serve_type, "tcp"))
    {
      char *bind_to;

      if (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_number (config,
                                                 "merchant",
                                                 "PORT",
                                                 &port))
      {
        GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                                   "merchant",
                                   "PORT");
        GNUNET_SCHEDULER_shutdown ();
        return;
      }
      if (GNUNET_OK ==
          GNUNET_CONFIGURATION_get_value_string (config,
                                                 "merchant",
                                                 "BIND_TO",
                                                 &bind_to))
      {
        char port_str[6];
        struct addrinfo hints;
        struct addrinfo *res;
        int ec;
        struct GNUNET_NETWORK_Handle *nh;

        GNUNET_snprintf (port_str,
                         sizeof (port_str),
                         "%u",
                         (uint16_t) port);
        memset (&hints, 0, sizeof (hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE | AI_IDN;
        if (0 !=
            (ec = getaddrinfo (bind_to,
                               port_str,
                               &hints,
                               &res)))
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Failed to resolve BIND_TO address `%s': %s\n",
                      bind_to,
                      gai_strerror (ec));
          GNUNET_free (bind_to);
          GNUNET_SCHEDULER_shutdown ();
          return;
        }
        GNUNET_free (bind_to);

        if (NULL == (nh = GNUNET_NETWORK_socket_create (res->ai_family,
                                                        res->ai_socktype,
                                                        res->ai_protocol)))
        {
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                               "socket");
          freeaddrinfo (res);
          GNUNET_SCHEDULER_shutdown ();
          return;
        }
        if (GNUNET_OK !=
            GNUNET_NETWORK_socket_bind (nh,
                                        res->ai_addr,
                                        res->ai_addrlen))
        {
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                               "bind");
          freeaddrinfo (res);
          GNUNET_SCHEDULER_shutdown ();
          return;
        }
        freeaddrinfo (res);
        if (GNUNET_OK !=
            GNUNET_NETWORK_socket_listen (nh,
                                          UNIX_BACKLOG))
        {
          GNUNET_log_strerror (GNUNET_ERROR_TYPE_ERROR,
                               "listen");
          GNUNET_SCHEDULER_shutdown ();
          return;
        }
        fh = GNUNET_NETWORK_get_fd (nh);
        GNUNET_NETWORK_socket_free_memory_only_ (nh);
      }
      else
      {
        fh = -1;
      }
    }
    else
    {
      // not reached
      GNUNET_assert (0);
    }
  }
  mhd = MHD_start_daemon (MHD_USE_SUSPEND_RESUME | MHD_USE_DUAL_STACK,
                          port,
                          NULL, NULL,
                          &url_handler, NULL,
                          MHD_OPTION_LISTEN_SOCKET, fh,
                          MHD_OPTION_NOTIFY_COMPLETED, &handle_mhd_completion_callback, NULL,
                          MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int) 10 /* 10s */,
                          MHD_OPTION_END);
  if (NULL == mhd)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to launch HTTP service, exiting.\n");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  result = GNUNET_OK;
  mhd_task = prepare_daemon ();
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
  struct GNUNET_GETOPT_CommandLineOption options[] = {

    GNUNET_GETOPT_option_flag ('C',
                               "connection-close",
                               "force HTTP connections to be closed after each request",
                               &TMH_merchant_connection_close),

    GNUNET_GETOPT_OPTION_END
  };

  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "taler-merchant-httpd",
                          "Taler merchant's HTTP backend interface",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;
}
