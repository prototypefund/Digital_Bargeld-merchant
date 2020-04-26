/*
  This file is part of TALER
  (C) 2019, 2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_private-get-orders.c
 * @brief implement GET /orders
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_private-get-orders.h"


/**
 * A pending GET /orders request that is in long polling mode.
 */
struct PendingOrder
{

  /**
   * Kept in a DLL.
   */
  struct PendingOrder *prev;

  /**
   * Kept in a DLL.
   */
  struct PendingOrder *next;

  /**
   * Which connection was suspended.
   */
  struct MHD_Connection *con;

  /**
   * Associated heap node.
   */
  struct GNUNET_CONTAINER_HeapNode *hn;

  /**
   * Which instance is this client polling?
   */
  char *instance_id;

  /**
   * At what time does this request expire? If set in the future, we
   * may wait this long for a payment to arrive before responding.
   */
  struct GNUNET_TIME_Absolute long_poll_timeout;

  /**
   * Array where we append matching orders. Must be
   * json_decref()'ed when done with the `struct PendingOrder`!
   */
  json_t *pa;

  /**
   * Filter to apply.
   */
  struct TALER_MERCHANTDB_OrderFilter of;
};


/**
 * Head of DLL of long-polling GET /orders requests.
 */
static struct PendingOrder *po_head;

/**
 * Tail of DLL of long-polling GET /orders requests.
 */
static struct PendingOrder *po_tail;

/**
 * Task to timeout pending orders.
 */
static struct GNUNET_SCHEDULER_Task *order_timeout_task;

/**
 * Heap for orders in long polling awaiting timeout.
 */
static struct GNUNET_CONTAINER_Heap *order_timeout_heap;


/**
 * We are shutting down, force resume of all GET /orders requests.
 */
void
TMH_force_get_orders_resume ()
{
  struct PendingOrder *po;

  while (NULL != (po = po_head))
  {
    GNUNET_CONTAINER_DLL_remove (po_head,
                                 po_tail,
                                 po);
    GNUNET_assert (po ==
                   GNUNET_CONTAINER_heap_remove_root (order_timeout_heap));
    MHD_resume_connection (po->con);
    json_decref (po->pa);
    GNUNET_free (po->instance_id);
    GNUNET_free (po);
  }
  if (NULL != order_timeout_task)
  {
    GNUNET_SCHEDULER_cancel (order_timeout_task);
    order_timeout_task = NULL;
  }
  if (NULL != order_timeout_heap)
  {
    GNUNET_CONTAINER_heap_destroy (order_timeout_heap);
    order_timeout_heap = NULL;
  }
}


/**
 * Task run to trigger timeouts on GET /orders requests with long polling.
 *
 * @param cls unused
 */
static void
order_timeout (void *cls)
{
  struct PendingOrder *po;

  (void) cls;
  order_timeout_task = NULL;
  while (1)
  {
    po = GNUNET_CONTAINER_heap_peek (order_timeout_heap);
    if (NULL == po)
    {
      /* release data structure, we don't need it right now */
      GNUNET_CONTAINER_heap_destroy (order_timeout_heap);
      order_timeout_heap = NULL;
      return;
    }
    if  (0 !=
         GNUNET_TIME_absolute_get_remaining (
           po->long_poll_timeout).rel_value_us)
      break;
    GNUNET_assert (po ==
                   GNUNET_CONTAINER_heap_remove_root (order_timeout_heap));
    po->hn = NULL;
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Resuming long polled job due to timeout\n");
    GNUNET_CONTAINER_DLL_remove (po_head,
                                 po_tail,
                                 po);
    json_decref (po->pa);
    MHD_resume_connection (po->con);
    GNUNET_free (po->instance_id);
    GNUNET_free (po);
  }
  order_timeout_task = GNUNET_SCHEDULER_add_at (po->long_poll_timeout,
                                                &order_timeout,
                                                NULL);
}


/**
 * Cleanup our "context", where we stored the JSON array
 * we are building for the response.
 *
 * @param ctx context to clean up, must be a `json_t *`
 */
static void
json_cleanup (void *ctx)
{
  json_t *j = ctx;

  json_decref (j);
}


/**
 * Add order details to our JSON array.
 *
 * @param[in,out] cls a `json_t *` JSON array to build
 * @param order_id ID of the order
 * @param order_serial serial ID of the order
 * @param creation_time when was the order created
 */
static void
add_order (void *cls,
           const char *order_id,
           uint64_t order_serial,
           struct GNUNET_TIME_Absolute creation_time)
{
  json_t *pa = cls;

