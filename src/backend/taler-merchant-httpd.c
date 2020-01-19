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
#include <taler/taler_mhd_lib.h>
#include <taler/taler_bank_service.h>
#include <taler/taler_exchange_service.h>
#include "taler_merchantdb_lib.h"
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_order.h"
#include "taler-merchant-httpd_proposal.h"
#include "taler-merchant-httpd_pay.h"
#include "taler-merchant-httpd_track-transaction.h"
#include "taler-merchant-httpd_track-transfer.h"
#include "taler-merchant-httpd_tip-authorize.h"
#include "taler-merchant-httpd_tip-pickup.h"
#include "taler-merchant-httpd_tip-query.h"
#include "taler-merchant-httpd_tip-reserve-helper.h"
#include "taler-merchant-httpd_history.h"
#include "taler-merchant-httpd_refund.h"
#include "taler-merchant-httpd_check-payment.h"
#include "taler-merchant-httpd_poll-payment.h"
#include "taler-merchant-httpd_config.h"

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
static uint16_t port;

/**
 * This value tells the exchange by which date this merchant would like
 * to receive the funds for a deposited payment
 */
struct GNUNET_TIME_Relative default_wire_transfer_delay;

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
static int TMH_merchant_connection_close;

/**
 * Which currency do we use?
 */
char *TMH_currency;

/**
 * Inform the auditor for all deposit confirmations (global option)
 */
int TMH_force_audit;

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
 * MIN-Heap of suspended connections to resume when the timeout expires,
 * ordered by timeout. Values are of type `struct MHD_Connection`
 */
struct GNUNET_CONTAINER_Heap *resume_timeout_heap;

/**
 * Hash map from H(order_id,merchant_pub) to `struct MHD_Connection`
 * entries to resume when a payment is made for the given order.
 */
struct GNUNET_CONTAINER_MultiHashMap *payment_trigger_map;

/**
 * Task responsible for timeouts in the #resume_timeout_heap.
 */
struct GNUNET_SCHEDULER_Task *resume_timeout_task;


/**
 * Return #GNUNET_YES if given a valid correlation ID and
 * #GNUNET_NO otherwise.
 *
 * @returns #GNUNET_YES iff given a valid correlation ID
 */
