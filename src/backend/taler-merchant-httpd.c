/*
  This file is part of TALER
  (C) 2014-2018 Taler Systems SA

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
#include <taler/taler_wire_lib.h>
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
#include "taler-merchant-httpd_tip-pickup.h"
#include "taler-merchant-httpd_tip-query.h"
#include "taler-merchant-httpd_history.h"
#include "taler-merchant-httpd_refund.h"
#include "taler-merchant-httpd_check-payment.h"
#include "taler-merchant-httpd_trigger-pay.h"

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
 * Locations from the configuration.  Mapping from
 * label to location data.
 */
json_t *default_locations;

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
 * Which currency do we use?
 */
char *TMH_currency;

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
      { "/public/pay", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_pay, MHD_HTTP_OK },
      { "/public/pay", NULL, "text/plain",
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
      { "/order", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_proposal_put, MHD_HTTP_OK },
      { "/public/proposal", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_proposal_lookup, MHD_HTTP_OK},
      { "/proposal", NULL, "text/plain",
        "Only GET/POST are allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },
      { "/refund", MHD_HTTP_METHOD_POST, "application/json",
        NULL, 0,
        &MH_handler_refund_increase, MHD_HTTP_OK},
      { "/public/refund", MHD_HTTP_METHOD_GET, "text/plain",
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
      /* backwards compatibility alias for /public/tip-pickup */
      { "/tip-pickup", MHD_HTTP_METHOD_POST, "text/plain",
        NULL, 0,
        &MH_handler_tip_pickup, MHD_HTTP_OK},
      { "/public/tip-pickup", MHD_HTTP_METHOD_POST, "text/plain",
        NULL, 0,
        &MH_handler_tip_pickup, MHD_HTTP_OK},
      { "/tip-pickup", NULL, "application/json",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED},
      { "/tip-query", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_tip_query, MHD_HTTP_OK},
      { "/check-payment", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_check_payment, MHD_HTTP_OK},
      { "/public/trigger-pay", MHD_HTTP_METHOD_GET, "text/plain",
        NULL, 0,
        &MH_handler_trigger_pay, MHD_HTTP_OK},
      {NULL, NULL, NULL, NULL, 0, 0 }
    };
  static struct TMH_RequestHandler h404 =
    {
      "", NULL, "text/html",
      "<html><title>404: not found</title></html>", 0,
      &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
    };
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Handling request (%s) for URL `%s'\n",
              method,
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
  struct WireMethod *wm;

  while (NULL != (wm = (mi->wm_head)))
  {
    GNUNET_CONTAINER_DLL_remove (mi->wm_head,
                                 mi->wm_tail,
                                 wm);
    json_decref (wm->j_wire);
    GNUNET_free (wm->wire_method);
    GNUNET_free (wm);
  }

  GNUNET_free (mi->id);
  GNUNET_free (mi->keyfile);
  GNUNET_free (mi->name);
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
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
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
 * Set if we should immediately #MHD_run again.
 */
static int triggered;


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
  do {
    triggered = 0;
    GNUNET_assert (MHD_YES == MHD_run (mhd));
  } while (0 != triggered);
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
  if (NULL != mhd_task)
  {
    GNUNET_SCHEDULER_cancel (mhd_task);
    mhd_task = NULL;
    run_daemon (NULL);
  }
  else
  {
    triggered = 1;
  }
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
 * Callback that looks for 'merchant-location-*' sections,
 * and populates @a default_locations.
 *
 * @param cls closure
 * @section section name this callback gets
 */
static void
locations_iterator_cb (void *cls,
                       const char *section)
{
  static const char *keys[] = {
    "country",
    "city",
    "state",
    "region",
    "province",
    "zip_code",
    "street",
    "street_number",
    NULL,
  };
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  const char *prefix = "merchant-location-";
  const char *substr = strstr (section, prefix);
  const char *locname;
  json_t *loc;

  if ( (NULL == substr) || (substr != section) )
    return;
  locname = section + strlen (prefix);
  if (0 == strlen (locname))
    return;
  GNUNET_assert (json_is_object (default_locations));

  loc = json_object ();
  json_object_set_new (default_locations,
                       locname,
                       loc);
  for (unsigned int pos = 0; NULL != keys[pos]; pos++)
  {
    char *val;

    if (GNUNET_OK ==
        GNUNET_CONFIGURATION_get_value_string (cfg,
                                               section,
                                               keys[pos],
                                               &val))
    {
      json_object_set_new (loc,
                           keys[pos],
                           json_string (val));
      GNUNET_free (val);
    }
  }
}


/**
 * Closure for the #wireformat_iterator_cb().
 */
struct WireFormatIteratorContext
{
  /**
   * The global iteration context.
   */
  struct IterateInstancesCls *iic;

  /**
   * The merchant instance we are currently building.
   */
  struct MerchantInstance *mi;

  /**
   * Set to #GNUNET_YES if the default instance was found.
   */
  int default_instance;
};


/**
 * Callback that looks for 'account-*' sections,
 * and populates our wire method according to the data
 *
 * @param cls closure with a `struct WireFormatIteratorContext *`
 * @section section name this callback gets
 */
static void
wireformat_iterator_cb (void *cls,
                        const char *section)
{
  struct WireFormatIteratorContext *wfic = cls;
  struct MerchantInstance *mi = wfic->mi;
  struct IterateInstancesCls *iic = wfic->iic;
  char *instance_option;
  struct WireMethod *wm;
  char *payto;
  char *fn;
  char *plugin_name;
  struct TALER_WIRE_Plugin *plugin;
  json_t *j;
  enum TALER_ErrorCode ec;
  struct GNUNET_HashCode h_wire;

  if (0 != strncasecmp (section,
                        "account-",
                        strlen ("account-")))
    return;
  GNUNET_asprintf (&instance_option,
                   "HONOR_%s",
                   mi->id);
  if (GNUNET_YES !=
      GNUNET_CONFIGURATION_get_value_yesno (iic->config,
                                            section,
                                            instance_option))
  {
    GNUNET_free (instance_option);
    return;
  }
  GNUNET_free (instance_option);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (iic->config,
                                             section,
                                             "URL",
                                             &payto))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "URL");
    iic->ret = GNUNET_SYSERR;
    return;
  }

  /* check payto://-URL is well-formed and matches plugin */
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (iic->config,
                                             section,
                                             "PLUGIN",
                                             &plugin_name))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "PLUGIN");
    GNUNET_free (payto);
    GNUNET_free (instance_option);
    iic->ret = GNUNET_SYSERR;
    return;
  }
  if (NULL ==
      (plugin = TALER_WIRE_plugin_load (iic->config,
                                        plugin_name)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to load wire plugin `%s'\n",
                plugin_name);
    GNUNET_free (plugin_name);
    GNUNET_free (instance_option);
    GNUNET_free (payto);
    iic->ret = GNUNET_SYSERR;
    return;
  }
  if (TALER_EC_NONE !=
      (ec = plugin->wire_validate (plugin->cls,
                                   payto)))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "payto:// URL `%s' not supported by plugin `%s'\n",
                payto,
                plugin_name);
    GNUNET_free (plugin_name);
    GNUNET_free (instance_option);
    GNUNET_free (payto);
    TALER_WIRE_plugin_unload (plugin);
    iic->ret = GNUNET_SYSERR;
    return;
  }
  TALER_WIRE_plugin_unload (plugin);
  GNUNET_free (plugin_name);

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (iic->config,
                                               section,
                                               "WIRE_RESPONSE",
                                               &fn))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "WIRE_RESPONSE");
    GNUNET_free (payto);
    iic->ret = GNUNET_SYSERR;
    return;
  }

  /* Try loading existing JSON from file */
  if (GNUNET_YES ==
      GNUNET_DISK_file_test (fn))
  {
    json_error_t err;
    char *url;

    if (NULL ==
        (j = json_load_file (fn,
                             JSON_REJECT_DUPLICATES,
                             &err)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to load JSON from `%s': %s at %d:%d\n",
                  fn,
                  err.text,
                  err.line,
                  err.column);
      GNUNET_free (fn);
      GNUNET_free (payto);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    url = TALER_JSON_wire_to_payto (j);
    if (NULL == url)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "URL missing in `%s', disabling account `%s'\n",
                  fn,
                  section);
      GNUNET_free (fn);
      GNUNET_free (payto);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    if (0 != strcasecmp (url,
                         payto))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "URL `%s' does not match configuration `%s', disabling account `%s'\n",
                  url,
                  payto,
                  section);
      GNUNET_free (fn);
      GNUNET_free (payto);
      GNUNET_free (url);
      iic->ret = GNUNET_SYSERR;
      return;
    }
    GNUNET_free (url);
  }
  else /* need to generate JSON */
  {
    j = TALER_JSON_wire_signature_make (payto,
                                        NULL);
    if (0 != json_dump_file (j,
                             fn,
                             JSON_COMPACT | JSON_SORT_KEYS))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to write hashed wire details to `%s'\n",
                  fn);
      GNUNET_free (fn);
      GNUNET_free (payto);
      json_decref (j);
      iic->ret = GNUNET_SYSERR;
      return;
    }
  }
  GNUNET_free (fn);

  if (GNUNET_OK !=
      TALER_JSON_wire_signature_hash (j,
                                      &h_wire))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to hash wire input\n");
    GNUNET_free (fn);
    GNUNET_free (payto);
    json_decref (j);
    iic->ret = GNUNET_SYSERR;
    return;
  }


  wm = GNUNET_new (struct WireMethod);
  wm->wire_method = TALER_WIRE_payto_get_method (payto);
  GNUNET_free (payto);
  GNUNET_asprintf (&instance_option,
                   "ACTIVE_%s",
                   mi->id);
  wm->active = GNUNET_CONFIGURATION_get_value_yesno (iic->config,
                                                     section,
                                                     instance_option);
  GNUNET_free (instance_option);
  if (GNUNET_YES == wm->active)
    GNUNET_CONTAINER_DLL_insert (mi->wm_head,
                                 mi->wm_tail,
                                 wm);
  else
    GNUNET_CONTAINER_DLL_insert_tail (mi->wm_head,
                                      mi->wm_tail,
                                      wm);

  wm->j_wire = j;
  wm->h_wire = h_wire;
}


/**
 * Callback that looks for 'instance-*' sections,
 * and populates accordingly each instance's data
 *
 * @param cls closure of type `struct IterateInstancesCls`
 * @section section name this callback gets
 */
static void
instances_iterator_cb (void *cls,
                       const char *section)
{
  struct IterateInstancesCls *iic = cls;
  char *token;
  struct MerchantInstance *mi;
  struct GNUNET_CRYPTO_EddsaPrivateKey *pk;
  /* used as hashmap keys */
  struct GNUNET_HashCode h_pk;
  struct GNUNET_HashCode h_id;

  if (0 != strncasecmp (section,
                        "instance-",
                        strlen ("instance-")))
    return;
  /** Get id **/
  token = strrchr (section, '-');
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Extracted token: %s\n",
              token + 1);
  mi = GNUNET_new (struct MerchantInstance);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (iic->config,
                                             section,
                                             "NAME",
                                             &mi->name))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "NAME");
    GNUNET_free (mi);
    iic->ret = GNUNET_SYSERR;
    return;
  }

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (iic->config,
                                               section,
                                               "KEYFILE",
                                               &mi->keyfile))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "KEYFILE");
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    iic->ret = GNUNET_SYSERR;
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
        GNUNET_CONFIGURATION_get_value_filename (iic->config,
                                                 section,
                                                 "TIP_RESERVE_PRIV_FILENAME",
                                                 &tip_reserves))
    {
      GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "TIP_RESERVE_PRIV_FILENAME");
      GNUNET_free (mi->keyfile);
      GNUNET_free (mi->name);
      GNUNET_free (mi);
      iic->ret = GNUNET_SYSERR;
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
      GNUNET_free (mi->keyfile);
      GNUNET_free (mi->name);
      GNUNET_free (mi);
      iic->ret = GNUNET_SYSERR;
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
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    iic->ret = GNUNET_SYSERR;
    return;
  }
  mi->privkey.eddsa_priv = *pk;
  GNUNET_CRYPTO_eddsa_key_get_public (pk,
                                      &mi->pubkey.eddsa_pub);
  GNUNET_free (pk);

  mi->id = GNUNET_strdup (token + 1);
  if (0 == strcasecmp ("default",
                       mi->id))
    iic->default_instance = GNUNET_YES;

  GNUNET_CRYPTO_hash (mi->id,
                      strlen (mi->id),
                      &h_id);
  if (GNUNET_OK !=
      GNUNET_CONTAINER_multihashmap_put (by_id_map,
                                         &h_id,
                                         mi,
                                         GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to put an entry into the 'by_id' hashmap\n");
    iic->ret = GNUNET_SYSERR;
    GNUNET_free (mi->keyfile);
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    return;
  }
  GNUNET_CRYPTO_hash (&mi->pubkey.eddsa_pub,
                      sizeof (struct GNUNET_CRYPTO_EddsaPublicKey),
                      &h_pk);
  if (GNUNET_OK !=
      GNUNET_CONTAINER_multihashmap_put (by_kpub_map,
                                         &h_pk,
                                         mi,
                                         GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to put an entry into the 'by_kpub_map' hashmap\n");
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CONTAINER_multihashmap_remove (by_id_map,
                                                         &h_id,
                                                         mi));
    iic->ret = GNUNET_SYSERR;
    GNUNET_free (mi->keyfile);
    GNUNET_free (mi->name);
    GNUNET_free (mi);
    return;
  }

  /* Initialize wireformats */
  {
    struct WireFormatIteratorContext wfic = {
      .iic = iic,
      .mi = mi
    };

    GNUNET_CONFIGURATION_iterate_sections (iic->config,
                                           &wireformat_iterator_cb,
                                           &wfic);
  }
  if (NULL == mi->wm_head)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to load wire formats for instance `%s'\n",
                mi->id);
    iic->ret = GNUNET_SYSERR;
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
 * Iterate over locations in config in order to populate
 * the location data.
 *
 * @param config configuration handle
 * @return #GNUNET_OK if successful, #GNUNET_SYSERR upon errors
 */
