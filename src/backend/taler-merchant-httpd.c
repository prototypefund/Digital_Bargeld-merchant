/*
  This file is part of TALER
  (C) 2014-2020 Taler Systems SA

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
#include <taler/taler_bank_service.h>
#include <taler/taler_exchange_service.h>
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_config.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_private-get-instances.h"
#include "taler-merchant-httpd_private-get-instances-ID.h"
#include "taler-merchant-httpd_private-post-instances.h"

/**
 * Backlog for listen operation on unix-domain sockets.
 */
#define UNIX_BACKLOG 500


/**
 * Which currency do we use?
 */
char *TMH_currency;

/**
 * Inform the auditor for all deposit confirmations (global option)
 */
int TMH_force_audit;

/**
 * Connection handle to the our database
 */
struct TALER_MERCHANTDB_Plugin *TMH_db;

/**
 * Hashmap pointing at merchant instances by 'id'. An 'id' is
 * just a string that identifies a merchant instance. When a frontend
 * needs to specify an instance to the backend, it does so by 'id'
 */
struct GNUNET_CONTAINER_MultiHashMap *TMH_by_id_map;

/**
 * The port we are running on
 */
static uint16_t port;

/**
 * Should a "Connection: close" header be added to each HTTP response?
 */
static int merchant_connection_close;

/**
 * Task running the HTTP server.
 */
static struct GNUNET_SCHEDULER_Task *mhd_task;

/**
 * Global return code
 */
static int result;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
 * MIN-Heap of suspended connections to resume when the timeout expires,
 * ordered by timeout. Values are of type `struct MHD_Connection`
 */
static struct GNUNET_CONTAINER_Heap *resume_timeout_heap;

/**
 * Hash map from H(order_id,merchant_pub) to `struct MHD_Connection`
 * entries to resume when a payment is made for the given order.
 */
static struct GNUNET_CONTAINER_MultiHashMap *payment_trigger_map;

/**
 * Task responsible for timeouts in the #resume_timeout_heap.
 */
static struct GNUNET_SCHEDULER_Task *resume_timeout_task;

/**
 * Our configuration.
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;


/**
 * Decrement reference counter of @a mi, and free if it hits zero.
 *
 * @param[in,out] mi merchant instance to update and possibly free
 */
void
TMH_instance_decref (struct TMH_MerchantInstance *mi)
{
  struct TMH_WireMethod *wm;

  mi->rc--;
  if (0 != mi->rc)
    return;
  while (NULL != (wm = (mi->wm_head)))
  {
    GNUNET_CONTAINER_DLL_remove (mi->wm_head,
                                 mi->wm_tail,
                                 wm);
    json_decref (wm->j_wire);
    GNUNET_free (wm->wire_method);
    GNUNET_free (wm);
  }

  GNUNET_free (mi->settings.id);
  GNUNET_free (mi->settings.name);
  json_decref (mi->settings.address);
  json_decref (mi->settings.jurisdiction);
  GNUNET_free (mi);
}


/**
 * Callback that frees all the instances in the hashmap
 *
 * @param cls closure, NULL
 * @param key current key
 * @param value a `struct TMH_MerchantInstance`
 */
