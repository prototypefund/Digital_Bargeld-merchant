/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file taler-mint-httpd_keystate.c
 * @brief management of our coin signing keys
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#include "platform.h"
#include <pthread.h>
#include "taler-mint-httpd_keystate.h"
#include "taler_mintdb_plugin.h"


/**
 * Snapshot of the (coin and signing) keys (including private keys) of
 * the mint.  There can be multiple instances of this struct, as it is
 * reference counted and only destroyed once the last user is done
 * with it.  The current instance is acquired using
 * #TMH_KS_acquire().  Using this function increases the
 * reference count.  The contents of this structure (except for the
 * reference counter) should be considered READ-ONLY until it is
 * ultimately destroyed (as there can be many concurrent users).
 */
struct TMH_KS_StateHandle
{
  /**
   * JSON array with denomination keys.  (Currently not really used
   * after initialization.)
   */
  json_t *denom_keys_array;

  /**
   * JSON array with signing keys. (Currently not really used
   * after initialization.)
   */
  json_t *sign_keys_array;

  /**
   * Cached JSON text that the mint will send for a "/keys" request.
   * Includes our @e TMH_master_public_key public key, the signing and
   * denomination keys as well as the @e reload_time.
   */
  char *keys_json;

  /**
   * Mapping from denomination keys to denomination key issue struct.
   * Used to lookup the key by hash.
   */
  struct GNUNET_CONTAINER_MultiHashMap *denomkey_map;

  /**
   * Hash context we used to combine the hashes of all denomination
   * keys into one big hash.
   */
  struct GNUNET_HashContext *hash_context;

  /**
   * When did we initiate the key reloading?
   */
  struct GNUNET_TIME_Absolute reload_time;

  /**
   * When is the next key invalid and we have to reload? (We also
   * reload on SIGUSR1.)
   */
  struct GNUNET_TIME_Absolute next_reload;

  /**
   * Mint signing key that should be used currently.
   */
  struct TALER_MINTDB_PrivateSigningKeyInformationP current_sign_key_issue;

  /**
   * Reference count.  The struct is released when the RC hits zero.
   */
  unsigned int refcnt;
};


/**
 * Mint key state.  Never use directly, instead access via
 * #TMH_KS_acquire() and #TMH_KS_release().
 */
static struct TMH_KS_StateHandle *internal_key_state;

/**
 * Mutex protecting access to #internal_key_state.
 */
static pthread_mutex_t internal_key_state_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Pipe used for signaling reloading of our key state.
 */
static int reload_pipe[2];


/**
 * Convert the public part of a denomination key issue to a JSON
 * object.
 *
 * @param pk public key of the denomination key
 * @param dki the denomination key issue
 * @return a JSON object describing the denomination key isue (public part)
 */
static json_t *
denom_key_issue_to_json (const struct TALER_DenominationPublicKey *pk,
                         const struct TALER_MINTDB_DenominationKeyInformationP *dki)
{
  struct TALER_Amount value;
  struct TALER_Amount fee_withdraw;
  struct TALER_Amount fee_deposit;
  struct TALER_Amount fee_refresh;

  TALER_amount_ntoh (&value,
                     &dki->properties.value);
  TALER_amount_ntoh (&fee_withdraw,
                     &dki->properties.fee_withdraw);
  TALER_amount_ntoh (&fee_deposit,
                     &dki->properties.fee_deposit);
  TALER_amount_ntoh (&fee_refresh,
                     &dki->properties.fee_refresh);
  return
    json_pack ("{s:o, s:o, s:o, s:o, s:o, s:o, s:o, s:o, s:o, s:o}",
               "master_sig",
               TALER_json_from_data (&dki->signature,
                                     sizeof (struct GNUNET_CRYPTO_EddsaSignature)),
               "stamp_start",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (dki->properties.start)),
               "stamp_expire_withdraw",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (dki->properties.expire_withdraw)),
               "stamp_expire_deposit",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (dki->properties.expire_spend)),
               "stamp_expire_legal",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (dki->properties.expire_legal)),
               "denom_pub",
               TALER_json_from_rsa_public_key (pk->rsa_public_key),
               "value",
               TALER_json_from_amount (&value),
               "fee_withdraw",
               TALER_json_from_amount (&fee_withdraw),
               "fee_deposit",
               TALER_json_from_amount (&fee_deposit),
               "fee_refresh",
               TALER_json_from_amount (&fee_refresh));
}


