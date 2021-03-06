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
 * @file backend/taler-merchant-httpd_exchanges.c
 * @brief logic this HTTPD keeps for each exchange we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd_exchanges.h"


/**
 * Delay after which we'll re-fetch key information from the exchange.
 */
#define RELOAD_DELAY GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MINUTES, 2)

/**
 * Delay after which we'll allow clients to force us to re-fetch key
 * information from the exchange if we don't know the denomination key.
 */
#define FORCED_RELOAD_DELAY GNUNET_TIME_relative_multiply ( \
    GNUNET_TIME_UNIT_MINUTES, 15)

/**
 * Threshold after which exponential backoff should not increase.
 */
#define RETRY_BACKOFF_THRESHOLD GNUNET_TIME_relative_multiply ( \
    GNUNET_TIME_UNIT_SECONDS, 60)


/**
 * Perform our exponential back-off calculation, starting at 1 ms
 * and then going by a factor of 2 up unto a maximum of RETRY_BACKOFF_THRESHOLD.
 *
 * @param r current backoff time, initially zero
 */
#define RETRY_BACKOFF(r) GNUNET_TIME_relative_min (RETRY_BACKOFF_THRESHOLD, \
                                                   GNUNET_TIME_relative_multiply ( \
                                                     GNUNET_TIME_relative_max ( \
                                                       GNUNET_TIME_UNIT_MILLISECONDS, \
                                                       (r)), 2));


/**
 * Exchange
 */
struct Exchange;


/**
 * Information we keep for a pending #MMH_EXCHANGES_find_exchange() operation.
 */
struct TMH_EXCHANGES_FindOperation
{

  /**
   * Kept in a DLL.
   */
  struct TMH_EXCHANGES_FindOperation *next;

  /**
   * Kept in a DLL.
   */
  struct TMH_EXCHANGES_FindOperation *prev;

  /**
   * Function to call with the result.
   */
  TMH_EXCHANGES_FindContinuation fc;

  /**
   * Closure for @e fc.
   */
  void *fc_cls;

  /**
   * Exchange we wait for the /keys for.
   */
  struct Exchange *my_exchange;

  /**
   * Wire method we care about for fees, NULL if we do not care about wire fees.
   */
  char *wire_method;

  /**
   * Task scheduled to asynchronously return the result to
   * the find continuation.
   */
  struct GNUNET_SCHEDULER_Task *at;

};


/**
 * Information about wire transfer fees of an exchange, by wire method.
 */
struct FeesByWireMethod
{

  /**
   * Kept in a DLL.
   */
  struct FeesByWireMethod *next;

  /**
   * Kept in a DLL.
   */
  struct FeesByWireMethod *prev;

  /**
   * Wire method these fees are for.
   */
  char *wire_method;

  /**
   * Applicable fees, NULL if unknown/error.
   */
  struct TALER_EXCHANGE_WireAggregateFees *af;

};


/**
 * Exchange
 */
struct Exchange
{

  /**
   * Kept in a DLL.
   */
  struct Exchange *next;

  /**
   * Kept in a DLL.
   */
  struct Exchange *prev;

  /**
   * Head of FOs pending for this exchange.
   */
  struct TMH_EXCHANGES_FindOperation *fo_head;

  /**
   * Tail of FOs pending for this exchange.
   */
  struct TMH_EXCHANGES_FindOperation *fo_tail;

  /**
   * (base) URL of the exchange.
   */
  char *url;

  /**
   * A connection to this exchange
   */
  struct TALER_EXCHANGE_Handle *conn;

  /**
   * Active /wire request to the exchange, or NULL.
   */
  struct TALER_EXCHANGE_WireHandle *wire_request;

  /**
   * Task to re-run /wire after some delay.
   */
  struct GNUNET_SCHEDULER_Task *wire_task;

  /**
   * Head of wire fees from /wire request.
   */
  struct FeesByWireMethod *wire_fees_head;

  /**
   * Tail of wire fees from /wire request.
   */
  struct FeesByWireMethod *wire_fees_tail;

  /**
   * Master public key, guaranteed to be set ONLY for
   * trusted exchanges.
   */
  struct TALER_MasterPublicKeyP master_pub;

  /**
   * How soon can may we, at the earliest, re-download /keys?
   */
  struct GNUNET_TIME_Absolute first_retry;

  /**
   * How long should we wait between the next retry?
   */
  struct GNUNET_TIME_Relative retry_delay;

  /**
   * How long should we wait between the next retry for /wire?
   */
  struct GNUNET_TIME_Relative wire_retry_delay;