static int
instance_free_cb (void *cls,
                  const struct GNUNET_HashCode *key,
                  void *value)
{
  struct TMH_MerchantInstance *mi = value;

  (void) cls;
  (void) key;
  TMH_instance_decref (mi);
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
static void
compute_pay_key (const char *order_id,
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
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Pay key for %s is %s\n",
              order_id,
              GNUNET_h2s (key));
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
 * @param order_id the order that we are waiting on
 * @param mi the merchant instance we are waiting on
 * @param sc connection to suspend
 * @param min_refund refund amount we are waiting on to be exceeded before resuming,
 *                   NULL if we are not waiting for refunds
 */
void
TMH_long_poll_suspend (const char *order_id,
                       const struct TMH_MerchantInstance *mi,
                       struct TMH_SuspendedConnection *sc,
                       const struct TALER_Amount *min_refund)
{
  compute_pay_key (order_id,
                   &mi->merchant_pub,
                   &sc->key);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending operation on key %s\n",
              GNUNET_h2s (&sc->key));
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONTAINER_multihashmap_put (payment_trigger_map,
                                                    &sc->key,
                                                    sc,
                                                    GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE));
  if (NULL != min_refund)
  {
    sc->awaiting_refund = true;
    sc->refund_expected = *min_refund;
  }
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
 * Function called to resume suspended connections.
 *
 * @param cls pointer to a `struct TALER_Amount` indicating the refund amount, or NULL
 * @param key key in the #payment_trigger_map
 * @param value a `struct TMH_SuspendedConnection` to resume
 * @return #GNUNET_OK (continue to iterate)
 */
static int
resume_operation (void *cls,
                  const struct GNUNET_HashCode *key,
                  void *value)
{
  const struct TALER_Amount *have_refund = cls;
  struct TMH_SuspendedConnection *sc = value;

  if ( (sc->awaiting_refund) &&
       ( (NULL == have_refund) ||
         (1 != TALER_amount_cmp (have_refund,
                                 &sc->refund_expected)) ) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Not awaking client, refund amount of %s not yet satisfied\n",
                TALER_amount2s (&sc->refund_expected));
    return GNUNET_OK; /* skip */
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming operation suspended pending payment on key %s\n",
              GNUNET_h2s (key));
  GNUNET_assert (GNUNET_YES ==
                 GNUNET_CONTAINER_multihashmap_remove (payment_trigger_map,
                                                       key,
                                                       sc));
  GNUNET_assert (sc ==
                 GNUNET_CONTAINER_heap_remove_node (sc->hn));
  sc->hn = NULL;
  MHD_resume_connection (sc->con);
  TMH_trigger_daemon ();
  return GNUNET_OK;
}


/**
 * Find out if we have any clients long-polling for @a order_id to be
 * confirmed at merchant @a mpub, and if so, tell them to resume.
 *
 * @param order_id the order that was paid or refunded
 * @param mi the merchant instance where the payment or refund happened
 * @param have_refund refunded amount, NULL if there was no refund
 */
void
TMH_long_poll_resume (const char *order_id,
                      const struct TMH_MerchantInstance *mi,
                      const struct TALER_Amount *have_refund)
{
  struct GNUNET_HashCode key;

  compute_pay_key (order_id,
                   &mi->merchant_pub,
                   &key);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming operations suspended pending payment on key %s\n",
              GNUNET_h2s (&key));
  GNUNET_CONTAINER_multihashmap_get_multiple (payment_trigger_map,
                                              &key,
                                              &resume_operation,
                                              (void *) have_refund);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "%u operations remain suspended pending payment\n",
              GNUNET_CONTAINER_multihashmap_size (payment_trigger_map));
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
#if 0
  TMH_force_pc_resume ();
  TMH_force_trh_resume ();
  TMH_force_refund_resume ();
  TMH_force_tip_pickup_resume ();