  GNUNET_assert (0 ==
                 json_array_append_new (
                   pa,
                   json_pack (
                     "{s:s, s:I, s:o}",
                     "order_id",
                     order_id,
                     "row_id",
                     (json_int_t) order_serial,
                     "timestamp",
                     GNUNET_JSON_from_time_abs (creation_time))));
}


/**
 * There has been a change or addition of a new @a order_id.  Wake up
 * long-polling clients that may have been waiting for this event.
 *
 * FIXME: Here we go over all long polling clients.  We should consider moving
 * the global DLL into the *instance* data structure (note: that has then
 * implications in case an instance is deleted, i.e. we would then need to
 * trigger all the long pollers!).
 *
 * @param instance_id the instance where the order changed
 * @param order_id the order that changed
 * @param paid is the order paid by the customer?
 * @param refunded was the order refunded?
 * @param wire was the merchant paid via wire transfer?
 * @param data execution date of the order
 * @param order_serial_id serial ID of the order in the database
 */
void
TMH_notify_order_change (const char *instance_id,
                         const char *order_id,
                         bool paid,
                         bool refunded,
                         bool wired,
                         struct GNUNET_TIME_Absolute date,
                         uint64_t order_serial_id)
{
  struct PendingOrder *pn;

  for (struct PendingOrder *po = po_head;
       NULL != po;
       po = pn)
  {
    pn = po->next;
    if (! ( ( ((TALER_MERCHANTDB_YNA_YES == po->of.paid) == paid) ||
              (TALER_MERCHANTDB_YNA_ALL == po->of.paid) ) &&
            ( ((TALER_MERCHANTDB_YNA_YES == po->of.refunded) == refunded) ||
              (TALER_MERCHANTDB_YNA_ALL == po->of.refunded) ) &&
            ( ((TALER_MERCHANTDB_YNA_YES == po->of.wired) == wired) ||
              (TALER_MERCHANTDB_YNA_ALL == po->of.wired) ) ) )
      continue;
    if (0 != strcmp (instance_id,
                     po->instance_id))
      continue;
    if (po->of.delta > 0)
    {
      if (order_serial_id < po->of.start_row)
        continue;
      if (date.abs_value_us < po->of.date.abs_value_us)
        continue;
      po->of.delta--;
    }
    else
    {
      if (order_serial_id > po->of.start_row)
        continue;
      if (date.abs_value_us > po->of.date.abs_value_us)
        continue;
      po->of.delta++;
    }
    add_order (po->pa,
               order_id,
               order_serial_id,
               date);
    GNUNET_CONTAINER_DLL_remove (po_head,
                                 po_tail,
                                 po);
    GNUNET_assert (po ==
                   GNUNET_CONTAINER_heap_remove_node (po->hn));
    MHD_resume_connection (po->con);
    json_decref (po->pa);
    GNUNET_free (po->instance_id);
    GNUNET_free (po);
  }
}


/**
 * Convert query argument to @a yna value.
 *
 * @param connection connection to take query argument from
 * @param arg argument to try for
 * @param[out] value to set
 * @return true on success, false if the parameter was malformed
 */
static bool
arg_to_yna (struct MHD_Connection *connection,
            const char *arg,
            enum TALER_MERCHANTDB_YesNoAll *yna)
{
  const char *str;

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     arg);
  if (NULL == str)
  {
    *yna = TALER_MERCHANTDB_YNA_ALL;
    return true;
  }
  if (0 == strcasecmp (str, "yes"))
  {
    *yna = TALER_MERCHANTDB_YNA_YES;
    return true;
  }
  if (0 == strcasecmp (str, "no"))
  {
    *yna = TALER_MERCHANTDB_YNA_NO;
    return true;
  }
  if (0 == strcasecmp (str, "all"))
  {
    *yna = TALER_MERCHANTDB_YNA_ALL;
    return true;
  }
  return false;
}


/**
 * Handle a GET "/orders" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] hc context with further information about the request
 * @return MHD result code
 */
MHD_RESULT
TMH_private_get_orders (const struct TMH_RequestHandler *rh,
                        struct MHD_Connection *connection,
                        struct TMH_HandlerContext *hc)
{
  json_t *pa;
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_MERCHANTDB_OrderFilter of;

  if (NULL != hc->ctx)
  {
    /* resumed from long-polling, return answer we already have
       in 'hc->ctx' */
    return TALER_MHD_reply_json_pack (connection,
                                      MHD_HTTP_OK,
                                      "{s:O}",
                                      "orders", hc->ctx);
  }

