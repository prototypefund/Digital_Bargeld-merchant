/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

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
 * @file merchant/backend/taler-merchant-httpd.h
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#ifndef TALER_MERCHANT_HTTPD_H
#define TALER_MERCHANT_HTTPD_H

#include "platform.h"
#include "taler_merchantdb_lib.h"
#include <taler/taler_mhd_lib.h>
#include <gnunet/gnunet_mhd_compat.h>

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)


/**
 * Supported wire method.  Kept in a DLL.
 */
struct TMH_WireMethod
{
  /**
   * Next entry in DLL.
   */
  struct TMH_WireMethod *next;

  /**
   * Previous entry in DLL.
   */
  struct TMH_WireMethod *prev;

  /**
   * Which wire method / payment target identifier is @e j_wire using?
   */
  char *wire_method;

  /**
   * Wire details for this instance
   */
  struct json_t *j_wire;

  /**
   * Hash of our wire format details as given in #j_wire.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Is this wire method active (should it be included in new contracts)?
   */
  bool active;

  /**
   * Are we currently in a transaction to delete this account?
   */
  bool deleting;

};


/**
 * Information that defines a merchant "instance". That way, a single
 * backend can account for several merchants, as used to do in donation
 * shops
 */
struct TMH_MerchantInstance
{

  /**
   * Next entry in DLL.
   */
  struct TMH_WireMethod *wm_head;

  /**
   * Previous entry in DLL.
   */
  struct TMH_WireMethod *wm_tail;

  /**
   * Merchant's private key.
   */
  struct TALER_MerchantPrivateKeyP merchant_priv;

  /**
   * Merchant's public key
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * General settings for an instance.
   */
  struct TALER_MERCHANTDB_InstanceSettings settings;

  /**
   * Reference counter on this structure. Only destroyed if the
   * counter hits zero.
   */
  unsigned int rc;
};


/**
 * @brief Struct describing an URL and the handler for it.
 *
 * The overall URL is always @e url_prefix, optionally followed by the
 * id_segment, which is optionally followed by the @e url_suffix. It is NOT
 * allowed for the @e url_prefix to be directly followed by the @e url_suffix.
 * A @e url_suffix SHOULD only be used with a @e method of #MHD_HTTP_METHOD_POST.
 */
struct TMH_RequestHandler;

/**
 * This information is stored in the "connection_cls" of MHD for
 * every request that we process.
 * Individual handlers can evaluate tis memebers and
 * are allowed to update @e cc and @e ctx to store and clean up
 * handler-specific data.
 */
struct TMH_HandlerContext;


/**
 * @brief Struct describing an URL and the handler for it.
 *
 * The overall URL is always @e url_prefix, optionally followed by the
 * id_segment, which is optionally followed by the @e url_suffix. It is NOT
 * allowed for the @e url_prefix to be directly followed by the @e url_suffix.
 * A @e url_suffix SHOULD only be used with a @e method of #MHD_HTTP_METHOD_POST.
 */
struct TMH_RequestHandler
{

  /**
   * URL prefix the handler is for.
   */
  const char *url_prefix;

  /**
   * Does this request include an identifier segment
   * (product_id, reserve_pub, order_id, tip_id) in the
   * second segment?
   */
  bool have_id_segment;

  /**
   * Does this request handler work without an instance?
   */
  bool skip_instance;

  /**
   * URL suffix the handler is for.
   */
  const char *url_suffix;

  /**
   * Method the handler is for, NULL for "all".
   */
  const char *method;

  /**
   * Mime type to use in reply (hint, can be NULL).
   */
  const char *mime_type;

  /**
   * Raw data for the @e handler (can be NULL).
   */
  const void *data;

  /**
   * Number of bytes in @e data.
   */
  size_t data_size;

  /**
   * Handler to be called for this URL/METHOD combination.
   *
   * @param rh this struct
   * @param connection the MHD connection to handle
   * @param[in,out] hc context with further information about the request
   * @return MHD result code
   */
  MHD_RESULT
  (*handler)(const struct TMH_RequestHandler *rh,
             struct MHD_Connection *connection,
             struct TMH_HandlerContext *hc);

  /**
   * Default response code to use.
   */
  unsigned int response_code;
};


/**
 * Signature of a function used to clean up the context
 * we keep in the "connection_cls" of MHD when handling
 * a request.
 *
 * @param ctxt the context to clean up.
 */
typedef void
(*TMH_ContextCleanup)(void *ctx);


/**
 * This information is stored in the "connection_cls" of MHD for
 * every request that we process.
 * Individual handlers can evaluate tis memebers and
 * are allowed to update @e cc and @e ctx to store and clean up
 * handler-specific data.
 */
