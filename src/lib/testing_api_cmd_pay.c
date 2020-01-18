/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

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
 * @file lib/testing_api_cmd_pay.c
 * @brief command to test the /pay feature.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"

#define AMOUNT_WITH_FEE 0
#define AMOUNT_WITHOUT_FEE 1
#define REFUND_FEE 2

/**
 * State for a /pay CMD.
 */
struct PayState
{
  /**
   * Contract terms hash code.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Expected HTTP response status code.
   */
  unsigned int http_status;

  /**
   * Reference to a command that can provide a order id,
   * typically a /proposal test command.
   */
  const char *proposal_reference;

  /**
   * Reference to a command that can provide a coin, so
   * we can pay here.
   */
  const char *coin_reference;

  /**
   * The merchant base URL.
   */
  const char *merchant_url;

  /**
   * Amount to be paid, plus the deposit fee.
   */
  const char *amount_with_fee;

  /**
   * Amount to be paid, including NO fees.
   */
  const char *amount_without_fee;

  /**
   * Fee for refunding this payment.
   */
  const char *refund_fee;

  /**
   * Handle to the /pay operation.
   */
  struct TALER_MERCHANT_Pay *po;

};


/**
 * State for a "pay again" CMD.
 */
struct PayAgainState
{

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

  /**
   * Reference to the "pay" command to abort.
   */
  const char *pay_reference;

  /**
   * Reference to the coins to use.
   */
  const char *coin_reference;

  /**
   * Merchant URL.
   */
  const char *merchant_url;

  /**
   * Refund fee.
   */
  const char *refund_fee;

  /**
   * Handle to a "pay again" operation.
   */
  struct TALER_MERCHANT_Pay *pao;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;
};


/**
 * State for a "pay abort" CMD.
 */
struct PayAbortState
{

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

  /**
   * Reference to the "pay" command to abort.
   */
  const char *pay_reference;

  /**
   * Merchant URL.
   */
  const char *merchant_url;

  /**
   * Handle to a "pay abort" operation.
   */
  struct TALER_MERCHANT_Pay *pao;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * How many refund permissions this CMD got
   * the right for.  Roughly, there is one refund
   * permission for one coin.
   */
  unsigned int num_refunds;

  /**
   * The actual refund data.
   */
  struct TALER_MERCHANT_RefundEntry *res;

  /**
   * The contract whose payment is to be aborted.
   */
  struct GNUNET_HashCode h_contract;