  /**
   * Task where we retry fetching /keys from the exchange.
   */
  struct GNUNET_SCHEDULER_Task *retry_task;

  /**
   * #GNUNET_YES to indicate that there is an ongoing
   * transfer we are waiting for,
   * #GNUNET_NO to indicate that key data is up-to-date.
   */
  int pending;

  /**
   * #GNUNET_YES if this exchange is from our configuration and
   * explicitly trusted, #GNUNET_NO if we need to check each
   * key to be sure it is trusted.
   */
  int trusted;

};


/**
 * Context for all exchange operations (useful to the event loop)
 */
static struct GNUNET_CURL_Context *merchant_curl_ctx;

/**
 * Context for integrating #merchant_curl_ctx with the
 * GNUnet event loop.
 */
static struct GNUNET_CURL_RescheduleContext *merchant_curl_rc;

/**
 * Head of exchanges we know about.
 */
static struct Exchange *exchange_head;

/**
 * Tail of exchanges we know about.
 */
static struct Exchange *exchange_tail;

/**
 * List of our trusted exchanges for inclusion in contracts.
 */
json_t *TMH_trusted_exchanges;


/**
 * Function called with information about who is auditing
 * a particular exchange and what key the exchange is using.
 *
 * @param cls closure, will be `struct Exchange` so that
 *   when this function gets called, it will change the flag 'pending'
 *   to 'false'. Note: 'keys' is automatically saved inside the exchange's
 *   handle, which is contained inside 'struct Exchange', when
 *   this callback is called. Thus, once 'pending' turns 'false',
 *   it is safe to call 'TALER_EXCHANGE_get_keys()' on the exchange's handle,
 *   in order to get the "good" keys.
 * @param hr http response details
 * @param keys information about the various keys used
 *        by the exchange
 * @param compat version compatibility data
 */
static void
keys_mgmt_cb (void *cls,
              const struct TALER_EXCHANGE_HttpResponse *hr,
              const struct TALER_EXCHANGE_Keys *keys,
              enum TALER_EXCHANGE_VersionCompatibility compat);


/**
 * Retry getting information from the given exchange in
 * the closure.
 *
 * @param cls the exchange
 *
 */
static void
retry_exchange (void *cls)
{
  struct Exchange *exchange = cls;

  /* might be a scheduled reload and not our first attempt */
  exchange->retry_task = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Connecting to exchange %s in retry_exchange()\n",
              exchange->url);
  if (NULL != exchange->conn)
  {
    TALER_EXCHANGE_disconnect (exchange->conn);
    exchange->conn = NULL;
  }
  exchange->conn = TALER_EXCHANGE_connect (merchant_curl_ctx,
                                           exchange->url,
                                           &keys_mgmt_cb,
                                           exchange,
                                           TALER_EXCHANGE_OPTION_END);
  /* Note: while the API spec says 'returns NULL on error', the implementation
     actually never returns NULL. */
  GNUNET_break (NULL != exchange->conn);
}


/**
 * Function called with information about the wire fees
 * for each wire method.  Stores the wire fees with the
 * exchange for laster use.
 *
 * @param cls closure
 * @param wire_method name of the wire method (i.e. "sepa")
 * @param fees fee structure for this method
 * @return #TALER_EC_NONE on success
 */
