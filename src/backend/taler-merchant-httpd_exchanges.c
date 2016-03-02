/*
  This file is part of TALER
  (C) 2014, 2015 GNUnet e.V.

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
 * @file backend/taler-merchant-httpd_exchanges.c
 * @brief logic this HTTPD keeps for each exchange we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_exchanges.h"


/**
 * How often do we retry fetching /keys?
 */
#define KEYS_RETRY_FREQ GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MINUTES, 60)


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
   * Task scheduled to asynchrnously return the result.
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
   * At what time should we try to fetch /keys again?
   */
  struct GNUNET_TIME_Absolute retry_time;

  /**
   * Flag which indicates whether some HTTP transfer between
   * this merchant and the exchange is still ongoing
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
static struct TALER_EXCHANGE_Context *ctx;

/**
 * Task we use to drive the interaction with this exchange.
 */
static struct GNUNET_SCHEDULER_Task *poller_task;

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
              const struct TALER_EXCHANGE_Keys *keys)
{
  struct Exchange *exchange = cls;
  struct TMH_EXCHANGES_FindOperation *fo;

  if (NULL != keys)
  {
    exchange->pending = GNUNET_NO;
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to fetch /keys from `%s'\n",
                exchange->uri);
    TALER_EXCHANGE_disconnect (exchange->conn);
    exchange->conn = NULL;
    exchange->pending = GNUNET_SYSERR; /* failed hard */
    exchange->retry_time = GNUNET_TIME_relative_to_absolute (KEYS_RETRY_FREQ);
  }
  while (NULL != (fo = exchange->fo_head))
  {
    GNUNET_CONTAINER_DLL_remove (exchange->fo_head,
                                 exchange->fo_tail,
                                 fo);
    fo->fc (fo->fc_cls,
            (NULL != keys) ? exchange->conn : NULL,
            exchange->trusted);
    GNUNET_free (fo);
  }
}


/**
 * Task that runs the exchange's event loop using the GNUnet scheduler.
 *
 * @param cls a `struct Exchange *`
 * @param tc scheduler context (unused)
 */
static void
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

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "In exchange context polling task\n");

  poller_task = NULL;
  TALER_EXCHANGE_perform (ctx);
  max_fd = -1;
  timeout = -1;
  FD_ZERO (&read_fd_set);
  FD_ZERO (&write_fd_set);
  FD_ZERO (&except_fd_set);
  TALER_EXCHANGE_get_select_info (ctx,
                              &read_fd_set,
                              &write_fd_set,
                              &except_fd_set,
                              &max_fd,
                              &timeout);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "In exchange context polling task, max_fd=%d, timeout=%ld\n",
              max_fd, timeout);
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
                                 NULL);
  GNUNET_NETWORK_fdset_destroy (rs);
  GNUNET_NETWORK_fdset_destroy (ws);
}


/**
 * Task to return find operation result asynchronously to caller.
 *
 * @param cls a `struct TMH_EXCHANGES_FindOperation`
 * @param tc unused
 */
static void
return_result (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct TMH_EXCHANGES_FindOperation *fo = cls;
  struct Exchange *exchange = fo->my_exchange;

  fo->at = NULL;
  GNUNET_CONTAINER_DLL_remove (exchange->fo_head,
                               exchange->fo_tail,
                               fo);
  fo->fc (fo->fc_cls,
          (GNUNET_SYSERR == exchange->pending) ? NULL : exchange->conn,
          exchange->trusted);
  GNUNET_free (fo);
  GNUNET_SCHEDULER_cancel (poller_task);
  GNUNET_SCHEDULER_add_now (&context_task,
                            NULL);
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

  if (NULL == ctx)
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
    if (0 == strcmp (exchange->uri,
                     chosen_exchange))
      break;
  if (NULL == exchange)
  {
    /* This is a new exchange */
    exchange = GNUNET_new (struct Exchange);
    exchange->uri = GNUNET_strdup (chosen_exchange);
    exchange->pending = GNUNET_YES;
    GNUNET_CONTAINER_DLL_insert (exchange_head,
                                 exchange_tail,
                                 exchange);
  }

  /* check if we should resume this exchange */
  if ( (GNUNET_SYSERR == exchange->pending) &&
       (0 == GNUNET_TIME_absolute_get_remaining (exchange->retry_time).rel_value_us) )
    exchange->pending = GNUNET_YES;


  fo = GNUNET_new (struct TMH_EXCHANGES_FindOperation);
  fo->fc = fc;
  fo->fc_cls = fc_cls;
  fo->my_exchange = exchange;
  GNUNET_CONTAINER_DLL_insert (exchange->fo_head,
                               exchange->fo_tail,
                               fo);

  if (GNUNET_NO == exchange->pending)
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
    exchange->conn = TALER_EXCHANGE_connect (ctx,
                                     exchange->uri,
                                     &keys_mgmt_cb,
                                     exchange,
                                     TALER_EXCHANGE_OPTION_END);
    GNUNET_break (NULL != exchange->conn);
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
 * about exchanges and parses the entries.
 *
 * @param cls closure, with a `const struct GNUNET_CONFIGURATION_Handle *`
 * @param section name of the section
 */
static void
parse_exchanges (void *cls,
             const char *section)
{
  const struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  char *uri;
  char *mks;
  struct Exchange *exchange;

  if (0 != strncasecmp (section,
                        "exchange-",
                        strlen ("exchange-")))
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
  GNUNET_CONTAINER_DLL_insert (exchange_head,
                               exchange_tail,
                               exchange);
  exchange->pending = GNUNET_YES;
  exchange->conn = TALER_EXCHANGE_connect (ctx,
                                   exchange->uri,
                                   &keys_mgmt_cb,
                                   exchange,
                                   TALER_EXCHANGE_OPTION_END);
  GNUNET_break (NULL != exchange->conn);
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

  ctx = TALER_EXCHANGE_init ();
  if (NULL == ctx)
    return GNUNET_SYSERR;
  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &parse_exchanges,
                                         (void *) cfg);
  /* build JSON with list of trusted exchanges */
  trusted_exchanges = json_array ();
  for (exchange = exchange_head; NULL != exchange; exchange = exchange->next)
  {
    if (GNUNET_YES != exchange->trusted)
      continue;
    j_exchange = json_pack ("{s:s, s:o}",
                        "url", exchange->uri,
                        "master_pub", TALER_json_from_data (&exchange->master_pub,
                                                            sizeof (struct TALER_MasterPublicKeyP)));
    json_array_append_new (trusted_exchanges,
                           j_exchange);
  }
  poller_task = GNUNET_SCHEDULER_add_now (&context_task,
                                          NULL);
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
    GNUNET_free (exchange->uri);
    GNUNET_free (exchange);
  }
  if (NULL != poller_task)
  {
    GNUNET_SCHEDULER_cancel (poller_task);
    poller_task = NULL;
  }
  TALER_EXCHANGE_fini (ctx);
}