  /**
   * Merchant public key.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;
};


/**
 * Parse the @a coins specification and grow the @a pc
 * array with the coins found, updating @a npc.
 *
 * @param[in,out] pc pointer to array of coins found
 * @param[in,out] npc length of array at @a pc
 * @param[in] coins string specifying coins to add to @a pc,
 *            clobbered in the process
 * @param is interpreter state
 * @param amount_with_fee total amount to be paid for a contract.
 * @param amount_without_fee to be removed, there is no
 *        per-contract fee, only per-coin exists.
 * @param refund_fee per-contract? per-coin?
 * @return #GNUNET_OK on success
 */
static int
build_coins (struct TALER_MERCHANT_PayCoin **pc,
             unsigned int *npc,
             char *coins,
             struct TALER_TESTING_Interpreter *is,
             const char *amount_with_fee,
             const char *amount_without_fee,
             const char *refund_fee)
{
  char *token;

  for (token = strtok (coins, ";");
       NULL != token;
       token = strtok (NULL, ";"))
  {
    const struct TALER_TESTING_Command *coin_cmd;
    char *ctok;
    unsigned int ci;
    struct TALER_MERCHANT_PayCoin *icoin;
    const struct TALER_EXCHANGE_DenomPublicKey *dpk;

    /* Token syntax is "LABEL[/NUMBER]" */
    ctok = strchr (token, '/');
    ci = 0;
    if (NULL != ctok)
    {
      *ctok = '\0';
      ctok++;
      if (1 != sscanf (ctok,
                       "%u",
                       &ci))
      {
        GNUNET_break (0);
        return GNUNET_SYSERR;
      }
    }

    coin_cmd = TALER_TESTING_interpreter_lookup_command
                 (is, token);

    if (NULL == coin_cmd)
    {
      GNUNET_break (0);
      return GNUNET_SYSERR;
    }

    GNUNET_array_grow (*pc,
                       *npc,
                       (*npc) + 1);

    icoin = &((*pc)[(*npc) - 1]);

    {
      const struct TALER_CoinSpendPrivateKeyP *coin_priv;
      const struct TALER_DenominationSignature *denom_sig;
      const struct TALER_Amount *denom_value;
      const struct TALER_EXCHANGE_DenomPublicKey *denom_pub;

      GNUNET_assert
        (GNUNET_OK == TALER_TESTING_get_trait_coin_priv
          (coin_cmd, 0, &coin_priv));

      GNUNET_assert
        (GNUNET_OK == TALER_TESTING_get_trait_denom_pub
          (coin_cmd, 0, &denom_pub));

      GNUNET_assert
        (GNUNET_OK == TALER_TESTING_get_trait_denom_sig
          (coin_cmd, 0, &denom_sig));

      GNUNET_assert
        (GNUNET_OK == TALER_TESTING_get_trait_amount_obj
          (coin_cmd, 0, &denom_value));

      icoin->coin_priv = *coin_priv;
      icoin->denom_pub = denom_pub->key;
      icoin->denom_sig = *denom_sig;
      icoin->denom_value = *denom_value;
      icoin->amount_with_fee = *denom_value;
    }
    GNUNET_assert (NULL != (dpk =
                              TALER_TESTING_find_pk (is->keys,
                                                     &icoin->denom_value)));

    GNUNET_assert (GNUNET_SYSERR !=
                   TALER_amount_subtract (&icoin->amount_without_fee,
                                          &icoin->denom_value,
                                          &dpk->fee_deposit));
    GNUNET_assert (GNUNET_OK ==
                   TALER_TESTING_get_trait_url (coin_cmd,
                                                0,
                                                &icoin->exchange_url));
    GNUNET_assert (GNUNET_OK ==
                   TALER_string_to_amount (refund_fee,
                                           &icoin->refund_fee));
  }

  return GNUNET_OK;
}


/**
 * Function called with the result of a /pay operation.
 * Checks whether the merchant signature is valid and the
 * HTTP response code matches our expectation.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200)
 *        for successful deposit; 0 if the exchange's reply is
 *        bogus (fails to follow the protocol)
 * @param ec taler-specific error object
 * @param obj the received JSON reply, should be kept as proof
 *        (and, in case of errors, be forwarded to the customer)
 */
static void
pay_cb (void *cls,
        unsigned int http_status,
        enum TALER_ErrorCode ec,
        const json_t *obj)
{
  struct PayState *ps = cls;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  const char *error_name;
  unsigned int error_line;
  const struct TALER_MerchantPublicKeyP *merchant_pub;

  ps->po = NULL;
  if (ps->http_status != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label (
                  ps->is));
    TALER_TESTING_FAIL (ps->is);
  }
  if (MHD_HTTP_OK == http_status)
  {
    /* Check signature */
    struct PaymentResponsePS mr;
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("sig",
                                   &sig),
      GNUNET_JSON_spec_fixed_auto ("h_contract_terms",
                                   &ps->h_contract_terms),
      GNUNET_JSON_spec_end ()
    };
    const struct TALER_TESTING_Command *proposal_cmd;

    GNUNET_assert (GNUNET_OK == GNUNET_JSON_parse (
                     obj, spec,
                     &error_name,
                     &error_line));

