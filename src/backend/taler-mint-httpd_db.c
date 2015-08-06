/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

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
 * @file taler-mint-httpd_db.c
 * @brief High-level (transactional-layer) database operations for the mint.
 * @author Christian Grothoff
 */
#include "platform.h"
#include <pthread.h>
#include <jansson.h>
#include "taler-mint-httpd_responses.h"
#include "taler-mint-httpd_keystate.h"


/**
 * Calculate the total value of all transactions performed.
 * Stores @a off plus the cost of all transactions in @a tl
 * in @a ret.
 *
 * @param tl transaction list to process
 * @param off offset to use as the starting value
 * @param ret where the resulting total is to be stored
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on errors
 */
static int
calculate_transaction_list_totals (struct TALER_MINTDB_TransactionList *tl,
                                   const struct TALER_Amount *off,
                                   struct TALER_Amount *ret)
{
  struct TALER_Amount spent = *off;
  struct TALER_MINTDB_TransactionList *pos;

  for (pos = tl; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_MINTDB_TT_DEPOSIT:
      if (GNUNET_OK !=
          TALER_amount_add (&spent,
                            &spent,
                            &pos->details.deposit->amount_with_fee))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      break;
    case TALER_MINTDB_TT_REFRESH_MELT:
      if (GNUNET_OK !=
          TALER_amount_add (&spent,
                            &spent,
                            &pos->details.melt->amount_with_fee))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
      break;
    case TALER_MINTDB_TT_LOCK:
      /* should check if lock is still active,
         and if it is for THIS operation; if
         lock is inactive, delete it; if lock
         is for THIS operation, ignore it;
         if lock is for another operation,
         count it! */
      GNUNET_assert (0);  // FIXME: not implemented! (#3625)
      return GNUNET_SYSERR;
    }
  }
  *ret = spent;
  return GNUNET_OK;
}


/**
 * Execute a deposit.  The validity of the coin and signature
 * have already been checked.  The database must now check that
 * the coin is not (double or over) spent, and execute the
 * transaction (record details, generate success or failure response).
 *
 * @param connection the MHD connection to handle
 * @param deposit information about the deposit
 * @return MHD result code
 */
int
TMH_DB_execute_deposit (struct MHD_Connection *connection,
                        const struct TALER_MINTDB_Deposit *deposit)
{
  struct TALER_MINTDB_Session *session;
  struct TALER_MINTDB_TransactionList *tl;
  struct TALER_Amount spent;
  struct TALER_Amount value;
  struct TALER_Amount amount_without_fee;
  struct TMH_KS_StateHandle *mks;
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki;
  int ret;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls,
                                                  TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (GNUNET_YES ==
      TMH_plugin->have_deposit (TMH_plugin->cls,
                                session,
                                deposit))
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_subtract (&amount_without_fee,
                                          &deposit->amount_with_fee,
                                          &deposit->deposit_fee));
    return TMH_RESPONSE_reply_deposit_success (connection,
                                               &deposit->coin.coin_pub,
                                               &deposit->h_wire,
                                               &deposit->h_contract,
                                               deposit->transaction_id,
                                               deposit->timestamp,
                                               deposit->refund_deadline,
                                               &deposit->merchant_pub,
                                               &amount_without_fee);
  }
  mks = TMH_KS_acquire ();
  dki = TMH_KS_denomination_key_lookup (mks,
                                        &deposit->coin.denom_pub,
					TMH_KS_DKU_DEPOSIT);
  TALER_amount_ntoh (&value,
                     &dki->issue.properties.value);
  TMH_KS_release (mks);

  if (GNUNET_OK !=
      TMH_plugin->start (TMH_plugin->cls,
                         session))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  /* fee for THIS transaction */
  spent = deposit->amount_with_fee;
  /* add cost of all previous transactions */
  tl = TMH_plugin->get_coin_transactions (TMH_plugin->cls,
                                          session,
                                          &deposit->coin.coin_pub);
  if (GNUNET_OK !=
      calculate_transaction_list_totals (tl,
                                         &spent,
                                         &spent))
  {
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  /* Check that cost of all transactions is smaller than
     the value of the coin. */
  if (0 < TALER_amount_cmp (&spent,
                            &value))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    ret = TMH_RESPONSE_reply_deposit_insufficient_funds (connection,
                                                         tl);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return ret;
  }
  TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                          tl);

  if (GNUNET_OK !=
      TMH_plugin->insert_deposit (TMH_plugin->cls,
                                  session,
                                  deposit))
  {
    TALER_LOG_WARNING ("Failed to store /deposit information in database\n");
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  if (GNUNET_OK !=
      TMH_plugin->commit (TMH_plugin->cls,
                          session))
  {
    TALER_LOG_WARNING ("/deposit transaction commit failed\n");
    return TMH_RESPONSE_reply_commit_error (connection);
  }
  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_subtract (&amount_without_fee,
                                        &deposit->amount_with_fee,
                                        &deposit->deposit_fee));
  return TMH_RESPONSE_reply_deposit_success (connection,
                                             &deposit->coin.coin_pub,
                                             &deposit->h_wire,
                                             &deposit->h_contract,
                                             deposit->transaction_id,
                                             deposit->timestamp,
                                             deposit->refund_deadline,
                                             &deposit->merchant_pub,
                                             &amount_without_fee);
}


/**
 * Execute a /withdraw/status.  Given the public key of a reserve,
 * return the associated transaction history.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve to check
 * @return MHD result code
 */
