/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
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
 * Used by the iterator of the various merchant's instances given
 * in configuration
 */
struct IterateInstancesCls
{

  /**
   * Handle for the configuration beig parsed
   */
  const struct GNUNET_CONFIGURATION_Handle *config;

  /**
   * Current index in the global array of #MerchantInstance
   * types. Used by the callback in order to know which index
   * is associated to the element being processed.
   */
  unsigned int current_index;

  /**
   * Flag indicating whether config contains a default instance
   */
  unsigned int default_instance;

  /**
   * Wire plugin
   */
  struct TALER_WIRE_Plugin *plugin;

  /**
   * Tells if the parsing encountered any error. We need this
   * field since the iterator must return void
   */
   unsigned int ret;
};


/**
 * Supported wire method.  Kept in a DLL.
 */
struct WireMethod
{
  /**
   * Next entry in DLL.
   */
  struct WireMethod *next;

  /**
   * Previous entry in DLL.
   */
  struct WireMethod *prev;

  /**
   * Which wire method is @e j_wire using?  Points into @e j_wire.
   */
  const char *wire_method;

  /**
   * Wire details for this instance
   */
  struct json_t *j_wire;

  /**
   * Hash of our wire format details as given in #j_wire.
   */
  struct GNUNET_HashCode h_wire;

};


/**
 * Information that defines a merchant "instance". Tha4673t way, a single
 * backend can account for several merchants, as used to do in donation
 * shops
 */
struct MerchantInstance
{

  /**
   * Instance's mnemonic identifier. This value lives as long as
   * the configuration is kept in memory, as it's as substring of
   * a section name
   */
  char *id;

  /**
   * Legal name of the merchant.
   */
  char *name;

  /**
   * File holding the merchant's private key
   */
  char *keyfile;

  /**
   * Next entry in DLL.
   */
  struct WireMethod *wm_head;

  /**
   * Previous entry in DLL.
   */
  struct WireMethod *wm_tail;

  /**
   * Merchant's private key
   */
  struct TALER_MerchantPrivateKeyP privkey;

  /**
   * Merchant's public key
   */
  struct TALER_MerchantPublicKeyP pubkey;

  /**
   * Exchange this instance uses for tipping, NULL if tipping
   * is not supported.
   */
  char *tip_exchange;

  /**
   * What is the private key of the reserve used for signing tips by this exchange?
   * Only valid if @e tip_exchange is non-null.
   */
  struct TALER_ReservePrivateKeyP tip_reserve;
};


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
 * invoked to clean up the memory when the connection is completed.
 */
struct TM_HandlerContext
{

  /**
   * Function to execute the handler-specific cleanup of the
   * (typically larger) context.
   */
  TM_ContextCleanup cc;

  /**
   * Which request handler is handling this request?
   */
  const struct TMH_RequestHandler *rh;

};


/**
 * Locations from the configuration.  Mapping from
 * label to location data.
 */
extern json_t *default_locations;

/**
 * Default maximum wire fee to assume, unless stated differently in the proposal
 * already.
 */
extern struct TALER_Amount default_max_wire_fee;

/**
 * Default max deposit fee that the merchant is willing to
 * pay; if deposit costs more, then the customer will cover
 * the difference.
 */
extern struct TALER_Amount default_max_deposit_fee;

/**
 * Default factor for wire fee amortization.
 */
extern unsigned long long default_wire_fee_amortization;

/**
 * Hash of our wire format details as given in #j_wire.
 */
extern struct GNUNET_HashCode h_wire;

/**
 * Our private key, corresponds to #pubkey.
 */
extern struct TALER_MerchantPrivateKeyP privkey;

/**
 * Our public key, corresponds to #privkey.
 */
extern struct TALER_MerchantPublicKeyP pubkey;

/**
 * Should a "Connection: close" header be added to each HTTP response?
 */
extern int TMH_merchant_connection_close;

/**
 * Handle to the database backend.
 */
extern struct TALER_MERCHANTDB_Plugin *db;

/**
 * If the frontend does NOT specify an execution date, how long should
 * we tell the exchange to wait to aggregate transactions before
 * executing the wire transfer?  This delay is added to the current
 * time when we generate the advisory execution time for the exchange.
 */
extern struct GNUNET_TIME_Relative wire_transfer_delay;

/**
 * If the frontend does NOT specify a payment deadline, how long should
 * offers we make be valid by default?
 */
extern struct GNUNET_TIME_Relative default_pay_deadline;

/**
 * Kick MHD to run now, to be called after MHD_resume_connection().
 * Basically, we need to explicitly resume MHD's event loop whenever
 * we made progress serving a request.  This function re-schedules
 * the task processing MHD's activities to run immediately.
 */
void
TMH_trigger_daemon (void);

/**
 * Lookup a merchant instance by its name.
 *
 * @param name name of the instance to resolve
 * @return NULL if that instance is unknown to us
 */
struct MerchantInstance *
TMH_lookup_instance (const char *name);


/**
 * Extract merchant instance from the given JSON
 *
 * @param json the JSON to inspect; it is not required to
 * comply with any particular format. It will only be checked
 * if the field "instance" is there.
 * @return a pointer to a #struct MerchantInstance. This will be
 * the 'default' merchant if the frontend did not specif any
 * "instance" field. The user should not care to free the returned
 * value, as it is taken from a global array that will be freed
 * by the general shutdown routine. NULL if the frontend specified
 * a wrong instance
 */
struct MerchantInstance *
TMH_lookup_instance_json (struct json_t *json);


/**
 * Make an absolute URL to the backend.
 *
 * @param connection MHD connection to take header values from
 * @param path path of the url
 * @param ... NULL-terminated key-value pairs (char *) for query parameters
 * @returns the URL, must be freed with #GNUNET_free
 */
char *
TMH_make_absolute_backend_url (struct MHD_Connection *connection, char *path, ...);


/**
 * Convert an amount in a JSON object from the string amount format to the JSON
 * amount format.  Does nothing if the field is missing or already a JSON
 * object.
 *
 * @param json json with the amount to convert
 * @param field_name name of the field to convert
 * @returns #GNUNET_OK on success, #GNUNET_SYSERR on invalid format
 */
int
TMH_convert_amount (json_t *json, char *field_name);

#endif
