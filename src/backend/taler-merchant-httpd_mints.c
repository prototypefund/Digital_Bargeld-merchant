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
 * Mint
 */
struct Mint
{
  /**
   * Hostname
   */
  char *hostname;

  /**
   * A connection to this mint
   */
  struct TALER_MINT_Handle *conn;

  /**
   * This mint's context (useful to the event loop)
   */
  struct TALER_MINT_Context *ctx;

  /**
   * Task we use to drive the interaction with this mint.
   */
  struct GNUNET_SCHEDULER_Task *poller_task;

  /**
   * Flag which indicates whether some HTTP transfer between
   * this merchant and the mint is still ongoing
   */
  int pending;

};


/**
 * Trusted mints (FIXME: they are NOT all trusted!).
 */
static struct Mint **mints;

/**
 * Length of the #mints array.
 */
static unsigned int nmints;

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
  /* HOT UPDATE: the merchants need the denomination keys!
    Because it wants to (firstly) verify the deposit confirmation
    sent by the mint, and the signed blob depends (among the
    other things) on the coin's deposit fee. That information
    is never communicated by the wallet to the merchant.
    Again, the merchant needs it because it wants to verify that
    the wallet didn't exceede the limit imposed by the merchant
    on the total deposit fee for a purchase */

  if (NULL != keys)
  {
    ((struct Mint *) cls)->pending = 0;
  }
  else
    printf ("no keys gotten\n");
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
  struct Mint *mint = cls;
  long timeout;
  int max_fd;
  fd_set read_fd_set;
  fd_set write_fd_set;
  fd_set except_fd_set;
  struct GNUNET_NETWORK_FDSet *rs;
  struct GNUNET_NETWORK_FDSet *ws;
  struct GNUNET_TIME_Relative delay;

  mint->poller_task = NULL;
  TALER_MINT_perform (mint->ctx);
  max_fd = -1;
  timeout = -1;
  FD_ZERO (&read_fd_set);
  FD_ZERO (&write_fd_set);
  FD_ZERO (&except_fd_set);
  TALER_MINT_get_select_info (mint->ctx,
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
  mint->poller_task =
    GNUNET_SCHEDULER_add_select (GNUNET_SCHEDULER_PRIORITY_DEFAULT,
                                 delay,
                                 rs,
                                 ws,
                                 &context_task,
                                 mint);
  GNUNET_NETWORK_fdset_destroy (rs);
  GNUNET_NETWORK_fdset_destroy (ws);
}


/**
 * Find a mint that matches @a chosen_mint. If we cannot connect
 * to the mint, or if it is not acceptable, @a fc is called with
 * NULL for the mint.
 *
 * @param chosen_mint URI of the mint we would like to talk to
 * @param fc function to call with the handles for the mint
 * @param fc_cls closure for @a fc
 */
void
TMH_MINTS_find_mint (const char *chosen_mint,
                     TMH_MINTS_FindContinuation fc,
                     void *fc_cls)
{
  unsigned int mint_index;

  for (mint_index = 0; mint_index <= nmints; mint_index++)
  {
    /* no mint found in array */
    if (mint_index == nmints)
    {
      mint_index = -1;
      break;
    }

    /* test it by checking public key --- FIXME: hostname or public key!?
       Should probably be URI, not hostname anyway! */
    if (0 == strcmp (mints[mint_index]->hostname,
                     chosen_mint))
      break;

  }
  /* FIXME: if the mint is not found, we should download /keys
     and check if the given mint is audited by an acceptable auditor...
     #4075! */
  if (-1 == mint_index)
    fc (fc_cls, NULL);
  fc (fc_cls,
      mints[mint_index]->conn);
  GNUNET_SCHEDULER_cancel (mints[mint_index]->poller_task);
  GNUNET_SCHEDULER_add_now (&context_task,
                            mints[mint_index]->ctx);

}


/**
 * Parses "trusted" mints listed in the configuration.
 *
 * @param cfg the configuration
 * @return the number of mints found; #GNUNET_SYSERR upon error in
 *          parsing.
 */
int
TMH_MINTS_init (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  char *mints_str;
  char *token_nf;               /* do no free (nf) */
  char *mint_section;
  char *mint_hostname;
  struct Mint **r_mints;
  struct Mint *mint;
  unsigned int cnt;
  int ok;
  const struct TALER_MINT_Keys *keys;
  json_t *j_mint;

  ok = 0;
  mints_str = NULL;
  token_nf = NULL;
  mint_section = NULL;
  mint_hostname = NULL;
  r_mints = NULL;
  cnt = 0;
  EXITIF (GNUNET_OK !=
          GNUNET_CONFIGURATION_get_value_string (cfg,
                                                 "merchant",
                                                 "TRUSTED_MINTS",
                                                 &mints_str));
  for (token_nf = strtok (mints_str, " ");
       NULL != token_nf;
       token_nf = strtok (NULL, " "))
  {
    GNUNET_assert (0 < GNUNET_asprintf (&mint_section,
                                        "mint-%s", token_nf));
    EXITIF (GNUNET_OK !=
            GNUNET_CONFIGURATION_get_value_string (cfg,
                                                   mint_section,
                                                   "HOSTNAME",
                                                   &mint_hostname));
    mint = GNUNET_new (struct Mint);
    mint->hostname = mint_hostname;
    GNUNET_array_append (r_mints,
                         cnt,
                         mint);
    GNUNET_free (mint_section);
    mint_section = NULL;
  }
  ok = 1;

  mints = r_mints;
  nmints = cnt;

  for (cnt = 0; cnt < nmints; cnt++)
  {
    EXITIF (NULL == (mints[cnt]->ctx = TALER_MINT_init ()));
    mints[cnt]->pending = 1;
    mints[cnt]->conn = TALER_MINT_connect (mints[cnt]->ctx,
                                           mints[cnt]->hostname,
                                           &keys_mgmt_cb,
                                           mints[cnt],
                                           TALER_MINT_OPTION_END);
    EXITIF (NULL == mints[cnt]->conn);
    mints[cnt]->poller_task =
      GNUNET_SCHEDULER_add_now (&context_task,
                                mints[cnt]);
  }

  trusted_mints = json_array ();
  for (cnt = 0; cnt < nmints; cnt++)
  {
    if (! mints[cnt]->pending)
    {
      keys = TALER_MINT_get_keys (mints[cnt]->conn);
      j_mint = json_pack ("{s:s, s:o}",
                          "url", mints[cnt]->hostname,
                          "master_pub",
                          TALER_json_from_data
                          (&keys->master_pub.eddsa_pub,
                           sizeof (keys->master_pub.eddsa_pub)));
      json_array_append_new (trusted_mints, j_mint);
    }
  }

 EXITIF_exit:
  GNUNET_free_non_null (mints_str);
  GNUNET_free_non_null (mint_section);
  GNUNET_free_non_null (mint_hostname);
  if (! ok)
  {
    GNUNET_free_non_null (r_mints);
    return GNUNET_SYSERR;
  }
  return cnt;
}


/**
 * Function called to shutdown the mints subsystem.
 */
void
TMH_MINTS_done ()
{
  unsigned int cnt;

  for (cnt = 0; cnt < nmints; cnt++)
  {
    if (NULL != mints[cnt]->conn)
      TALER_MINT_disconnect (mints[cnt]->conn);
    if (NULL != mints[cnt]->poller_task)
    {
      GNUNET_SCHEDULER_cancel (mints[cnt]->poller_task);
      mints[cnt]->poller_task = NULL;
    }
  }
}
