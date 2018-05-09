/*
  This file is part of TALER
  (C) 2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file include/taler_testing_lib.h
 * @brief API for writing an interpreter to test Taler components
 * @author Christian Grothoff <christian@grothoff.org>
 * @author Marcello Stanisci
 */
#ifndef TALER_MERCHANT_TESTING_LIB_H
#define TALER_MERCHANT_TESTING_LIB_H

#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"

/* ********************* Helper functions ********************* */


#define MERCHANT_FAIL() \
  do {GNUNET_break (0); return NULL; } while (0)


#define CMD_NOT_FOUND "Command not found"
#define TRAIT_NOT_FOUND "Trait not found"

/**
 * Prepare the merchant execution.  Create tables and check if
 * the port is available.
 *
 * @param config_filename configuration filename.
 *
 * @return the base url, or NULL upon errors.  Must be freed
 *         by the caller.
 */
char *
TALER_TESTING_prepare_merchant (const char *config_filename);

/**
 * Start the merchant backend process.  Assume the port
 * is available and the database is clean.  Use the "prepare
 * merchant" function to do such tasks.
 *
 * @param config_filename configuration filename.
 *
 * @return the process, or NULL if the process could not
 *         be started.
 */
struct GNUNET_OS_Process *
TALER_TESTING_run_merchant (const char *config_filename,
                            const char *merchant_url);

/* ******************* Generic interpreter logic ************ */

/* ************** Specific interpreter commands ************ */

/**
 * Make the /proposal command.
 *
 * @param label command label
 * @param merchant_url merchant base url.
 * @param ctx context
 * @param http_status HTTP status code.
 * @param order the order
 * @param instance the merchant instance
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_proposal (const char *label,
                            const char *merchant_url,
                            struct GNUNET_CURL_Context *ctx,
                            unsigned int http_status,
                            const char *order,
                            const char *instance);

/**
 * Make a "proposal lookup" command.
 *
 * @param label command label
 * @param http_status expected HTTP response code
 * @param proposal_reference reference to a proposal command
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_proposal_lookup
  (const char *label,
   struct GNUNET_CURL_Context *ctx,
   const char *merchant_url,
   unsigned int http_status,
   const char *proposal_reference,
   const char *order_id);

/**
 * Make a "check payment" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param ctx CURL context.
 * @param http_status expected HTTP response code.
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param expect_paid GNUNET_YES if we expect the proposal to be
 *        paid, GNUNET_NO otherwise.
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment (const char *label,
                                 const char *merchant_url,
                                 struct GNUNET_CURL_Context *ctx,
                                 unsigned int http_status,
                                 const char *proposal_reference,
                                 unsigned int expect_paid);

/**
 * Make a "pay" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param ctx CURL context.
 * @param http_status expected HTTP response code.
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param coin_reference reference to any command which is able
 *        to provide coins to use for paying.
 * @param amount_with_fee amount to pay, including the deposit
 *        fee
 * @param amount_without_fee amount to pay, no fees included.
 * @param refund_fee fee for refunding this payment.
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay (const char *label,
                       const char *merchant_url,
                       struct GNUNET_CURL_Context *ctx,
                       unsigned int http_status,
                       const char *proposal_reference,
                       const char *coin_reference,
                       const char *amount_with_fee,
                       const char *amount_without_fee,
                       const char *refund_fee);

/**
 * Make a "pay again" test command.
 *
 * @param label command label
 * @param merchant_url merchant base URL
 * @param pay_reference reference to the payment to replay
 * @param coin_reference reference to the coins to use
 * @param ctx main CURL context
 * @param http_status expected HTTP response code
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay_again (const char *label,
                             const char *merchant_url,
                             const char *pay_reference,
                             const char *coin_reference,
                             const char *refund_fee,
                             struct GNUNET_CURL_Context *ctx,
                             unsigned int http_status);
/**
 * Make a "pay abort" test command.
 *
 * @param label command label
 * @param merchant_url merchant base URL
 * @param pay_reference reference to the payment to abort
 * @param ctx main CURL context
 * @param http_status expected HTTP response code
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay_abort (const char *label,
                             const char *merchant_url,
                             const char *pay_reference,
                             struct GNUNET_CURL_Context *ctx,
                             unsigned int http_status);

/**
 * FIXME.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay_abort_refund
  (const char *label,
   struct TALER_EXCHANGE_Handle *exchange,
   const char *abort_reference,
   unsigned int num_coins,
   const char *refund_amount,
   const char *refund_fee,
   unsigned int http_status);

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   const char *increase_reference,
   const char *pay_reference,
   const char *order_id);


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_increase
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   const char *reason,
   const char *order_id,
   const char *refund_amount,
   const char *refund_fee);


/**
 * Make a "history" command.
 *
 * @param label command label
 * @param merchant_url merchant base URL
 * @param ctx main CURL context
 * @param http_status expected HTTP response code
 * @param time FIXME
 * @param nresult how many results are expected
 * @param start FIXME.
 * @param nrows how many row we want to receive, at most.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_history (const char *label,
                           const char *merchant_url,
                           struct GNUNET_CURL_Context *ctx,
                           unsigned int http_status,
                           struct GNUNET_TIME_Absolute time,
                           unsigned int nresult,
                           unsigned int start,
                           unsigned int nrows);

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transaction
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *transfer_reference,
   const char *pay_reference,
   const char *wire_fee);

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transfer
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *check_bank_reference,
   const char *pay_reference);

/* ****** Specific traits supported by this component ******* */

