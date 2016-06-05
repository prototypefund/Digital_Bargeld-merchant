/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_track-deposit.c
 * @brief implement API for tracking deposits and wire transfers
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_parsing.h"
#include "taler-merchant-httpd_auditors.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_responses.h"
#include "taler-merchant-httpd_track_deposit.h"


/**
 * How long to wait before giving up processing with the exchange?
 */
#define TRACK_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))

extern char *TMH_merchant_currency_string;


/**
 * Context used for handing /track/deposit requests.
 */
struct DepositTrackContext
{

  /**
   * This MUST be first!
   */
  struct TM_HandlerContext hc;

  /**
   * Handle to the exchange.
   */
  struct TALER_EXCHANGE_Handle *eh;

  /**
   * Handle for the /wire/deposits request.
   */
  struct TALER_EXCHANGE_WireDepositsHandle *wdh;

  /**
   *
   */
  struct TALER_WireDepositDetails *details;

  /**
   * Argument for the /wire/deposits request.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Length of the @e details array.
   */
  unsigned int details_length;

  /**
   * HTTP connection we are handling.
   */
  struct MHD_Connection *connection;

  /**
   * Response code to return.
   */
  unsigned int response_code;

  /**
   *
   */
  json_t *json;

  /**
   * Error message.
   */
  const char *error;

};


/**
 * Function called with detailed wire transfer data, including all
 * of the coin transactions that were combined into the wire transfer.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param wtid extracted wire transfer identifier, or NULL if the exchange could
 *             not provide any (set only if @a http_status is #MHD_HTTP_OK)
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
static void
wire_deposit_cb (void *cls,
                 unsigned int http_status,
                 const json_t *json,
                 const struct GNUNET_HashCode *h_wire,
                 const struct TALER_Amount *total_amount,
                 unsigned int details_length,
                 const struct TALER_WireDepositDetails *details)
{
  struct DepositTrackContext *rctx = cls;
  // unsigned int i;

  rctx->wdh = NULL;
  if (NULL == total_amount)
  {
    rctx->error = "failed to obtain /wire/deposit response from exchange";
    rctx->json = json_incref ((json_t *) json);
    rctx->response_code = http_status;
    MHD_resume_connection (rctx->connection);
    return;
  }
  rctx->details_length = details_length;
  rctx->details = GNUNET_new_array (details_length,
                                    struct TALER_WireDepositDetails);
  memcpy (rctx->details,
          details,
          details_length * sizeof (struct TALER_WireDepositDetails));
  GNUNET_break (0);
  /* FIXME: now check that these details match what we have in
     our database... */


}


/**
 * Function called with information about who is auditing
 * a particular exchange and what key the exchange is using.
 *
 * @param cls closure
 * @param keys information about the various keys used
 *        by the exchange, NULL if /keys failed
 */
static void
cert_cb (void *cls,
         const struct TALER_EXCHANGE_Keys *keys)
{
  struct DepositTrackContext *rctx = cls;

  if (NULL == keys)
  {
    rctx->error = "failed to obtain /keys from exchange";
    rctx->response_code = MHD_HTTP_SERVICE_UNAVAILABLE;
    MHD_resume_connection (rctx->connection);
    return;
  }
  rctx->wdh = TALER_EXCHANGE_wire_deposits (rctx->eh,
                                            &rctx->wtid,
                                            &wire_deposit_cb,
                                            rctx);
}


/**
 * Manages a /track/wtid call, thus it calls the /track/deposit
 * offered by the exchange in order to return the set of deposits
 * (of coins) associated with a given wire transfer.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_track_deposit (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size)
{
  struct DepositTrackContext *rctx;
  const char *str;
  const char *uri;

  rctx = *connection_cls;
  if (NULL != rctx)
  {
    // ...
  }
  uri = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "exchange");
  if (NULL == uri)
    return TMH_RESPONSE_reply_external_error (connection,
                                              "exchange argument missing");
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "wtid");
  if (NULL == str)
    return TMH_RESPONSE_reply_external_error (connection,
                                              "wtid argument missing");
  if (GNUNET_OK !=
      GNUNET_STRINGS_string_to_data (str,
                                     strlen (str),
                                     &rctx->wtid,
                                     sizeof (rctx->wtid)))
    return TMH_RESPONSE_reply_external_error (connection,
                                              "wtid argument malformed");
  rctx->eh = TALER_EXCHANGE_connect (NULL /* FIXME */,
                                     uri,
                                     &cert_cb,
                                     rctx,
                                     TALER_EXCHANGE_OPTION_END);

  GNUNET_break (0);
  return MHD_NO;
}

/* end of taler-merchant-httpd_track-deposit.c */
