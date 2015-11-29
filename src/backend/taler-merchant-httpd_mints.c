/*
  This file is part of TALER
  (C) 2014, 2015 Christian Grothoff (and other contributing authors)

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
 * @file backend/taler-merchant-httpd_mints.c
 * @brief logic this HTTPD keeps for each mint we interact with
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include "taler-merchant-httpd_mints.h"


/**
 * How often do we retry fetching /keys?
 */
#define KEYS_RETRY_FREQ GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_MINUTES, 60)


/**
 * Mint
 */
struct Mint;


/**
 * Information we keep for a pending #MMH_MINTS_find_mint() operation.
 */
struct TMH_MINTS_FindOperation
{

  /**
   * Kept in a DLL.
   */
  struct TMH_MINTS_FindOperation *next;

  /**
   * Kept in a DLL.
   */
  struct TMH_MINTS_FindOperation *prev;

  /**
   * Function to call with the result.
   */
  TMH_MINTS_FindContinuation fc;

  /**
   * Closure for @e fc.
   */
  void *fc_cls;

  /**
   * Mint we wait for the /keys for.
   */
  struct Mint *my_mint;

  /**
   * Task scheduled to asynchrnously return the result.
   */
  struct GNUNET_SCHEDULER_Task *at;

};


/**
 * Mint
 */
struct Mint
{

  /**
   * Kept in a DLL.
   */
  struct Mint *next;

  /**
   * Kept in a DLL.
   */
  struct Mint *prev;

  /**
   * Head of FOs pending for this mint.
   */
  struct TMH_MINTS_FindOperation *fo_head;

  /**
   * Tail of FOs pending for this mint.
   */
  struct TMH_MINTS_FindOperation *fo_tail;

  /**
   * (base) URI of the mint.
   */
  char *uri;

  /**
   * A connection to this mint
   */
  struct TALER_MINT_Handle *conn;

  /**
   * Master public key, guaranteed to be set ONLY for
   * trusted mints.
   */
  struct TALER_MasterPublicKeyP master_pub;

  /**
   * At what time should we try to fetch /keys again?
   */
  struct GNUNET_TIME_Absolute retry_time;

  /**
   * Flag which indicates whether some HTTP transfer between
   * this merchant and the mint is still ongoing
   */
  int pending;

  /**
   * #GNUNET_YES if this mint is from our configuration and
   * explicitly trusted, #GNUNET_NO if we need to check each
   * key to be sure it is trusted.
   */
  int trusted;

};


/**
 * Context for all mint operations (useful to the event loop)
 */
static struct TALER_MINT_Context *ctx;

/**
 * Task we use to drive the interaction with this mint.
 */
static struct GNUNET_SCHEDULER_Task *poller_task;

/**
 * Head of mints we know about.
 */
static struct Mint *mint_head;

/**
 * Tail of mints we know about.
 */
static struct Mint *mint_tail;

/**
 * List of our trusted mints for inclusion in contracts.
 */
json_t *trusted_mints;


/**
 * Function called with information about who is auditing
 * a particular mint and what key the mint is using.
 *
 * @param cls closure, will be `struct Mint` so that
 *   when this function gets called, it will change the flag 'pending'
 *   to 'false'. Note: 'keys' is automatically saved inside the mint's
 *   handle, which is contained inside 'struct Mint', when
 *   this callback is called. Thus, once 'pending' turns 'false',
 *   it is safe to call 'TALER_MINT_get_keys()' on the mint's handle,
 *   in order to get the "good" keys.
 * @param keys information about the various keys used
 *        by the mint
 */