int
TMH_DB_execute_withdraw_status (struct MHD_Connection *connection,
                                const struct TALER_ReservePublicKeyP *reserve_pub)
{
  struct TALER_MINTDB_Session *session;
  struct TALER_MINTDB_ReserveHistory *rh;
  int res;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls,
                                                  TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  rh = TMH_plugin->get_reserve_history (TMH_plugin->cls,
                                        session,
                                        reserve_pub);
  if (NULL == rh)
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         "{s:s, s:s}",
                                         "error", "Reserve not found",
                                         "parameter", "withdraw_pub");
  res = TMH_RESPONSE_reply_withdraw_status_success (connection,
                                                    rh);
  TMH_plugin->free_reserve_history (TMH_plugin->cls,
                                    rh);
  return res;
}


/**
 * Execute a "/withdraw/sign". Given a reserve and a properly signed
 * request to withdraw a coin, check the balance of the reserve and
 * if it is sufficient, store the request and return the signed
 * blinded envelope.
 *
 * @param connection the MHD connection to handle
 * @param reserve public key of the reserve
 * @param denomination_pub public key of the denomination requested
 * @param blinded_msg blinded message to be signed
 * @param blinded_msg_len number of bytes in @a blinded_msg
 * @param signature signature over the withdraw request, to be stored in DB
 * @return MHD result code
 */