static enum TALER_ErrorCode
process_wire_fees (struct Exchange *exchange,
                   const struct TALER_MasterPublicKeyP *master_pub,
                   const char *wire_method,
                   const struct TALER_EXCHANGE_WireAggregateFees *fees)
{
  struct FeesByWireMethod *f;
  struct TALER_EXCHANGE_WireAggregateFees *endp;
  struct TALER_EXCHANGE_WireAggregateFees *af;

  for (f = exchange->wire_fees_head; NULL != f; f = f->next)
    if (0 == strcasecmp (wire_method,
                         f->wire_method))
      break;
  if (NULL == f)
  {
    f = GNUNET_new (struct FeesByWireMethod);
    f->wire_method = GNUNET_strdup (wire_method);
    GNUNET_CONTAINER_DLL_insert (exchange->wire_fees_head,
                                 exchange->wire_fees_tail,
                                 f);
  }
  endp = f->af;
  while ( (NULL != endp) &&
          (NULL != endp->next) )
    endp = endp->next;
  while ( (NULL != endp) &&
          (fees->start_date.abs_value_us < endp->end_date.abs_value_us) )
    fees = fees->next;
  if ( (NULL != endp) &&
       (fees->start_date.abs_value_us != endp->end_date.abs_value_us) )
  {
    /* Hole in the fee structure, not allowed! */
    GNUNET_break_op (0);
    return TALER_EC_HOLE_IN_WIRE_FEE_STRUCTURE;
  }
  while (NULL != fees)
  {
    struct GNUNET_HashCode h_wire_method;
    enum GNUNET_DB_QueryStatus qs;

    af = GNUNET_new (struct TALER_EXCHANGE_WireAggregateFees);
    *af = *fees;
    GNUNET_CRYPTO_hash (wire_method,
                        strlen (wire_method) + 1,
                        &h_wire_method);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Storing wire fee for `%s' and method `%s' at %s in DB; the fee is %s\n",
                TALER_B2S (master_pub),
                wire_method,
                GNUNET_STRINGS_absolute_time_to_string (af->start_date),
                TALER_amount2s (&af->wire_fee));
    db->preflight (db->cls);
    if (GNUNET_OK !=
        db->start (db->cls,
                   "store wire fee"))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to start database transaction to store exchange wire fees (will try to continue anyway)!\n");
      GNUNET_free (af);
      fees = fees->next;
      continue;
    }
    qs = db->store_wire_fee_by_exchange (db->cls,
                                         master_pub,
                                         &h_wire_method,
                                         &af->wire_fee,
                                         &af->closing_fee,
                                         af->start_date,
                                         af->end_date,
                                         &af->master_sig);
    if (0 > qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Failed to persist exchange wire fees in merchant DB (will try to continue anyway)!\n");
      GNUNET_free (af);
      fees = fees->next;
      db->rollback (db->cls);
      continue;
    }
    if (0 == qs)
    {
      /* Entry was already in DB, fine, continue as if we had succeeded */
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "Fees already in DB, rolling back transaction attempt!\n");
      db->rollback (db->cls);
    }
    if (0 < qs)
    {
      /* Inserted into DB, make sure transaction completes */
      qs = db->commit (db->cls);
      if (0 > qs)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to persist exchange wire fees in merchant DB (will try to continue anyway)!\n");
        GNUNET_free (af);
        fees = fees->next;
        continue;
      }
    }
    af->next = NULL;
    if (NULL == endp)
      f->af = af;
    else
      endp->next = af;
    endp = af;
    fees = fees->next;
  }
  return TALER_EC_NONE;
}


/**
 * Function called with information about the wire accounts
 * of the exchanage.  Stores the wire fees with the
 * exchange for laster use.
 *
 * @param exchange the exchange
 * @param master_pub public key of the exchange
 * @param accounts_len length of the @a accounts array
 * @param accounts list of wire accounts of the exchange
 * @return #TALER_EC_NONE on success
 */
static enum TALER_ErrorCode
process_wire_accounts (struct Exchange *exchange,
                       const struct TALER_MasterPublicKeyP *master_pub,
                       unsigned int accounts_len,
                       const struct TALER_EXCHANGE_WireAccount *accounts)
{
  for (unsigned int i = 0; i<accounts_len; i++)
  {
    enum TALER_ErrorCode ec;
    char *method;

    method = TALER_payto_get_method (accounts[i].payto_uri);
    if (NULL == method)
    {
      /* malformed payto:// URI returned by exchange */
      GNUNET_break_op (0);
      return TALER_EC_PAYTO_MALFORMED;
    }
    ec = process_wire_fees (exchange,
                            master_pub,
                            method,
                            accounts[i].fees);
    GNUNET_free (method);
    if (TALER_EC_NONE != ec)
      return ec;
  }
  return TALER_EC_NONE;
}


/**
 * Obtain applicable fees for @a exchange and @a wire_method.
 *
 * @param exchange the exchange to query
 * @param now current time
 * @param wire_method the wire method we want the fees for
 * @return NULL if we do not have fees for this method yet
 */
static struct TALER_EXCHANGE_WireAggregateFees *
get_wire_fees (struct Exchange *exchange,
               struct GNUNET_TIME_Absolute now,
               const char *wire_method)
{
  for (struct FeesByWireMethod *fbw = exchange->wire_fees_head;
       NULL != fbw;
       fbw = fbw->next)
    if (0 == strcasecmp (fbw->wire_method,
                         wire_method) )
    {
      struct TALER_EXCHANGE_WireAggregateFees *af;

      /* Advance through list up to current time */
      while ( (NULL != (af = fbw->af)) &&
              (now.abs_value_us >= af->end_date.abs_value_us) )
      {
        fbw->af = af->next;
        GNUNET_free (af);
      }
      return af;
    }
  return NULL;
}