  if (! (arg_to_yna (connection,
                     "paid",
                     &of.paid)) )
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "paid");
  if (! (arg_to_yna (connection,
                     "refunded",
                     &of.refunded)) )
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "refunded");
  if (! (arg_to_yna (connection,
                     "wired",
                     &of.wired)) )
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "wired");
  {
    const char *start_row_str;

    start_row_str = MHD_lookup_connection_value (connection,
                                                 MHD_GET_ARGUMENT_KIND,
                                                 "start");
    if (NULL == start_row_str)
    {
      of.start_row = UINT64_MAX;
    }
    else
    {
      char dummy[2];
      unsigned long long ull;

      if (1 !=
          sscanf (start_row_str,
                  "%llu%1s",
                  &ull,
                  dummy))
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "date");
      of.start_row = (uint64_t) ull;
    }
  }
  {
    const char *delta_str;

    delta_str = MHD_lookup_connection_value (connection,
                                             MHD_GET_ARGUMENT_KIND,
                                             "delta");
    if (NULL == delta_str)
    {
      of.delta = -20;
    }
    else
    {
      char dummy[2];
      long long ll;

      if (1 !=
          sscanf (delta_str,
                  "%lld%1s",
                  &ll,
                  dummy))
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "delta");
      of.delta = (uint64_t) ll;
    }
  }
  {
    const char *date_str;

    date_str = MHD_lookup_connection_value (connection,
                                            MHD_GET_ARGUMENT_KIND,
                                            "date");
    if (NULL == date_str)
    {
      if (of.delta > 0)
        of.date = GNUNET_TIME_UNIT_ZERO_ABS;
      else
        of.date = GNUNET_TIME_UNIT_FOREVER_ABS;
    }
    else
    {
      if (GNUNET_OK !=
          GNUNET_STRINGS_fancy_time_to_absolute (date_str,
                                                 &of.date))
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "date");
    }
  }
  {
    const char *timeout_ms_str;

    timeout_ms_str = MHD_lookup_connection_value (connection,
                                                  MHD_GET_ARGUMENT_KIND,
                                                  "timeout_ms");
    if (NULL == timeout_ms_str)
    {
      of.timeout = GNUNET_TIME_UNIT_ZERO;
    }
    else
    {
      char dummy[2];
      unsigned long long ull;

      if (1 !=
          sscanf (timeout_ms_str,
                  "%lld%1s",
                  &ull,
                  dummy))
        return TALER_MHD_reply_with_error (connection,
                                           MHD_HTTP_BAD_REQUEST,
                                           TALER_EC_PARAMETER_MALFORMED,
                                           "timeout_ms");
      of.timeout = GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MILLISECONDS,
                                                  ull);
    }
  }

  pa = json_array ();
  GNUNET_assert (NULL != pa);
  qs = TMH_db->lookup_orders (TMH_db->cls,
                              hc->instance->settings.id,
                              &of,
                              &add_order,
                              pa);
  if (0 > qs)
  {
    GNUNET_break (0);
    json_decref (pa);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_ORDERS_GET_DB_LOOKUP_ERROR,
                                       "failed to lookup orders in database");
  }
  if ( (0 == qs) &&
       (of.timeout.rel_value_us > 0) )
  {
    struct PendingOrder *po;

    /* setup timeout heap (if not yet exists) */
    if (NULL == order_timeout_heap)
      order_timeout_heap
        = GNUNET_CONTAINER_heap_create (GNUNET_CONTAINER_HEAP_ORDER_MIN);
    hc->ctx = pa;
    hc->cc = &json_cleanup;
    po = GNUNET_new (struct PendingOrder);
    po->instance_id = GNUNET_strdup (hc->instance->settings.id);
    po->con = connection;
    po->pa = json_incref (pa);
    po->hn = GNUNET_CONTAINER_heap_insert (order_timeout_heap,
                                           po,
                                           po->long_poll_timeout.abs_value_us);
    po->long_poll_timeout = GNUNET_TIME_relative_to_absolute (of.timeout);
    po->of = of;
    GNUNET_CONTAINER_DLL_insert (po_head,
                                 po_tail,
                                 po);
    MHD_suspend_connection (connection);
    /* start timeout task */
    po = GNUNET_CONTAINER_heap_peek (order_timeout_heap);
    if (NULL != order_timeout_task)
      GNUNET_SCHEDULER_cancel (order_timeout_task);
    order_timeout_task = GNUNET_SCHEDULER_add_at (po->long_poll_timeout,
                                                  &order_timeout,
                                                  NULL);
    return MHD_YES;
  }
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:o}",
                                    "orders", pa);
}


/* end of taler-merchant-httpd_private-get-orders.c */
