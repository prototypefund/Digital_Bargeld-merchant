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
 * @param merchant_url merchant base URL, used to check
 *        if the merchant was started right.
 *
 * @return the process, or NULL if the process could not
 *         be started.
 */
struct GNUNET_OS_Process *
TALER_TESTING_run_merchant (const char *config_filename,
                            const char *merchant_url);

/* ************** Specific interpreter commands ************ */


/**
 * Define a "config" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        "config" request.
 * @param http_code expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_config (const char *label,
                          const char *merchant_url,
                          unsigned int http_code);


/**
 * Make the "proposal" command.
 *
 * @param label command label
 * @param merchant_url base URL of the merchant serving
 *        the proposal request.
 * @param http_status expected HTTP status.
 * @param order the order to PUT to the merchant.
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_proposal (const char *label,
                            const char *merchant_url,
                            unsigned int http_status,
                            const char *order);

/**
 * Make a "proposal lookup" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant backend
 *        serving the proposal lookup request.
 * @param http_status expected HTTP response code.
 * @param proposal_reference reference to a "proposal" CMD.
 * @param order_id order id to lookup, can be NULL.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_proposal_lookup (const char *label,
                                   const char *merchant_url,
                                   unsigned int http_status,
                                   const char *proposal_reference,
                                   const char *order_id);

/**
 * Make a "check payment" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param http_status expected HTTP response code.
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param expect_paid #GNUNET_YES if we expect the proposal to be
 *        paid, #GNUNET_NO otherwise.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment (const char *label,
                                 const char *merchant_url,
                                 unsigned int http_status,
                                 const char *proposal_reference,
                                 unsigned int expect_paid);


/**
 * Make a "check payment" test command with long polling support.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param timeout how long to wait during long polling for the reply
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment_start (const char *label,
                                       const char *merchant_url,
                                       const char *proposal_reference,
                                       struct GNUNET_TIME_Relative timeout);


/**
 * Expect completion of a long-polled "check payment" test command.
 *
 * @param label command label.
 * @param check_start_reference payment start operation that should have
 *                   completed
 * @param http_status expected HTTP response code.
 * @param expect_paid #GNUNET_YES if we expect the proposal to be
 *        paid, #GNUNET_NO otherwise.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_check_payment_conclude (const char *label,
                                          unsigned int http_status,
                                          const char *poll_start_reference,
                                          unsigned int expect_paid);


/**
 * Start a long-polled "poll-payment" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param min_refund minimum refund to wait for
 * @param timeout which timeout to use
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_poll_payment_start (const char *label,
                                      const char *merchant_url,
                                      const char *proposal_reference,
                                      const char *min_refund,
                                      struct GNUNET_TIME_Relative timeout);


/**
 * Expect completion of a long-polled "poll payment" test command.
 *
 * @param label command label.
 * @param poll_start_reference payment start operation that should have
 *                   completed
 * @param http_status expected HTTP response code.
 * @param expect_paid #GNUNET_YES if we expect the proposal to be
 *        paid, #GNUNET_NO otherwise.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_poll_payment_conclude (const char *label,
                                         unsigned int http_status,
                                         const char *poll_start_reference,
                                         unsigned int expect_paid);


/**
 * Make a "pay" test command.
 *
 * @param label command label.
 * @param merchant_url merchant base url
 * @param http_status expected HTTP response code.
 * @param proposal_reference the proposal whose payment status
 *        is going to be checked.
 * @param coin_reference reference to any command which is able
 *        to provide coins to use for paying.
 * @param amount_with_fee amount to pay, including the deposit
 *        fee
 * @param amount_without_fee amount to pay, no fees included.
 * @param refund_fee fee for refunding this payment.
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay (const char *label,
                       const char *merchant_url,
                       unsigned int http_status,
                       const char *proposal_reference,
                       const char *coin_reference,
                       const char *amount_with_fee,
                       const char *amount_without_fee,
                       const char *refund_fee);

/**
 * Make a "pay again" test command.  Its purpose is to
 * take all the data from a aborted "pay" CMD, and use
 * good coins - found in @a coin_reference - to correctly
 * pay for it.
 *
 * @param label command label
 * @param merchant_url merchant base URL
 * @param pay_reference reference to the payment to replay
 * @param coin_reference reference to the coins to use
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
                             unsigned int http_status);

/**
 * Make a "pay abort" test command.
 *
 * @param label command label
 * @param merchant_url merchant base URL
 * @param pay_reference reference to the payment to abort
 * @param http_status expected HTTP response code
 *
 * @return the command
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay_abort (const char *label,
                             const char *merchant_url,
                             const char *pay_reference,
                             unsigned int http_status);

/**
 * Make a "pay abort refund" CMD.  This command uses the
 * refund permission from a "pay abort" CMD, and redeems it
 * at the exchange.
 *
 * @param label command label.
 * @param abort_reference reference to the "pay abort" CMD that
 *        will offer the refund permission.
 * @param num_coins how many coins are expected to be refunded.
 * @param refund_amount the amount we are going to redeem as
 *        refund.
 * @param refund_fee the refund fee (FIXME: who pays it?)
 * @param http_status expected HTTP response code.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_pay_abort_refund (const char *label,
                                    const char *abort_reference,
                                    unsigned int num_coins,
                                    const char *refund_amount,
                                    const char *refund_fee,
                                    unsigned int http_status);

/**
 * Define a "refund lookup" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        "refund lookup" request.
 * @param increase_reference reference to a "refund increase" CMD
 *        that will offer the amount to check the looked up refund
 *        against.  Must NOT be NULL.
 * @param pay_reference reference to the "pay" CMD whose coins got
 *        refunded.  It is used to double-check if the refunded
 *        coins were actually spent in the first place.
 * @param order_id order id whose refund status is to be looked up.
 * @param http_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup (const char *label,
                                 const char *merchant_url,
                                 const char *increase_reference,
                                 const char *pay_reference,
                                 const char *order_id,
                                 unsigned int http_code);

/**
 * Define a "refund lookup" CMD, equipped with a expected refund
 * amount.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        "refund lookup" request.
 * @param increase_reference reference to a "refund increase" CMD
 *        that will offer the amount to check the looked up refund
 *        against.  Can be NULL, takes precedence over @a
 *        refund_amount.
 * @param pay_reference reference to the "pay" CMD whose coins got
 *        refunded.  It is used to double-check if the refunded
 *        coins were actually spent in the first place.
 * @param order_id order id whose refund status is to be looked up.
 * @param http_code expected HTTP response code.
 * @param refund_amount expected refund amount.  Must be defined
 *        if @a increase_reference is NULL.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_lookup_with_amount (const char *label,
                                             const char *merchant_url,
                                             const char *increase_reference,
                                             const char *pay_reference,
                                             const char *order_id,
                                             unsigned int http_code,
                                             const char *refund_amount);


/**
 * Define a "refund increase" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the backend serving the
 *        "refund increase" request.
 * @param reason refund justification, human-readable.
 * @param order_id order id of the contract to refund.
 * @param refund_amount amount to be refund-increased.
 * @param refund_fee refund fee.
 * @param http_code expected HTTP response code.
 *
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_refund_increase (const char *label,
                                   const char *merchant_url,
                                   const char *reason,
                                   const char *order_id,
                                   const char *refund_amount,
                                   const char *refund_fee,
                                   unsigned int http_code);

/**
 * Make a "history" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        request.
 * @param http_status expected HTTP response code
 * @param time limit towards the past for the history
 *        records we want returned.
 * @param nresult how many results are expected
 * @param nrows how many row we want to receive, at most.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_history_default_start (const char *label,
                                         const char *merchant_url,
                                         unsigned int http_status,
                                         struct GNUNET_TIME_Absolute time,
                                         unsigned int nresult,
                                         long long nrows);

/**
 * Make a "history" command.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        request.
 * @param http_status expected HTTP response code
 * @param time limit towards the past for the history
 *        records we want returned.
 * @param nresult how many results are expected
 * @param start first row id we want in the result.
 * @param nrows how many row we want to receive, at most.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_history (const char *label,
                           const char *merchant_url,
                           unsigned int http_status,
                           struct GNUNET_TIME_Absolute time,
                           unsigned int nresult,
                           unsigned long long start,
                           long long nrows);

/**
 * Define a "track transaction" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        /track/transaction request.
 * @param http_status expected HTTP response code.
 * @param pay_reference used to retrieve the order id to track.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transaction (const char *label,
                                              const char *merchant_url,
                                              unsigned int http_status,
                                              const char *pay_reference);

/**
 * Define a "track transfer" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        /track/transfer request.
 * @param http_status expected HTTP response code.
 * @param check_bank_reference reference to a "check bank" CMD
 *        that will provide the WTID and exchange URL to issue
 *        the track against.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transfer (const char *label,
                                           const char *merchant_url,
                                           unsigned int http_status,
                                           const char *check_bank_reference);

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
TALER_TESTING_make_trait_merchant_sig (
  unsigned int index,
  const struct TALER_MerchantSignatureP *merchant_sig);


/**
 * Obtain a merchant signature over a contract from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param merchant_sig[out] set to the wanted signature.
 *
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_merchant_sig (
  const struct TALER_TESTING_Command *cmd,
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
 * @param cmd command to extract the trait from.
 * @param index which reference to pick if @a cmd has multiple
 *        on offer.
 * @param[out] proposal_reference set to the wanted reference.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_proposal_reference (
  const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const char **proposal_reference);

/**
 * Offer a proposal reference.
 *
 * @param index which reference to offer if there are
 *        multiple on offer.
 * @param proposal_reference pointer to the reference to offer.
 *
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_proposal_reference (unsigned int index,
                                             const char *proposal_reference);

/**
 * Offer a coin reference.
 *
 * @param index which reference to offer if there are
 *        multiple on offer.
 * @param coin_reference set to the offered reference.
 *
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_coin_reference (unsigned int index,
                                         const char *coin_reference);

/**
 * Obtain a reference to any command that can provide coins as
 * traits.
 *
 * @param cmd command to extract trait from
 * @param index which reference to pick if @a cmd has multiple
 *        on offer
 * @param[out] coin_reference set to the wanted reference.
 *        NOTE: a _single_ reference can contain
 *        _multiple_ instances, using semi-colon as separator.
 *        For example, a _single_ reference can be this:
 *        "coin-ref-1", or even this: "coin-ref-1;coin-ref-2".
 *        The "pay" command contains functions that can parse
 *        such format.
 *
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_coin_reference (const struct TALER_TESTING_Command *cmd,
                                        unsigned int index,
                                        const char **coin_reference);


/**
 * Obtain planchet secrets from a @a cmd.
 *
 * @param cmd command to extract trait from.
 * @param index index of the trait.
 * @param planchet_secrets[out] set to the wanted secrets.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_planchet_secrets (
  const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  struct TALER_PlanchetSecretsP **planchet_secrets);

/**
 * Offer planchet secrets.
 *
 * @param index of the trait.
 * @param planchet_secrets set to the offered secrets.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_planchet_secrets (
  unsigned int index,
  const struct TALER_PlanchetSecretsP *planchet_secrets);

/**
 * Offer tip id.
 *
 * @param index which tip id to offer if there are
 *        multiple on offer.
 * @param tip_id set to the offered tip id.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_tip_id (unsigned int index,
                                 const struct GNUNET_HashCode *tip_id);

/**
 * Obtain tip id from a @a cmd.
 *
 * @param cmd command to extract the trait from.
 * @param index which tip id to pick if @a
 *        cmd has multiple on offer
 * @param[out] tip_id set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_tip_id (const struct TALER_TESTING_Command *cmd,
                                unsigned int index,
                                const struct GNUNET_HashCode **tip_id);


/**
 * Offer contract terms hash code.
 *
 * @param index which hashed contract terms to
 *        offer if there are multiple on offer
 * @param h_contract_terms set to the offered hashed
 *        contract terms.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_h_contract_terms (
  unsigned int index,
  const struct GNUNET_HashCode *h_contract_terms);


/**
 * Obtain contract terms hash from a @a cmd.
 *
 * @param cmd command to extract the trait from.
 * @param index index number of the trait to fetch.
 * @param[out] h_contract_terms set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_h_contract_terms (
  const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const struct GNUNET_HashCode **h_contract_terms);

/**
 * Offer refund entry.
 *
 * @param index index number of the trait to offer.
 * @param refund_entry set to the offered refund entry.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_refund_entry (
  unsigned int index,
  const struct TALER_MERCHANT_RefundEntry *refund_entry);


/**
 * Obtain refund entry from a @a cmd.
 *
 * @param cmd command to extract the trait from.
 * @param index the trait index.
 * @param[out] refund_entry set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_refund_entry (
  const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const struct TALER_MERCHANT_RefundEntry **refund_entry);

/**
 * Create a /tip-authorize CMD, specifying the Taler error code
 * that is expected to be returned by the backend.
 *
 * @param label this command label
 * @param merchant_url the base URL of the merchant that will
 *        serve the /tip-authorize request.
 * @param exchange_url the base URL of the exchange that owns
 *        the reserve from which the tip is going to be gotten.
 * @param http_status the HTTP response code which is expected
 *        for this operation.
 * @param justification human-readable justification for this
 *        tip authorization.
 * @param amount the amount to authorize for tipping.
 * @param ec expected Taler-defined error code.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize_with_ec (const char *label,
                                         const char *merchant_url,
                                         const char *exchange_url,
                                         unsigned int http_status,
                                         const char *justification,
                                         const char *amount,
                                         enum TALER_ErrorCode ec);


/**
 * This commands does not query the backend at all,
 * but just makes up a fake authorization id that will
 * be subsequently used by the "pick up" CMD in order
 * to test against such a case.
 *
 * @param label command label.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize_fake (const char *label);


/**
 * Create a /tip-authorize CMD.
 *
 * @param label this command label
 * @param merchant_url the base URL of the merchant that will
 *        serve the /tip-authorize request.
 * @param exchange_url the base URL of the exchange that owns
 *        the reserve from which the tip is going to be gotten.
 * @param http_status the HTTP response code which is expected
 *        for this operation.
 * @param justification human-readable justification for this
 *        tip authorization.
 * @param amount the amount to authorize for tipping.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_authorize (const char *label,
                                 const char *merchant_url,
                                 const char *exchange_url,
                                 unsigned int http_status,
                                 const char *justification,
                                 const char *amount);

/**
 * Define a /tip-query CMD.
 *
 * @param label the command label
 * @param merchant_url base URL of the merchant which will
 *        server the /tip-query request.
 * @param http_status expected HTTP response code for the
 *        /tip-query request.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query (const char *label,
                             const char *merchant_url,
                             unsigned int http_status);

/**
 * Define a /tip-query CMD equipped with a expected amount.
 *
 * @param label the command label
 * @param merchant_url base URL of the merchant which will
 *        server the /tip-query request.
 * @param http_status expected HTTP response code for the
 *        /tip-query request.
 * @param expected_amount_picked_up expected amount already
 *        picked up.
 * @param expected_amount_authorized expected amount that was
 *        authorized in the first place.
 * @param expected_amount_available FIXME what is this?
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_query_with_amounts (const char *label,
                                          const char *merchant_url,
                                          unsigned int http_status,
                                          const char *expected_amount_picked_up,
                                          const char *expected_amount_authorized,
                                          const char *expected_amount_available);

/**
 * Define a /tip-pickup CMD, equipped with the expected error
 * code.
 *
 * @param label the command label
 * @param merchant_url base URL of the backend which will serve
 *        the /tip-pickup request.
 * @param http_status expected HTTP response code.
 * @param authorize_reference reference to a /tip-autorize CMD
 *        that offers a tip id to pick up.
 * @param amounts array of string-defined amounts that specifies
 *        which denominations will be accepted for tipping.
 * @param ec expected Taler error code.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup_with_ec (const char *label,
                                      const char *merchant_url,
                                      unsigned int http_status,
                                      const char *authorize_reference,
                                      const char **amounts,
                                      enum TALER_ErrorCode ec);

/**
 * Define a /tip-pickup CMD.
 *
 * @param label the command label
 * @param merchant_url base URL of the backend which will serve
 *        the /tip-pickup request.
 * @param http_status expected HTTP response code.
 * @param authorize_reference reference to a /tip-autorize CMD
 *        that offers a tip id to pick up.
 * @param amounts array of string-defined amounts that specifies
 *        which denominations will be accepted for tipping.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_tip_pickup (const char *label,
                              const char *merchant_url,
                              unsigned int http_status,
                              const char *authorize_reference,
                              const char **amounts);

/**
 * Make the instruction pointer point to @a new_ip
 * only if @a counter is greater than zero.
 *
 * @param label command label
 * @param new_ip new instruction pointer's value.  Note that,
 *    when the next instruction will be called, the interpreter
 *    will increment the ip _anyway_ so this value must be
 *    set to the index of the instruction we want to execute next
 *    MINUS one.
 * @param counter counts how many times the rewinding has
 * to happen.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_rewind_ip (const char *label,
                             int new_ip,
                             unsigned int *counter);

#endif