int
TMH_DB_execute_withdraw_sign (struct MHD_Connection *connection,
                              const struct TALER_ReservePublicKeyP *reserve,
                              const struct TALER_DenominationPublicKey *denomination_pub,
                              const char *blinded_msg,
                              size_t blinded_msg_len,
                              const struct TALER_ReserveSignatureP *signature)
{
  struct TALER_MINTDB_Session *session;
  struct TALER_MINTDB_ReserveHistory *rh;
  const struct TALER_MINTDB_ReserveHistory *pos;
  struct TMH_KS_StateHandle *key_state;
  struct TALER_MINTDB_CollectableBlindcoin collectable;
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki;
  struct TALER_MINTDB_DenominationKeyIssueInformation *tdki;
  struct GNUNET_CRYPTO_rsa_Signature *sig;
  struct TALER_Amount amount_required;
  struct TALER_Amount deposit_total;
  struct TALER_Amount withdraw_total;
  struct TALER_Amount balance;
  struct TALER_Amount value;
  struct TALER_Amount fee_withdraw;
  struct GNUNET_HashCode h_blind;
  int res;

  GNUNET_CRYPTO_hash (blinded_msg,
                      blinded_msg_len,
                      &h_blind);

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls,
                                                  TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  res = TMH_plugin->get_withdraw_info (TMH_plugin->cls,
                                       session,
                                       &h_blind,
                                       &collectable);
  if (GNUNET_SYSERR == res)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* Don't sign again if we have already signed the coin */
  if (GNUNET_YES == res)
  {
    res = TMH_RESPONSE_reply_withdraw_sign_success (connection,
                                                    &collectable);
    GNUNET_CRYPTO_rsa_signature_free (collectable.sig.rsa_signature);
    GNUNET_CRYPTO_rsa_public_key_free (collectable.denom_pub.rsa_public_key);
    return res;
  }
  GNUNET_assert (GNUNET_NO == res);

  /* Check if balance is sufficient */
  key_state = TMH_KS_acquire ();
  dki = TMH_KS_denomination_key_lookup (key_state,
                                        denomination_pub,
					TMH_KS_DKU_WITHDRAW);
  if (NULL == dki)
  {
    TMH_KS_release (key_state);
    return TMH_RESPONSE_reply_json_pack (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         "{s:s}",
                                         "error",
                                         "Denomination not found");
  }
  if (GNUNET_OK !=
      TMH_plugin->start (TMH_plugin->cls,
                         session))
  {
    GNUNET_break (0);
    TMH_KS_release (key_state);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  rh = TMH_plugin->get_reserve_history (TMH_plugin->cls,
                                        session,
                                        reserve);
  if (NULL == rh)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    TMH_KS_release (key_state);
    return TMH_RESPONSE_reply_arg_unknown (connection,
                                           "reserve_pub");
  }

  /* calculate amount required including fees */
  TALER_amount_ntoh (&value,
                     &dki->issue.properties.value);
  TALER_amount_ntoh (&fee_withdraw,
                     &dki->issue.properties.fee_withdraw);

  if (GNUNET_OK !=
      TALER_amount_add (&amount_required,
                        &value,
                        &fee_withdraw))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    TMH_KS_release (key_state);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* calculate balance of the reserve */
  res = 0;
  for (pos = rh; NULL != pos; pos = pos->next)
  {
    switch (pos->type)
    {
    case TALER_MINTDB_RO_BANK_TO_MINT:
      if (0 == (res & 1))
        deposit_total = pos->details.bank->amount;
      else
        if (GNUNET_OK !=
            TALER_amount_add (&deposit_total,
                              &deposit_total,
                              &pos->details.bank->amount))
        {
          TMH_plugin->rollback (TMH_plugin->cls,
                                session);
          TMH_KS_release (key_state);
          return TMH_RESPONSE_reply_internal_db_error (connection);
        }
      res |= 1;
      break;
    case TALER_MINTDB_RO_WITHDRAW_COIN:
      tdki = TMH_KS_denomination_key_lookup (key_state,
                                             &pos->details.withdraw->denom_pub,
					     TMH_KS_DKU_WITHDRAW);
      TALER_amount_ntoh (&value,
                         &tdki->issue.properties.value);
      if (0 == (res & 2))
        withdraw_total = value;
      else
        if (GNUNET_OK !=
            TALER_amount_add (&withdraw_total,
                              &withdraw_total,
                              &value))
        {
          TMH_plugin->rollback (TMH_plugin->cls,
                                session);
          TMH_KS_release (key_state);
          return TMH_RESPONSE_reply_internal_db_error (connection);
        }
      res |= 2;
      break;
    }
  }
  if (0 == (res & 1))
  {
    /* did not encounter any deposit operations, how can we have a reserve? */
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (0 == (res & 2))
  {
    /* did not encounter any withdraw operations, set to zero */
    TALER_amount_get_zero (deposit_total.currency,
                           &withdraw_total);
  }
  /* All reserve balances should be non-negative */
  GNUNET_assert (GNUNET_SYSERR !=
                 TALER_amount_subtract (&balance,
                                        &deposit_total,
                                        &withdraw_total));
  if (0 < TALER_amount_cmp (&amount_required,
                            &balance))
  {
    TMH_KS_release (key_state);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    res = TMH_RESPONSE_reply_withdraw_sign_insufficient_funds (connection,
                                                               rh);
    TMH_plugin->free_reserve_history (TMH_plugin->cls,
                                      rh);
    return res;
  }
  TMH_plugin->free_reserve_history (TMH_plugin->cls,
                                    rh);

  /* Balance is good, sign the coin! */
  sig = GNUNET_CRYPTO_rsa_sign (dki->denom_priv.rsa_private_key,
                                blinded_msg,
                                blinded_msg_len);
  TMH_KS_release (key_state);
  if (NULL == sig)
  {
    GNUNET_break (0);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "Internal error");
  }
  collectable.sig.rsa_signature = sig;
  collectable.denom_pub = *denomination_pub;
  collectable.amount_with_fee = amount_required;
  collectable.withdraw_fee = fee_withdraw;
  collectable.reserve_pub = *reserve;
  collectable.h_coin_envelope = h_blind;
  collectable.reserve_sig = *signature;
  if (GNUNET_OK !=
      TMH_plugin->insert_withdraw_info (TMH_plugin->cls,
                                        session,
                                        &collectable))
  {
    GNUNET_break (0);
    GNUNET_CRYPTO_rsa_signature_free (sig);
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (GNUNET_OK !=
      TMH_plugin->commit (TMH_plugin->cls,
                          session))
  {
    TALER_LOG_WARNING ("/withdraw/sign transaction commit failed\n");
    return TMH_RESPONSE_reply_commit_error (connection);
  }
  res = TMH_RESPONSE_reply_withdraw_sign_success (connection,
                                                  &collectable);
  GNUNET_CRYPTO_rsa_signature_free (sig);
  return res;
}


/**
 * Parse coin melt requests from a JSON object and write them to
 * the database.
 *
 * @param connection the connection to send errors to
 * @param session the database connection
 * @param key_state the mint's key state
 * @param session_hash hash identifying the refresh session
 * @param coin_details details about the coin being melted
 * @param oldcoin_index what is the number assigned to this coin
 * @return #GNUNET_OK on success,
 *         #GNUNET_NO if an error message was generated,
 *         #GNUNET_SYSERR on internal errors (no response generated)
 */
static int
refresh_accept_melts (struct MHD_Connection *connection,
                      struct TALER_MINTDB_Session *session,
                      const struct TMH_KS_StateHandle *key_state,
                      const struct GNUNET_HashCode *session_hash,
                      const struct TMH_DB_MeltDetails *coin_details,
                      uint16_t oldcoin_index)
{
  struct TALER_MINTDB_DenominationKeyInformationP *dki;
  struct TALER_MINTDB_TransactionList *tl;
  struct TALER_Amount coin_value;
  struct TALER_Amount coin_residual;
  struct TALER_Amount spent;
  struct TALER_MINTDB_RefreshMelt melt;
  int res;

  dki = &TMH_KS_denomination_key_lookup (key_state,
                                         &coin_details->coin_info.denom_pub,
					 TMH_KS_DKU_DEPOSIT)->issue;

  if (NULL == dki)
    return (MHD_YES ==
            TMH_RESPONSE_reply_arg_unknown (connection,
                                            "denom_pub"))
        ? GNUNET_NO : GNUNET_SYSERR;

  TALER_amount_ntoh (&coin_value,
                     &dki->properties.value);
  /* fee for THIS transaction; the melt amount includes the fee! */
  spent = coin_details->melt_amount_with_fee;
  /* add historic transaction costs of this coin */
  tl = TMH_plugin->get_coin_transactions (TMH_plugin->cls,
                                          session,
                                          &coin_details->coin_info.coin_pub);
  if (GNUNET_OK !=
      calculate_transaction_list_totals (tl,
                                         &spent,
                                         &spent))
  {
    GNUNET_break (0);
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  /* Refuse to refresh when the coin's value is insufficient
     for the cost of all transactions. */
  if (TALER_amount_cmp (&coin_value,
                        &spent) < 0)
  {
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_subtract (&coin_residual,
                                          &spent,
                                          &coin_details->melt_amount_with_fee));
    res = (MHD_YES ==
           TMH_RESPONSE_reply_refresh_melt_insufficient_funds (connection,
                                                               &coin_details->coin_info.coin_pub,
                                                               coin_value,
                                                               tl,
                                                               coin_details->melt_amount_with_fee,
                                                               coin_residual))
        ? GNUNET_NO : GNUNET_SYSERR;
    TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                            tl);
    return res;
  }
  TMH_plugin->free_coin_transaction_list (TMH_plugin->cls,
                                          tl);

  melt.coin = coin_details->coin_info;
  melt.coin_sig = coin_details->melt_sig;
  melt.session_hash = *session_hash;
  melt.amount_with_fee = coin_details->melt_amount_with_fee;
  if (GNUNET_OK !=
      TMH_plugin->insert_refresh_melt (TMH_plugin->cls,
                                       session,
                                       oldcoin_index,
                                       &melt))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Execute a "/refresh/melt".  We have been given a list of valid
 * coins and a request to melt them into the given
 * @a refresh_session_pub.  Check that the coins all have the
 * required value left and if so, store that they have been
 * melted and confirm the melting operation to the client.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash code of the session the coins are melted into
 * @param num_new_denoms number of entries in @a denom_pubs, size of y-dimension of @a commit_coin array
 * @param denom_pubs public keys of the coins we want to withdraw in the end
 * @param coin_count number of entries in @a coin_melt_details, size of y-dimension of @a commit_link array
 * @param coin_melt_details signatures and (residual) value of the respective coin should be melted
 * @param commit_coin 2d array of coin commitments (what the mint is to sign
 *                    once the "/refres/reveal" of cut and choose is done),
 *                    x-dimension must be #TALER_CNC_KAPPA
 * @param commit_link 2d array of coin link commitments (what the mint is
 *                    to return via "/refresh/link" to enable linkage in the
 *                    future)
 *                    x-dimension must be #TALER_CNC_KAPPA
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_melt (struct MHD_Connection *connection,
                             const struct GNUNET_HashCode *session_hash,
                             unsigned int num_new_denoms,
                             const struct TALER_DenominationPublicKey *denom_pubs,
                             unsigned int coin_count,
                             const struct TMH_DB_MeltDetails *coin_melt_details,
                             struct TALER_MINTDB_RefreshCommitCoin *const* commit_coin,
                             struct TALER_MINTDB_RefreshCommitLinkP *const* commit_link)
{
  struct TMH_KS_StateHandle *key_state;
  struct TALER_MINTDB_RefreshSession refresh_session;
  struct TALER_MINTDB_Session *session;
  int res;
  unsigned int i;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls,
                                                  TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  if (GNUNET_OK !=
      TMH_plugin->start (TMH_plugin->cls,
                         session))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  res = TMH_plugin->get_refresh_session (TMH_plugin->cls,
                                         session,
                                         session_hash,
                                         &refresh_session);
  if (GNUNET_YES == res)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    res = TMH_RESPONSE_reply_refresh_melt_success (connection,
                                                   session_hash,
                                                   refresh_session.noreveal_index);
    return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
  }
  if (GNUNET_SYSERR == res)
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* store 'global' session data */
  refresh_session.num_oldcoins = coin_count;
  refresh_session.num_newcoins = num_new_denoms;
  refresh_session.noreveal_index
      = GNUNET_CRYPTO_random_u32 (GNUNET_CRYPTO_QUALITY_STRONG,
                                  TALER_CNC_KAPPA);
  if (GNUNET_OK !=
      (res = TMH_plugin->create_refresh_session (TMH_plugin->cls,
                                                 session,
                                                 session_hash,
                                                 &refresh_session)))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  /* Melt old coins and check that they had enough residual value */
  key_state = TMH_KS_acquire ();
  for (i=0;i<coin_count;i++)
  {
    if (GNUNET_OK !=
        (res = refresh_accept_melts (connection,
                                     session,
                                     key_state,
                                     session_hash,
                                     &coin_melt_details[i],
                                     i)))
    {
      TMH_KS_release (key_state);
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      return (GNUNET_SYSERR == res) ? MHD_NO : MHD_YES;
    }
  }
  TMH_KS_release (key_state);

  /* store requested new denominations */
  if (GNUNET_OK !=
      TMH_plugin->insert_refresh_order (TMH_plugin->cls,
                                        session,
                                        session_hash,
                                        num_new_denoms,
                                        denom_pubs))
  {
    TMH_plugin->rollback (TMH_plugin->cls,
                          session);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  for (i = 0; i < TALER_CNC_KAPPA; i++)
  {
    if (GNUNET_OK !=
        TMH_plugin->insert_refresh_commit_coins (TMH_plugin->cls,
                                                 session,
                                                 session_hash,
                                                 i,
                                                 num_new_denoms,
                                                 commit_coin[i]))
    {
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      return TMH_RESPONSE_reply_internal_db_error (connection);
    }
  }
  for (i = 0; i < TALER_CNC_KAPPA; i++)
  {
    if (GNUNET_OK !=
        TMH_plugin->insert_refresh_commit_links (TMH_plugin->cls,
                                                 session,
                                                 session_hash,
                                                 i,
                                                 coin_count,
                                                 commit_link[i]))
    {
      TMH_plugin->rollback (TMH_plugin->cls,
                            session);
      return TMH_RESPONSE_reply_internal_db_error (connection);
    }
  }

  if (GNUNET_OK !=
      TMH_plugin->commit (TMH_plugin->cls,
                          session))
  {
    TALER_LOG_WARNING ("/refresh/melt transaction commit failed\n");
    return TMH_RESPONSE_reply_commit_error (connection);
  }
  return TMH_RESPONSE_reply_refresh_melt_success (connection,
                                                  session_hash,
                                                  refresh_session.noreveal_index);
}


/**
 * Send an error response with the details of the original melt
 * commitment and the location of the mismatch.
 *
 * @param connection the MHD connection to handle
 * @param session database connection to use
 * @param session_hash hash of session to query
 * @param off commitment offset to check
 * @param index index of the mismatch
 * @param object_name name of the object with the problem
 * @return #GNUNET_NO if we generated the error message
 *         #GNUNET_SYSERR if we could not even generate an error message
 */
static int
send_melt_commitment_error (struct MHD_Connection *connection,
                            struct TALER_MINTDB_Session *session,
                            const struct GNUNET_HashCode *session_hash,
                            unsigned int off,
                            unsigned int index,
                            const char *object_name)
{
  struct TALER_MINTDB_MeltCommitment *mc;
  int ret;

  mc = TMH_plugin->get_melt_commitment (TMH_plugin->cls,
                                        session,
                                        session_hash);
  if (NULL == mc)
  {
    GNUNET_break (0);
    return (MHD_YES ==
            TMH_RESPONSE_reply_internal_error (connection,
                                               "Melt commitment assembly"))
      ? GNUNET_NO : GNUNET_SYSERR;
  }
  ret = (MHD_YES ==
         TMH_RESPONSE_reply_refresh_reveal_missmatch (connection,
                                                      mc,
                                                      off,
                                                      index,
                                                      object_name))
    ? GNUNET_NO : GNUNET_SYSERR;
  TMH_plugin->free_melt_commitment (TMH_plugin->cls,
                                    mc);
  return ret;
}


/**
 * Check if the given @a transfer_privs correspond to an honest
 * commitment for the given session.
 * Checks that the transfer private keys match their commitments.
 * Then derives the shared secret for each #TALER_CNC_KAPPA, and check that they match.
 *
 * @param connection the MHD connection to handle
 * @param session database connection to use
 * @param session_hash hash of session to query
 * @param off commitment offset to check
 * @param num_oldcoins size of the @a transfer_privs and @a melts arrays
 * @param transfer_privs private transfer keys
 * @param melts array of melted coins
 * @param num_newcoins number of newcoins being generated
 * @param denom_pubs array of @a num_newcoins keys for the new coins
 * @return #GNUNET_OK if the committment was honest,
 *         #GNUNET_NO if there was a problem and we generated an error message
 *         #GNUNET_SYSERR if we could not even generate an error message
 */
static int
check_commitment (struct MHD_Connection *connection,
                  struct TALER_MINTDB_Session *session,
                  const struct GNUNET_HashCode *session_hash,
                  unsigned int off,
                  unsigned int num_oldcoins,
                  const struct TALER_TransferPrivateKeyP *transfer_privs,
                  const struct TALER_MINTDB_RefreshMelt *melts,
                  unsigned int num_newcoins,
                  const struct TALER_DenominationPublicKey *denom_pubs)
{
  unsigned int j;
  struct TALER_LinkSecretP last_shared_secret;
  int secret_initialized = GNUNET_NO;
  struct TALER_MINTDB_RefreshCommitLinkP *commit_links;
  struct TALER_MINTDB_RefreshCommitCoin *commit_coins;

  commit_links = GNUNET_malloc (num_oldcoins *
                                sizeof (struct TALER_MINTDB_RefreshCommitLinkP));
  if (GNUNET_OK !=
      TMH_plugin->get_refresh_commit_links (TMH_plugin->cls,
                                            session,
                                            session_hash,
                                            off,
                                            num_oldcoins,
                                            commit_links))
  {
    GNUNET_break (0);
    GNUNET_free (commit_links);
    return (MHD_YES == TMH_RESPONSE_reply_internal_db_error (connection))
        ? GNUNET_NO : GNUNET_SYSERR;
  }

  for (j = 0; j < num_oldcoins; j++)
  {
    struct TALER_LinkSecretP shared_secret;
    struct TALER_TransferPublicKeyP transfer_pub_check;

    GNUNET_CRYPTO_ecdhe_key_get_public (&transfer_privs[j].ecdhe_priv,
                                        &transfer_pub_check.ecdhe_pub);
    if (0 !=
        memcmp (&transfer_pub_check,
                &commit_links[j].transfer_pub,
                sizeof (struct TALER_TransferPublicKeyP)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "transfer keys do not match\n");
      GNUNET_free (commit_links);
      return send_melt_commitment_error (connection,
                                         session,
                                         session_hash,
                                         off,
                                         j,
                                         "transfer key");
    }

    if (GNUNET_OK !=
	TALER_link_decrypt_secret (&commit_links[j].shared_secret_enc,
				   &transfer_privs[j],
				   &melts[j].coin.coin_pub,
				   &shared_secret))
    {
      GNUNET_free (commit_links);
      return (MHD_YES ==
	      TMH_RESPONSE_reply_internal_error (connection,
						 "Transfer secret decryption error"))
	? GNUNET_NO : GNUNET_SYSERR;
    }
    if (GNUNET_NO == secret_initialized)
    {
      secret_initialized = GNUNET_YES;
      last_shared_secret = shared_secret;
    }
    else if (0 != memcmp (&shared_secret,
                          &last_shared_secret,
                          sizeof (struct GNUNET_HashCode)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "shared secrets do not match\n");
      GNUNET_free (commit_links);
      return send_melt_commitment_error (connection,
                                         session,
                                         session_hash,
                                         off,
                                         j,
                                         "transfer secret");
    }
  }
  GNUNET_break (GNUNET_YES == secret_initialized);
  GNUNET_free (commit_links);

  /* Check that the commitments for all new coins were correct */
  commit_coins = GNUNET_malloc (num_newcoins *
                                sizeof (struct TALER_MINTDB_RefreshCommitCoin));

  if (GNUNET_OK !=
      TMH_plugin->get_refresh_commit_coins (TMH_plugin->cls,
                                            session,
                                            session_hash,
                                            off,
                                            num_newcoins,
                                            commit_coins))
  {
    GNUNET_break (0);
    GNUNET_free (commit_coins);
    return (MHD_YES == TMH_RESPONSE_reply_internal_db_error (connection))
        ? GNUNET_NO : GNUNET_SYSERR;
  }

  for (j = 0; j < num_newcoins; j++)
  {
    struct TALER_RefreshLinkDecrypted *link_data;
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct GNUNET_HashCode h_msg;
    char *buf;
    size_t buf_len;

    link_data = TALER_refresh_decrypt (commit_coins[j].refresh_link,
                                       &last_shared_secret);
    if (NULL == link_data)
    {
      GNUNET_break (0);
      GNUNET_free (commit_coins);
      return (MHD_YES == TMH_RESPONSE_reply_internal_error (connection,
                                                            "Decryption error"))
          ? GNUNET_NO : GNUNET_SYSERR;
    }

    GNUNET_CRYPTO_eddsa_key_get_public (&link_data->coin_priv.eddsa_priv,
                                        &coin_pub.eddsa_pub);
    GNUNET_CRYPTO_hash (&coin_pub,
                        sizeof (struct TALER_CoinSpendPublicKeyP),
                        &h_msg);
    if (0 == (buf_len =
              GNUNET_CRYPTO_rsa_blind (&h_msg,
                                       link_data->blinding_key.rsa_blinding_key,
                                       denom_pubs[j].rsa_public_key,
                                       &buf)))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "blind failed\n");
      GNUNET_free (commit_coins);
      return (MHD_YES == TMH_RESPONSE_reply_internal_error (connection,
                                                            "Blinding error"))
          ? GNUNET_NO : GNUNET_SYSERR;
    }

    if ( (buf_len != commit_coins[j].coin_ev_size) ||
         (0 != memcmp (buf,
                       commit_coins[j].coin_ev,
                       buf_len)) )
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "blind envelope does not match for k=%u, old=%d\n",
                  off,
                  (int) j);
      GNUNET_free (commit_coins);
      return send_melt_commitment_error (connection,
                                         session,
                                         session_hash,
                                         off,
                                         j,
                                         "envelope");
    }
    GNUNET_free (buf);
  }
  GNUNET_free (commit_coins);

  return GNUNET_OK;
}