/**
 * Get the relative time value that describes how
 * far in the future do we want to provide coin keys.
 *
 * @return the provide duration
 */
static struct GNUNET_TIME_Relative
TALER_MINT_conf_duration_provide ()
{
  struct GNUNET_TIME_Relative rel;

  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_time (cfg,
                                           "mint_keys",
                                           "lookahead_provide",
                                           &rel))
  {
    GNUNET_log_config_invalid (GNUNET_ERROR_TYPE_ERROR,
                               "mint_keys",
                               "lookahead_provide",
                               "time value required");
    GNUNET_assert (0);
  }
  return rel;
}


/**
 * Iterator for (re)loading/initializing denomination keys.
 *
 * @param cls closure
 * @param dki the denomination key issue
 * @param alias coin alias
 * @return #GNUNET_OK to continue to iterate,
 *  #GNUNET_NO to stop iteration with no error,
 *  #GNUNET_SYSERR to abort iteration with error!
 */
static int
reload_keys_denom_iter (void *cls,
                        const char *alias,
                        const struct TALER_MINTDB_DenominationKeyIssueInformation *dki)
{
  struct TMH_KS_StateHandle *ctx = cls;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_TIME_Absolute horizon;
  struct GNUNET_HashCode denom_key_hash;
  struct TALER_MINTDB_DenominationKeyIssueInformation *d2;
  struct TALER_MINTDB_Session *session;
  int res;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Loading denomination key `%s'\n",
              alias);
  horizon = GNUNET_TIME_relative_to_absolute (TALER_MINT_conf_duration_provide ());
  if (GNUNET_TIME_absolute_ntoh (dki->issue.properties.start).abs_value_us >
      horizon.abs_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Skipping future denomination key `%s'\n",
                alias);
    return GNUNET_OK;
  }
  now = GNUNET_TIME_absolute_get ();
  if (GNUNET_TIME_absolute_ntoh (dki->issue.properties.expire_spend).abs_value_us <
      now.abs_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Skipping expired denomination key `%s'\n",
                alias);
    return GNUNET_OK;
  }

  GNUNET_CRYPTO_rsa_public_key_hash (dki->denom_pub.rsa_public_key,
                                     &denom_key_hash);
  GNUNET_CRYPTO_hash_context_read (ctx->hash_context,
                                   &denom_key_hash,
                                   sizeof (struct GNUNET_HashCode));
  session = TMH_plugin->get_session (TMH_plugin->cls,
                                     TMH_test_mode);
  if (NULL == session)
    return GNUNET_SYSERR;
  /* Try to insert DKI into DB until we succeed; note that if the DB
     failure is persistent, this code may loop forever (as there is no
     sane alternative, we cannot continue without the DKI being in the
     DB). */
  res = GNUNET_SYSERR;
  while (GNUNET_OK != res)
  {
    res = TMH_plugin->start (TMH_plugin->cls,
                             session);
    if (GNUNET_OK != res)
    {
      /* Transaction start failed!? Very bad error, log and retry */
      GNUNET_break (0);
      continue;
    }
    res = TMH_plugin->get_denomination_info (TMH_plugin->cls,
                                             session,
                                             &dki->denom_pub,
                                             NULL);
    if (GNUNET_SYSERR == res)
    {
      /* Fetch failed!? Very bad error, log and retry */
      GNUNET_break (0);
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      continue;
    }
    if (GNUNET_OK == res)
    {
      /* Record exists, we're good, just exit */
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      break;
    }
    res = TMH_plugin->insert_denomination_info (TMH_plugin->cls,
                                                session,
                                                &dki->denom_pub,
                                                &dki->issue);
    if (GNUNET_OK != res)
    {
      /* Insert failed!? Very bad error, log and retry */
      GNUNET_break (0);
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      continue;
    }
    res = TMH_plugin->commit (TMH_plugin->cls,
                              session);
    /* If commit succeeded, we're done, otherwise we retry; this
       time without logging, as theroetically commits can fail
       in a transactional DB due to concurrent activities that
       cannot be reconciled. This should be rare for DKIs, but
       as it is possible we just retry until we succeed. */
  }

  d2 = GNUNET_new (struct TALER_MINTDB_DenominationKeyIssueInformation);
  d2->issue = dki->issue;
  d2->denom_priv.rsa_private_key
    = GNUNET_CRYPTO_rsa_private_key_dup (dki->denom_priv.rsa_private_key);
  d2->denom_pub.rsa_public_key
    = GNUNET_CRYPTO_rsa_public_key_dup (dki->denom_pub.rsa_public_key);
  res = GNUNET_CONTAINER_multihashmap_put (ctx->denomkey_map,
                                           &denom_key_hash,
                                           d2,
                                           GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY);
  if (GNUNET_OK != res)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Duplicate denomination key `%s'\n",
                alias);
    GNUNET_CRYPTO_rsa_private_key_free (d2->denom_priv.rsa_private_key);
    GNUNET_CRYPTO_rsa_public_key_free (d2->denom_pub.rsa_public_key);
    GNUNET_free (d2);
    return GNUNET_OK;
  }
  json_array_append_new (ctx->denom_keys_array,
                         denom_key_issue_to_json (&dki->denom_pub,
                                                  &dki->issue));
  return GNUNET_OK;
}