static int
is_valid_correlation_id (const char *correlation_id)
{
  if (strlen (correlation_id) >= 64)
    return GNUNET_NO;
  for (size_t i = 0; i < strlen (correlation_id); i++)
    if (! (isalnum (correlation_id[i]) || (correlation_id[i] == '-')))
      return GNUNET_NO;
  return GNUNET_YES;
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

  (void) cls;
  (void) key;
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
 * Callback that frees all the elements in the #payment_trigger_map.
 * This function should actually never be called, as by the time we
 * get to it, all payment triggers should have been cleaned up!
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct TMH_SuspendedConnection`
 * @return #GNUNET_OK
 */
static int
payment_trigger_free (void *cls,
                      const struct GNUNET_HashCode *key,
                      void *value)
{
  struct TMH_SuspendedConnection *sc = value;

  (void) cls;
  (void) key;
  (void) sc; /* cannot really 'clean up' */
  GNUNET_break (0);
  return GNUNET_OK;
}


/**
 * Compute @a key to use for @a order_id and @a mpub in our
 * #payment_trigger_map.
 *
 * @param order_id an order ID
 * @param mpub an instance public key
 * @param key[out] set to the hash map key to use
 */
void
TMH_compute_pay_key (const char *order_id,
                     const struct TALER_MerchantPublicKeyP *mpub,
                     struct GNUNET_HashCode *key)
{
  size_t olen = strlen (order_id);
  char buf[sizeof (*mpub) + olen];

  memcpy (buf,
          mpub,
          sizeof (*mpub));
  memcpy (&buf[sizeof (*mpub)],
          order_id,
          olen);
  GNUNET_CRYPTO_hash (buf,
                      sizeof (buf),
                      key);
}


/**
 * Resume processing all suspended connections past timeout.
 *
 * @param cls unused
 */
static void
do_resume (void *cls)
{
  struct TMH_SuspendedConnection *sc;

  (void) cls;
  resume_timeout_task = NULL;
  while (1)
  {
    sc = GNUNET_CONTAINER_heap_peek (resume_timeout_heap);
    if (NULL == sc)
      return;
    if  (0 !=
         GNUNET_TIME_absolute_get_remaining (
           sc->long_poll_timeout).rel_value_us)
      break;
    GNUNET_assert (sc ==
                   GNUNET_CONTAINER_heap_remove_root (resume_timeout_heap));
    sc->hn = NULL;
    GNUNET_assert (GNUNET_YES ==
                   GNUNET_CONTAINER_multihashmap_remove (payment_trigger_map,
                                                         &sc->key,
                                                         sc));
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Resuming long polled job due to timeout\n");
    MHD_resume_connection (sc->con);
  }
  resume_timeout_task = GNUNET_SCHEDULER_add_at (sc->long_poll_timeout,
                                                 &do_resume,
                                                 NULL);
}


/**
 * Suspend connection from @a sc until payment has been received.
 *
 * @param sc connection to suspend
 */
void
TMH_long_poll_suspend (struct TMH_SuspendedConnection *sc)
{
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONTAINER_multihashmap_put (payment_trigger_map,
                                                    &sc->key,
                                                    sc,
                                                    GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE));
  sc->hn = GNUNET_CONTAINER_heap_insert (resume_timeout_heap,
                                         sc,
                                         sc->long_poll_timeout.abs_value_us);
  MHD_suspend_connection (sc->con);
  if (NULL != resume_timeout_task)
  {
    GNUNET_SCHEDULER_cancel (resume_timeout_task);
    resume_timeout_task = NULL;
  }
  sc = GNUNET_CONTAINER_heap_peek (resume_timeout_heap);
  resume_timeout_task = GNUNET_SCHEDULER_add_at (sc->long_poll_timeout,
                                                 &do_resume,
                                                 NULL);
}


/**
 * Create a taler://pay/ URI for the given @a con and @a order_id
 * and @a session_id and @a instance_id.
 *
 * @param con HTTP connection
 * @param order_id the order id
 * @param session_id session, may be NULL
 * @param instance_id instance, may be "default"
 * @return corresponding taler://pay/ URI, or NULL on missing "host"
 */
char *
TMH_make_taler_pay_uri (struct MHD_Connection *con,
                        const char *order_id,
                        const char *session_id,
                        const char *instance_id)
{
  const char *host;
  const char *forwarded_host;
  const char *uri_path;
  const char *uri_instance_id;
  const char *query;
  char *result;

  host = MHD_lookup_connection_value (con,
                                      MHD_HEADER_KIND,
                                      "Host");
  forwarded_host = MHD_lookup_connection_value (con,
                                                MHD_HEADER_KIND,
                                                "X-Forwarded-Host");

  uri_path = MHD_lookup_connection_value (con,
                                          MHD_HEADER_KIND,
                                          "X-Forwarded-Prefix");
  if (NULL == uri_path)
    uri_path = "-";
  if (NULL != forwarded_host)
    host = forwarded_host;
  if (0 == strcmp (instance_id,
                   "default"))
    uri_instance_id = "-";
  else
    uri_instance_id = instance_id;
  if (NULL == host)
  {
    /* Should never happen, at least the host header should be defined */
    GNUNET_break (0);
    return NULL;
  }

  if (GNUNET_YES == TALER_mhd_is_https (con))
    query = "";
  else
    query = "?insecure=1";
  GNUNET_assert (NULL != order_id);
  GNUNET_assert (0 < GNUNET_asprintf (&result,
                                      "taler://pay/%s/%s/%s/%s%s%s%s",
                                      host,
                                      uri_path,
                                      uri_instance_id,
                                      order_id,
                                      (NULL == session_id) ? "" : "/",
                                      (NULL == session_id) ? "" : session_id,
                                      query));
  return result;
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
  struct TMH_SuspendedConnection *sc;

  (void) cls;
  MH_force_pc_resume ();
  MH_force_trh_resume ();
  if (NULL != mhd_task)
  {
    GNUNET_SCHEDULER_cancel (mhd_task);
    mhd_task = NULL;
  }
  /* resume all suspended connections, must be done before stopping #mhd */
  if (NULL != resume_timeout_heap)
  {
    while (NULL != (sc = GNUNET_CONTAINER_heap_remove_root (
                      resume_timeout_heap)))
    {
      sc->hn = NULL;
      GNUNET_assert (GNUNET_YES ==
                     GNUNET_CONTAINER_multihashmap_remove (payment_trigger_map,
                                                           &sc->key,
                                                           sc));
      MHD_resume_connection (sc->con);
    }
    GNUNET_CONTAINER_heap_destroy (resume_timeout_heap);
    resume_timeout_heap = NULL;
  }
  if (NULL != resume_timeout_task)
  {
    GNUNET_SCHEDULER_cancel (resume_timeout_task);
    resume_timeout_task = NULL;
  }
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
  if (NULL != payment_trigger_map)
  {
    GNUNET_CONTAINER_multihashmap_iterate (payment_trigger_map,
                                           &payment_trigger_free,
                                           NULL);
    GNUNET_CONTAINER_multihashmap_destroy (payment_trigger_map);
    payment_trigger_map = NULL;
  }
  if (NULL != by_id_map)
  {
    GNUNET_CONTAINER_multihashmap_iterate (by_id_map,
                                           &hashmap_free,
                                           NULL);
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
  struct GNUNET_SCHEDULER_Task *ret;
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
  json_t *j;
  struct GNUNET_HashCode jh_wire;
  char *wire_file_mode;

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
                                             "PAYTO_URI",
                                             &payto))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "PAYTO_URI");
    iic->ret = GNUNET_SYSERR;
    return;
  }

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
    struct GNUNET_HashCode salt;
    char *salt_str;

    GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_NONCE,
                                &salt,
                                sizeof (salt));
    salt_str = GNUNET_STRINGS_data_to_string_alloc (&salt,
                                                    sizeof (salt));
    j = json_pack ("{s:s, s:s}",
                   "payto_uri", payto,
                   "salt", salt_str);
    GNUNET_free (salt_str);

    /* Make sure every path component exists.  */
    if (GNUNET_OK != GNUNET_DISK_directory_create_for_file (fn))
    {
      GNUNET_log_strerror_file (GNUNET_ERROR_TYPE_ERROR,
                                "mkdir",
                                fn);
      GNUNET_free (fn);
      GNUNET_free (payto);
      json_decref (j);
      iic->ret = GNUNET_SYSERR;
      return;
    }

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

    if (GNUNET_OK == GNUNET_CONFIGURATION_get_value_string
          (iic->config,
          section,
          "WIRE_FILE_MODE",
          &wire_file_mode))
    {
      errno = 0;
      mode_t mode = (mode_t) strtoul (wire_file_mode, NULL, 8);
      if (0 != errno)
      {
        GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                   section,
                                   "WIRE_FILE_MODE",
                                   "Must be octal number\n");
        iic->ret = GNUNET_SYSERR;
        GNUNET_free (fn);
        return;
      }
      if (0 != chmod (fn, mode))
      {
        TALER_LOG_ERROR ("chmod failed on %s\n", fn);
        iic->ret = GNUNET_SYSERR;
        GNUNET_free (fn);
        return;
      }
    }
  }

  GNUNET_free (fn);

  if (GNUNET_OK !=
      TALER_JSON_merchant_wire_signature_hash (j,
                                               &jh_wire))
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
  wm->wire_method = TALER_payto_get_method (payto);
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
  wm->h_wire = jh_wire;
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
    struct GNUNET_CRYPTO_EddsaPrivateKey *tip_pk;

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
    tip_pk = GNUNET_CRYPTO_eddsa_key_create_from_file (tip_reserves);
    if (NULL == tip_pk)
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
    mi->tip_reserve.eddsa_priv = *tip_pk;
    GNUNET_free (tip_pk);
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
 * Lookup a merchant instance by its instance ID.
 *
 * @param instance_id identifier of the instance to resolve
 * @return NULL if that instance is unknown to us
 */
static struct MerchantInstance *
lookup_instance (const char *instance_id)
{
  struct GNUNET_HashCode h_instance;

  if (NULL == instance_id)
    instance_id = "default";

  GNUNET_CRYPTO_hash (instance_id,
                      strlen (instance_id),
                      &h_instance);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Looking for by-id key %s of '%s' in hashmap\n",
              GNUNET_h2s (&h_instance),
              instance_id);
  /* We're fine if that returns NULL, the calling routine knows how
     to handle that */
  return GNUNET_CONTAINER_multihashmap_get (by_id_map,
                                            &h_instance);
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
  static struct TMH_RequestHandler handlers[] = {
    /* Landing page, tell humans to go away. */
    { "/", MHD_HTTP_METHOD_GET, "text/plain",
      "Hello, I'm a merchant's Taler backend. This HTTP server is not for humans.\n",
      0,
      &TMH_MHD_handler_static_response, MHD_HTTP_OK },
    { "/agpl", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &TMH_MHD_handler_agpl_redirect, MHD_HTTP_FOUND },
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
      &MH_handler_order_post, MHD_HTTP_OK },
    { "/refund", MHD_HTTP_METHOD_POST, "application/json",
      NULL, 0,
      &MH_handler_refund_increase, MHD_HTTP_OK},
    { "/tip-authorize", MHD_HTTP_METHOD_POST, "text/plain",
      NULL, 0,
      &MH_handler_tip_authorize, MHD_HTTP_OK},
    { "/tip-query", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_tip_query, MHD_HTTP_OK},
    { "/check-payment", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_check_payment, MHD_HTTP_OK},
    { "/config", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_config, MHD_HTTP_OK},
    {NULL, NULL, NULL, NULL, 0, 0 }
  };
  static struct TMH_RequestHandler public_handlers[] = {
    { "/pay", MHD_HTTP_METHOD_POST, "application/json",
      NULL, 0,
      &MH_handler_pay, MHD_HTTP_OK },
    { "/proposal", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_proposal_lookup, MHD_HTTP_OK },
    { "/tip-pickup", MHD_HTTP_METHOD_POST, "text/plain",
      NULL, 0,
      &MH_handler_tip_pickup, MHD_HTTP_OK },
    { "/refund", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_refund_lookup, MHD_HTTP_OK },
    { "/tip-pickup", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_tip_pickup_get, MHD_HTTP_OK },
    { "/poll-payment", MHD_HTTP_METHOD_GET, "text/plain",
      NULL, 0,
      &MH_handler_poll_payment, MHD_HTTP_OK},
    {NULL, NULL, NULL, NULL, 0, 0 }
  };
  static struct TMH_RequestHandler h404 = {
    "", NULL, "text/html",
    "<html><title>404: not found</title><body>404: not found</body></html>", 0,
    &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
  };

  struct TM_HandlerContext *hc = *con_cls;
  struct GNUNET_AsyncScopeId aid;
  const char *correlation_id = NULL;
  struct MerchantInstance *instance;
  const char *effective_url;
  /* Is a publicly facing endpoint being requested? */
  int is_public;
  /* Matching URL found, but maybe method doesn't match */
  int url_found = GNUNET_NO;
  int ret;
  struct TMH_RequestHandler *selected_handler = NULL;

  (void) cls;
  (void) version;
  if (NULL == hc)
  {
    GNUNET_async_scope_fresh (&aid);
    /* We only read the correlation ID on the first callback for every client */
    correlation_id = MHD_lookup_connection_value (connection,
                                                  MHD_HEADER_KIND,
                                                  "Taler-Correlation-Id");
    if ((NULL != correlation_id) &&
        (GNUNET_YES != is_valid_correlation_id (correlation_id)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "illegal incoming correlation ID\n");
      correlation_id = NULL;
    }
  }
  else
  {
    aid = hc->async_scope_id;
  }

  GNUNET_SCHEDULER_begin_async_scope (&aid);

  if (NULL != correlation_id)
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Handling request for (%s) URL '%s', correlation_id=%s\n",
                method,
                url,
                correlation_id);
  else
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Handling request (%s) for URL '%s'\n",
                method,
                url);

  effective_url = url;

  {
    const char *public_prefix = "/public/";

    if (0 == strncmp (effective_url,
                      public_prefix,
                      strlen (public_prefix)))
    {
      is_public = GNUNET_YES;
      effective_url = effective_url + strlen (public_prefix) - 1;
    }
    else
    {
      is_public = GNUNET_NO;
    }
  }

  /* Find out the merchant backend instance for the request.
   * If there is an instance, remove the instance specification
   * from the beginning of the request URL. */
  {
    const char *instance_prefix = "/instances/";

    if (0 == strncmp (effective_url,
                      instance_prefix,
                      strlen (instance_prefix)))
    {
      /* url starts with "/instances/" */
      const char *istart = effective_url + strlen (instance_prefix);
      const char *slash = strchr (istart, '/');
      char *instance_id;

      if (NULL == slash)
      {
        return TMH_MHD_handler_static_response (&h404,
                                                connection,
                                                con_cls,
                                                upload_data,
                                                upload_data_size,
                                                NULL);
      }
      instance_id = GNUNET_strndup (istart,
                                    slash - istart);
      instance = lookup_instance (instance_id);
      GNUNET_free (instance_id);
      effective_url = slash;
    }
    else
    {
      instance = lookup_instance (NULL);
    }
  }
  if (NULL == instance)
    return TALER_MHD_reply_json_pack (connection,
                                      MHD_HTTP_NOT_FOUND,
                                      "{s:I, s:s}",
                                      "code",
                                      (json_int_t) TALER_EC_INSTANCE_UNKNOWN,
                                      "error",
                                      "merchant instance unknown");

  if (GNUNET_NO == is_public)
  {
    for (unsigned int i = 0; NULL != handlers[i].url; i++)
    {
      struct TMH_RequestHandler *rh = &handlers[i];

      if ( (0 != strcasecmp (effective_url, rh->url)) )
        continue;
      url_found = GNUNET_YES;
      if (0 == strcasecmp (method,
                           MHD_HTTP_METHOD_OPTIONS))
      {
        return TALER_MHD_reply_cors_preflight (connection);
      }
      if ( (rh->method != NULL) &&
           (0 != strcasecmp (method, rh->method)) )
        continue;
      selected_handler = rh;
      break;
    }
  }

  if (NULL == selected_handler)
  {
    for (unsigned int i = 0; NULL != public_handlers[i].url; i++)
    {
      struct TMH_RequestHandler *rh = &public_handlers[i];

      if ( (0 != strcasecmp (effective_url, rh->url)) )
        continue;
      url_found = GNUNET_YES;
      if (0 == strcasecmp (method,
                           MHD_HTTP_METHOD_OPTIONS))
      {
        return TALER_MHD_reply_cors_preflight (connection);
      }
      if ( (rh->method != NULL) && (0 != strcasecmp (method, rh->method)) )
        continue;
      selected_handler = rh;
      break;
    }
  }

  if (NULL == selected_handler)
  {
    if (GNUNET_YES == url_found)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "invalid request: method '%s' for '%s' not allowed\n",
                  method,
                  url);
      return TALER_MHD_reply_json_pack (connection,
                                        MHD_HTTP_METHOD_NOT_ALLOWED,
                                        "{s:s}",
                                        "error",
                                        "method not allowed");
    }
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "invalid request: URL '%s' not handled\n",
                url);
    return TMH_MHD_handler_static_response (&h404,
                                            connection,
                                            con_cls,
                                            upload_data,
                                            upload_data_size,
                                            instance);
  }

  ret = selected_handler->handler (selected_handler,
                                   connection,
                                   con_cls,
                                   upload_data,
                                   upload_data_size,
                                   instance);
  hc = *con_cls;
  if (NULL != hc)
  {
    hc->rh = selected_handler;
    /* Store the async context ID, so we can restore it if
     * we get another callack for this request. */
    hc->async_scope_id = aid;
  }
  return ret;
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
  enum TALER_MHD_GlobalOptions go;

  (void) cls;
  (void) args;
  (void) cfgfile;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Starting taler-merchant-httpd\n");
  go = TALER_MHD_GO_NONE;
  if (TMH_merchant_connection_close)
    go |= TALER_MHD_GO_FORCE_CONNECTION_CLOSE;
  TALER_MHD_setup (go);

  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 NULL);
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
  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno (config,
                                            "merchant",
                                            "FORCE_AUDIT"))
    TMH_force_audit = GNUNET_YES;
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
                                           &default_wire_transfer_delay))
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

  fh = TALER_MHD_bind (config,
                       "merchant",
                       &port);
  if ( (0 == port) &&
       (-1 == fh) )
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  resume_timeout_heap
    = GNUNET_CONTAINER_heap_create (GNUNET_CONTAINER_HEAP_ORDER_MIN);
  payment_trigger_map
    = GNUNET_CONTAINER_multihashmap_create (16,
                                            GNUNET_YES);
  mhd = MHD_start_daemon (MHD_USE_SUSPEND_RESUME | MHD_USE_DUAL_STACK,
                          port,
                          NULL, NULL,
                          &url_handler, NULL,
                          MHD_OPTION_LISTEN_SOCKET, fh,
                          MHD_OPTION_NOTIFY_COMPLETED,
                          &handle_mhd_completion_callback, NULL,
                          MHD_OPTION_CONNECTION_TIMEOUT, (unsigned
                                                          int) 10 /* 10s */,
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