    mr.purpose.purpose = htonl (
      TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
    mr.purpose.size = htonl (sizeof (mr));
    mr.h_contract_terms = ps->h_contract_terms;

    /* proposal reference was used at least once, at this point */
    GNUNET_assert
      (NULL !=
      (proposal_cmd =
         TALER_TESTING_interpreter_lookup_command (ps->is,
                                                   ps->proposal_reference)));

    if (GNUNET_OK !=
        TALER_TESTING_get_trait_merchant_pub (proposal_cmd,
                                              0,
                                              &merchant_pub))
      TALER_TESTING_FAIL (ps->is);

    if (GNUNET_OK !=
        GNUNET_CRYPTO_eddsa_verify (TALER_SIGNATURE_MERCHANT_PAYMENT_OK,
                                    &mr.purpose,
                                    &sig,
                                    &merchant_pub->eddsa_pub))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Merchant signature given in response to /pay"
                  " invalid\n");
      TALER_TESTING_FAIL (ps->is);
    }
  }

  TALER_TESTING_interpreter_next (ps->is);
}


/**
 * Callback for a "pay abort" operation.  Mainly, check HTTP
 * response code was as expected and stores refund permissions
 * in the state.
 *
 * @param cls closure.
 * @param http_status HTTP response code.
 * @param ec Taler error code.
 * @param merchant_pub public key of the merchant refunding the
 *        contract.
 * @param h_contract the contract involved in the refund.
 * @param num_refunds how many refund permissions have been
 *        issued.
 * @param res array containing the refund permissions.
 * @param obj raw response body.
 */
static void
pay_abort_cb (void *cls,
              unsigned int http_status,
              enum TALER_ErrorCode ec,
              const struct TALER_MerchantPublicKeyP *merchant_pub,
              const struct GNUNET_HashCode *h_contract,
              unsigned int num_refunds,
              const struct TALER_MERCHANT_RefundEntry *res,
              const json_t *obj)
{
  struct PayAbortState *pas = cls;

  pas->pao = NULL;
  if (pas->http_status != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label
                  (pas->is));
    TALER_TESTING_FAIL (pas->is);
  }
  if ( (MHD_HTTP_OK == http_status) &&
       (TALER_EC_NONE == ec) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Received %u refunds\n",
                num_refunds);
    pas->num_refunds = num_refunds;
    pas->res = GNUNET_new_array (num_refunds,
                                 struct TALER_MERCHANT_RefundEntry);
    memcpy (pas->res,
            res,
            num_refunds * sizeof (struct TALER_MERCHANT_RefundEntry));
    pas->h_contract = *h_contract;
    pas->merchant_pub = *merchant_pub;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Successful pay-abort (HTTP status: %u)\n",
              http_status);
  TALER_TESTING_interpreter_next (pas->is);
}


/**
 * Function used by both "pay" and "abort" operations.
 * It prepares data and sends the "pay" request to the
 * backend.
 *
 * @param merchant_url base URL of the merchant serving the
 *        request.
 * @param coin_reference reference to the CMD(s) that offer
 *        "coins" traits.  It is possible to give multiple
 *        references by using semicolons to separate them.
 * @param proposal_refere reference to a "proposal" CMD.
 * @param is interpreter state.
 * @param amount_with_fee amount to be paid, including deposit
 *        fee.
 * @param amount_without_fee amount to be paid, without deposit
 *        fee.
 * @param refund_fee refund fee.
 * @param api_func "lib" function that will be called to either
 *        issue a "pay" or "abort" request.
 * @param api_cb callback for @a api_func.
 * @param cls closure.
 *
 * @return handle to the operation, NULL if errors occur.
 */