/**
 * Check if we have any remaining pending requests for the
 * given @a exchange, and if we have the required data, call
 * the callback.
 *
 * @param exchange the exchange to check for pending find operations
 * @return #GNUNET_YES if we need /wire data from @a exchange
 */
static int
process_find_operations (struct Exchange *exchange)
{
  struct TMH_EXCHANGES_FindOperation *fn;
  struct GNUNET_TIME_Absolute now;
  int need_wire;

  now = GNUNET_TIME_absolute_get ();
  need_wire = GNUNET_NO;
  for (struct TMH_EXCHANGES_FindOperation *fo = exchange->fo_head;
       NULL != fo;
       fo = fn)
  {
    const struct TALER_Amount *wire_fee;

    fn = fo->next;
    if (NULL != fo->wire_method)
    {
      struct TALER_EXCHANGE_WireAggregateFees *af;

      /* Find fee structure for our wire method */
      af = get_wire_fees (exchange,
                          now,
                          fo->wire_method);
      if (NULL == af)
      {
        need_wire = GNUNET_YES;
        continue;
      }
      if (af->start_date.abs_value_us > now.abs_value_us)
      {
        /* Disagreement on the current time */
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Exchange's earliest fee is %s adhead of our time. Clock skew issue?\n",
                    GNUNET_STRINGS_relative_time_to_string (
                      GNUNET_TIME_absolute_get_remaining (af->start_date),
                      GNUNET_YES));
        continue;
      }
      /* found fee, great! */
      wire_fee = &af->wire_fee;
    }
    else
    {
      /* no wire transfer method given, so we yield no fee */
      wire_fee = NULL;
    }
    {
      struct TALER_EXCHANGE_HttpResponse hr = {
        .http_status = MHD_HTTP_OK,
      };

      fo->fc (fo->fc_cls,
              &hr,
              exchange->conn,
              wire_fee,
              exchange->trusted);
    }
    TMH_EXCHANGES_find_exchange_cancel (fo);
  }
  return need_wire;
}


/**
 * Check if we have any remaining pending requests for the
 * given @a exchange, and if we have the required data, call
 * the callback.  If requests without /wire data remain,
 * retry the /wire request after some delay.
 *
 * @param cls a `struct Exchange` to check
 */
static void
wire_task_cb (void *cls);


/**
 * Callbacks of this type are used to serve the result of submitting a
 * wire format inquiry request to a exchange.
 *
 * If the request fails to generate a valid response from the
 * exchange, @a http_status will also be zero.
 *
 * Must only be called if 'exchange->pending' is #GNUNET_NO,
 * that is #TALER_EXCHANGE_get_keys() will succeed.
 *
 * @param cls closure, a `struct Exchange`
 * @param hr HTTP response details
 * @param accounts_len length of the @a accounts array
 * @param accounts list of wire accounts of the exchange, NULL on error
 */