/**
 * Mint a coin as part of a refresh operation.  Obtains the
 * envelope from the database and performs the signing operation.
 *
 * @param connection the MHD connection to handle
 * @param session database connection to use
 * @param session_hash hash of session to query
 * @param key_state key state to lookup denomination pubs
 * @param denom_pub denomination key for the coin to create
 * @param commit_coin the coin that was committed
 * @param coin_off number of the coin
 * @return NULL on error, otherwise signature over the coin
 */
static struct TALER_DenominationSignature
refresh_mint_coin (struct MHD_Connection *connection,
                   struct TALER_MINTDB_Session *session,
                   const struct GNUNET_HashCode *session_hash,
                   struct TMH_KS_StateHandle *key_state,
                   const struct TALER_DenominationPublicKey *denom_pub,
                   const struct TALER_MINTDB_RefreshCommitCoin *commit_coin,
                   unsigned int coin_off)
{
  struct TALER_MINTDB_DenominationKeyIssueInformation *dki;
  struct TALER_DenominationSignature ev_sig;

  dki = TMH_KS_denomination_key_lookup (key_state,
                                        denom_pub,
					TMH_KS_DKU_WITHDRAW);
  if (NULL == dki)
  {
    GNUNET_break (0);
    ev_sig.rsa_signature = NULL;
    return ev_sig;
  }
  ev_sig.rsa_signature
      = GNUNET_CRYPTO_rsa_sign (dki->denom_priv.rsa_private_key,
                                commit_coin->coin_ev,
                                commit_coin->coin_ev_size);
  if (NULL == ev_sig.rsa_signature)
  {
    GNUNET_break (0);
    return ev_sig;
  }
  if (GNUNET_OK !=
      TMH_plugin->insert_refresh_out (TMH_plugin->cls,
                                              session,
                                              session_hash,
                                              coin_off,
                                              &ev_sig))
  {
    GNUNET_break (0);
    GNUNET_CRYPTO_rsa_signature_free (ev_sig.rsa_signature);
    ev_sig.rsa_signature = NULL;
  }
  return ev_sig;
}


