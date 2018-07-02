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
 * @file lib/testing_api_cmd_track.c
 * @brief command to test /track/transaction and /track/transfer.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "track transaction" CMD.
 */
struct TrackTransactionState
{
  /**
   * Handle for a pending /track/transaction request.
   */
  struct TALER_MERCHANT_TrackTransactionHandle *tth;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * CURL context.
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

  /**
   * Reference to a "pay" CMD, used to get the order
   * id to issue the track against.
   */
  const char *pay_reference;

  /**
   * Subject line of the wire transfer that payed
   * the tracked contract back.  WARNING: impredictible
   * behaviour if _multiple_ wire transfers were
   * issued to pay this contract back.
   */
  const char *wtid_str; 

  /**
   * Binary form of @a wtid_str, expected by other commands
   * in this form.  FIXME: ponder if one of the forms (string
   * or binary) should be fired from this state.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * base URL of the exchange that issued (or was supposed to,
   * in case 202 Accepted was returned) the wire transfer to
   * pay the tracked contract back.
   */
  const char *exchange_url;

};


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
   * CURL context.
   */
  struct GNUNET_CURL_Context *ctx;

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
 * Function called with detailed wire transfer data; checks
 * if HTTP response code matches the expectation, and stores
 * in the state what came from the backend.
 *
 * @param cls closure
 * @param http_status HTTP status code we got,
 *        0 on exchange protocol violation
 * @param ec taler-specific error code
 * @param json original json reply
 */
static void
track_transaction_cb (void *cls,
                      unsigned int http_status,
                      enum TALER_ErrorCode ec,
                      const json_t *json)
{
  struct TrackTransactionState *tts = cls;
  json_t *wtid_str;
  json_t *exchange_url;

  tts->tth = NULL;
  if (tts->http_status != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label
                  (tts->is));
    TALER_TESTING_interpreter_fail (tts->is);
    return;
  }
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "/track/transaction, response code: %u\n",
              http_status);
  if (MHD_HTTP_OK == http_status)
  {
    /* Only storing first element's wtid, as this works around
     * the disability of the real bank to provide a "bank check"
     * CMD as the fakebank does.  */
  
    if (NULL == (wtid_str = json_object_get
      (json_array_get (json, 0), "wtid")))
    {
      TALER_TESTING_interpreter_fail (tts->is);
      return; 
    }
  
    if (NULL == (exchange_url = json_object_get
      (json_array_get (json, 0), "exchange")))
    {
    
      TALER_TESTING_interpreter_fail (tts->is);
      return;
    }
  
    tts->exchange_url = GNUNET_strdup
      (json_string_value (exchange_url));
    tts->wtid_str = GNUNET_strdup
      (json_string_value (wtid_str));
  }
  TALER_TESTING_interpreter_next (tts->is);
}

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
track_transfer_cb
  (void *cls,
   unsigned int http_status,
   enum TALER_ErrorCode ec,
   const struct TALER_ExchangePublicKeyP *sign_key,
   const json_t *json,
   const struct GNUNET_HashCode *h_wire,
   const struct TALER_Amount *total_amount,
   unsigned int details_length,
   const struct TALER_MERCHANT_TrackTransferDetails *details)
{
  /* FIXME, deeper checks should be implemented here. */
  struct TrackTransferState *tts = cls;

  tts->tth = NULL;
  if (tts->http_status != http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                http_status,
                ec,
                TALER_TESTING_interpreter_get_current_label
                  (tts->is));
    TALER_TESTING_interpreter_fail (tts->is);
    return;
  }
  switch (http_status)
  {
    case MHD_HTTP_OK:
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
  struct TALER_WireTransferIdentifierRawP *wtid;
  const struct TALER_TESTING_Command *check_bank_cmd;
  const char *exchange_url;

  tts->is = is;
  check_bank_cmd = TALER_TESTING_interpreter_lookup_command
    (is, tts->check_bank_reference);
  if (NULL == check_bank_cmd)
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK != TALER_TESTING_get_trait_wtid
      (check_bank_cmd, 0, &wtid))
    TALER_TESTING_FAIL (is);
  if (GNUNET_OK != TALER_TESTING_get_trait_url
      (check_bank_cmd, 0, &exchange_url))
    TALER_TESTING_FAIL (is);
  tts->tth = TALER_MERCHANT_track_transfer (tts->ctx,
                                            tts->merchant_url,
                                            "default",
                                            "x-taler-bank",
                                            wtid,
                                            exchange_url,
                                            &track_transfer_cb,
                                            tts);
  GNUNET_assert (NULL != tts->tth);
}

