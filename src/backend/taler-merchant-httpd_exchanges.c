/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
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
 * Threshold after which exponential backoff should not increase.
 */
#define RETRY_BACKOFF_THRESHOLD GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 60)


/**
 * Perform our exponential back-off calculation, starting at 1 ms
 * and then going by a factor of 2 up unto a maximum of RETRY_BACKOFF_THRESHOLD.
 *
 * @param r current backoff time, initially zero
 */
#define RETRY_BACKOFF(r) GNUNET_TIME_relative_min (RETRY_BACKOFF_THRESHOLD, \
   GNUNET_TIME_relative_multiply (GNUNET_TIME_relative_max (GNUNET_TIME_UNIT_MILLISECONDS, (r)), 2));


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
   * Task scheduled to asynchronously return the result to
   * the find continuation.
   */
  struct GNUNET_SCHEDULER_Task *at;

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
   * (base) URI of the exchange.
   */
  char *uri;

  /**
   * A connection to this exchange
   */
  struct TALER_EXCHANGE_Handle *conn;

  /**
   * Master public key, guaranteed to be set ONLY for
   * trusted exchanges.
   */
  struct TALER_MasterPublicKeyP master_pub;

  /**
   * How long should we wait between the next retry?
   */
  struct GNUNET_TIME_Relative retry_delay;

  /**
   * Task where we retry fetching /keys from the exchange.
   */
  struct GNUNET_SCHEDULER_Task *retry_task;

  /**
   * GNUNET_YES to indicate that there is an ongoing
   * transfer we're waiting for,
   * GNUNET_NO to indicate that key data is up-to-date.
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
json_t *trusted_exchanges;


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
 * @param keys information about the various keys used
 *        by the exchange
 */
static void
keys_mgmt_cb (void *cls,
              const struct TALER_EXCHANGE_Keys *keys);


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

  exchange->retry_task = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Connecting to exchange exchange %s in retry_exchange\n",
              exchange->uri);

  exchange->conn = TALER_EXCHANGE_connect (merchant_curl_ctx,
                                           exchange->uri,
                                           &keys_mgmt_cb,
                                           exchange,
                                           TALER_EXCHANGE_OPTION_END);
  GNUNET_break (NULL != exchange->conn);
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
 * @param keys information about the various keys used
 *        by the exchange
 */