/**
 * Execute a "/refresh/reveal".  The client is revealing to us the
 * transfer keys for @a #TALER_CNC_KAPPA-1 sets of coins.  Verify that the
 * revealed transfer keys would allow linkage to the blinded coins,
 * and if so, return the signed coins for corresponding to the set of
 * coins that was not chosen.
 *
 * @param connection the MHD connection to handle
 * @param session_hash hash identifying the refresh session
 * @param num_oldcoins size of y-dimension of @a transfer_privs array
 * @param transfer_privs array with the revealed transfer keys,
 *                      x-dimension must be #TALER_CNC_KAPPA - 1
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_reveal (struct MHD_Connection *connection,
                               const struct GNUNET_HashCode *session_hash,
                               unsigned int num_oldcoins,
                               struct TALER_TransferPrivateKeyP **transfer_privs)
{
  int res;
  struct TALER_MINTDB_Session *session;
  struct TALER_MINTDB_RefreshSession refresh_session;
  struct TMH_KS_StateHandle *key_state;
  struct TALER_MINTDB_RefreshMelt *melts;
  struct TALER_DenominationPublicKey *denom_pubs;
  struct TALER_DenominationSignature *ev_sigs;
  struct TALER_MINTDB_RefreshCommitCoin *commit_coins;
  unsigned int i;
  unsigned int j;
  unsigned int off;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls,
                                                  TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  res = TMH_plugin->get_refresh_session (TMH_plugin->cls,
                                         session,
                                         session_hash,
                                         &refresh_session);
  if (GNUNET_NO == res)
    return TMH_RESPONSE_reply_arg_invalid (connection,
                                           "session_hash");
  if (GNUNET_SYSERR == res)
    return TMH_RESPONSE_reply_internal_db_error (connection);
  if (0 == refresh_session.num_oldcoins)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  melts = GNUNET_malloc (refresh_session.num_oldcoins *
                         sizeof (struct TALER_MINTDB_RefreshMelt));
  for (j=0;j<refresh_session.num_oldcoins;j++)
  {
    if (GNUNET_OK !=
        TMH_plugin->get_refresh_melt (TMH_plugin->cls,
                                      session,
                                      session_hash,
                                      j,
                                      &melts[j]))
    {
      GNUNET_break (0);
      GNUNET_free (melts);
      return TMH_RESPONSE_reply_internal_db_error (connection);
    }
  }
  denom_pubs = GNUNET_malloc (refresh_session.num_newcoins *
                              sizeof (struct TALER_DenominationPublicKey));
  if (GNUNET_OK !=
      TMH_plugin->get_refresh_order (TMH_plugin->cls,
                                     session,
                                     session_hash,
                                     refresh_session.num_newcoins,
                                     denom_pubs))
  {
    GNUNET_break (0);
    GNUNET_free (denom_pubs);
    GNUNET_free (melts);
    return (MHD_YES == TMH_RESPONSE_reply_internal_db_error (connection))
        ? GNUNET_NO : GNUNET_SYSERR;
  }


  off = 0;
  for (i=0;i<TALER_CNC_KAPPA - 1;i++)
  {
    if (i == refresh_session.noreveal_index)
      off = 1;
    if (GNUNET_OK !=
        (res = check_commitment (connection,
                                 session,
                                 session_hash,
                                 i + off,
                                 refresh_session.num_oldcoins,
                                 transfer_privs[i + off],
                                 melts,
                                 refresh_session.num_newcoins,
                                 denom_pubs)))
    {
      for (j=0;j<refresh_session.num_newcoins;j++)
        GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
      GNUNET_free (denom_pubs);
      GNUNET_free (melts);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
  }
  GNUNET_free (melts);

  /* Client request OK, start transaction */
  if (GNUNET_OK !=
      TMH_plugin->start (TMH_plugin->cls,
                         session))
  {
    GNUNET_break (0);
    for (j=0;j<refresh_session.num_newcoins;j++)
      GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
    GNUNET_free (denom_pubs);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }

  commit_coins = GNUNET_malloc (refresh_session.num_newcoins *
                                sizeof (struct TALER_MINTDB_RefreshCommitCoin));
  if (GNUNET_OK !=
      TMH_plugin->get_refresh_commit_coins (TMH_plugin->cls,
                                            session,
                                            session_hash,
                                            refresh_session.noreveal_index,
                                            refresh_session.num_newcoins,
                                            commit_coins))
  {
    GNUNET_break (0);
    GNUNET_free (commit_coins);
    for (j=0;j<refresh_session.num_newcoins;j++)
      GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
    GNUNET_free (denom_pubs);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ev_sigs = GNUNET_malloc (refresh_session.num_newcoins *
                           sizeof (struct TALER_DenominationSignature));
  key_state = TMH_KS_acquire ();
  for (j=0;j<refresh_session.num_newcoins;j++)
  {
    ev_sigs[j] = refresh_mint_coin (connection,
                                    session,
                                    session_hash,
                                    key_state,
                                    &denom_pubs[j],
                                    &commit_coins[j],
                                    j);
    if (NULL == ev_sigs[j].rsa_signature)
    {
      TMH_KS_release (key_state);
      for (i=0;i<j;i++)
        GNUNET_CRYPTO_rsa_signature_free (ev_sigs[i].rsa_signature);
      GNUNET_free (ev_sigs);
      for (j=0;j<refresh_session.num_newcoins;j++)
        GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
      GNUNET_free (denom_pubs);
      GNUNET_free (commit_coins);
      return TMH_RESPONSE_reply_internal_db_error (connection);
    }
  }
  TMH_KS_release (key_state);
  for (j=0;j<refresh_session.num_newcoins;j++)
    GNUNET_CRYPTO_rsa_public_key_free (denom_pubs[j].rsa_public_key);
  GNUNET_free (denom_pubs);
  GNUNET_free (commit_coins);

  if (GNUNET_OK !=
      TMH_plugin->commit (TMH_plugin->cls,
                          session))
  {
    TALER_LOG_WARNING ("/refresh/reveal transaction commit failed\n");
    for (i=0;i<refresh_session.num_newcoins;i++)
      GNUNET_CRYPTO_rsa_signature_free (ev_sigs[i].rsa_signature);
    GNUNET_free (ev_sigs);
    return TMH_RESPONSE_reply_commit_error (connection);
  }

  res = TMH_RESPONSE_reply_refresh_reveal_success (connection,
                                                   refresh_session.num_newcoins,
                                                   ev_sigs);
  for (i=0;i<refresh_session.num_newcoins;i++)
    GNUNET_CRYPTO_rsa_signature_free (ev_sigs[i].rsa_signature);
  GNUNET_free (ev_sigs);
  return res;
}