static struct TALER_MERCHANT_Pay *
_pay_run (const char *merchant_url,
          const char *coin_reference,
          const char *proposal_reference,
          struct TALER_TESTING_Interpreter *is,
          const char *amount_with_fee,
          const char *amount_without_fee,
          const char *refund_fee,
          struct TALER_MERCHANT_Pay *(*api_func)(),
          void (*api_cb)(),
          void *cls)
{
  const struct TALER_TESTING_Command *proposal_cmd;
  const json_t *contract_terms;
  const char *order_id;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct GNUNET_TIME_Absolute pay_deadline;
  struct GNUNET_TIME_Absolute timestamp;
  struct TALER_MerchantPublicKeyP merchant_pub;
  struct GNUNET_HashCode h_wire;
  const struct GNUNET_HashCode *h_proposal;
  struct TALER_Amount total_amount;
  struct TALER_Amount max_fee;
  const char *error_name;
  unsigned int error_line;
  struct TALER_MERCHANT_PayCoin *pay_coins;
  unsigned int npay_coins;
  char *cr;
  struct TALER_MerchantSignatureP *merchant_sig;
  struct TALER_MERCHANT_Pay *ret;

  proposal_cmd = TALER_TESTING_interpreter_lookup_command (is,
                                                           proposal_reference);

  if (NULL == proposal_cmd)
  {
    GNUNET_break (0);
    return NULL;
  }

  if (GNUNET_OK !=
      TALER_TESTING_get_trait_contract_terms (proposal_cmd,
                                              0,
                                              &contract_terms))
  {
    GNUNET_break (0);
    return NULL;
  }

  /* Get information that needs to be put verbatim in the
   * deposit permission */
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_string ("order_id",
                             &order_id),
    GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                    &refund_deadline),
    GNUNET_JSON_spec_absolute_time ("pay_deadline",
                                    &pay_deadline),
    GNUNET_JSON_spec_absolute_time ("timestamp",
                                    &timestamp),
    GNUNET_JSON_spec_fixed_auto ("merchant_pub",
                                 &merchant_pub),
    GNUNET_JSON_spec_fixed_auto ("h_wire",
                                 &h_wire),
    TALER_JSON_spec_amount ("amount",
                            &total_amount),
    TALER_JSON_spec_amount ("max_fee",
                            &max_fee),
    GNUNET_JSON_spec_end ()
  };

  if (GNUNET_OK !=
      GNUNET_JSON_parse (contract_terms,
                         spec,
                         &error_name,
                         &error_line))
  {
    char *js;

    js = json_dumps (contract_terms,
                     JSON_INDENT (1));
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Parser failed on %s:%u for input `%s'\n",
                error_name,
                error_line,
                js);
    free (js);
    GNUNET_break_op (0);
    return NULL;
  }

  cr = GNUNET_strdup (coin_reference);
  pay_coins = NULL;
  npay_coins = 0;
  if (GNUNET_OK !=
      build_coins (&pay_coins,
                   &npay_coins,
                   cr,
                   is,
                   amount_with_fee,
                   amount_without_fee,
                   refund_fee))
  {
    GNUNET_array_grow (pay_coins,
                       npay_coins,
                       0);
    GNUNET_free (cr);
    GNUNET_break (0);
    return NULL;
  }

  GNUNET_free (cr);
  if (GNUNET_OK != TALER_TESTING_get_trait_merchant_sig
        (proposal_cmd, 0, &merchant_sig))
  {
    GNUNET_break (0);
    return NULL;
  }


  if (GNUNET_OK != TALER_TESTING_get_trait_h_contract_terms
        (proposal_cmd, 0, &h_proposal))
  {
    GNUNET_break (0);
    return NULL;
  }

  ret = api_func (is->ctx,
                  merchant_url,
                  h_proposal,
                  &total_amount,
                  &max_fee,
                  &merchant_pub,
                  merchant_sig,
                  timestamp,
                  refund_deadline,
                  pay_deadline,
                  &h_wire,
                  order_id,
                  npay_coins,
                  pay_coins,
                  api_cb,
                  cls);

  GNUNET_array_grow (pay_coins,
                     npay_coins,
                     0);
  return ret;
}


/**
 * Run a "pay" CMD.
 *
 * @param cls closure.
 * @param cmd current CMD being run.
 * @param is interpreter state.
 */
static void
pay_run (void *cls,
         const struct TALER_TESTING_Command *cmd,
         struct TALER_TESTING_Interpreter *is)
{

  struct PayState *ps = cls;

  ps->is = is;
  if (NULL == (ps->po = _pay_run (ps->merchant_url,
                                  ps->coin_reference,
                                  ps->proposal_reference,
                                  is,
                                  ps->amount_with_fee,
                                  ps->amount_without_fee,
                                  ps->refund_fee,
                                  &TALER_MERCHANT_pay_wallet,
                                  &pay_cb,
                                  ps)))
    TALER_TESTING_FAIL (is);
}


/**
 * Free a "pay" CMD, and cancel it if need be.
 *
 * @param cls closure.
 * @param cmd command currently being freed.
 */
static void
pay_cleanup (void *cls,
             const struct TALER_TESTING_Command *cmd)
{
  struct PayState *ps = cls;

  if (NULL != ps->po)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command `%s' did not complete.\n",
                TALER_TESTING_interpreter_get_current_label (
                  ps->is));
    TALER_MERCHANT_pay_cancel (ps->po);
  }

  GNUNET_free (ps);
}