#endif
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
  if (NULL != TMH_db)
  {
    TALER_MERCHANTDB_plugin_unload (TMH_db);
    TMH_db = NULL;
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
  if (NULL != TMH_by_id_map)
  {
    GNUNET_CONTAINER_multihashmap_iterate (TMH_by_id_map,
                                           &instance_free_cb,
                                           NULL);
    GNUNET_CONTAINER_multihashmap_destroy (TMH_by_id_map);
    TMH_by_id_map = NULL;
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
  struct TMH_HandlerContext *hc = *con_cls;

  if (NULL == hc)
    return;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Finished handling request for `%s' with status %d\n",
              hc->url,
              (int) toe);
  if (NULL != hc->cc)
    hc->cc (hc->ctx);
  TALER_MHD_parse_post_cleanup_callback (hc->json_parse_context);
  GNUNET_free_non_null (hc->infix);
  if (NULL != hc->request_body)
    json_decref (hc->request_body);
  GNUNET_free (hc);
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
 * @param cls NULL
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
prepare_daemon (void)
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
 * Lookup a merchant instance by its instance ID.
 *
 * @param instance_id identifier of the instance to resolve
 * @return NULL if that instance is unknown to us
 */
struct TMH_MerchantInstance *
TMH_lookup_instance (const char *instance_id)
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
  return GNUNET_CONTAINER_multihashmap_get (TMH_by_id_map,
                                            &h_instance);
}


/**
 * Add instance definition to our active set of instances.
 *
 * @param[in,out] mi merchant instance details to define
 * @return #GNUNET_OK on success, #GNUNET_NO if the same ID is in use already
 */
int
TMH_add_instance (struct TMH_MerchantInstance *mi)
{
  struct GNUNET_HashCode h_instance;
  const char *id;
  int ret;

  id = mi->settings.id;
  if (NULL == id)
    id = "default";
  GNUNET_CRYPTO_hash (id,
                      strlen (id),
                      &h_instance);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Looking for by-id key %s of `%s' in hashmap\n",
              GNUNET_h2s (&h_instance),
              id);
  ret = GNUNET_CONTAINER_multihashmap_put (TMH_by_id_map,
                                           &h_instance,
                                           mi,
                                           GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
  if (GNUNET_OK == ret)
    mi->rc++;
  return ret;
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
 *         #MHD_NO if the socket must be closed due to a serious
 *         error while handling the request
 */
static MHD_RESULT
url_handler (void *cls,
             struct MHD_Connection *connection,
             const char *url,
             const char *method,
             const char *version,
             const char *upload_data,
             size_t *upload_data_size,
             void **con_cls)
{
  static struct TMH_RequestHandler private_handlers[] = {
    {
      .url_prefix = "/",
      .method = MHD_HTTP_METHOD_GET,
      .mime_type = "text/plain",
      .skip_instance = true,
      .data = "This is a GNU Taler merchant backend. See https://taler.net/.\n",
      .data_size = strlen (
        "This is a GNU Taler merchant backend. See https://taler.net/.\n"),
      .handler = &TMH_MHD_handler_static_response,
      .response_code = MHD_HTTP_OK
    },
    {
      .url_prefix = "/agpl",
      .method = MHD_HTTP_METHOD_GET,
      .skip_instance = true,
      .handler = &TMH_MHD_handler_agpl_redirect
    },
    {
      .url_prefix = "/instances",
      .method = MHD_HTTP_METHOD_GET,
      .skip_instance = true,
      .handler = &TMH_private_get_instances
    },
    /* GET /instances/$ID/: */
    {
      .url_prefix = "/",
      .method = MHD_HTTP_METHOD_GET,
      .handler = &TMH_private_get_instances_ID
    },
#if 0
    /* DELETE /instances/$ID/: */
    {
      .url_prefix = "/",
      .method = MHD_HTTP_METHOD_DELETE,
      .handler = &TMH_private_delete_instances_ID
    },
    /* PATCH /instances/$ID/: */
    {
      .url_prefix = "/",
      .method = MHD_HTTP_METHOD_PATCH,
      .handler = &TMH_private_patch_instances_ID
    },
#endif
    {
      .url_prefix = "/instances",
      .method = MHD_HTTP_METHOD_POST,
      .skip_instance = true,
      .handler = &TMH_private_post_instances
    },
    {
      NULL
    }
  };
  static struct TMH_RequestHandler public_handlers[] = {
    {
      .url_prefix = "/",
      .method = MHD_HTTP_METHOD_GET,
      .mime_type = "text/plain",
      .skip_instance = true,
      .data = "This is a GNU Taler merchant backend. See https://taler.net/.\n",
      .data_size = strlen (
        "This is a GNU Taler merchant backend. See https://taler.net/.\n"),
      .handler = &TMH_MHD_handler_static_response,
      .response_code = MHD_HTTP_OK
    },
    {
      .url_prefix = "/agpl",
      .method = MHD_HTTP_METHOD_GET,
      .skip_instance = true,
      .handler = &TMH_MHD_handler_agpl_redirect
    },
    {
      .url_prefix = "/config",
      .method = MHD_HTTP_METHOD_GET,
      .skip_instance = true,
      .handler = &MH_handler_config
    },
    {
      NULL
    }
  };
  static struct TMH_RequestHandler h404 = {
    .mime_type = "text/html",
    .data = "<html><title>404: not found</title>"
            "<body>404: not found</body></html>",
    .data_size = strlen ("<html><title>404: not found</title>"
                         "<body>404: not found</body></html>"),
    .handler = &TMH_MHD_handler_static_response,
    .response_code = MHD_HTTP_NOT_FOUND
  };
  struct TMH_HandlerContext *hc = *con_cls;
  struct TMH_RequestHandler *handlers;

  (void) cls;
  (void) version;
  if (NULL != hc)
  {
    GNUNET_assert (NULL != hc->rh);
    GNUNET_SCHEDULER_begin_async_scope (&hc->async_scope_id);
    if ( (hc->has_body) &&
         (NULL == hc->request_body) )
    {
      int res;

      res = TALER_MHD_parse_post_json (connection,
                                       &hc->json_parse_context,
                                       upload_data,
                                       upload_data_size,
                                       &hc->request_body);
      if (GNUNET_SYSERR == res)
        return MHD_NO;
      /* A error response was already generated */
      if ( (GNUNET_NO == res) ||
           /* or, need more data to accomplish parsing */
           (NULL == hc->request_body) )
        return MHD_YES;
    }
    return hc->rh->handler (hc->rh,
                            connection,
                            hc);
  }
  hc = GNUNET_new (struct TMH_HandlerContext);
  *con_cls = hc;
  GNUNET_async_scope_fresh (&hc->async_scope_id);
  GNUNET_SCHEDULER_begin_async_scope (&hc->async_scope_id);
  hc->url = url;
  {
    const char *correlation_id;

    correlation_id = MHD_lookup_connection_value (connection,
                                                  MHD_HEADER_KIND,
                                                  "Taler-Correlation-Id");
    if ( (NULL != correlation_id) &&
         (GNUNET_YES != GNUNET_CURL_is_valid_scope_id (correlation_id)) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "illegal incoming correlation ID\n");
      correlation_id = NULL;
    }
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
  }

  if (0 == strcasecmp (method,
                       MHD_HTTP_METHOD_HEAD))
    method = MHD_HTTP_METHOD_GET; /* MHD will deal with the rest */

  /* Find out the merchant backend instance for the request.
   * If there is an instance, remove the instance specification
   * from the beginning of the request URL. */
  {
    const char *instance_prefix = "/instances/";

    if (0 == strncmp (url,
                      instance_prefix,
                      strlen (instance_prefix)))
    {
      /* url starts with "/instances/" */
      const char *istart = url + strlen (instance_prefix);
      const char *slash = strchr (istart, '/');
      char *instance_id;

      if (NULL == slash)
      {
        return TMH_MHD_handler_static_response (&h404,
                                                connection,
                                                hc);
      }
      instance_id = GNUNET_strndup (istart,
                                    slash - istart);
      hc->instance = TMH_lookup_instance (instance_id);
      GNUNET_free (instance_id);
      url = slash;
    }
    else
    {
      /* use 'default' */
      hc->instance = TMH_lookup_instance (NULL);
    }
  }

  {
    const char *private_prefix = "/private/";

    if (0 == strncmp (url,
                      private_prefix,
                      strlen (private_prefix)))
    {
      handlers = private_handlers;
      url += strlen (private_prefix) - 1;
    }
    else
    {
      handlers = public_handlers;
    }
  }
  if (strcmp (url,
              ""))
    url = "/"; /* code below does not like empty string */

  {
    /* Matching URL found, but maybe method doesn't match */
    size_t prefix_strlen; /* i.e. 8 for "/orders/", or 7 for "/config" */
    const char *infix_url = NULL; /* i.e. "$ORDER_ID", no '/'-es */
    size_t infix_strlen = 0; /* number of characters in infix_url */
    const char *suffix_url = NULL; /* i.e. "/refund", includes '/' at the beginning */
    size_t suffix_strlen = 0; /* number of characters in suffix_url */

    {
      const char *slash;

      slash = strchr (&url[1], '/');
      if (NULL == slash)
      {
        prefix_strlen = strlen (url);
      }
      else
      {
        prefix_strlen = slash - url + 1; /* includes both '/'-es if present! */
        infix_url = slash + 1;
        slash = strchr (&infix_url[1], '/');
        if (NULL == slash)
        {
          infix_strlen = strlen (infix_url);
        }
        else
        {
          infix_strlen = slash - infix_url;
          suffix_url = slash;
          suffix_strlen = strlen (suffix_url);
        }
        hc->infix = GNUNET_strndup (infix_url,
                                    infix_strlen);
      }
    }

    {
      bool url_found = false;

      for (unsigned int i = 0; NULL != handlers[i].url_prefix; i++)
      {
        struct TMH_RequestHandler *rh = &handlers[i];

        if ( (prefix_strlen != strlen (rh->url_prefix)) ||
             (0 != memcmp (url,
                           rh->url_prefix,
                           prefix_strlen)) )
          continue;
        if ( (NULL == infix_url)
             ^ (GNUNET_NO == rh->have_id_segment) )
          continue; /* infix existence missmatch */
        if ( (NULL == suffix_url)
             ^ (NULL != rh->url_suffix) )
          continue; /* suffix existence missmatch */
        if ( (NULL != suffix_url) &&
             ( (suffix_strlen != strlen (rh->url_suffix)) ||
               (0 != memcmp (suffix_url,
                             rh->url_suffix,
                             suffix_strlen)) ) )
          continue; /* suffix content missmatch */
        url_found = true;
        if (0 == strcasecmp (method,
                             MHD_HTTP_METHOD_OPTIONS))
        {
          return TALER_MHD_reply_cors_preflight (connection);
        }
        if ( (rh->method != NULL) &&
             (0 != strcasecmp (method, rh->method)) )
          continue;
        hc->rh = rh;
        break;
      }
      if ( (NULL == hc->rh) &&
           (url_found) )
        return TALER_MHD_reply_json_pack (connection,
                                          MHD_HTTP_METHOD_NOT_ALLOWED,
                                          "{s:s}",
                                          "error",
                                          "method not allowed");
      if (NULL == hc->rh)
        return TMH_MHD_handler_static_response (&h404,
                                                connection,
                                                hc);
    }
  }
  /* At this point, we must have found a handler */
  GNUNET_assert (NULL != hc->rh);
  if ( (NULL == hc->instance) &&
       (GNUNET_YES != hc->rh->skip_instance) )
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_INSTANCE_UNKNOWN,
                                       "merchant instance unknown");
  hc->has_body = ( (0 == strcasecmp (method,
                                     MHD_HTTP_METHOD_POST)) ||
                   (0 == strcasecmp (method,
                                     MHD_HTTP_METHOD_PATCH)) );
  if (hc->has_body)
  {
    /* FIXME: Maybe check for maximum upload size here
       and refuse if it is too big? (Note: maximum upload
       size may need to vary based on the handler.) */

    GNUNET_break (NULL == hc->request_body); /* can't have it already */
    return MHD_YES; /* proceed with upload */
  }
  return hc->rh->handler (hc->rh,
                          connection,
                          hc);
}