/**
 * Closure for #handle_transfer_data().
 */
struct HTD_Context
{

  /**
   * Session link data we collect.
   */
  struct TMH_RESPONSE_LinkSessionInfo *sessions;

  /**
   * Database session. Nothing to do with @a sessions.
   */
  struct TALER_MINTDB_Session *session;

  /**
   * MHD connection, for queueing replies.
   */
  struct MHD_Connection *connection;

  /**
   * Number of sessions the coin was melted into.
   */
  unsigned int num_sessions;

  /**
   * How are we expected to proceed. #GNUNET_SYSERR if we
   * failed to return an error (should return #MHD_NO).
   * #GNUNET_NO if we succeeded in queueing an MHD error
   * (should return #MHD_YES from #TMH_execute_refresh_link),
   * #GNUNET_OK if we should call #TMH_RESPONSE_reply_refresh_link_success().
   */
  int status;
};


/**
 * Function called with the session hashes and transfer secret
 * information for a given coin.  Gets the linkage data and
 * builds the reply for the client.
 *
 *
 * @param cls closure, a `struct HTD_Context`
 * @param session_hash a session the coin was melted in
 * @param transfer_pub public transfer key for the session
 * @param shared_secret_enc set to shared secret for the session
 */
static void
handle_transfer_data (void *cls,
                      const struct GNUNET_HashCode *session_hash,
                      const struct TALER_TransferPublicKeyP *transfer_pub,
                      const struct TALER_EncryptedLinkSecretP *shared_secret_enc)
{
  struct HTD_Context *ctx = cls;
  struct TALER_MINTDB_LinkDataList *ldl;
  struct TMH_RESPONSE_LinkSessionInfo *lsi;

  if (GNUNET_OK != ctx->status)
    return;
  ldl = TMH_plugin->get_link_data_list (TMH_plugin->cls,
                                        ctx->session,
                                        session_hash);
  if (NULL == ldl)
  {
    GNUNET_break (0);
    ctx->status = GNUNET_NO;
    if (MHD_NO ==
        TMH_RESPONSE_reply_json_pack (ctx->connection,
                                      MHD_HTTP_NOT_FOUND,
                                      "{s:s}",
                                      "error",
                                      "link data not found (link)"))
      ctx->status = GNUNET_SYSERR;
    return;
  }
  GNUNET_array_grow (ctx->sessions,
                     ctx->num_sessions,
                     ctx->num_sessions + 1);
  lsi = &ctx->sessions[ctx->num_sessions - 1];
  lsi->transfer_pub = *transfer_pub;
  lsi->shared_secret_enc = *shared_secret_enc;
  lsi->ldl = ldl;
}


