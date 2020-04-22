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
 * @file lib/testing_api_cmd_tip_authorize.c
 * @brief command to test the tipping.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a /tip-authorize CMD.
 */
struct TipAuthorizeState
{

  /**
   * Merchant base URL.
   */
  const char *merchant_url;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

  /**
   * Human-readable justification for the
   * tip authorization carried on by this CMD.
   */
  const char *justification;

  /**
   * Amount that should be authorized for tipping.
   */
  const char *amount;

  /**
   * Expected Taler error code for this CMD.
   */
  enum TALER_ErrorCode expected_ec;

  /**
   * Tip taler:// URI.
   */
  const char *tip_uri;

  /**
   * The tip id; set when the CMD succeeds.
   */
  struct GNUNET_HashCode tip_id;

  /**
   * Expiration date for this tip.
   */
  struct GNUNET_TIME_Absolute tip_expiration;

  /**
   * Handle to the on-going /tip-authorize request.
   */
  struct TALER_MERCHANT_TipAuthorizeOperation *tao;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;
};


/**
 * Callback for a /tip-authorize request.  Set into the state
 * what was returned from the backend (@a tip_id and @a
 * tip_expiration).
 *
 * @param cls closure
 * @param hr HTTP response we got
 * @param taler_tip_uri URI to let the wallet know about the tip
 * @param tip_id unique identifier for the tip
 */
static void
tip_authorize_cb (void *cls,
                  const struct TALER_MERCHANT_HttpResponse *hr,
                  struct GNUNET_HashCode *tip_id,
                  const char *taler_tip_uri)
{
  struct TipAuthorizeState *tas = cls;

  tas->tao = NULL;
  if (tas->http_status != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                hr->ec,
                TALER_TESTING_interpreter_get_current_label (tas->is));
    TALER_TESTING_interpreter_fail (tas->is);
    return;
  }

  if (tas->expected_ec != hr->ec)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected error code %d (%u) to command %s\n",
                (int) hr->ec,
                hr->http_status,
                TALER_TESTING_interpreter_get_current_label (tas->is));
    TALER_TESTING_interpreter_fail (tas->is);
    return;
  }
  if ( (MHD_HTTP_OK == hr->http_status) &&
       (TALER_EC_NONE == hr->ec) )
  {
    tas->tip_uri = strdup (taler_tip_uri);
    tas->tip_id = *tip_id;
  }
  TALER_TESTING_interpreter_next (tas->is);
}


/**
 * Offers information from the /tip-authorize CMD state to other
 * commands.
 *
 * @param cls closure
 * @param ret[out] result (could be anything)
 * @param trait name of the trait
 * @param index index number of the object to extract.
 * @return #GNUNET_OK on success
 */
static int
tip_authorize_traits (void *cls,
                      const void **ret,
                      const char *trait,
                      unsigned int index)
{
  struct TipAuthorizeState *tas = cls;
  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_tip_id (0, &tas->tip_id),
    TALER_TESTING_trait_end (),
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
}


/**
 * Runs the /tip-authorize CMD
 *
 * @param cls closure
 * @param cmd the CMD representing _this_ command
 * @param is interpreter state
 */
static void
tip_authorize_run (void *cls,
                   const struct TALER_TESTING_Command *cmd,
                   struct TALER_TESTING_Interpreter *is)
{
  struct TipAuthorizeState *tas = cls;
  struct TALER_Amount amount;

  tas->is = is;
  if (GNUNET_OK != TALER_string_to_amount (tas->amount,
                                           &amount))
    TALER_TESTING_FAIL (is);

  tas->tao = TALER_MERCHANT_tip_authorize (is->ctx,
                                           tas->merchant_url,
                                           "http://merchant.com/pickup",
                                           "http://merchant.com/continue",
                                           &amount,
                                           tas->justification,
                                           tip_authorize_cb,
                                           tas);

  GNUNET_assert (NULL != tas->tao);
}


/**
 * Run the /tip-authorize CMD, the "fake" version of it.
 *
 * @param cls closure
 * @param cmd the CMD representing _this_ command
 * @param is interpreter state *
 */
static void
tip_authorize_fake_run (void *cls,
                        const struct TALER_TESTING_Command *cmd,
                        struct TALER_TESTING_Interpreter *is)
{
  struct TipAuthorizeState *tas = cls;

  /* Make up a tip id.  */
  GNUNET_CRYPTO_random_block (GNUNET_CRYPTO_QUALITY_WEAK,
                              &tas->tip_id,
                              sizeof (struct GNUNET_HashCode));

  TALER_TESTING_interpreter_next (is);
}


/**
 * Free the state from a /tip-authorize CMD, and possibly
 * cancel any pending operation.
 *
 * @param cls closure
 * @param cmd the /tip-authorize CMD that is about to be freed.
 */
static void
tip_authorize_cleanup (void *cls,
                       const struct TALER_TESTING_Command *cmd)
{
  struct TipAuthorizeState *tas = cls;

  if (NULL != tas->tao)
  {
    TALER_LOG_WARNING ("Tip-autorize operation"
                       " did not complete\n");
    TALER_MERCHANT_tip_authorize_cancel (tas->tao);
  }
  GNUNET_free (tas);
}


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
                                         enum TALER_ErrorCode ec)
{
  struct TipAuthorizeState *tas;

  tas = GNUNET_new (struct TipAuthorizeState);
  tas->merchant_url = merchant_url;
  tas->justification = justification;
  tas->amount = amount;
  tas->http_status = http_status;
  tas->expected_ec = ec;
  {
    struct TALER_TESTING_Command cmd = {
      .label = label,
      .cls = tas,
      .run = &tip_authorize_run,
      .cleanup = &tip_authorize_cleanup,
      .traits = &tip_authorize_traits
    };

    return cmd;
  }
}


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
                                 const char *amount)
{
  struct TipAuthorizeState *tas;

  tas = GNUNET_new (struct TipAuthorizeState);
  tas->merchant_url = merchant_url;
  tas->justification = justification;
  tas->amount = amount;
  tas->http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .label = label,
      .cls = tas,
      .run = &tip_authorize_run,
      .cleanup = &tip_authorize_cleanup,
      .traits = &tip_authorize_traits
    };

    return cmd;
  }
}


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
TALER_TESTING_cmd_tip_authorize_fake (const char *label)
{
  struct TipAuthorizeState *tas;

  tas = GNUNET_new (struct TipAuthorizeState);
  {
    struct TALER_TESTING_Command cmd = {
      .label = label,
      .cls = tas,
      .run = &tip_authorize_fake_run,
      .cleanup = &tip_authorize_cleanup,
      .traits = &tip_authorize_traits
    };

    return cmd;
  }
}


/* end of testing_api_cmd_tip_authorize.c */