/**
 * Convert the public part of a sign key issue to a JSON object.
 *
 * @param ski the sign key issue
 * @return a JSON object describing the sign key isue (public part)
 */
static json_t *
sign_key_issue_to_json (const struct TALER_MintSigningKeyValidityPS *ski)
{
  return
    json_pack ("{s:o, s:o, s:o, s:o, s:o, s:o}",
               "stamp_start",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (ski->start)),
               "stamp_expire",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (ski->expire)),
               "stamp_end",
               TALER_json_from_abs (GNUNET_TIME_absolute_ntoh (ski->end)),
               "master_pub",
               TALER_json_from_data (&ski->master_public_key,
                                     sizeof (struct TALER_MasterPublicKeyP)),
               "master_sig",
               TALER_json_from_data (&ski->signature,
                                     sizeof (struct TALER_MasterSignatureP)),
               "key",
               TALER_json_from_data (&ski->signkey_pub,
                                     sizeof (struct TALER_MintPublicKeyP)));
}


/**
 * Iterator for sign keys.
 *
 * @param cls closure
 * @param filename name of the file the key came from
 * @param ski the sign key issue
 * @return #GNUNET_OK to continue to iterate,
 *  #GNUNET_NO to stop iteration with no error,
 *  #GNUNET_SYSERR to abort iteration with error!
 */
static int
reload_keys_sign_iter (void *cls,
                       const char *filename,
                       const struct TALER_MINTDB_PrivateSigningKeyInformationP *ski)
{
  struct TMH_KS_StateHandle *ctx = cls;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_TIME_Absolute horizon;

  horizon = GNUNET_TIME_relative_to_absolute (TALER_MINT_conf_duration_provide ());
  if (GNUNET_TIME_absolute_ntoh (ski->issue.start).abs_value_us >
      horizon.abs_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Skipping future signing key `%s'\n",
                filename);
    return GNUNET_OK;
  }
  now = GNUNET_TIME_absolute_get ();
  if (GNUNET_TIME_absolute_ntoh (ski->issue.expire).abs_value_us <
      now.abs_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Skipping expired signing key `%s'\n",
                filename);
    return GNUNET_OK;
  }

  /* The signkey is valid at this time, check if it's more recent than
     what we have so far! */
  if ( (GNUNET_TIME_absolute_ntoh (ctx->current_sign_key_issue.issue.start).abs_value_us <
        GNUNET_TIME_absolute_ntoh (ski->issue.start).abs_value_us) &&
       (GNUNET_TIME_absolute_ntoh (ski->issue.start).abs_value_us <
        now.abs_value_us) )
  {
    /* We use the most recent one, if it is valid now (not just in the near future) */
    ctx->current_sign_key_issue = *ski;
  }
  json_array_append_new (ctx->sign_keys_array,
                         sign_key_issue_to_json (&ski->issue));

  return GNUNET_OK;
}


/**
 * Iterator for freeing denomination keys.
 *
 * @param cls closure with the `struct TMH_KS_StateHandle`
 * @param key key for the denomination key
 * @param value coin details
 * @return #GNUNET_OK to continue to iterate,
 *  #GNUNET_NO to stop iteration with no error,
 *  #GNUNET_SYSERR to abort iteration with error!
 */
static int
free_denom_key (void *cls,
                const struct GNUNET_HashCode *key,
                void *value)
{
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki = value;

  GNUNET_CRYPTO_rsa_private_key_free (dki->denom_priv.rsa_private_key);
  GNUNET_CRYPTO_rsa_public_key_free (dki->denom_pub.rsa_public_key);
  GNUNET_free (dki);
  return GNUNET_OK;
}


