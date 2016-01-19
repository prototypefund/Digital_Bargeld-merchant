/*
  This file is part of TALER
  (C) 2014, 2015 GNUnet e.V.

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
 * @file merchant/backend/taler-merchant-httpd.h
 * @brief HTTP serving layer mainly intended to communicate with the frontend
 * @author Marcello Stanisci
 */
#ifndef TALER_MERCHANT_HTTPD_H
#define TALER_MERCHANT_HTTPD_H

#include "platform.h"
#include "taler_merchantdb_lib.h"
#include <microhttpd.h>

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)


/**
 * @brief Struct describing an URL and the handler for it.
 */
struct TMH_RequestHandler
{

  /**
   * URL the handler is for.
   */
  const char *url;

  /**
   * Method the handler is for, NULL for "all".
   */
  const char *method;

  /**
   * Mime type to use in reply (hint, can be NULL).
   */
  const char *mime_type;

  /**
   * Raw data for the @e handler
   */
  const void *data;

  /**
   * Number of bytes in @e data, 0 for 0-terminated.
   */
  size_t data_size;

  /**
   * Function to call to handle the request.
   *
   * @param rh this struct
   * @param mime_type the @e mime_type for the reply (hint, can be NULL)
   * @param connection the MHD connection to handle
   * @param[in,out] connection_cls the connection's closure (can be updated)
   * @param upload_data upload data
   * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
   * @return MHD result code
   */
  int (*handler)(struct TMH_RequestHandler *rh,
                 struct MHD_Connection *connection,
                 void **connection_cls,
                 const char *upload_data,
                 size_t *upload_data_size);

  /**
   * Default response code.
   */
  int response_code;
};


/**
 * Each MHD response handler that sets the "connection_cls" to a
 * non-NULL value must use a struct that has this struct as its first
 * member.  This struct contains a single callback, which will be
 * invoked to clean up the memory when the contection is completed.
 */
struct TM_HandlerContext;

/**
 * Signature of a function used to clean up the context
 * we keep in the "connection_cls" of MHD when handling
 * a request.
 *
 * @param hc header of the context to clean up.
 */
typedef void
(*TM_ContextCleanup)(struct TM_HandlerContext *hc);


/**
 * Each MHD response handler that sets the "connection_cls" to a
 * non-NULL value must use a struct that has this struct as its first
 * member.  This struct contains a single callback, which will be
 * invoked to clean up the memory when the contection is completed.
 */
struct TM_HandlerContext
{

  /**
   * Function to execute the handler-specific cleanup of the
   * (typically larger) context.
   */
  TM_ContextCleanup cc;

};


/**
 * Our wire format details in JSON format (with salt).
 */
extern json_t *j_wire;

/**
 * Hash of our wire format details as given in #j_wire.
 */
extern struct GNUNET_HashCode h_wire;

/**
 * Our private key (for the merchant to sign contracts).
 */
extern struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;

/**
 * Our public key, corresponds to #privkey.
 */ 
extern struct TALER_MerchantPublicKeyP pubkey;

/**
 * Handle to the database backend.
 */ 
extern struct TALER_MERCHANTDB_Plugin *db;

/**
 * If the frontend does NOT specify an execution date, how long should
 * we tell the mint to wait to aggregate transactions before
 * executing?  This delay is added to the current time when we
 * generate the advisory execution time for the mint.
 */
extern struct GNUNET_TIME_Relative edate_delay;

/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 * Basically, we need to explicitly resume MHD's event loop whenever
 * we made progress serving a request.  This function re-schedules
 * the task processing MHD's activities to run immediately.
 */
void
TMH_trigger_daemon (void);


#endif