static void
iterate_locations (const struct GNUNET_CONFIGURATION_Handle *config)
{
  GNUNET_assert (NULL == default_locations);
  default_locations = json_object ();
  GNUNET_CONFIGURATION_iterate_sections (config,
                                         &locations_iterator_cb,
                                         (void *) config);
}


/**
 * Iterate over each merchant instance, in order to populate
 * each instance's own data
 *
 * @param config configuration handle
 * @return #GNUNET_OK if successful, #GNUNET_SYSERR upon errors
 *          (for example, if no "default" instance is defined)
 */
static int
iterate_instances (const struct GNUNET_CONFIGURATION_Handle *config)
{
  struct IterateInstancesCls iic;

  iic.config = config;
  iic.default_instance = GNUNET_NO;
  iic.ret = GNUNET_OK;
  GNUNET_CONFIGURATION_iterate_sections (config,
                                         &instances_iterator_cb,
                                         &iic);

  if (GNUNET_NO == iic.default_instance)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "No default merchant instance found\n");
    return GNUNET_SYSERR;
  }
  if (GNUNET_OK != iic.ret)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "At least one instance was not successfully parsed\n");
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
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
  int fh;

  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Starting taler-merchant-httpd\n");

  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 NULL);
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_log_setup ("taler-merchant-httpd",
                                   "WARNING",
                                   NULL));
  if (GNUNET_SYSERR ==
      GNUNET_CONFIGURATION_get_value_string (config,
                                             "taler",
                                             "CURRENCY",
                                             &TMH_currency))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "taler",
                               "CURRENCY");
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

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
     (by_id_map = GNUNET_CONTAINER_multihashmap_create (1,
                                                        GNUNET_NO)))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }

  if (NULL ==
     (by_kpub_map = GNUNET_CONTAINER_multihashmap_create (1,
                                                          GNUNET_NO)))
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

  if (GNUNET_OK !=
      iterate_instances (config))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  iterate_locations (config);

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
main (int argc,
      char *const *argv)
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