static void
handle_wire_data (void *cls,
                  const struct TALER_EXCHANGE_HttpResponse *hr,
                  unsigned int accounts_len,
                  const struct TALER_EXCHANGE_WireAccount *accounts)
{
  struct Exchange *exchange = cls;
  const struct TALER_EXCHANGE_Keys *keys;
  enum TALER_ErrorCode ecx;

  exchange->wire_request = NULL;
  if (MHD_HTTP_OK != hr->http_status)
  {
    struct TMH_EXCHANGES_FindOperation *fo;

    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to obtain /wire details from `%s': %u/%d\n",
                exchange->url,
                hr->http_status,
                hr->ec);
    while (NULL != (fo = exchange->fo_head))
    {
      fo->fc (fo->fc_cls,
              hr,
              NULL,
              NULL,
              GNUNET_NO);
      TMH_EXCHANGES_find_exchange_cancel (fo);
    }
    return;
  }
  keys = TALER_EXCHANGE_get_keys (exchange->conn);
  GNUNET_assert (NULL != keys);
  ecx = process_wire_accounts (exchange,
                               &keys->master_pub,
                               accounts_len,
                               accounts);
  if (TALER_EC_NONE != ecx)
  {
    /* Report hard failure to all callbacks! */
    struct TMH_EXCHANGES_FindOperation *fo;
    struct TALER_EXCHANGE_HttpResponse hrx = {
      .ec = ecx,
      .http_status = 0,
      .reply = hr->reply
    };

    GNUNET_break_op (0);
    while (NULL != (fo = exchange->fo_head))
    {
      fo->fc (fo->fc_cls,
              &hrx,
              NULL,
              NULL,
              GNUNET_NO);
      TMH_EXCHANGES_find_exchange_cancel (fo);
    }
    return;
  }
  if ( (GNUNET_YES ==
        process_find_operations (exchange)) &&
       (NULL == exchange->wire_task) &&
       (NULL == exchange->wire_request) )
  {
    /* need to run /wire again. But as we DID get a successful reply,
       and as the exchange is unlikely to offer new wire methods very
       frequently, start with some significant delay */
    exchange->wire_retry_delay
      = GNUNET_TIME_relative_max (GNUNET_TIME_UNIT_MINUTES,
                                  exchange->wire_retry_delay);
    exchange->wire_retry_delay = RETRY_BACKOFF (exchange->wire_retry_delay);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Exchange does not support our wire method. Retrying in %s\n",
                GNUNET_STRINGS_relative_time_to_string (
                  exchange->wire_retry_delay,
                  GNUNET_YES));
    exchange->wire_task
      = GNUNET_SCHEDULER_add_delayed (exchange->wire_retry_delay,
                                      &wire_task_cb,
                                      exchange);
  }
}


/**
 * Check if we have any remaining pending requests for the
 * given @a exchange, and if we have the required data, call
 * the callback.  If requests without /wire data remain,
 * retry the /wire request after some delay.
 *
 * Must only be called if 'exchange->pending' is #GNUNET_NO,
 * that is #TALER_EXCHANGE_get_keys() will succeed.
 *
 * @param cls a `struct Exchange` to check
 */
static void
wire_task_cb (void *cls)
{
  struct Exchange *exchange = cls;

  exchange->wire_task = NULL;
  GNUNET_assert (GNUNET_NO == exchange->pending);
  if (GNUNET_YES !=
      process_find_operations (exchange))
    return; /* no more need */
  GNUNET_assert (NULL == exchange->wire_request);
  exchange->wire_request = TALER_EXCHANGE_wire (exchange->conn,
                                                &handle_wire_data,
                                                exchange);
}


/**
 * Function called with information about who is auditing
 * a particular exchange and what key the exchange is using.
 *
 * @param cls closure, will be `struct Exchange` so that
 *   when this function gets called, it will change the flag 'pending'
 *   to 'false'. Note: 'keys' is automatically saved inside the exchange's
 *   handle, which is contained inside 'struct Exchange', when
 *   this callback is called. Thus, once 'pending' turns 'false',
 *   it is safe to call 'TALER_EXCHANGE_get_keys()' on the exchange's handle,
 *   in order to get the "good" keys.
 * @param hr http response details
 * @param keys information about the various keys used
 *        by the exchange
 * @param compat version compatibility data
 */
static void
keys_mgmt_cb (void *cls,
              const struct TALER_EXCHANGE_HttpResponse *hr,
              const struct TALER_EXCHANGE_Keys *keys,
              enum TALER_EXCHANGE_VersionCompatibility compat)
{
  struct Exchange *exchange = cls;
  struct GNUNET_TIME_Absolute expire;
  struct GNUNET_TIME_Relative delay;

  if (NULL == keys)
  {
    struct TMH_EXCHANGES_FindOperation *fo;

    exchange->pending = GNUNET_YES;
    if (NULL != exchange->wire_request)
    {
      TALER_EXCHANGE_wire_cancel (exchange->wire_request);
      exchange->wire_request = NULL;
    }
    if (NULL != exchange->wire_task)
    {
      GNUNET_SCHEDULER_cancel (exchange->wire_task);
      exchange->wire_task = NULL;
    }
    while (NULL != (fo = exchange->fo_head))
    {
      fo->fc (fo->fc_cls,
              hr,
              NULL,
              NULL,
              GNUNET_NO);
      TMH_EXCHANGES_find_exchange_cancel (fo);
    }
    if (TALER_EXCHANGE_VC_INCOMPATIBLE_NEWER == compat)
    {
      /* Log hard error: we likely need admin help! */
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Exchange `%s' runs an incompatible more recent version of the Taler protocol. Will not retry. This client may need to be updated.\n",
                  exchange->url);
      /* Theoretically, the exchange could downgrade,
         but let's not be too aggressive about retries
         on this one. */
      exchange->retry_delay = GNUNET_TIME_relative_max (GNUNET_TIME_UNIT_HOURS,
                                                        exchange->retry_delay);
    }
    exchange->retry_delay = RETRY_BACKOFF (exchange->retry_delay);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to fetch /keys from `%s': %d/%u, retrying in %s\n",
                exchange->url,
                (int) hr->ec,
                hr->http_status,
                GNUNET_STRINGS_relative_time_to_string (exchange->retry_delay,
                                                        GNUNET_YES));
    GNUNET_assert (NULL == exchange->retry_task);
    exchange->first_retry = GNUNET_TIME_relative_to_absolute (
      exchange->retry_delay);
    exchange->retry_task = GNUNET_SCHEDULER_add_delayed (exchange->retry_delay,
                                                         &retry_exchange,
                                                         exchange);
    return;
  }
  if ( (GNUNET_YES == exchange->trusted) &&
       (0 != GNUNET_memcmp (&exchange->master_pub,
                            &keys->master_pub)) )
  {
    /* master pub differs => do not trust the exchange (without auditor) */
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Master public key of exchange `%s' differs from our configuration. Not trusting exchange.\n",
                exchange->url);
    exchange->trusted = GNUNET_NO;
  }
  if (GNUNET_NO == exchange->trusted)
    exchange->master_pub = keys->master_pub;

  if (0 != (TALER_EXCHANGE_VC_NEWER & compat))
  {
    /* Warn user exactly once about need to upgrade */
    static int once;

    if (0 == once)
    {
      once = 1;
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Exchange `%s' runs a more recent version of the Taler protocol. You may want to update this client.\n",
                  exchange->url);
    }
  }
  expire = TALER_EXCHANGE_check_keys_current (exchange->conn,
                                              GNUNET_NO,
                                              GNUNET_NO);
  exchange->first_retry = GNUNET_TIME_relative_to_absolute (RELOAD_DELAY);
  if (0 == expire.abs_value_us)
    delay = RELOAD_DELAY;
  else
    delay = GNUNET_TIME_absolute_get_remaining (expire);
  exchange->retry_delay = GNUNET_TIME_UNIT_ZERO;
  if (NULL != exchange->retry_task)
    GNUNET_SCHEDULER_cancel (exchange->retry_task);
  exchange->retry_task
    = GNUNET_SCHEDULER_add_delayed (delay,
                                    &retry_exchange,
                                    exchange);
  exchange->pending = GNUNET_NO;
  if ( (GNUNET_YES ==
        process_find_operations (exchange)) &&
       (NULL == exchange->wire_request) &&
       (NULL == exchange->wire_task) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Got key data, but also need wire data. Will request /wire now\n");
    exchange->wire_request = TALER_EXCHANGE_wire (exchange->conn,
                                                  &handle_wire_data,
                                                  exchange);
  }
}


/**
 * Task to return find operation result asynchronously to caller.
 *
 * @param cls a `struct TMH_EXCHANGES_FindOperation`
 */
static void
return_result (void *cls)
{
  struct TMH_EXCHANGES_FindOperation *fo = cls;
  struct Exchange *exchange = fo->my_exchange;

  fo->at = NULL;
  if ( (GNUNET_YES ==
        process_find_operations (exchange)) &&
       (NULL == exchange->wire_request) &&
       (GNUNET_NO == exchange->pending) &&
       (NULL != exchange->wire_task) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Do not have current wire data. Will re-request /wire in 1 minute\n");
    exchange->wire_task
      = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_MINUTES,
                                      &wire_task_cb,
                                      exchange);
  }
}


/**
 * Find a exchange that matches @a chosen_exchange. If we cannot connect
 * to the exchange, or if it is not acceptable, @a fc is called with
 * NULL for the exchange.
 *
 * @param chosen_exchange URL of the exchange we would like to talk to
 * @param wire_method the wire method we will use with @a chosen_exchange, NULL for none
 * @param force_reload try to force reloading /keys from the exchange ASAP; note
 *        that IF the forced reload fails, it is possible @a fc won't be called at all
 *        until a /keys download succeeds; only use #GNUNET_YES if a new /keys request
 *        is mandatory. If the force reload request is not allowed due to our rate limiting,
 *        then @a fc will be called immediately with the existing /keys data
 * @param fc function to call with the handles for the exchange
 * @param fc_cls closure for @a fc
 * @return NULL on error
 */
struct TMH_EXCHANGES_FindOperation *
TMH_EXCHANGES_find_exchange (const char *chosen_exchange,
                             const char *wire_method,
                             int force_reload,
                             TMH_EXCHANGES_FindContinuation fc,
                             void *fc_cls)
{
  struct Exchange *exchange;
  struct TMH_EXCHANGES_FindOperation *fo;
  struct GNUNET_TIME_Absolute now;

  if (NULL == merchant_curl_ctx)
  {
    GNUNET_break (0);
    return NULL;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Trying to find chosen exchange `%s'\n",
              chosen_exchange);
  /* Check if the exchange is known */
  for (exchange = exchange_head; NULL != exchange; exchange = exchange->next)
  {
    /* test it by checking URL */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Comparing chosen exchange url '%s' with known url '%s'.\n",
                chosen_exchange,
                exchange->url);
    if (0 == strcmp (exchange->url,
                     chosen_exchange))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "The exchange `%s' is already known (good)\n",
                  chosen_exchange);
      break;
    }
  }
  if (NULL == exchange)
  {
    /* This is a new exchange */
    exchange = GNUNET_new (struct Exchange);
    exchange->url = GNUNET_strdup (chosen_exchange);
    exchange->pending = GNUNET_YES;
    GNUNET_CONTAINER_DLL_insert (exchange_head,
                                 exchange_tail,
                                 exchange);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "The exchange `%s' is new\n",
                chosen_exchange);
  }

  fo = GNUNET_new (struct TMH_EXCHANGES_FindOperation);
  fo->fc = fc;
  fo->fc_cls = fc_cls;
  fo->my_exchange = exchange;
  if (NULL != wire_method)
    fo->wire_method = GNUNET_strdup (wire_method);
  GNUNET_CONTAINER_DLL_insert (exchange->fo_head,
                               exchange->fo_tail,
                               fo);
  now = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&now);
  if ( (force_reload) &&
       (0 == GNUNET_TIME_absolute_get_remaining (
          exchange->first_retry).rel_value_us) )
  {
    /* increment exponential-backoff */
    exchange->retry_delay = RETRY_BACKOFF (exchange->retry_delay);
    /* do not allow forced check until both backoff and #FORCED_RELOAD_DELAY
       are satisified again */
    exchange->first_retry
      = GNUNET_TIME_relative_to_absolute (GNUNET_TIME_relative_max (
                                            exchange->retry_delay,
                                            FORCED_RELOAD_DELAY));
    TALER_EXCHANGE_check_keys_current (exchange->conn,
                                       GNUNET_YES,
                                       GNUNET_NO);
    return fo;
  }


  if ( (GNUNET_YES != exchange->pending) &&
       ( (NULL == fo->wire_method) ||
         (NULL != get_wire_fees (exchange,
                                 now,
                                 fo->wire_method)) ) )
  {
    /* We are not currently waiting for a reply, immediately
       return result */
    GNUNET_assert (NULL == fo->at);
    fo->at = GNUNET_SCHEDULER_add_now (&return_result,
                                       fo);
    return fo;
  }

  /* If new or resumed, (re)try fetching /keys */
  if ( (NULL == exchange->conn) &&
       (NULL == exchange->retry_task) &&
       (GNUNET_YES == exchange->pending) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Do not have current key data. Will request /keys now\n");
    exchange->retry_task = GNUNET_SCHEDULER_add_now (&retry_exchange,
                                                     exchange);
  }
  else if ( (GNUNET_NO == exchange->pending) &&
            (NULL == exchange->wire_task) &&
            (NULL == exchange->wire_request) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Do not have required wire data. Will re-request /wire now\n");
    exchange->wire_task = GNUNET_SCHEDULER_add_now (&wire_task_cb,
                                                    exchange);
  }
  return fo;
}