/**
 * Execute a "/refresh/link".  Returns the linkage information that
 * will allow the owner of a coin to follow the refresh trail to
 * the refreshed coin.
 *
 * @param connection the MHD connection to handle
 * @param coin_pub public key of the coin to link
 * @return MHD result code
 */
int
TMH_DB_execute_refresh_link (struct MHD_Connection *connection,
                             const struct TALER_CoinSpendPublicKeyP *coin_pub)
{
  struct HTD_Context ctx;
  int res;
  unsigned int i;

  if (NULL == (ctx.session = TMH_plugin->get_session (TMH_plugin->cls,
                                                      TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ctx.connection = connection;
  ctx.num_sessions = 0;
  ctx.sessions = NULL;
  ctx.status = GNUNET_OK;
  res = TMH_plugin->get_transfer (TMH_plugin->cls,
                                  ctx.session,
                                  coin_pub,
                                  &handle_transfer_data,
                                  &ctx);
  if (GNUNET_SYSERR == ctx.status)
  {
    res = MHD_NO;
    goto cleanup;
  }
  if (GNUNET_NO == ctx.status)
  {
    res = MHD_YES;
    goto cleanup;
  }
  GNUNET_assert (GNUNET_OK == ctx.status);
  if (0 == ctx.num_sessions)
    return TMH_RESPONSE_reply_arg_unknown (connection,
                                           "coin_pub");
  res = TMH_RESPONSE_reply_refresh_link_success (connection,
                                                 ctx.num_sessions,
                                                 ctx.sessions);
 cleanup:
  for (i=0;i<ctx.num_sessions;i++)
    TMH_plugin->free_link_data_list (TMH_plugin->cls,
                                     ctx.sessions[i].ldl);
  GNUNET_free (ctx.sessions);
  return res;
}


/**
 * Add an incoming transaction to the database.  Checks if the
 * transaction is fresh (not a duplicate) and if so adds it to
 * the database.
 *
 * @param connection the MHD connection to handle
 * @param reserve_pub public key of the reserve
 * @param amount amount to add to the reserve
 * @param execution_time when did we receive the wire transfer
 * @param wire details about the wire transfer
 * @return MHD result code
 */
int
TMH_DB_execute_admin_add_incoming (struct MHD_Connection *connection,
                                   const struct TALER_ReservePublicKeyP *reserve_pub,
                                   const struct TALER_Amount *amount,
                                   struct GNUNET_TIME_Absolute execution_time,
                                   json_t *wire)
{
  struct TALER_MINTDB_Session *session;
  int ret;

  if (NULL == (session = TMH_plugin->get_session (TMH_plugin->cls,
                                                  TMH_test_mode)))
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  ret = TMH_plugin->reserves_in_insert (TMH_plugin->cls,
                                        session,
                                        reserve_pub,
                                        amount,
                                        execution_time,
                                        wire);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_db_error (connection);
  }
  return TMH_RESPONSE_reply_json_pack (connection,
                                       MHD_HTTP_OK,
                                       "{s:s}",
                                       "status",
                                       (GNUNET_OK == ret)
                                       ? "NEW"
                                       : "DUP");
}


/* end of taler-mint-httpd_db.c */