/**
 * Offer internal data useful to other commands.
 *
 * @param cls closure
 * @param ret[out] result
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
pay_traits (void *cls,
            const void **ret,
            const char *trait,
            unsigned int index)
{

  struct PayState *ps = cls;
  const char *order_id;
  const struct TALER_TESTING_Command *proposal_cmd;
  const struct TALER_MerchantPublicKeyP *merchant_pub;

  if (NULL ==
      (proposal_cmd = TALER_TESTING_interpreter_lookup_command
                        (ps->is, ps->proposal_reference)))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  if (GNUNET_OK != TALER_TESTING_get_trait_order_id
        (proposal_cmd, 0, &order_id))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  if (GNUNET_OK != TALER_TESTING_get_trait_merchant_pub
        (proposal_cmd,
        0,
        &merchant_pub))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  {
    struct TALER_TESTING_Trait traits[] = {
      TALER_TESTING_make_trait_string
        (AMOUNT_WITH_FEE, ps->amount_with_fee),
      TALER_TESTING_make_trait_string
        (AMOUNT_WITHOUT_FEE, ps->amount_without_fee),
      TALER_TESTING_make_trait_string
        (REFUND_FEE, ps->refund_fee),
      TALER_TESTING_make_trait_proposal_reference
        (0, ps->proposal_reference),
      TALER_TESTING_make_trait_coin_reference
        (0, ps->coin_reference),
      TALER_TESTING_make_trait_order_id (0, order_id),
      TALER_TESTING_make_trait_merchant_pub (0, merchant_pub),
      TALER_TESTING_trait_end ()
    };

    return TALER_TESTING_get_trait (traits,
                                    ret,
                                    trait,
                                    index);
  }
}


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
 *
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
                       const char *refund_fee)
{
  struct PayState *ps;

  ps = GNUNET_new (struct PayState);
  ps->http_status = http_status;
  ps->proposal_reference = proposal_reference;
  ps->coin_reference = coin_reference;
  ps->merchant_url = merchant_url;
  ps->amount_with_fee = amount_with_fee;
  ps->amount_without_fee = amount_without_fee;
  ps->refund_fee = refund_fee;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = ps,
      .label = label,
      .run = &pay_run,
      .cleanup = &pay_cleanup,
      .traits = &pay_traits
    };

    return cmd;
  }
}


/**
 * Free a "pay abort" CMD, and cancel it if need be.
 *
 * @param cls closure.
 * @param cmd command currently being freed.
 */
