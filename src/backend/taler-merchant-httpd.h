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
#include <microhttpd.h>
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
struct WireMethod
{
  /**
   * Next entry in DLL.
   */
  struct WireMethod *next;

  /**
   * Previous entry in DLL.
   */
  struct WireMethod *prev;

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
  int active;

};


/**
 * Information that defines a merchant "instance". That way, a single
 * backend can account for several merchants, as used to do in donation
 * shops
 */
struct MerchantInstance
{

  /**
   * Instance's mnemonic identifier. This value lives as long as
   * the configuration is kept in memory, as it's as substring of
   * a section name
   */
  char *id;

  /**
   * Legal name of the merchant.
   */
  char *name;

  /**
   * File holding the merchant's private key
   */
  char *keyfile;

  /**
   * Next entry in DLL.
   */
  struct WireMethod *wm_head;

  /**
   * Previous entry in DLL.
   */
  struct WireMethod *wm_tail;

  /**
   * Merchant's private key
   */
  struct TALER_MerchantPrivateKeyP privkey;

  /**
   * Merchant's public key
   */
  struct TALER_MerchantPublicKeyP pubkey;

  /**
   * Exchange this instance uses for tipping, NULL if tipping
   * is not supported.
   */
  char *tip_exchange;

  /**
   * What is the private key of the reserve used for signing tips by this exchange?
   * Only valid if @e tip_exchange is non-null.
   */
  struct TALER_ReservePrivateKeyP tip_reserve;
};


/**
 * @brief Struct describing an URL and the handler for it.
 */
struct TMH_RequestHandler
{

  /**
   * URL the handler is for.
   */
  const char *url;

  /**
   * Method the handler is for, NULL for "all".
   */
  const char *method;

  /**
   * Mime type to use in reply (hint, can be NULL).
   */
  const char *mime_type;

  /**
   * Raw data for the @e handler
   */
  const void *data;

  /**
   * Number of bytes in @e data, 0 for 0-terminated.
   */
  size_t data_size;

  /**
   * Function to call to handle the request.
   *
   * @param rh this struct
   * @param mime_type the @e mime_type for the reply (hint, can be NULL)
   * @param connection the MHD connection to handle
   * @param[in,out] connection_cls the connection's closure (can be updated)
   * @param upload_data upload data
   * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
   * @param mi merchant backend instance, never NULL
   * @return MHD result code
   */
  MHD_RESULT (*handler)(struct TMH_RequestHandler *rh,
                        struct MHD_Connection *connection,
                        void **connection_cls,
                        const char *upload_data,
                        size_t *upload_data_size,
                        struct MerchantInstance *mi);

  /**
   * Default response code.
   */
  unsigned int response_code;
};


/**
 * Each MHD response handler that sets the "connection_cls" to a
 * non-NULL value must use a struct that has this struct as its first
 * member.  This struct contains a single callback, which will be
 * invoked to clean up the memory when the contection is completed.
 */
struct TM_HandlerContext;

/**
 * Signature of a function used to clean up the context
 * we keep in the "connection_cls" of MHD when handling
 * a request.
 *
 * @param hc header of the context to clean up.
 */
typedef void
(*TM_ContextCleanup)(struct TM_HandlerContext *hc);


/**
 * Each MHD response handler that sets the "connection_cls" to a
 * non-NULL value must use a struct that has this struct as its first
 * member.  This struct contains a single callback, which will be
 * invoked to clean up the memory when the connection is completed.
 */
struct TM_HandlerContext
{

  /**
   * Function to execute the handler-specific cleanup of the
   * (typically larger) context.
   */
  TM_ContextCleanup cc;

  /**
   * Which request handler is handling this request?
   */
  const struct TMH_RequestHandler *rh;

  /**
   * Asynchronous request context id.
   */
  struct GNUNET_AsyncScopeId async_scope_id;
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
   * Associated heap node.
   */
  struct GNUNET_CONTAINER_HeapNode *hn;

