/*
  This file is part of TALER
  (C) 2014, 2015 Christian Grothoff (and other contributing authors)

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
 * Mint
 */
struct MERCHANT_Mint
{
  /**
   * Hostname
   */
  char *hostname;

  /**
   * A connection to this mint
   */
  struct TALER_MINT_Handle *conn;

  /**
   * This mint's context (useful to the event loop)
   */
  struct TALER_MINT_Context *ctx;

  /**
   * Task we use to drive the interaction with this mint.
   */
  struct GNUNET_SCHEDULER_Task *poller_task;

  /**
   * Flag which indicates whether some HTTP transfer between
   * this merchant and the mint is still ongoing
   */
  int pending;

};


struct MERCHANT_WIREFORMAT_Sepa
{
  /**
   * The international bank account number
   */
  char *iban;

  /**
   * Name of the bank account holder
   */
  char *name;

  /**
   *The bank identification code
   */
  char *bic;

  /**
   * The latest payout date when the payment corresponding to this account has
   * to take place.  A value of 0 indicates a transfer as soon as possible.
   */
  struct GNUNET_TIME_AbsoluteNBO payout;
};


struct MERCHANT_Auditor
{
  /**
   * Auditor's legal name
   */
  char *name;

};



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


struct TM_HandlerContext
{

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



extern struct MERCHANT_Auditor *auditors;
extern unsigned int nauditors;

extern struct MERCHANT_Mint **mints;

extern struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;


extern PGconn *db_conn;


extern unsigned int nmints;

extern struct GNUNET_TIME_Relative edate_delay;


void
context_task (void *cls,
              const struct GNUNET_SCHEDULER_TaskContext *tc);




/**
 * Take the global wire details and return a JSON containing them,
 * compliantly with the Taler's API.
 *
 * @param wire the merchant's wire details
 * @param salt the nounce for hashing the wire details with
 * @return JSON representation of the wire details, NULL upon errors
 */
json_t *
MERCHANT_get_wire_json (const struct MERCHANT_WIREFORMAT_Sepa *wire,
                        uint64_t salt);


/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 */
void
TM_trigger_daemon (void);


#endif
