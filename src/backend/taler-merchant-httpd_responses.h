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
 * @file taler-merchant-httpd_responses.h
 * @brief API for generating the various replies of the exchange; these
 *        functions are called TMH_RESPONSE_reply_ and they generate
 *        and queue MHD response objects for a given connection.
 * @author Florian Dold
 * @author Benedikt Mueller
 * @author Christian Grothoff
 */
#ifndef TALER_EXCHANGE_HTTPD_RESPONSES_H
#define TALER_EXCHANGE_HTTPD_RESPONSES_H
#include <gnunet/gnunet_util_lib.h>
#include <jansson.h>
#include <microhttpd.h>
#include <pthread.h>

/**
 * Make JSON response object.
 *
 * @param json the json object
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_json (const json_t *json);


/**
 * Send JSON object as response.
 *
 * @param connection the MHD connection
 * @param json the json object
 * @param response_code the http response code
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json (struct MHD_Connection *connection,
                         const json_t *json,
                         unsigned int response_code);


/**
 * Make JSON response object.
 *
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_json_pack (const char *fmt,
                             ...);

/**
 * Information about a coin aggregated in a wire transfer for a
 * /track/transaction response.
 */
struct TMH_CoinWireTransfer
{

  /**
   * Public key of the coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Value of the coin including deposit fee.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Deposit fee for the coin.
   */
  struct TALER_Amount deposit_fee;

};

/**
 * Information about a wire transfer for a /track/transaction response.
 */
struct TMH_TransactionWireTransfer
{

  /**
   * Wire transfer identifier this struct is about.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Number of coins of the selected transaction that
   * is covered by this wire transfer.
   */
  unsigned int num_coins;

  /**
   * Information about the coins of the selected transaction
   * that are part of the wire transfer.
   */
  struct TMH_CoinWireTransfer *coins;

};


/**
 * Generate /track/transaction response.
 *
 * @param num_transfers how many wire transfers make up the transaction
 * @param transfers data on each wire transfer
 * @return MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_track_transaction_ok (unsigned int num_transfers,
                                        const struct TMH_TransactionWireTransfer *transfers);


/**
 * Function to call to handle the request by building a JSON
 * reply from a format string and varargs.
 *
 * @param connection the MHD connection to handle
 * @param response_code HTTP response code to use
 * @param fmt format string for pack
 * @param ... varargs
 * @return MHD result code
 */
int
TMH_RESPONSE_reply_json_pack (struct MHD_Connection *connection,
                              unsigned int response_code,
                              const char *fmt,
                              ...);


/**
 * Send a response indicating that the JSON was malformed.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_invalid_json (struct MHD_Connection *connection);


/**
 * Send a response indicating that we did not find the @a object
 * needed for the reply.
 *
 * @param connection the MHD connection to use
 * @param object name of the object we did not find
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_not_found (struct MHD_Connection *connection,
                              const char *object);


/**
 * Send a response indicating that the request was malformed.
 *
 * @param connection the MHD connection to use
 * @param issue description of what was wrong with the request
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_bad_request (struct MHD_Connection *connection,
                                const char *issue);


/**
 * Send a response indicating an internal error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the internal error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_internal_error (struct MHD_Connection *connection,
                                   const char *hint);


/**
 * Create a response indicating an internal error.
 *
 * @param hint hint about the internal error's nature
 * @return a MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_internal_error (const char *hint);


/**
 * Send a response indicating an external error.
 *
 * @param connection the MHD connection to use
 * @param hint hint about the error's nature
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_external_error (struct MHD_Connection *connection,
                                   const char *hint);


/**
 * Create a response indicating an external error.
 *
 * @param hint hint about the internal error's nature
 * @return a MHD response object
 */
struct MHD_Response *
TMH_RESPONSE_make_external_error (const char *hint);


/**
 * Send a response indicating that the request was too big.
 *
 * @param connection the MHD connection to use
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_request_too_large (struct MHD_Connection *connection);


/**
 * Add headers we want to return in every response.
 * Useful for testing, like if we want to always close
 * connections.
 *
 * @param response response to modify
 */
void
TMH_RESPONSE_add_global_headers (struct MHD_Response *response);

/**
 * Send a response indicating a missing argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is missing
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_missing (struct MHD_Connection *connection,
                                const char *param_name);

/**
 * Send a response indicating an invalid argument.
 *
 * @param connection the MHD connection to use
 * @param param_name the parameter that is invalid
 * @return a MHD result code
 */
int
TMH_RESPONSE_reply_arg_invalid (struct MHD_Connection *connection,
                                const char *param_name);
#endif