/**
 * Abort pending find operation.
 *
 * @param fo handle to operation to abort
 */
void
TMH_EXCHANGES_find_exchange_cancel (struct TMH_EXCHANGES_FindOperation *fo)
{
  struct Exchange *exchange = fo->my_exchange;

  if (NULL != fo->at)
  {
    GNUNET_SCHEDULER_cancel (fo->at);
    fo->at = NULL;
  }
  GNUNET_CONTAINER_DLL_remove (exchange->fo_head,
                               exchange->fo_tail,
                               fo);
  GNUNET_free_non_null (fo->wire_method);
  GNUNET_free (fo);
}


/**
 * Function called on each configuration section. Finds sections
 * about exchanges, parses the entries and tries to connect to
 * it in order to fetch /keys.
 *
 * @param cls closure, with a `const struct GNUNET_CONFIGURATION_Handle *`
 * @param section name of the section
 */
static void
accept_exchanges (void *cls,
                  const char *section)
{
  const struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  char *url;
  char *mks;
  struct Exchange *exchange;
  char *currency;

  if (0 != strncasecmp (section,
                        "merchant-exchange-",
                        strlen ("merchant-exchange-")))
    return;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "CURRENCY",
                                             &currency))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "CURRENCY");
    return;
  }
  if (0 != strcasecmp (currency,
                       TMH_currency))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Exchange given in section `%s' is for another currency. Skipping.\n",
                section);
    GNUNET_free (currency);
    return;
  }
  GNUNET_free (currency);
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "EXCHANGE_BASE_URL",
                                             &url))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "EXCHANGE_BASE_URL");
    return;
  }
  exchange = GNUNET_new (struct Exchange);
  exchange->url = url;
  if (GNUNET_OK ==
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "MASTER_KEY",
                                             &mks))
  {
    if (GNUNET_OK ==
        GNUNET_CRYPTO_eddsa_public_key_from_string (mks,
                                                    strlen (mks),
                                                    &exchange->master_pub.
                                                    eddsa_pub))
    {
      exchange->trusted = GNUNET_YES;
    }
    else
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "MASTER_KEY",
                                 _ ("ill-formed EdDSA key"));
    }
    GNUNET_free (mks);
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "MASTER_KEY missing in section '%s', not trusting exchange\n",
                section);

  }
  GNUNET_CONTAINER_DLL_insert (exchange_head,
                               exchange_tail,
                               exchange);
  exchange->pending = GNUNET_YES;
  GNUNET_assert (NULL == exchange->retry_task);
  exchange->retry_task = GNUNET_SCHEDULER_add_now (&retry_exchange,
                                                   exchange);
}


/**
 * Parses "trusted" exchanges listed in the configuration.
 *
 * @param cfg the configuration
 * @return #GNUNET_OK on success; #GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TMH_EXCHANGES_init (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  merchant_curl_ctx
    = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                        &merchant_curl_rc);
  if (NULL == merchant_curl_ctx)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  GNUNET_CURL_enable_async_scope_header (merchant_curl_ctx,
                                         "Taler-Correlation-Id");
  merchant_curl_rc = GNUNET_CURL_gnunet_rc_create (merchant_curl_ctx);
  /* get exchanges from the merchant configuration and try to connect to them */
  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &accept_exchanges,
                                         (void *) cfg);
  /* build JSON with list of trusted exchanges (will be included in contracts) */
  TMH_trusted_exchanges = json_array ();
  for (struct Exchange *exchange = exchange_head;
       NULL != exchange;
       exchange = exchange->next)
  {
    json_t *j_exchange;

    if (GNUNET_YES != exchange->trusted)
      continue;
    j_exchange = json_pack ("{s:s, s:o}",
                            "url", exchange->url,
                            "master_pub", GNUNET_JSON_from_data_auto (
                              &exchange->master_pub));
    GNUNET_assert (0 ==
                   json_array_append_new (TMH_trusted_exchanges,
                                          j_exchange));
  }
  return GNUNET_OK;
}


/**
 * Function called to shutdown the exchanges subsystem.
 */
void
TMH_EXCHANGES_done ()
{
  struct Exchange *exchange;

  while (NULL != (exchange = exchange_head))
  {
    struct FeesByWireMethod *f;

    GNUNET_CONTAINER_DLL_remove (exchange_head,
                                 exchange_tail,
                                 exchange);
    while (NULL != (f = exchange->wire_fees_head))
    {
      struct TALER_EXCHANGE_WireAggregateFees *af;

      GNUNET_CONTAINER_DLL_remove (exchange->wire_fees_head,
                                   exchange->wire_fees_tail,
                                   f);
      while (NULL != (af = f->af))
      {
        f->af = af->next;
        GNUNET_free (af);
      }
      GNUNET_free (f->wire_method);
      GNUNET_free (f);
    }
    if (NULL != exchange->wire_request)
    {
      TALER_EXCHANGE_wire_cancel (exchange->wire_request);
      exchange->wire_request = NULL;
    }
    if (NULL != exchange->wire_task)
    {
      GNUNET_SCHEDULER_cancel (exchange->wire_task);
      exchange->wire_task = NULL;
    }
    if (NULL != exchange->conn)
    {
      TALER_EXCHANGE_disconnect (exchange->conn);
      exchange->conn = NULL;
    }
    if (NULL != exchange->retry_task)
    {
      GNUNET_SCHEDULER_cancel (exchange->retry_task);
      exchange->retry_task = NULL;
    }
    GNUNET_assert (NULL == exchange->fo_head);
    GNUNET_assert (NULL == exchange->fo_tail);
    GNUNET_free (exchange->url);
    GNUNET_free (exchange);
  }
  GNUNET_CURL_fini (merchant_curl_ctx);
  merchant_curl_ctx = NULL;
  GNUNET_CURL_gnunet_rc_destroy (merchant_curl_rc);
  merchant_curl_rc = NULL;
  json_decref (TMH_trusted_exchanges);
  TMH_trusted_exchanges = NULL;
}


/* end of taler-merchant-httpd_exchanges.c */