/**
 * Offer a merchant signature over a contract.
 *
 * @param index which signature to offer if there are multiple
 *        on offer
 * @param merchant_sig set to the offered signature.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_merchant_sig
  (unsigned int index,
   const struct TALER_MerchantSignatureP *merchant_sig);

/**
 * Obtain a merchant signature over a contract from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param merchant_sig[out] set to the wanted signature.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_merchant_sig
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   struct TALER_MerchantSignatureP **merchant_sig);


/**
 * Obtain a reference to a proposal command.  Any command that
 * works with proposals, might need to offer their reference to
 * it.  Notably, the "pay" command, offers its proposal reference
 * to the "pay abort" command as the latter needs to reconstruct
 * the same data needed by the former in order to use the "pay
 * abort" API.
 *
 * @param cmd command to extract trait from
 * @param index which reference to pick if @a cmd has multiple
 *        on offer
 * @param proposal_reference[out] set to the wanted reference.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_proposal_reference
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const char **proposal_reference);

/**
 * Offer a proposal reference.
 *
 * @param index which reference to offer if there are
 *        multiple on offer
 * @param proposal_reference set to the offered reference.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_proposal_reference
  (unsigned int index,
   const char *proposal_reference);

/**
 * Offer a coin reference.
 *
 * @param index which reference to offer if there are
 *        multiple on offer
 * @param coin_reference set to the offered reference.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_coin_reference
  (unsigned int index,
   const char *coin_reference);

/**
 * Obtain a reference to any command that can provide coins as
 * traits.
 *
 * @param cmd command to extract trait from
 * @param index which reference to pick if @a cmd has multiple
 *        on offer
 * @param coin_reference[out] set to the wanted reference. NOTE:
 *        a _single_ reference can contain _multiple_ token,
 *        using semi-colon as separator.  For example, a _single_
 *        reference can be this: "coin-ref-1", or even this:
 *        "coin-ref-1;coin-ref-2".  The "pay" command contains
 *        functions that can parse such format.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_coin_reference
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const char **coin_reference);


/**
 * Obtain planchet secrets from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param planchet_secrets[out] set to the wanted secrets.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_planchet_secrets
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   struct TALER_PlanchetSecretsP **planchet_secrets);


/**
 * Offer planchet secrets.
 *
 * @param index which secrets to offer if there are multiple
 *        on offer
 * @param planchet_secrets set to the offered secrets.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_planchet_secrets
  (unsigned int index,
   const struct TALER_PlanchetSecretsP *planchet_secrets);

/**
 * Offer tip id.
 *
 * @param index which tip id to offer if there are
 *        multiple on offer
 * @param planchet_secrets set to the offered secrets.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_tip_id
  (unsigned int index,
   const struct GNUNET_HashCode *tip_id);


/**
 * Obtain tip id from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param tip_id[out] set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_tip_id
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const struct GNUNET_HashCode **tip_id);

/**
 * Offer contract terms hash code.
 *
 * @param index which hash code to offer if there are
 *        multiple on offer
 * @param h_contract_terms set to the offered hash code.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_h_contract_terms
  (unsigned int index,
   const struct GNUNET_HashCode *h_contract_terms);

/**
 * Obtain contract terms hash from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which hash code to pick if @a cmd has multiple
 *        on offer
 * @param h_contract_terms[out] set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_h_contract_terms
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const struct GNUNET_HashCode **h_contract_terms);


/**
 * Offer refund entry.
 *
 * @param index which tip id to offer if there are
 *        multiple on offer
 * @param refund_entry set to the offered refund entry.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_refund_entry
  (unsigned int index,
   const struct TALER_MERCHANT_RefundEntry *refund_entry);


/**
 * Obtain refund entry from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param refund_entry[out] set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_refund_entry
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const struct TALER_MERCHANT_RefundEntry **refund_entry);


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize_with_ec
  (const char *label,
   const char *merchant_url,
   const char *exchange_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *instance,
   const char *justification,
   const char *amount,
   enum TALER_ErrorCode ec);


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize (const char *label,
                                 const char *merchant_url,
                                 const char *exchange_url,
                                 struct GNUNET_CURL_Context *ctx,
                                 unsigned int http_status,
                                 const char *instance,
                                 const char *justification,
                                 const char *amount);

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query (const char *label,
                             const char *merchant_url,
                             struct GNUNET_CURL_Context *ctx,
                             unsigned int http_status,
                             const char *instance);

/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query_with_amounts
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *instance,
   const char *expected_amount_picked_up,
   const char *expected_amount_authorized,
   const char *expected_amount_available);


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup_with_ec
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *authorize_reference,
   const char **amounts,
   struct TALER_EXCHANGE_Handle *exchange,
   enum TALER_ErrorCode ec);


/**
 * FIXME
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *authorize_reference,
   const char **amounts,
   struct TALER_EXCHANGE_Handle *exchange);

/**
 * Make the instruction pointer point to @a new_ip
 * only if @a counter is greater than zero.
 *
 * @param label command label
 * @param new_ip new instruction pointer's value.  Note that,
 * when the next instruction will be called, the interpreter
 * will increment the ip under the hood so this value must be
 * set to the index of the instruction we want to execute next
 * MINUS one.
 * @param counter counts how many times the rewinding has
 * to happen.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_rewind_ip
  (const char *label,
   int new_ip,
   unsigned int *counter);

#endif