static void
keys_mgmt_cb (void *cls,
              const struct TALER_EXCHANGE_Keys *keys)
{
  struct Exchange *exchange = cls;
  struct TMH_EXCHANGES_FindOperation *fo;

  GNUNET_assert (GNUNET_YES == exchange->pending);

  if (NULL == keys)
  {
    exchange->retry_delay = RETRY_BACKOFF (exchange->retry_delay);
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to fetch /keys from `%s', retrying in %s\n",
                exchange->uri,
                GNUNET_STRINGS_relative_time_to_string (exchange->retry_delay, GNUNET_YES));
    TALER_EXCHANGE_disconnect (exchange->conn);
    exchange->conn = NULL;
    exchange->retry_task = GNUNET_SCHEDULER_add_delayed (exchange->retry_delay,
                                                         &retry_exchange,
                                                         exchange);
    return;
  }

  exchange->pending = GNUNET_NO;
  while (NULL != (fo = exchange->fo_head))
  {
    GNUNET_CONTAINER_DLL_remove (exchange->fo_head,
                                 exchange->fo_tail,
                                 fo);
    fo->fc (fo->fc_cls,
            exchange->conn,
            exchange->trusted);
    GNUNET_free (fo);
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
  GNUNET_CONTAINER_DLL_remove (exchange->fo_head,
                               exchange->fo_tail,
                               fo);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Returning result for exchange %s, trusted=%d\n",
              exchange->uri, exchange->trusted);
  fo->fc (fo->fc_cls,
          (GNUNET_SYSERR == exchange->pending) ? NULL : exchange->conn,
          exchange->trusted);
  GNUNET_free (fo);
}


/**
 * Find a exchange that matches @a chosen_exchange. If we cannot connect
 * to the exchange, or if it is not acceptable, @a fc is called with
 * NULL for the exchange.
 *
 * @param chosen_exchange URI of the exchange we would like to talk to
 * @param fc function to call with the handles for the exchange
 * @param fc_cls closure for @a fc
 * @return NULL on error
 */
struct TMH_EXCHANGES_FindOperation *
TMH_EXCHANGES_find_exchange (const char *chosen_exchange,
			     TMH_EXCHANGES_FindContinuation fc,
			     void *fc_cls)
{
  struct Exchange *exchange;
  struct TMH_EXCHANGES_FindOperation *fo;

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
    /* test it by checking public key --- FIXME: hostname or public key!?
       Should probably be URI, not hostname anyway! */
  {
    if (0 == strcmp (exchange->uri,
                     chosen_exchange))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "The exchange `%s' is already known\n",
                  chosen_exchange);
      break;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Comparing chosen exchange url '%s' with known url '%s'.\n",
                chosen_exchange, exchange->uri);
  }
  if (NULL == exchange)
  {
    /* This is a new exchange */
    exchange = GNUNET_new (struct Exchange);
    exchange->uri = GNUNET_strdup (chosen_exchange);
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
  GNUNET_CONTAINER_DLL_insert (exchange->fo_head,
                               exchange->fo_tail,
                               fo);

  if (GNUNET_YES != exchange->pending) // can post coins
  {
    /* We are not currently waiting for a reply, immediately
       return result */
    fo->at = GNUNET_SCHEDULER_add_now (&return_result,
                                       fo);
    return fo;
  }

  /* If new or resumed, retry fetching /keys */
  if ( (NULL == exchange->conn) &&
       (GNUNET_YES == exchange->pending) )
  {
    exchange->retry_task = GNUNET_SCHEDULER_add_now (&retry_exchange,
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
  char *uri;
  char *mks;
  struct Exchange *exchange;

  if (0 != strncasecmp (section,
                        "merchant-exchange-",
                        strlen ("merchant-exchange-")))
    return;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "URI",
                                             &uri))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               section,
                               "URI");
    return;
  }
  exchange = GNUNET_new (struct Exchange);
  exchange->uri = uri;
  if (GNUNET_OK ==
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "MASTER_KEY",
                                             &mks))
  {
    if (GNUNET_OK ==
        GNUNET_CRYPTO_eddsa_public_key_from_string (mks,
                                                    strlen (mks),
                                                    &exchange->master_pub.eddsa_pub))
    {
      exchange->trusted = GNUNET_YES;
    }
    else
    {
      GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                                 section,
                                 "MASTER_KEY",
                                 _("ill-formed key"));
    }
    GNUNET_free (mks);
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "MASTER_KEY not given in section '%s', not trusting exchange\n",
                section);

  }
  GNUNET_CONTAINER_DLL_insert (exchange_head,
                               exchange_tail,
                               exchange);
  exchange->pending = GNUNET_YES;
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
  struct Exchange *exchange;
  json_t *j_exchange;

  merchant_curl_ctx = GNUNET_CURL_init (&GNUNET_CURL_gnunet_scheduler_reschedule,
                                        &merchant_curl_rc);
  if (NULL == merchant_curl_ctx)
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  merchant_curl_rc = GNUNET_CURL_gnunet_rc_create (merchant_curl_ctx);
  /* get exchanges from the merchant configuration and try to connect to them */
  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &accept_exchanges,
                                         (void *) cfg);
  /* build JSON with list of trusted exchanges (will be included in contracts) */
  trusted_exchanges = json_array ();
  for (exchange = exchange_head; NULL != exchange; exchange = exchange->next)
  {
    if (GNUNET_YES != exchange->trusted)
      continue;
    j_exchange = json_pack ("{s:s, s:o}",
                            "url", exchange->uri,
                            "master_pub", GNUNET_JSON_from_data_auto (&exchange->master_pub));
    json_array_append_new (trusted_exchanges,
                           j_exchange);
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
    GNUNET_CONTAINER_DLL_remove (exchange_head,
                                 exchange_tail,
                                 exchange);
    if (NULL != exchange->conn)
      TALER_EXCHANGE_disconnect (exchange->conn);
    if (NULL != exchange->retry_task)
      GNUNET_SCHEDULER_cancel (exchange->retry_task);
    GNUNET_free (exchange->uri);
    GNUNET_free (exchange);
  }
  GNUNET_CURL_fini (merchant_curl_ctx);
  merchant_curl_ctx = NULL;
  GNUNET_CURL_gnunet_rc_destroy (merchant_curl_rc);
  merchant_curl_rc = NULL;
}

/* end of taler-merchant-httpd_exchanges.c */