static void
pay_abort_cleanup (void *cls,
                   const struct TALER_TESTING_Command *cmd)
{
  struct PayAbortState *pas = cls;

  if (NULL != pas->pao)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command `%s' did not complete.\n",
                TALER_TESTING_interpreter_get_current_label (
                  pas->is));
    TALER_MERCHANT_pay_cancel (pas->pao);
  }
  GNUNET_free_non_null (pas->res);
  GNUNET_free (pas);
}


/**
 * Run a "pay abort" CMD.
 *
 * @param cls closure
 * @param cmd command being run.
 * @param is interpreter state
 */
static void
pay_abort_run (void *cls,
               const struct TALER_TESTING_Command *cmd,
               struct TALER_TESTING_Interpreter *is)
{

  struct PayAbortState *pas = cls;
  const struct TALER_TESTING_Command *pay_cmd;

  const char *proposal_reference;
  const char *coin_reference;
  const char *amount_with_fee;
  const char *amount_without_fee;
  const char *refund_fee;

  pas->is = is;
  pay_cmd = TALER_TESTING_interpreter_lookup_command
              (is, pas->pay_reference);
  if (NULL == pay_cmd)
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_proposal_reference
        (pay_cmd, 0, &proposal_reference))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_coin_reference
        (pay_cmd, 0, &coin_reference))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_string
        (pay_cmd, AMOUNT_WITH_FEE, &amount_with_fee))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_string
        (pay_cmd, AMOUNT_WITHOUT_FEE, &amount_without_fee))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_string
        (pay_cmd, REFUND_FEE, &refund_fee))
    TALER_TESTING_FAIL (is);

  if (NULL == (pas->pao = _pay_run (pas->merchant_url,
                                    coin_reference,
                                    proposal_reference,
                                    is,
                                    amount_with_fee,
                                    amount_without_fee,
                                    refund_fee,
                                    &TALER_MERCHANT_pay_abort,
                                    &pay_abort_cb,
                                    pas)))
    TALER_TESTING_FAIL (is);
}


/**
 * Offer internal data useful to other commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
pay_abort_traits (void *cls,
                  const void **ret,
                  const char *trait,
                  unsigned int index)
{
  struct PayAbortState *pas = cls;
  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_merchant_pub
      (0, &pas->merchant_pub),
    TALER_TESTING_make_trait_h_contract_terms
      (0, &pas->h_contract),
    TALER_TESTING_make_trait_refund_entry
      (0, pas->res),
    TALER_TESTING_make_trait_uint (0, &pas->num_refunds),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


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
                             unsigned int http_status)
{
  struct PayAbortState *pas;

  pas = GNUNET_new (struct PayAbortState);
  pas->http_status = http_status;
  pas->pay_reference = pay_reference;
  pas->merchant_url = merchant_url;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pas,
      .label = label,
      .run = &pay_abort_run,
      .cleanup = &pay_abort_cleanup,
      .traits = &pay_abort_traits
    };

    return cmd;
  }
}


/**
 * Function called with the result of a /pay again operation,
 * check signature and HTTP response code are good.
 *
 * @param cls closure with the interpreter state
 * @param http_status HTTP response code, #MHD_HTTP_OK (200)
 *        for successful deposit; 0 if the exchange's reply is
 *        bogus (fails to follow the protocol)
 * @param ec taler-specific error object
 * @param obj the received JSON reply, should be kept as proof
 *        (and, in case of errors, be forwarded to the customer)
 */
static void
pay_again_cb (void *cls,
              unsigned int http_status,
              enum TALER_ErrorCode ec,
              const json_t *obj)
{
  struct PayAgainState *pas = cls;
  struct GNUNET_CRYPTO_EddsaSignature sig;
  const char *error_name;
  unsigned int error_line;
  const struct TALER_TESTING_Command *pay_cmd;
  const struct TALER_MerchantPublicKeyP *merchant_pub;

  pas->pao = NULL;
  if (pas->http_status != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label
                  (pas->is));
    TALER_TESTING_interpreter_fail (pas->is);
    return;
  }

  if (NULL ==
      (pay_cmd = TALER_TESTING_interpreter_lookup_command
                   (pas->is, pas->pay_reference)))
    TALER_TESTING_FAIL (pas->is);

  if (MHD_HTTP_OK == http_status)
  {
    struct PaymentResponsePS mr;
    /* Check signature */
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_fixed_auto ("sig",
                                   &sig),
      GNUNET_JSON_spec_fixed_auto ("h_contract_terms",
                                   &mr.h_contract_terms),
      GNUNET_JSON_spec_end ()
    };

    GNUNET_assert (GNUNET_OK == GNUNET_JSON_parse (obj,
                                                   spec,
                                                   &error_name,
                                                   &error_line));
    mr.purpose.purpose = htonl
                           (TALER_SIGNATURE_MERCHANT_PAYMENT_OK);
    mr.purpose.size = htonl (sizeof (mr));

    if (GNUNET_OK != TALER_TESTING_get_trait_merchant_pub
          (pay_cmd, 0, &merchant_pub))
      TALER_TESTING_FAIL (pas->is);

    if (GNUNET_OK != GNUNET_CRYPTO_eddsa_verify
          (TALER_SIGNATURE_MERCHANT_PAYMENT_OK,
          &mr.purpose,
          &sig,
          &merchant_pub->eddsa_pub))
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Merchant signature given in"
                  " response to /pay invalid\n");
      TALER_TESTING_FAIL (pas->is);
    }
  }

  TALER_TESTING_interpreter_next (pas->is);
}


/**
 * Run a "pay again" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being run.
 * @param is interpreter state.
 */
static void
pay_again_run (void *cls,
               const struct TALER_TESTING_Command *cmd,
               struct TALER_TESTING_Interpreter *is)
{
  struct PayAgainState *pas = cls;
  const struct TALER_TESTING_Command *pay_cmd;
  const char *proposal_reference;
  const char *amount_with_fee;
  const char *amount_without_fee;

  pas->is = is;
  pay_cmd = TALER_TESTING_interpreter_lookup_command
              (is, pas->pay_reference);
  if (NULL == pay_cmd)
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_proposal_reference
        (pay_cmd, 0, &proposal_reference))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_string
        (pay_cmd, AMOUNT_WITH_FEE, &amount_with_fee))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_string
        (pay_cmd, AMOUNT_WITHOUT_FEE, &amount_without_fee))
    TALER_TESTING_FAIL (is);

  if (NULL == (pas->pao = _pay_run (pas->merchant_url,
                                    pas->coin_reference,
                                    proposal_reference,
                                    is,
                                    amount_with_fee,
                                    amount_without_fee,
                                    pas->refund_fee,
                                    &TALER_MERCHANT_pay_wallet,
                                    &pay_again_cb,
                                    pas)))
    TALER_TESTING_FAIL (is);
}


/**
 * Free and possibly cancel a "pay again" CMD.
 *
 * @param cls closure.
 * @param cmd command currently being freed.
 */
static void
pay_again_cleanup (void *cls,
                   const struct TALER_TESTING_Command *cmd)
{
  struct PayAgainState *pas = cls;

  if (NULL != pas->pao)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Command `%s' did not complete.\n",
                TALER_TESTING_interpreter_get_current_label (
                  pas->is));
    TALER_MERCHANT_pay_cancel (pas->pao);
  }
  GNUNET_free (pas);
}


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
                             unsigned int http_status)
{
  struct PayAgainState *pas;

  pas = GNUNET_new (struct PayAgainState);
  pas->http_status = http_status;
  pas->pay_reference = pay_reference;
  pas->coin_reference = coin_reference;
  pas->merchant_url = merchant_url;
  pas->refund_fee = refund_fee;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = pas,
      .label = label,
      .run = &pay_again_run,
      .cleanup = &pay_again_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_pay.c */