  /**
   * Key of this entry in the #payment_trigger_map
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
  int awaiting_refund;

};


/**
 * Locations from the configuration.  Mapping from
 * label to location data.
 */
extern json_t *default_locations;

/**
 * Default maximum wire fee to assume, unless stated differently in the proposal
 * already.
 */
extern struct TALER_Amount default_max_wire_fee;

/**
 * Default max deposit fee that the merchant is willing to
 * pay; if deposit costs more, then the customer will cover
 * the difference.
 */
extern struct TALER_Amount default_max_deposit_fee;

/**
 * Default factor for wire fee amortization.
 */
extern unsigned long long default_wire_fee_amortization;

/**
 * MIN-Heap of suspended connections to resume when the timeout expires,
 * ordered by timeout. Values are of type `struct TMH_SuspendedConnection`
 */
extern struct GNUNET_CONTAINER_Heap *resume_timeout_heap;

/**
 * Task responsible for timeouts in the #resume_timeout_heap.
 */
extern struct GNUNET_SCHEDULER_Task *resume_timeout_task;

/**
 * Hash map from H(order_id,merchant_pub) to `struct TMH_SuspendedConnection`
 * entries to resume when a payment is made for the given order.
 */
extern struct GNUNET_CONTAINER_MultiHashMap *payment_trigger_map;

/**
 * Which currency do we use?
 */
extern char *TMH_currency;

/**
 * Inform the auditor for all deposit confirmations (global option)
 */
extern int TMH_force_audit;

/**
 * Hash of our wire format details as given in #j_wire.
 */
extern struct GNUNET_HashCode h_wire;

/**
 * Our private key, corresponds to #pubkey.
 */
extern struct TALER_MerchantPrivateKeyP privkey;

/**
 * Our public key, corresponds to #privkey.
 */
extern struct TALER_MerchantPublicKeyP pubkey;

/**
 * Hashmap pointing at merchant instances by 'id'. An 'id' is
 * just a string that identifies a merchant instance. When a frontend
 * needs to specify an instance to the backend, it does so by 'id'
 */
extern struct GNUNET_CONTAINER_MultiHashMap *by_id_map;

/**
 * Handle to the database backend.
 */
extern struct TALER_MERCHANTDB_Plugin *db;

/**
 * If the frontend does NOT specify an execution date, how long should
 * we tell the exchange to wait to aggregate transactions before
 * executing the wire transfer?  This delay is added to the current
 * time when we generate the advisory execution time for the exchange.
 */
extern struct GNUNET_TIME_Relative default_wire_transfer_delay;

/**
 * If the frontend does NOT specify a payment deadline, how long should
 * offers we make be valid by default?
 */
extern struct GNUNET_TIME_Relative default_pay_deadline;

/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 * Basically, we need to explicitly resume MHD's event loop whenever
 * we made progress serving a request.  This function re-schedules
 * the task processing MHD's activities to run immediately.
 */
void
TMH_trigger_daemon (void);


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
                     struct GNUNET_HashCode *key);


/**
 * Suspend connection from @a sc until payment has been received.
 *
 * @param sc connection to suspend
 * @param min_refund refund amount we are waiting on to be exceeded before resuming,
 *                   NULL if we are not waiting for refunds
 */
void
TMH_long_poll_suspend (struct TMH_SuspendedConnection *sc,
                       const struct TALER_Amount *min_refund);


/**
 * Find out if we have any clients long-polling for @a order_id to be
 * confirmed at merchant @a mpub, and if so, tell them to resume.
 *
 * @param order_id the order that was paid
 * @param mpub the merchant's public key of the instance where the payment happened
 * @param refund_amount refunded amount, if the trigger was a refund, otherwise NULL
 */
void
TMH_long_poll_resume (const char *order_id,
                      const struct TALER_MerchantPublicKeyP *mpub,
                      const struct TALER_Amount *refund_amount);


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
                        const char *instance_id);


#endif