/**
 * Run the "track transaction" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
track_transaction_run (void *cls,
                       const struct TALER_TESTING_Command *cmd,
                       struct TALER_TESTING_Interpreter *is)
{
  struct TrackTransactionState *tts = cls;
  const char *order_id;
  const struct TALER_TESTING_Command *pay_cmd;

  tts->is = is;

  if ( NULL ==
     ( pay_cmd = TALER_TESTING_interpreter_lookup_command
      (is, tts->pay_reference)))
    TALER_TESTING_FAIL (is);

  if (GNUNET_OK != TALER_TESTING_get_trait_order_id
      (pay_cmd, 0, &order_id))
    TALER_TESTING_FAIL (is);

  tts->tth = TALER_MERCHANT_track_transaction
    (tts->ctx,
     tts->merchant_url,
     "default",
     order_id,
     &track_transaction_cb,
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
 * Free the state of a "track transaction" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
track_transaction_cleanup (void *cls,
                           const struct TALER_TESTING_Command *cmd)
{
  struct TrackTransactionState *tts = cls;

  if (NULL != tts->tth)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "/track/transaction (test) operation"
                " did not complete\n");
    TALER_MERCHANT_track_transaction_cancel (tts->tth);
  }

  /* Need to discard 'const' before freeing.  */
  GNUNET_free_non_null ((char *) tts->exchange_url);
  GNUNET_free_non_null ((char *) tts->wtid_str);

  GNUNET_free (tts);
}


/**
 * Offer internal data of a "track transaction" CMD, for
 * other CMDs to use.
 *
 * @param cls closure.
 * @param ret[out] return value.
 * @param trait name of the trait.
 * @param index index of the trait.
 *
 * @return GNUNET_OK if it is successful.
 */
static int
track_transaction_traits (void *cls,
                          void **ret,
                          const char *trait,
                          unsigned int index)
{
  struct TrackTransactionState *tts = cls;
  struct TALER_WireTransferIdentifierRawP *wtid_ptr;

  if (GNUNET_OK != GNUNET_STRINGS_string_to_data
      (tts->wtid_str,
       strlen (tts->wtid_str),
       &tts->wtid,
       sizeof (struct TALER_WireTransferIdentifierRawP)))
    wtid_ptr = NULL;
  else
    wtid_ptr = &tts->wtid;

  struct TALER_TESTING_Trait traits[] = {
    TALER_TESTING_make_trait_wtid (0, wtid_ptr),
    TALER_TESTING_make_trait_url (0, tts->exchange_url),
    TALER_TESTING_trait_end ()
  };

  return TALER_TESTING_get_trait (traits,
                                  ret,
                                  trait,
                                  index);
  return GNUNET_SYSERR;
}

/**
 * Define a "track transaction" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        /track/transaction request.
 * @param ctx CURL context.
 * @param http_status expected HTTP response code.
 * @param pay_reference used to retrieve the order id to track.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transaction
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *pay_reference)
{
  struct TrackTransactionState *tts;
  struct TALER_TESTING_Command cmd;

  tts = GNUNET_new (struct TrackTransactionState);
  tts->merchant_url = merchant_url;
  tts->ctx = ctx;
  tts->http_status = http_status;
  tts->pay_reference = pay_reference;

  cmd.cls = tts;
  cmd.label = label;
  cmd.run = &track_transaction_run;
  cmd.cleanup = &track_transaction_cleanup;
  cmd.traits = &track_transaction_traits;

  return cmd;
}


/**
 * Define a "track transfer" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        /track/transfer request.
 * @param ctx CURL context.
 * @param http_status expected HTTP response code.
 * @param check_bank_reference reference to a "check bank" CMD
 *        that will provide the WTID and exchange URL to issue
 *        the track against.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_track_transfer
  (const char *label,
   const char *merchant_url,
   struct GNUNET_CURL_Context *ctx,
   unsigned int http_status,
   const char *check_bank_reference)
{
  struct TrackTransferState *tts;
  struct TALER_TESTING_Command cmd;

  tts = GNUNET_new (struct TrackTransferState);
  tts->merchant_url = merchant_url;
  tts->ctx = ctx;
  tts->http_status = http_status;
  tts->check_bank_reference = check_bank_reference;

  cmd.cls = tts;
  cmd.label = label;
  cmd.run = &track_transfer_run;
  cmd.cleanup = &track_transfer_cleanup;

  return cmd;
}

/* end of testing_api_cmd_track.c */
