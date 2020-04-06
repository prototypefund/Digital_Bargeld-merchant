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
 * @file lib/testing_api_cmd_track_transfer.c
 * @brief command to test /track/transfer.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "track transfer" CMD.
 */
struct TrackTransferState
{

  /**
   * Handle for a "track transfer" request.
   */
  struct TALER_MERCHANT_TrackTransferHandle *tth;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

  /**
   * Reference for a "check bank" CMD.  It offers the
   * WTID to track.
   */
  const char *check_bank_reference;

};


/**
 * Callback for a /track/transfer operation, only checks if
 * response code is the expected one.
 *
 * @param cls closure for this function
 * @param http_status HTTP response code returned by the server
 * @param ec taler-specific error code
 * @param sign_key exchange key used to sign @a json, or NULL
 * @param json original json reply (may include signatures,
 *        those have then been validated already)
 * @param h_wire hash of the wire transfer address the transfer
 *        went to, or NULL on error
 * @param total_amount total amount of the wire transfer, or NULL
 *        if the exchange could not provide any @a wtid (set only
 *        if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined
 *        transactions
 */
static void
track_transfer_cb (void *cls,
                   const struct TALER_MERCHANT_HttpResponse *hr,
                   const struct TALER_ExchangePublicKeyP *sign_key,
                   const struct GNUNET_HashCode *h_wire,
                   const struct TALER_Amount *total_amount,
                   unsigned int details_length,
                   const struct TALER_MERCHANT_TrackTransferDetails *details)
{
  /* FIXME, deeper checks should be implemented here. */
  struct TrackTransferState *tts = cls;

  tts->tth = NULL;
  if (tts->http_status != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                (int) hr->ec,
                TALER_TESTING_interpreter_get_current_label (tts->is));
    TALER_TESTING_interpreter_fail (tts->is);
    return;
  }
  switch (hr->http_status)
  {
  /**
   * Check that all the deposits sum up to the total
   * transferred amount.  */
  case MHD_HTTP_OK:
    {
      json_t *deposits;
      const char *amount_str;
      struct TALER_Amount total;
      struct TALER_Amount wire_fee;
      struct TALER_Amount amount_iter;
      struct TALER_Amount deposit_fee_iter;
      struct TALER_Amount sum;
      size_t index;
      json_t *value;

      amount_str = json_string_value (json_object_get (hr->reply,
                                                       "total"));
      if (GNUNET_OK !=
          TALER_string_to_amount (amount_str,
                                  &total))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse amount `%s'\n",
                    amount_str);
        TALER_TESTING_FAIL (tts->is);
        return;
      }
      amount_str = json_string_value (json_object_get (hr->reply,
                                                       "wire_fee"));
      if (GNUNET_OK !=
          TALER_string_to_amount (amount_str,
                                  &wire_fee))
      {
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to parse amount `%s'\n",
                    amount_str);
        TALER_TESTING_FAIL (tts->is);
        return;
      }
      GNUNET_assert (GNUNET_OK ==
                     TALER_amount_get_zero (total.currency,
                                            &sum));
      deposits = json_object_get (hr->reply,
                                  "deposits_sums");
      json_array_foreach (deposits, index, value)
      {
        amount_str = json_string_value (json_object_get (value,
                                                         "deposit_value"));
        if (GNUNET_OK !=
            TALER_string_to_amount (amount_str,
                                    &amount_iter))
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Failed to parse amount `%s'\n",
                      amount_str);
          TALER_TESTING_FAIL (tts->is);
          return;
        }
        amount_str = json_string_value (json_object_get (value,
                                                         "deposit_fee"));
        if (GNUNET_OK !=
            TALER_string_to_amount (amount_str,
                                    &deposit_fee_iter))
        {
          GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                      "Failed to parse amount `%s'\n",
                      amount_str);
          TALER_TESTING_FAIL (tts->is);
          return;
        }
        GNUNET_assert (GNUNET_SYSERR !=
                       TALER_amount_add (&sum,
                                         &sum,
                                         &amount_iter));
        GNUNET_assert (GNUNET_SYSERR !=
                       TALER_amount_subtract (&sum,
                                              &sum,
                                              &deposit_fee_iter));
      }

      GNUNET_assert (GNUNET_SYSERR !=
                     TALER_amount_subtract (&sum,
                                            &sum,
                                            &wire_fee));
      if (0 != TALER_amount_cmp (&sum,
                                 &total))
      {
        GNUNET_break (0);
        TALER_LOG_ERROR (
          "Inconsistent amount transferred: Sum %s, claimed %s\n",
          TALER_amount_to_string (&sum),
          TALER_amount_to_string (&total));
        TALER_TESTING_interpreter_fail (tts->is);
      }
    }
    break;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Unhandled HTTP status.\n");
  }
  TALER_TESTING_interpreter_next (tts->is);
}


/**
 * Run the "track transfer" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
track_transfer_run (void *cls,
                    const struct TALER_TESTING_Command *cmd,
                    struct TALER_TESTING_Interpreter *is)
{
  struct TrackTransferState *tts = cls;
  const struct TALER_WireTransferIdentifierRawP *wtid;
  const struct TALER_TESTING_Command *check_bank_cmd;
  const char *exchange_url;

  tts->is = is;
  check_bank_cmd
    = TALER_TESTING_interpreter_lookup_command (is,
                                                tts->check_bank_reference);
  if (NULL == check_bank_cmd)
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_wtid (check_bank_cmd,
                                    0,
                                    &wtid))
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK !=
      TALER_TESTING_get_trait_url (check_bank_cmd,
                                   TALER_TESTING_UT_EXCHANGE_BASE_URL,
                                   &exchange_url))
    TALER_TESTING_FAIL (is);
  tts->tth = TALER_MERCHANT_track_transfer (is->ctx,
                                            tts->merchant_url,
                                            "x-taler-bank",
                                            wtid,
                                            exchange_url,
                                            &track_transfer_cb,
                                            tts);
  GNUNET_assert (NULL != tts->tth);
}


/**
 * Free the state of a "track transfer" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
track_transfer_cleanup (void *cls,
                        const struct TALER_TESTING_Command *cmd)
{
  struct TrackTransferState *tts = cls;

  if (NULL != tts->tth)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "/track/transfer (test) operation"
                " did not complete\n");
    TALER_MERCHANT_track_transfer_cancel (tts->tth);
  }
  GNUNET_free (tts);
}


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
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transfer (const char *label,
                                           const char *merchant_url,
                                           unsigned int http_status,
                                           const char *check_bank_reference)
{
  struct TrackTransferState *tts;

  tts = GNUNET_new (struct TrackTransferState);
  tts->merchant_url = merchant_url;
  tts->http_status = http_status;
  tts->check_bank_reference = check_bank_reference;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = tts,
      .label = label,
      .run = &track_transfer_run,
      .cleanup = &track_transfer_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_track_transfer.c */