/**
 * Release key state, free if necessary (if reference count gets to zero).
 * Internal method used when the mutex is already held.
 *
 * @param key_state the key state to release
 */
static void
TMH_KS_release_ (struct TMH_KS_StateHandle *key_state)
{
  GNUNET_assert (0 < key_state->refcnt);
  key_state->refcnt--;
  if (0 == key_state->refcnt)
  {
    if (NULL != key_state->denom_keys_array)
    {
      json_decref (key_state->denom_keys_array);
      key_state->denom_keys_array = NULL;
    }
    if (NULL != key_state->sign_keys_array)
    {
      json_decref (key_state->sign_keys_array);
      key_state->sign_keys_array = NULL;
    }
    if (NULL != key_state->denomkey_map)
    {
      GNUNET_CONTAINER_multihashmap_iterate (key_state->denomkey_map,
                                             &free_denom_key,
                                             key_state);
      GNUNET_CONTAINER_multihashmap_destroy (key_state->denomkey_map);
      key_state->denomkey_map = NULL;
    }
    GNUNET_free_non_null (key_state->keys_json);
    GNUNET_free (key_state);
  }
}


/**
 * Release key state, free if necessary (if reference count gets to zero).
 *
 * @param key_state the key state to release
 */
void
TMH_KS_release (struct TMH_KS_StateHandle *key_state)
{
  GNUNET_assert (0 == pthread_mutex_lock (&internal_key_state_mutex));
  TMH_KS_release_ (key_state);
  GNUNET_assert (0 == pthread_mutex_unlock (&internal_key_state_mutex));
}


/**
 * Acquire the key state of the mint.  Updates keys if necessary.
 * For every call to #TMH_KS_acquire(), a matching call
 * to #TMH_KS_release() must be made.
 *
 * @return the key state
 */