struct TMH_HandlerContext
{

  /**
   * Function to execute the handler-specific cleanup of the
   * (request-specific) context in @e ctx.
   */
  TMH_ContextCleanup cc;

  /**
   * Client-specific context we keep. Passed to @e cc.
   */
  void *ctx;

  /**
   * Which request handler is handling this request?
   */
  const struct TMH_RequestHandler *rh;

  /**
   * Which instance is handling this request?
   */
  struct TMH_MerchantInstance *instance;

  /**
   * Asynchronous request context id.
   */
  struct GNUNET_AsyncScopeId async_scope_id;

  /**
   * Our original URL, for logging.
   */
  const char *url;

  /**
   * Infix part of @a url.
   */
  char *infix;

  /**
   * JSON body that was uploaded, NULL if @e has_body is false.
   */
  json_t *request_body;

  /**
   * Placeholder for #TALER_MHD_parse_post_json() to keep its internal state.
   * Used when we parse the POSTed data.
   */
  void *json_parse_context;

  /**
   * Set to true if this is an #MHD_HTTP_METHOD_POST or #MHD_HTTP_METHOD_PATCH request.
   * (In principle #MHD_HTTP_METHOD_PUT may also belong, but we do not have PUTs
   * in the API today, so we do not test for PUT.)
   */
  bool has_body;
};


/**
 * Entry in a #resume_timeout_heap.
 */
struct TMH_SuspendedConnection
{
  /**
   * Which connection was suspended.
   */
  struct MHD_Connection *con;

  /**
   * Associated heap node.  Used internally by #TMH_long_poll_suspend()
   * and TMH_long_poll_resume().
   */
  struct GNUNET_CONTAINER_HeapNode *hn;

  /**
   * Key of this entry in the #payment_trigger_map.  Used internally by
   * #TMH_long_poll_suspend() and TMH_long_poll_resume().
   */
  struct GNUNET_HashCode key;

  /**
   * At what time does this request expire? If set in the future, we
   * may wait this long for a payment to arrive before responding.
   */
  struct GNUNET_TIME_Absolute long_poll_timeout;

  /**
   * Minimum refund amount to be exceeded (exclusive this value!) for resume.
   */
  struct TALER_Amount refund_expected;

  /**
   * #GNUNET_YES if we are waiting for a refund.
   */
  bool awaiting_refund;

};


/**
 * Which currency do we use?
 */
extern char *TMH_currency;

/**
 * Inform the auditor for all deposit confirmations (global option)
 */
extern int TMH_force_audit;

/**
 * Handle to the database backend.
 */
extern struct TALER_MERCHANTDB_Plugin *TMH_db;

/**
 * Hashmap pointing at merchant instances by 'id'. An 'id' is
 * just a string that identifies a merchant instance. When a frontend
 * needs to specify an instance to the backend, it does so by 'id'
 */
extern struct GNUNET_CONTAINER_MultiHashMap *TMH_by_id_map;

/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 * Basically, we need to explicitly resume MHD's event loop whenever
 * we made progress serving a request.  This function re-schedules
 * the task processing MHD's activities to run immediately.
 */
void
TMH_trigger_daemon (void);


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
                       const struct TALER_Amount *min_refund);


/**
 * Find out if we have any clients long-polling for @a order_id to be
 * confirmed at merchant @a mpub, and if so, tell them to resume.
 *
 * @param order_id the order that was paid or refunded
 * @param mi the merchant instance where the payment or refund happened
 * @param refund_amount refunded amount, if the trigger was a refund, otherwise NULL
 */
void
TMH_long_poll_resume (const char *order_id,
                      const struct TMH_MerchantInstance *mi,
                      const struct TALER_Amount *refund_amount);


/**
 * Decrement reference counter of @a mi, and free if it hits zero.
 *
 * @param[in,out] mi merchant instance to update and possibly free
 */
void
TMH_instance_decref (struct TMH_MerchantInstance *mi);


/**
 * Add instance definition to our active set of instances.
 *
 * @param[in,out] mi merchant instance details to define
 * @return #GNUNET_OK on success, #GNUNET_NO if the same ID is in use already
 */
int
TMH_add_instance (struct TMH_MerchantInstance *mi);

/**
 * Lookup a merchant instance by its instance ID.
 *
 * @param instance_id identifier of the instance to resolve
 * @return NULL if that instance is unknown to us
 */
struct TMH_MerchantInstance *
TMH_lookup_instance (const char *instance_id);


#endif