/**
 * Function called during startup to add all known instances to our
 * hash map in memory for faster lookups when we receive requests.
 *
 * @param cls closure, NULL, unused
 * @param merchant_pub public key of the instance
 * @param merchant_priv private key of the instance, NULL if not available
 * @param is detailed configuration settings for the instance
 * @param accounts_length length of the @a accounts array
 * @param accounts list of accounts of the merchant
 */
static void
add_instance_cb (void *cls,
                 const struct TALER_MerchantPublicKeyP *merchant_pub,
                 const struct TALER_MerchantPrivateKeyP *merchant_priv,
                 const struct TALER_MERCHANTDB_InstanceSettings *is,
                 unsigned int accounts_length,
                 const struct TALER_MERCHANTDB_AccountDetails accounts[])
{
  struct TMH_MerchantInstance *mi;

  (void) cls;
  GNUNET_assert (NULL != merchant_priv);
  mi = GNUNET_new (struct TMH_MerchantInstance);
  mi->settings = *is;
  mi->settings.id = GNUNET_strdup (mi->settings.id);
  mi->settings.name = GNUNET_strdup (mi->settings.name);
  mi->settings.address = json_incref (mi->settings.address);
  mi->settings.jurisdiction = json_incref (mi->settings.jurisdiction);
  mi->merchant_priv = *merchant_priv;
  mi->merchant_pub = *merchant_pub;
  for (unsigned int i = 0; i<accounts_length; i++)
  {
    const struct TALER_MERCHANTDB_AccountDetails *acc = &accounts[i];
    struct TMH_WireMethod *wm;

    wm = GNUNET_new (struct TMH_WireMethod);
    wm->h_wire = acc->h_wire;
    wm->j_wire = json_pack ("{s:s, s:s}",
                            "payto_uri", acc->payto_uri,
                            "salt", GNUNET_JSON_from_data_auto (&acc->salt));
    wm->wire_method = TALER_payto_get_method (acc->payto_uri);
    wm->active = acc->active;
    GNUNET_CONTAINER_DLL_insert (mi->wm_head,
                                 mi->wm_tail,
                                 wm);
  }
  GNUNET_assert (GNUNET_OK ==
                 TMH_add_instance (mi));
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
  cfg = config;
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Starting taler-merchant-httpd\n");
  go = TALER_MHD_GO_NONE;
  if (merchant_connection_close)
    go |= TALER_MHD_GO_FORCE_CONNECTION_CLOSE;
  TALER_MHD_setup (go);

  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_add_shutdown (&do_shutdown,
                                 NULL);
  if (GNUNET_OK !=
      TALER_config_get_currency (cfg,
                                 &TMH_currency))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (GNUNET_YES ==
      GNUNET_CONFIGURATION_get_value_yesno (cfg,
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
      (TMH_by_id_map = GNUNET_CONTAINER_multihashmap_create (1,
                                                             GNUNET_NO)))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  if (NULL ==
      (TMH_db = TALER_MERCHANTDB_plugin_load (cfg)))
  {
    GNUNET_SCHEDULER_shutdown ();
    return;
  }
  /* load instances */
  {
    enum GNUNET_DB_QueryStatus qs;

    qs = TMH_db->lookup_instances (TMH_db->cls,
                                   true,
                                   &add_instance_cb,
                                   NULL);
    if (0 > qs)
    {
      GNUNET_break (0);
      GNUNET_SCHEDULER_shutdown ();
      return;
    }
  }

  fh = TALER_MHD_bind (cfg,
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
                          MHD_OPTION_CONNECTION_TIMEOUT,
                          (unsigned int) 10 /* 10s */,
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
 * @return 0 ok, non-zero on error
 */
int
main (int argc,
      char *const *argv)
{
  struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_option_flag ('C',
                               "connection-close",
                               "force HTTP connections to be closed after each request",
                               &merchant_connection_close),
    GNUNET_GETOPT_option_timetravel ('T',
                                     "timetravel"),
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