struct TMH_KS_StateHandle *
TMH_KS_acquire (void)
{
  struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
  struct TMH_KS_StateHandle *key_state;
  json_t *keys;
  struct TALER_MintKeySetPS ks;
  struct TALER_MintSignatureP sig;

  GNUNET_assert (0 == pthread_mutex_lock (&internal_key_state_mutex));
  if ( (NULL != internal_key_state) &&
       (internal_key_state->next_reload.abs_value_us <= now.abs_value_us) )
  {
    TMH_KS_release_ (internal_key_state);
    internal_key_state = NULL;
  }
  if (NULL == internal_key_state)
  {
    key_state = GNUNET_new (struct TMH_KS_StateHandle);
    key_state->hash_context = GNUNET_CRYPTO_hash_context_start ();
    key_state->denom_keys_array = json_array ();
    GNUNET_assert (NULL != key_state->denom_keys_array);
    key_state->sign_keys_array = json_array ();
    GNUNET_assert (NULL != key_state->sign_keys_array);
    key_state->denomkey_map = GNUNET_CONTAINER_multihashmap_create (32,
                                                                    GNUNET_NO);
    key_state->reload_time = GNUNET_TIME_absolute_get ();
    TALER_round_abs_time (&key_state->reload_time);
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Loading keys from `%s'\n",
                TMH_mint_directory);
    TALER_MINTDB_denomination_keys_iterate (TMH_mint_directory,
                                            &reload_keys_denom_iter,
                                            key_state);
    TALER_MINTDB_signing_keys_iterate (TMH_mint_directory,
                                       &reload_keys_sign_iter,
                                       key_state);
    ks.purpose.size = htonl (sizeof (ks));
    ks.purpose.purpose = htonl (TALER_SIGNATURE_MINT_KEY_SET);
    ks.list_issue_date = GNUNET_TIME_absolute_hton (key_state->reload_time);
    GNUNET_CRYPTO_hash_context_finish (key_state->hash_context,
                                       &ks.hc);
    key_state->hash_context = NULL;
    GNUNET_assert (GNUNET_OK ==
                   GNUNET_CRYPTO_eddsa_sign (&key_state->current_sign_key_issue.signkey_priv.eddsa_priv,
                                             &ks.purpose,
                                             &sig.eddsa_signature));
    key_state->next_reload = GNUNET_TIME_absolute_ntoh (key_state->current_sign_key_issue.issue.expire);
    if (0 == key_state->next_reload.abs_value_us)
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "No valid signing key found!\n");

    keys = json_pack ("{s:o, s:o, s:o, s:o, s:o, s:o}",
                      "master_public_key",
                      TALER_json_from_data (&TMH_master_public_key,
                                            sizeof (struct GNUNET_CRYPTO_EddsaPublicKey)),
                      "signkeys", key_state->sign_keys_array,
                      "denoms", key_state->denom_keys_array,
                      "list_issue_date", TALER_json_from_abs (key_state->reload_time),
                      "eddsa_pub", TALER_json_from_data (&key_state->current_sign_key_issue.issue.signkey_pub,
                                                         sizeof (struct TALER_MintPublicKeyP)),
                      "eddsa_sig", TALER_json_from_data (&sig,
                                                         sizeof (struct TALER_MintSignatureP)));
    key_state->sign_keys_array = NULL;
    key_state->denom_keys_array = NULL;
    key_state->keys_json = json_dumps (keys,
                                       JSON_INDENT (2));
    json_decref (keys);
    internal_key_state = key_state;
  }
  key_state = internal_key_state;
  key_state->refcnt++;
  GNUNET_assert (0 == pthread_mutex_unlock (&internal_key_state_mutex));

  return key_state;
}


/**
 * Look up the issue for a denom public key.
 *
 * @param key_state state to look in
 * @param denom_pub denomination public key
 * @param use purpose for which the key is being located
 * @return the denomination key issue,
 *         or NULL if denom_pub could not be found
 */
struct TALER_MINTDB_DenominationKeyIssueInformation *
TMH_KS_denomination_key_lookup (const struct TMH_KS_StateHandle *key_state,
                                const struct TALER_DenominationPublicKey *denom_pub,
				enum TMH_KS_DenominationKeyUse use)
{
  struct GNUNET_HashCode hc;
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki;
  struct GNUNET_TIME_Absolute now;

  GNUNET_CRYPTO_rsa_public_key_hash (denom_pub->rsa_public_key,
                                     &hc);
  dki = GNUNET_CONTAINER_multihashmap_get (key_state->denomkey_map,
					   &hc);
  if (NULL == dki)
    return NULL;
  now = GNUNET_TIME_absolute_get ();
  if (now.abs_value_us <
      GNUNET_TIME_absolute_ntoh (dki->issue.properties.start).abs_value_us)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		"Not returning DKI for %s, as start time is in the future\n",
		GNUNET_h2s (&hc));
    return NULL;
  }
  now = GNUNET_TIME_absolute_get ();
  switch (use)
  {
  case TMH_KS_DKU_WITHDRAW:
    if (now.abs_value_us >
	GNUNET_TIME_absolute_ntoh (dki->issue.properties.expire_withdraw).abs_value_us)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		  "Not returning DKI for %s, as time to create coins has passed\n",
		  GNUNET_h2s (&hc));
      return NULL;
    }
    break;
  case TMH_KS_DKU_DEPOSIT:
    if (now.abs_value_us >
	GNUNET_TIME_absolute_ntoh (dki->issue.properties.expire_spend).abs_value_us)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
		  "Not returning DKI for %s, as time to spend coin has passed\n",
		  GNUNET_h2s (&hc));
      return NULL;
    }
    break;
  }
  return dki;
}


/**
 * Handle a signal, writing relevant signal numbers to the pipe.
 *
 * @param signal_number the signal number
 */
static void
handle_signal (int signal_number)
{
  ssize_t res;
  char c = signal_number;

  res = write (reload_pipe[1],
               &c,
               1);
  if ( (res < 0) &&
       (EINTR != errno) )
  {
    GNUNET_break (0);
    return;
  }
  if (0 == res)
  {
    GNUNET_break (0);
    return;
  }
}


/**
 * Call #handle_signal() to pass the received signal via
 * the control pipe.
 */
static void
handle_sigusr1 ()
{
  handle_signal (SIGUSR1);
}


/**
 * Call #handle_signal() to pass the received signal via
 * the control pipe.
 */
static void
handle_sigint ()
{
  handle_signal (SIGINT);
}


/**
 * Call #handle_signal() to pass the received signal via
 * the control pipe.
 */
static void
handle_sigterm ()
{
  handle_signal (SIGTERM);
}


/**
 * Call #handle_signal() to pass the received signal via
 * the control pipe.
 */
static void
handle_sighup ()
{
  handle_signal (SIGHUP);
}


/**
 * Read signals from a pipe in a loop, and reload keys from disk if
 * SIGUSR1 is received, terminate if SIGTERM/SIGINT is received, and
 * restart if SIGHUP is received.
 *
 * @return #GNUNET_SYSERR on errors,
 *         #GNUNET_OK to terminate normally
 *         #GNUNET_NO to restart an update version of the binary
 */
int
TMH_KS_loop (void)
{
  struct GNUNET_SIGNAL_Context *sigusr1;
  struct GNUNET_SIGNAL_Context *sigterm;
  struct GNUNET_SIGNAL_Context *sigint;
  struct GNUNET_SIGNAL_Context *sighup;
  int ret;

  if (0 != pipe (reload_pipe))
  {
    fprintf (stderr,
             "Failed to create pipe.\n");
    return GNUNET_SYSERR;
  }
  sigusr1 = GNUNET_SIGNAL_handler_install (SIGUSR1,
                                           &handle_sigusr1);
  sigterm = GNUNET_SIGNAL_handler_install (SIGTERM,
                                           &handle_sigterm);
  sigint = GNUNET_SIGNAL_handler_install (SIGINT,
                                          &handle_sigint);
  sighup = GNUNET_SIGNAL_handler_install (SIGHUP,
                                          &handle_sighup);

  ret = 0;
  while (0 == ret)
  {
    char c;
    ssize_t res;

    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "(re-)loading keys\n");
    if (NULL != internal_key_state)
    {
      TMH_KS_release (internal_key_state);
      internal_key_state = NULL;
    }
    /* This will re-initialize 'internal_key_state' with
       an initial refcnt of 1 */
    (void) TMH_KS_acquire ();

read_again:
    errno = 0;
    res = read (reload_pipe[0],
                &c,
                1);
    if ((res < 0) && (EINTR != errno))
    {
      GNUNET_break (0);
      ret = GNUNET_SYSERR;
      break;
    }
    if (EINTR == errno)
      goto read_again;
    switch (c)
    {
    case SIGUSR1:
      /* reload internal key state, we do this in the loop */
      break;
    case SIGTERM:
    case SIGINT:
      /* terminate */
      ret = GNUNET_OK;
      break;
    case SIGHUP:
      /* restart updated binary */
      ret = GNUNET_NO;
      break;
    default:
      /* unexpected character */
      GNUNET_break (0);
      break;
    }
  }
  if (NULL != internal_key_state)
  {
    TMH_KS_release (internal_key_state);
    internal_key_state = NULL;
  }
  GNUNET_SIGNAL_handler_uninstall (sigusr1);
  GNUNET_SIGNAL_handler_uninstall (sigterm);
  GNUNET_SIGNAL_handler_uninstall (sigint);
  GNUNET_SIGNAL_handler_uninstall (sighup);
  return ret;
}


/**
 * Sign the message in @a purpose with the mint's signing key.
 *
 * @param purpose the message to sign
 * @param[out] pub set to the current public signing key of the mint
 * @param[out] sig signature over purpose using current signing key
 */
void
TMH_KS_sign (const struct GNUNET_CRYPTO_EccSignaturePurpose *purpose,
             struct TALER_MintPublicKeyP *pub,
             struct TALER_MintSignatureP *sig)

{
  struct TMH_KS_StateHandle *key_state;

  key_state = TMH_KS_acquire ();
  *pub = key_state->current_sign_key_issue.issue.signkey_pub;
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CRYPTO_eddsa_sign (&key_state->current_sign_key_issue.signkey_priv.eddsa_priv,
                                           purpose,
                                           &sig->eddsa_signature));
  TMH_KS_release (key_state);
}


/**
 * Function to call to handle the request by sending
 * back static data from the @a rh.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
TMH_KS_handler_keys (struct TMH_RequestHandler *rh,
                     struct MHD_Connection *connection,
                     void **connection_cls,
                     const char *upload_data,
                     size_t *upload_data_size)
{
  struct TMH_KS_StateHandle *key_state;
  struct MHD_Response *response;
  int ret;

  key_state = TMH_KS_acquire ();
  response = MHD_create_response_from_buffer (strlen (key_state->keys_json),
                                              key_state->keys_json,
                                              MHD_RESPMEM_MUST_COPY);
  TMH_KS_release (key_state);
  if (NULL == response)
  {
    GNUNET_break (0);
    return MHD_NO;
  }
  (void) MHD_add_response_header (response,
                                  "Content-Type",
                                  rh->mime_type);
  ret = MHD_queue_response (connection,
                            rh->response_code,
                            response);
  MHD_destroy_response (response);
  return ret;
}


/* end of taler-mint-httpd_keystate.c */