static void
keys_mgmt_cb (void *cls,
              const struct TALER_MINT_Keys *keys)
{
  struct Mint *mint = cls;
  struct TMH_MINTS_FindOperation *fo;

  if (NULL != keys)
  {
    mint->pending = GNUNET_NO;
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Failed to fetch /keys from `%s'\n",
                mint->uri);
    TALER_MINT_disconnect (mint->conn);
    mint->conn = NULL;
    mint->pending = GNUNET_SYSERR; /* failed hard */
    mint->retry_time = GNUNET_TIME_relative_to_absolute (KEYS_RETRY_FREQ);
  }
  while (NULL != (fo = mint->fo_head))
  {
    GNUNET_CONTAINER_DLL_remove (mint->fo_head,
                                 mint->fo_tail,
                                 fo);
    fo->fc (fo->fc_cls,
            (NULL != keys) ? mint->conn : NULL);
    GNUNET_free (fo);
  }
}


/**
 * Task that runs the mint's event loop using the GNUnet scheduler.
 *
 * @param cls a `struct Mint *`
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

  poller_task = NULL;
  TALER_MINT_perform (ctx);
  max_fd = -1;
  timeout = -1;
  FD_ZERO (&read_fd_set);
  FD_ZERO (&write_fd_set);
  FD_ZERO (&except_fd_set);
  TALER_MINT_get_select_info (ctx,
                              &read_fd_set,
                              &write_fd_set,
                              &except_fd_set,
                              &max_fd,
                              &timeout);
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
 * @param cls a `struct TMH_MINTS_FindOperation`
 * @param tc unused
 */
static void
return_result (void *cls,
               const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct TMH_MINTS_FindOperation *fo = cls;
  struct Mint *mint = fo->my_mint;

  fo->at = NULL;
  GNUNET_CONTAINER_DLL_remove (mint->fo_head,
                               mint->fo_tail,
                               fo);
  fo->fc (fo->fc_cls,
          (GNUNET_SYSERR == mint->pending) ? NULL : mint->conn);
  GNUNET_free (fo);
  GNUNET_SCHEDULER_cancel (poller_task);
  GNUNET_SCHEDULER_add_now (&context_task,
                            NULL);
}


/**
 * Find a mint that matches @a chosen_mint. If we cannot connect
 * to the mint, or if it is not acceptable, @a fc is called with
 * NULL for the mint.
 *
 * @param chosen_mint URI of the mint we would like to talk to
 * @param fc function to call with the handles for the mint
 * @param fc_cls closure for @a fc
 * @return NULL on error
 */
struct TMH_MINTS_FindOperation *
TMH_MINTS_find_mint (const char *chosen_mint,
                     TMH_MINTS_FindContinuation fc,
                     void *fc_cls)
{
  struct Mint *mint;
  struct TMH_MINTS_FindOperation *fo;

  if (NULL == ctx)
  {
    GNUNET_break (0);
    return NULL;
  }
  /* Check if the mint is known */
  for (mint = mint_head; NULL != mint; mint = mint->next)
    /* test it by checking public key --- FIXME: hostname or public key!?
       Should probably be URI, not hostname anyway! */
    if (0 == strcmp (mint->uri,
                     chosen_mint))
      break;
  if (NULL == mint)
  {
    /* This is a new mint */
    mint = GNUNET_new (struct Mint);
    mint->uri = GNUNET_strdup (chosen_mint);
    mint->pending = GNUNET_YES;
    GNUNET_CONTAINER_DLL_insert (mint_head,
                                 mint_tail,
                                 mint);
  }

  /* check if we should resume this mint */
  if ( (GNUNET_SYSERR == mint->pending) &&
       (0 == GNUNET_TIME_absolute_get_remaining (mint->retry_time).rel_value_us) )
    mint->pending = GNUNET_YES;


  fo = GNUNET_new (struct TMH_MINTS_FindOperation);
  fo->fc = fc;
  fo->fc_cls = fc_cls;
  fo->my_mint = mint;
  GNUNET_CONTAINER_DLL_insert (mint->fo_head,
                               mint->fo_tail,
                               fo);

  if (GNUNET_NO == mint->pending)
  {
    /* We are not currently waiting for a reply, immediately
       return result */
    fo->at = GNUNET_SCHEDULER_add_now (&return_result,
                                       fo);
    return fo;
  }

  /* If new or resumed, retry fetching /keys */
  if ( (NULL == mint->conn) &&
       (GNUNET_YES == mint->pending) )
  {
    mint->conn = TALER_MINT_connect (ctx,
                                     mint->uri,
                                     &keys_mgmt_cb,
                                     mint,
                                     TALER_MINT_OPTION_END);
    GNUNET_break (NULL != mint->conn);
  }
  return fo;
}


/**
 * Abort pending find operation.
 *
 * @param fo handle to operation to abort
 */
void
TMH_MINTS_find_mint_cancel (struct TMH_MINTS_FindOperation *fo)
{
  struct Mint *mint = fo->my_mint;

  if (NULL != fo->at)
  {
    GNUNET_SCHEDULER_cancel (fo->at);
    fo->at = NULL;
  }
  GNUNET_CONTAINER_DLL_remove (mint->fo_head,
                               mint->fo_tail,
                               fo);
  GNUNET_free (fo);
}


/**
 * Function called on each configuration section. Finds sections
 * about mints and parses the entries.
 *
 * @param cls closure, with a `const struct GNUNET_CONFIGURATION_Handle *`
 * @param section name of the section
 */
static void
parse_mints (void *cls,
             const char *section)
{
  const struct GNUNET_CONFIGURATION_Handle *cfg = cfg;
  char *uri;
  char *mks;
  struct Mint *mint;

  if (0 != strncasecmp (section,
                        "mint-",
                        strlen ("mint-")))
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
  mint = GNUNET_new (struct Mint);
  mint->uri = uri;
  if (GNUNET_OK ==
      GNUNET_CONFIGURATION_get_value_string (cfg,
                                             section,
                                             "MASTER_KEY",
                                             &mks))
  {
    if (GNUNET_OK ==
        GNUNET_CRYPTO_eddsa_public_key_from_string (mks,
                                                    strlen (mks),
                                                    &mint->master_pub.eddsa_pub))
    {
      mint->trusted = GNUNET_YES;
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
  GNUNET_CONTAINER_DLL_insert (mint_head,
                               mint_tail,
                               mint);
  mint->pending = GNUNET_YES;
  mint->conn = TALER_MINT_connect (ctx,
                                   mint->uri,
                                   &keys_mgmt_cb,
                                   mint,
                                   TALER_MINT_OPTION_END);
  GNUNET_break (NULL != mint->conn);
}


/**
 * Parses "trusted" mints listed in the configuration.
 *
 * @param cfg the configuration
 * @return #GNUNET_OK on success; #GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TMH_MINTS_init (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  struct Mint *mint;
  json_t *j_mint;

  ctx = TALER_MINT_init ();
  if (NULL == ctx)
    return GNUNET_SYSERR;
  GNUNET_CONFIGURATION_iterate_sections (cfg,
                                         &parse_mints,
                                         (void *) cfg);
  /* build JSON with list of trusted mints */
  trusted_mints = json_array ();
  for (mint = mint_head; NULL != mint; mint = mint->next)
  {
    if (GNUNET_YES != mint->trusted)
      continue;
    j_mint = json_pack ("{s:s, s:o}",
                        "url", mint->uri,
                        "master_pub", TALER_json_from_data (&mint->master_pub,
                                                            sizeof (struct TALER_MasterPublicKeyP)));
    json_array_append_new (trusted_mints,
                           j_mint);
  }
  poller_task = GNUNET_SCHEDULER_add_now (&context_task,
                                          NULL);
  return GNUNET_OK;
}


/**
 * Function called to shutdown the mints subsystem.
 */
void
TMH_MINTS_done ()
{
  struct Mint *mint;

  while (NULL != (mint = mint_head))
  {
    GNUNET_CONTAINER_DLL_remove (mint_head,
                                 mint_tail,
                                 mint);
    if (NULL != mint->conn)
      TALER_MINT_disconnect (mint->conn);
    GNUNET_free (mint->uri);
    GNUNET_free (mint);
  }
  if (NULL != poller_task)
  {
    GNUNET_SCHEDULER_cancel (poller_task);
    poller_task = NULL;
  }
  TALER_MINT_fini (ctx);
}
