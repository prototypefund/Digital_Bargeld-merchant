/*
  This file is part of TALER
  Copyright (C) 2014-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LIB.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file include/taler_merchant_service.h
 * @brief C interface of libtalermerchant, a C library to use merchant's HTTP API
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#ifndef _TALER_MERCHANT_SERVICE_H
#define _TALER_MERCHANT_SERVICE_H

#include <taler/taler_util.h>
#include <taler/taler_error_codes.h>
#include <taler/taler_exchange_service.h>
#include <gnunet/gnunet_curl_lib.h>
#include <jansson.h>


/**
 * General information about the HTTP response we obtained
 * from the merchant for a request.
 */
struct TALER_MERCHANT_HttpResponse
{

  /**
   * The complete JSON reply. NULL if we failed to parse the
   * reply (too big, invalid JSON).
   */
  const json_t *reply;

  /**
   * The complete JSON reply from the exchange, if we generated an error in
   * response to an exchange error.  Usually set if @e http_status is
   * #MHD_HTTP_FAILED_DEPENDENDCY or #MHD_HTTP_SERVICE_UNAVAILABLE. NULL if we
   * failed to obtain a JSON reply from the exchange or if we did not receive
   * an error from the exchange.
   */
  const json_t *exchange_reply;

  /**
   * Set to the human-readable 'hint' that is optionally
   * provided by the exchange together with errors. NULL
   * if no hint was provided or if there was no error.
   */
  const char *hint;

  /**
   * The error hint from the exchange, if we generated an error in
   * response to an exchange error.  Usually set if @e http_status is
   * #MHD_HTTP_FAILED_DEPENDENDCY or #MHD_HTTP_SERVICE_UNAVAILABLE. NULL if we
   * failed to obtain a hint from the exchange or if we did not receive
   * an error from the exchange.
   */
  const char *exchange_hint;

  /**
   * HTTP status code for the response.  0 if the
   * HTTP request failed and we did not get any answer, or
   * if the answer was invalid and we set @a ec to a
   * client-side error code.
   */
  unsigned int http_status;

  /**
   * The HTTP status code from the exchange, if we generated an error in
   * response to an exchange error.  Usually set if @e http_status is
   * #MHD_HTTP_FAILED_DEPENDENDCY or #MHD_HTTP_SERVICE_UNAVAILABLE. 0 if we
   * failed to obtain a JSON reply from the exchange or if we did not receive
   * an error from the exchange.
   */
  unsigned int exchange_http_status;

  /**
   * Taler error code.  #TALER_EC_NONE if everything was
   * OK.  Usually set to the "code" field of an error
   * response, but may be set to values created at the
   * client side, for example when the response was
   * not in JSON format or was otherwise ill-formed.
   */
  enum TALER_ErrorCode ec;

  /**
   * The error code from the reply from the exchange, if we generated an error in
   * response to an exchange error.  Usually set if @e http_status is
   * #MHD_HTTP_FAILED_DEPENDENDCY or #MHD_HTTP_SERVICE_UNAVAILABLE. NULL if we
   * failed to obtain a error code from the exchange or if we did not receive
   * an error from the exchange.
   */
  enum TALER_ErrorCode exchange_code;

};


/**
 * Take a @a response from the merchant API that (presumably) contains
 * error details and setup the corresponding @a hr structure.  Internally
 * used to convert merchant's responses in to @a hr.
 *
 * @param response, if NULL we will report #TALER_EC_INVALIDD_RESPONSE in `ec`
 * @param http_status http status to use
 * @param[out] hr response object to initialize, fields will
 *        only be valid as long as @a response is valid as well
 */
void
TALER_MERCHANT_parse_error_details_ (const json_t *response,
                                     unsigned int http_status,
                                     struct TALER_MERCHANT_HttpResponse *hr);


/* ********************* /public/config ****************** */


/**
 * How compatible are the protocol version of the auditor and this
 * client?  The bits (1,2,4) can be used to test if the auditor's
 * version is incompatible, older or newer respectively.
 */
enum TALER_MERCHANT_VersionCompatibility
{

  /**
   * The auditor runs exactly the same protocol version.
   */
  TALER_MERCHANT_VC_MATCH = 0,

  /**
   * The auditor is too old or too new to be compatible with this
   * implementation (bit)
   */
  TALER_MERCHANT_VC_INCOMPATIBLE = 1,

  /**
   * The auditor is older than this implementation (bit)
   */
  TALER_MERCHANT_VC_OLDER = 2,

  /**
   * The auditor is too old to be compatible with
   * this implementation.
   */
  TALER_MERCHANT_VC_INCOMPATIBLE_OUTDATED
    = TALER_MERCHANT_VC_INCOMPATIBLE
      | TALER_MERCHANT_VC_OLDER,

  /**
   * The auditor is more recent than this implementation (bit).
   */
  TALER_MERCHANT_VC_NEWER = 4,

  /**
   * The auditor is too recent for this implementation.
   */
  TALER_MERCHANT_VC_INCOMPATIBLE_NEWER
    = TALER_MERCHANT_VC_INCOMPATIBLE
      | TALER_MERCHANT_VC_NEWER,

  /**
   * We could not even parse the version data.
   */
  TALER_MERCHANT_VC_PROTOCOL_ERROR = 8

};


/**
 * @brief Config information we get from the backend.
 */
struct TALER_MERCHANT_ConfigInformation
{
  /**
   * Currency used/supported by the merchant.
   */
  const char *currency;

  /**
   * Supported Taler protocol version by the merchant.
   * String in the format current:revision:age using the
   * semantics of GNU libtool.  See
   * https://www.gnu.org/software/libtool/manual/html_node/Versioning.html#Versioning
   */
  const char *version;

};


/**
 * Function called with information about the merchant.
 *
 * @param cls closure
 * @param hr HTTP response data
 * @param ci basic information about the merchant
 * @param compat protocol compatibility information
 */
typedef void
(*TALER_MERCHANT_ConfigCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  const struct TALER_MERCHANT_ConfigInformation *ci,
  enum TALER_MERCHANT_VersionCompatibility compat);


/**
 * Handle for a #TALER_MERCHANT_config_get() operation.
 */
struct TALER_MERCHANT_ConfigGetHandle;


/**
 * Get the config data of a merchant. Will connect to the merchant backend
 * and obtain information about the backend.  The respective information will
 * be passed to the @a config_cb once available.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param config_cb function to call with the
 *        backend's config information
 * @param config_cb_cls closure for @a config_cb
 * @return the config check handle; NULL upon error
 */
struct TALER_MERCHANT_ConfigGetHandle *
TALER_MERCHANT_config_get (struct GNUNET_CURL_Context *ctx,
                           const char *backend_url,
                           TALER_MERCHANT_ConfigCallback config_cb,
                           void *config_cb_cls);


/**
 * Cancel /config request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param vgh request to cancel.
 */
void
TALER_MERCHANT_config_get_cancel (struct TALER_MERCHANT_ConfigGetHandle *vgh);


/* ********************* /instances *********************** */


/**
 * @brief Information about a merchant instance.
 */
struct TALER_MERCHANT_InstanceInformation
{
  /**
   * Id of this instance.  This $ID can be used to construct the URL of the
   * instance, by combining it using "$MERCHANT_BASEURL/instances/$ID/".
   */
  const char *id;

  /**
   * Legal name of the merchant/instance.
   */
  const char *name;

  /**
   * Public key of the instance.
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * JSON array of payment targets (strings) supported by this backend
   * instance.
   */
  json_t *payment_targets;

};


/**
 * Handle for a GET /instances operation.
 */
struct TALER_MERCHANT_InstancesGetHandle;


/**
 * Function called with the result of the GET /instances operation.
 *
 * @param cls closure
 * @param hr HTTP response data
 * @param iis_length length of the @a iis array
 * @param iis array with instance information of length @a iis_length
 */
typedef void
(*TALER_MERCHANT_InstancesGetCallback)(
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  unsigned int iis_length,
  const struct TALER_MERCHANT_InstanceInformation iis[]);


/**
 * Get the instance data of a backend. Will connect to the merchant backend
 * and obtain information about the instances.  The respective information will
 * be passed to the @a instances_cb once available.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instances_cb function to call with the
 *        backend's instances information
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstancesGetHandle *
TALER_MERCHANT_instances_get (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              TALER_MERCHANT_InstancesGetCallback instances_cb,
                              void *instances_cb_cls);


/**
 * Cancel /instances request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param igh request to cancel.
 */
void
TALER_MERCHANT_instances_get_cancel (
  struct TALER_MERCHANT_InstancesGetHandle *igh);


/**
 * Handle for a POST /instances/$ID operation.
 */
struct TALER_MERCHANT_InstancesPostHandle;


/**
 * Function called with the result of the GET /instances/$ID operation.
 *
 * @param cls closure
 * @param hr HTTP response data
 */
typedef void
(*TALER_MERCHANT_InstancesPostCallback)(
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Setup an new instance in the backend.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id identity of the instance to get information about
 * @param payto_uris_length length of the @a accounts array
 * @param payto_uris URIs of the bank accounts of the merchant instance
 * @param name name of the merchant instance
 * @param address physical address of the merchant instance
 * @param jurisdiction jurisdiction of the merchant instance
 * @param default_max_wire_fee default maximum wire fee merchant is willing to fully pay
 * @param default_wire_fee_amortization default amortization factor for excess wire fees
 * @param default_max_deposit_fee default maximum deposit fee merchant is willing to pay
 * @param default_wire_transfer_delay default wire transfer delay merchant will ask for
 * @param default_pay_delay default validity period for offers merchant makes
 * @param cb function to call with the
 *        backend's instances information
 * @param cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstancesPostHandle *
TALER_MERCHANT_instances_post (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  unsigned int accounts_length,
  const char *payto_uris[],
  const char *name,
  const json_t *address,
  const json_t *jurisdiction,
  const struct TALER_Amount *default_max_wire_fee,
  uint32_t default_wire_fee_amortization,
  const struct TALER_Amount *default_max_deposit_fee,
  struct GNUNET_TIME_Relative default_wire_transfer_delay,
  struct GNUNET_TIME_Relative default_pay_delay,
  TALER_MERCHANT_InstancesPostCallback cb,
  void *cb_cls);


/**
 * Cancel /instances request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param igh request to cancel.
 */
void
TALER_MERCHANT_instances_post_cancel (
  struct TALER_MERCHANT_InstancesPostHandle *iph);


/**
 * Handle for a PATCH /instances/$ID operation.
 */
struct TALER_MERCHANT_InstancePatchHandle;


/**
 * Function called with the result of the GET /instances/$ID operation.
 *
 * @param cls closure
 * @param hr HTTP response data
 */
typedef void
(*TALER_MERCHANT_InstancePatchCallback)(
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Setup an new instance in the backend.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id identity of the instance to get information about
 * @param payto_uris_length length of the @a accounts array
 * @param payto_uris URIs of the bank accounts of the merchant instance
 * @param name name of the merchant instance
 * @param address physical address of the merchant instance
 * @param jurisdiction jurisdiction of the merchant instance
 * @param default_max_wire_fee default maximum wire fee merchant is willing to fully pay
 * @param default_wire_fee_amortization default amortization factor for excess wire fees
 * @param default_max_deposit_fee default maximum deposit fee merchant is willing to pay
 * @param default_wire_transfer_delay default wire transfer delay merchant will ask for
 * @param default_pay_delay default validity period for offers merchant makes
 * @param cb function to call with the
 *        backend's instances information
 * @param cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstancePatchHandle *
TALER_MERCHANT_instance_patch (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  unsigned int accounts_length,
  const char *payto_uris[],
  const char *name,
  const json_t *address,
  const json_t *jurisdiction,
  const struct TALER_Amount *default_max_wire_fee,
  uint32_t default_wire_fee_amortization,
  const struct TALER_Amount *default_max_deposit_fee,
  struct GNUNET_TIME_Relative default_wire_transfer_delay,
  struct GNUNET_TIME_Relative default_pay_delay,
  TALER_MERCHANT_InstancePatchCallback cb,
  void *cb_cls);


/**
 * Cancel /instances request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param igh request to cancel.
 */
void
TALER_MERCHANT_instance_patch_cancel (
  struct TALER_MERCHANT_InstancePatchHandle *iph);


/**
 * Handle for a GET /instances/$ID operation.
 */
struct TALER_MERCHANT_InstanceGetHandle;


/**
 * Details about a merchant's bank account.
 */
struct TALER_MERCHANT_Account
{
  /**
   * salt used to compute h_wire
   */
  struct GNUNET_HashCode salt;

  /**
   * payto:// URI of the account.
   */
  const char *payto_uri;

  /**
   * Hash of @e payto_uri and @e salt.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * true if the account is active,
   * false if it is historic.
   */
  bool active;
};


/**
 * Details about an instance.
 */
struct TALER_MERCHANT_InstanceDetails
{
  /**
   * Name of the merchant instance
   */
  const char *name;

  /**
   * public key of the merchant instance
   */
  const struct TALER_MerchantPublicKeyP *merchant_pub;

  /**
   * physical address of the merchant instance
   */
  const json_t *address;

  /**
   * jurisdiction of the merchant instance
   */
  const json_t *jurisdiction;

  /**
   * default maximum wire fee merchant is willing to fully pay
   */
  const struct TALER_Amount *default_max_wire_fee;

  /**
   * default amortization factor for excess wire fees
   */
  uint32_t default_wire_fee_amortization;

  /**
   * default maximum deposit fee merchant is willing to pay
   */
  const struct TALER_Amount *default_max_deposit_fee;

  /**
   * default wire transfer delay merchant will ask for
   */
  struct GNUNET_TIME_Relative default_wire_transfer_delay;

  /**
   * default validity period for offers merchant makes
   */
  struct GNUNET_TIME_Relative default_pay_delay;
};


/**
 * Function called with the result of the GET /instances/$ID operation.
 *
 * @param cls closure
 * @param hr HTTP response data
 * @param accounts_length length of the @a accounts array
 * @param accounts bank accounts of the merchant instance
 * @param details details about the instance configuration
 */
typedef void
(*TALER_MERCHANT_InstanceGetCallback)(
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  unsigned int accounts_length,
  const struct TALER_MERCHANT_Account accounts[],
  const struct TALER_MERCHANT_InstanceDetails *details);


/**
 * Get the details on one of the instances of a backend. Will connect to the
 * merchant backend and obtain information about the instance.  The respective
 * information will be passed to the @a cb once available.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id identity of the instance to get information about
 * @param cb function to call with the
 *        backend's instances information
 * @param cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstanceGetHandle *
TALER_MERCHANT_instance_get (struct GNUNET_CURL_Context *ctx,
                             const char *backend_url,
                             const char *instance_id,
                             TALER_MERCHANT_InstanceGetCallback cb,
                             void *cb_cls);


/**
 * Cancel /instances request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param igh request to cancel.
 */
void
TALER_MERCHANT_instance_get_cancel (
  struct TALER_MERCHANT_InstanceGetHandle *igh);


/**
 * Handle for a DELETE /instances operation.
 */
struct TALER_MERCHANT_InstanceDeleteHandle;


/**
 * Function called with the result of the DELETE /instances operation.
 *
 * @param cls closure
 * @param hr HTTP response data
 */
typedef void
(*TALER_MERCHANT_InstanceDeleteCallback)(
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Delete the private key of an instance of a backend, thereby disabling the
 * instance for future requests.  Will preserve the other instance data
 * (i.e. for taxation).
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id which instance should be deleted
 * @param instances_cb function to call with the
 *        backend's return
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstanceDeleteHandle *
TALER_MERCHANT_instance_delete (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  TALER_MERCHANT_InstanceDeleteCallback instances_cb,
  void *instances_cb_cls);


/**
 * Purge all data associated with an instance. Use with
 * extreme caution.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id which instance should be deleted
 * @param instances_cb function to call with the
 *        backend's return
 * @param instances_cb_cls closure for @a config_cb
 * @return the instances handle; NULL upon error
 */
struct TALER_MERCHANT_InstanceDeleteHandle *
TALER_MERCHANT_instance_purge (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  TALER_MERCHANT_InstanceDeleteCallback instances_cb,
  void *instances_cb_cls);


/**
 * Cancel /instances DELETE request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param idh request to cancel.
 */
void
TALER_MERCHANT_instance_delete_cancel (
  struct TALER_MERCHANT_InstanceDeleteHandle *idh);


/**
 * Cancel /instances DELETE request.  Must not be called by clients after
 * the callback was invoked.
 *
 * @param arg request to cancel.
 */
#define TALER_MERCHANT_instance_purge_cancel(arg) \
  TALER_MERCHANT_instance_delete_cancel (arg)


/* ********************* /products *********************** */


/**
 * Handle for a GET /products operation.
 */
struct TALER_MERCHANT_ProductsGetHandle;

/**
 * Individual product from the inventory (minimal information
 * returned via GET /products).
 */
struct TALER_MERCHANT_InventoryEntry
{
  /**
   * Product identifier.
   */
  const char *product_id;

};


/**
 * Function called with the result of the GET /products operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param products_length length of the @a products array
 * @param products array of products the requested instance offers
 */
typedef void
(*TALER_MERCHANT_ProductsGetCallback)(
  void *cls,
  struct TALER_MERCHANT_HttpResponse *hr,
  unsigned int products_length,
  const struct TALER_MERCHANT_InventoryEntry products[]);


/**
 * Make a GET /products request.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to query about its products,
 *                    NULL to query the default instance
 * @param cb function to call with the backend's inventory information
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductsGetHandle *
TALER_MERCHANT_products_get (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  TALER_MERCHANT_ProductsGetCallback cb,
  void *cb_cls);


/**
 * Cancel GET /products operation.
 *
 * @param pgh operation to cancel
 */
void
TALER_MERCHANT_products_get_cancel (
  struct TALER_MERCHANT_ProductsGetHandle *pgh);


/**
 * Handle for a GET /product/$ID operation. Gets details
 * about a single product. Do not confused with a
 * `struct TALER_MERCHANT_ProductsGetHandle`, which
 * obtains a list of all products.
 */
struct TALER_MERCHANT_ProductGetHandle;


/**
 * Function called with the result of the GET /products operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param description description of the product
 * @param description_i18n Map from IETF BCP 47 language tags to localized descriptions
 * @param unit unit in which the product is measured (liters, kilograms, packages, etc.)
 * @param price the price for one @a unit of the product, zero is used to imply that
 *              this product is not sold separately or that the price is not fixed and
 *              must be supplied by the front-end.  If non-zero, price must include
 *              applicable taxes.
 * @param image base64-encoded product image
 * @param taxes list of taxes paid by the merchant
 * @param total_stocked in @a units, -1 to indicate "infinite" (i.e. electronic books),
 *                does NOT indicate remaining stocks, to get remaining stocks,
 *                subtract @a total_sold and @a total_lost. Note that this still
 *                does not then say how many of the remaining inventory are locked.
 * @param total_sold in @a units, total number of @a unit of product sold
 * @param total_lost in @a units, total number of @a unit of product lost from inventory
 * @param location where the product is in stock
 * @param next_restock when the next restocking is expected to happen, 0 for unknown,
 *                     #GNUNET_TIME_UNIT_FOREVER_ABS for 'never'.
 */
typedef void
(*TALER_MERCHANT_ProductGetCallback)(
  void *cls,
  struct TALER_MERCHANT_HttpResponse *hr,
  const char *description,
  const json_t *description_i18n,
  const char *unit,
  const struct TALER_Amount *price,
  const json_t *image,
  const json_t *taxes,
  int64_t total_stocked,
  uint64_t total_sold,
  uint64_t total_lost,
  const json_t *location,
  struct GNUNET_TIME_Absolute next_restock);


/**
 * Make a GET /product/$ID request to get details about an
 * individual product.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to query about its products,
 *                    NULL to query the default instance
 * @param product_id identifier of the product to inquire about
 * @param cb function to call with the backend's product information
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductGetHandle *
TALER_MERCHANT_product_get (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  TALER_MERCHANT_ProductGetCallback cb,
  void *cb_cls);


/**
 * Cancel GET /products/$ID operation.
 *
 * @param pgh operation to cancel
 */
void
TALER_MERCHANT_product_get_cancel (
  struct TALER_MERCHANT_ProductGetHandle *pgh);


/**
 * Handle for a POST /products operation.
 */
struct TALER_MERCHANT_ProductsPostHandle;


/**
 * Function called with the result of the POST /products operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 */
typedef void
(*TALER_MERCHANT_ProductsPostCallback)(
  void *cls,
  struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Make a POST /products request to add a product to the
 * inventory.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to add a product to,
 *                    NULL to query the default instance
 * @param product_id identifier to use for the product
 * @param description description of the product
 * @param description_i18n Map from IETF BCP 47 language tags to localized descriptions
 * @param unit unit in which the product is measured (liters, kilograms, packages, etc.)
 * @param price the price for one @a unit of the product, zero is used to imply that
 *              this product is not sold separately or that the price is not fixed and
 *              must be supplied by the front-end.  If non-zero, price must include
 *              applicable taxes.
 * @param image base64-encoded product image
 * @param taxes list of taxes paid by the merchant
 * @param total_stocked in @a units, -1 to indicate "infinite" (i.e. electronic books)
 * @param address where the product is in stock
 * @param next_restock when the next restocking is expected to happen, 0 for unknown,
 *                     #GNUNET_TIME_UNIT_FOREVER_ABS for 'never'.
 * @param cb function to call with the backend's result
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductsPostHandle *
TALER_MERCHANT_products_post (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  const char *description,
  const json_t *description_i18n,
  const char *unit,
  const struct TALER_Amount *price,
  const json_t *image,
  const json_t *taxes,
  int64_t total_stocked,
  const json_t *address,
  struct GNUNET_TIME_Absolute next_restock,
  TALER_MERCHANT_ProductsPostCallback cb,
  void *cb_cls);


/**
 * Cancel POST /products operation.
 *
 * @param pph operation to cancel
 */
void
TALER_MERCHANT_products_post_cancel (
  struct TALER_MERCHANT_ProductsPostHandle *pph);


/**
 * Handle for a PATCH /products operation.
 */
struct TALER_MERCHANT_ProductPatchHandle;


/**
 * Function called with the result of the PATCH /products operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 */
typedef void
(*TALER_MERCHANT_ProductPatchCallback)(
  void *cls,
  struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Make a PATCH /products request to update product details in the
 * inventory.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to add a product to,
 *                    NULL to query the default instance
 * @param product_id identifier to use for the product; the product must exist,
 *                    or the transaction will fail with a #MHD_HTTP_NOT_FOUND
 *                    HTTP status code
 * @param description description of the product
 * @param description_i18n Map from IETF BCP 47 language tags to localized descriptions
 * @param unit unit in which the product is measured (liters, kilograms, packages, etc.)
 * @param price the price for one @a unit of the product, zero is used to imply that
 *              this product is not sold separately or that the price is not fixed and
 *              must be supplied by the front-end.  If non-zero, price must include
 *              applicable taxes.
 * @param image base64-encoded product image
 * @param taxes list of taxes paid by the merchant
 * @param total_stocked in @a units, -1 to indicate "infinite" (i.e. electronic books),
 *               must be larger than previous values
 * @param total_lost in @a units, must be larger than previous values, and may
 *               not exceed total_stocked minus total_sold; if it does, the transaction
 *               will fail with a #MHD_HTTP_CONFLICT HTTP status code
 * @param address where the product is in stock
 * @param next_restock when the next restocking is expected to happen
 * @param cb function to call with the backend's result
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductPatchHandle *
TALER_MERCHANT_product_patch (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  const char *description,
  const json_t *description_i18n,
  const char *unit,
  const struct TALER_Amount *price,
  const json_t *image,
  const json_t *taxes,
  int64_t total_stocked,
  uint64_t total_lost,
  const json_t *address,
  struct GNUNET_TIME_Absolute next_restock,
  TALER_MERCHANT_ProductPatchCallback cb,
  void *cb_cls);


/**
 * Cancel PATCH /products operation.
 *
 * @param pph operation to cancel
 */
void
TALER_MERCHANT_product_patch_cancel (
  struct TALER_MERCHANT_ProductPatchHandle *pph);


/**
 * Handle for a POST /products/$ID/lock operation.
 */
struct TALER_MERCHANT_ProductLockHandle;


/**
 * Function called with the result of the POST /product/$ID/lock operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 */
typedef void
(*TALER_MERCHANT_ProductLockCallback)(
  void *cls,
  struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Make a POST /products/$ID/lock request to reserve a certain
 * amount of product in inventory to a reservation UUID.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to query about its products,
 *                    NULL to query the default instance
 * @param product_id identifier of the product
 * @param uuid UUID that identifies the client holding the lock
 * @param duration how long should the lock be held
 * @param quantity how much product should be locked
 * @param cb function to call with the backend's lock status
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductLockHandle *
TALER_MERCHANT_product_lock (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  struct GNUNET_TIME_Relative duration,
  uint32_t quantity,
  TALER_MERCHANT_ProductLockCallback cb,
  void *cb_cls);


/**
 * Cancel POST /products/$ID/lock operation. Note that the
 * lock may or may not be acquired despite the cancellation.
 *
 * @param pdh operation to cancel
 */
void
TALER_MERCHANT_product_lock_cancel (
  struct TALER_MERCHANT_ProductLockHandle *plh);


/**
 * Handle for a DELETE /products/$ID operation.
 */
struct TALER_MERCHANT_ProductDeleteHandle;


/**
 * Function called with the result of the DELETE /product/$ID operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 */
typedef void
(*TALER_MERCHANT_ProductDeleteCallback)(
  void *cls,
  struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Make a DELETE /products/$ID request to delete a product from our
 * inventory.
 *
 * @param ctx the context
 * @param backend_url HTTP base URL for the backend
 * @param instance_id instance to query about its products,
 *                    NULL to query the default instance
 * @param product_id identifier of the product
 * @param cb function to call with the backend's deletion status
 * @param cb_cls closure for @a cb
 * @return the request handle; NULL upon error
 */
struct TALER_MERCHANT_ProductDeleteHandle *
TALER_MERCHANT_product_delete (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *instance_id,
  const char *product_id,
  TALER_MERCHANT_ProductDeleteCallback cb,
  void *cb_cls);


/**
 * Cancel DELETE /products/$ID operation.
 *
 * @param pdh operation to cancel
 */
void
TALER_MERCHANT_product_delete_cancel (
  struct TALER_MERCHANT_ProductDeleteHandle *pdh);


/* *********************   OLD ************************** */


/* ********************* /refund ************************** */

/**
 * Handle for a GET /refund operation.
 */
struct TALER_MERCHANT_RefundLookupOperation;


/**
 * Detail about a refund lookup result.
 */
struct TALER_MERCHANT_RefundDetail
{

  /**
   * Exchange response details.  Full details are only included
   * upon failure (HTTP status is not #MHD_HTTP_OK).
   */
  struct TALER_EXCHANGE_HttpResponse hr;

  /**
   * Coin this detail is about.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Refund transaction ID used.
   */
  uint64_t rtransaction_id;

  /**
   * Amount to be refunded for this coin.
   */
  struct TALER_Amount refund_amount;

  /**
   * Applicable refund transaction fee.
   */
  struct TALER_Amount refund_fee;

  /**
   * Public key of the exchange affirming the refund,
   * only valid if the @e hr http_status is #MHD_HTTP_OK.
   */
  struct TALER_ExchangePublicKeyP exchange_pub;

  /**
   * Signature of the exchange affirming the refund,
   * only valid if the @e hr http_status is #MHD_HTTP_OK.
   */
  struct TALER_ExchangeSignatureP exchange_sig;

};


/**
 * Callback to process a GET /refund request
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param h_contract_terms hash of the contract terms to which the refund is applied
 * @param merchant_pub public key of the merchant
 * @param num_details length of the @a details array
 * @param details details about the refund processing
 */
typedef void
(*TALER_MERCHANT_RefundLookupCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  unsigned int num_details,
  const struct TALER_MERCHANT_RefundDetail *details);


/**
 * Does a GET /refund.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param cb callback which will work the response gotten from the backend
 * @param cb_cls closure to pass to the callback
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_RefundLookupOperation *
TALER_MERCHANT_refund_lookup (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              const char *order_id,
                              TALER_MERCHANT_RefundLookupCallback cb,
                              void *cb_cls);

/**
 * Cancel a GET /refund request.
 *
 * @param rlo the refund increasing operation to cancel
 */
void
TALER_MERCHANT_refund_lookup_cancel (
  struct TALER_MERCHANT_RefundLookupOperation *rlo);


/**
 * Handle for a POST /refund operation.
 */
struct TALER_MERCHANT_RefundIncreaseOperation;


/**
 * Callback to process a POST /refund request
 *
 * @param cls closure
 * @param http_status HTTP status code for this request
 * @param ec taler-specific error code
 */
typedef void
(*TALER_MERCHANT_RefundIncreaseCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Increase the refund associated to a order
 *
 * @param ctx the CURL context used to connect to the backend
 * @param backend_url backend's base URL, including final "/"
 * @param order_id id of the order whose refund is to be increased
 * @param refund amount to which increase the refund
 * @param reason human-readable reason justifying the refund
 * @param cb callback processing the response from /refund
 * @param cb_cls closure for cb
 */
struct TALER_MERCHANT_RefundIncreaseOperation *
TALER_MERCHANT_refund_increase (struct GNUNET_CURL_Context *ctx,
                                const char *backend_url,
                                const char *order_id,
                                const struct TALER_Amount *refund,
                                const char *reason,
                                TALER_MERCHANT_RefundIncreaseCallback cb,
                                void *cb_cls);

/**
 * Cancel a POST /refund request.
 *
 * @param rio the refund increasing operation to cancel
 */
void
TALER_MERCHANT_refund_increase_cancel (
  struct TALER_MERCHANT_RefundIncreaseOperation *rio);


/* *********************  /proposal *********************** */


/**
 * Handle to a PUT /proposal operation
 */
struct TALER_MERCHANT_ProposalOperation;

/**
 * Callbacks of this type are used to serve the result of submitting a
 * /contract request to a merchant.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param order_id order id of the newly created order
 */
typedef void
(*TALER_MERCHANT_ProposalCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  const char *order_id);


/**
 * PUT an order to the backend and receives the related proposal.
 *
 * @param ctx execution context
 * @param backend_url URL of the backend
 * @param order basic information about this purchase, to be extended by the
 * backend
 * @param proposal_cb the callback to call when a reply for this request is available
 * @param proposal_cb_cls closure for @a proposal_cb
 * @return a handle for this request, NULL on error
 */
struct TALER_MERCHANT_ProposalOperation *
TALER_MERCHANT_order_put (struct GNUNET_CURL_Context *ctx,
                          const char *backend_url,
                          const json_t *order,
                          TALER_MERCHANT_ProposalCallback proposal_cb,
                          void *proposal_cb_cls);


/**
 * Cancel a PUT /proposal request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param po the proposal operation request handle
 */
void
TALER_MERCHANT_proposal_cancel (struct TALER_MERCHANT_ProposalOperation *po);


/**
 * Handle to a GET /proposal operation
 */
struct TALER_MERCHANT_ProposalLookupOperation;


/**
 * Callback called to work a GET /proposal response.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param contract_terms the details of the contract
 * @param sig merchant's signature over @a contract_terms
 * @param contract_hash hash over @a contract_terms
 */
typedef void
(*TALER_MERCHANT_ProposalLookupOperationCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  const json_t *contract_terms,
  const struct TALER_MerchantSignatureP *sig,
  const struct GNUNET_HashCode *contract_hash);


/**
 * Calls the GET /proposal API at the backend.  That is,
 * retrieve a proposal data by providing its transaction id.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id used to perform the lookup
 * @param nonce nonce to use, only used when requesting the proposal the first time,
 *              can be NULL to omit the nonce (after the first request)
 * @param plo_cb callback which will work the response gotten from the backend
 * @param plo_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_ProposalLookupOperation *
TALER_MERCHANT_proposal_lookup (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *order_id,
  const struct GNUNET_CRYPTO_EddsaPublicKey *nonce,
  TALER_MERCHANT_ProposalLookupOperationCallback plo_cb,
  void *plo_cb_cls);


/**
 * Cancel a GET /proposal request.
 *
 * @param plo handle to the request to be canceled
 */
void
TALER_MERCHANT_proposal_lookup_cancel (
  struct TALER_MERCHANT_ProposalLookupOperation *plo);


/* *********************  /pay *********************** */


/**
 * @brief Handle to a /pay operation at a merchant.  Note that we use
 * the same handle for interactions with frontends (API for wallets)
 * or backends (API for frontends).  The difference is that for the
 * frontend API, we need the private keys of the coins, while for
 * the backend API we need the public keys and signatures received
 * from the wallet.  Also, the frontend returns a redirect URL on
 * success, while the backend just returns a success status code.
 */
struct TALER_MERCHANT_Pay;


/**
 * Callbacks of this type are used to serve the result of submitting a
 * /pay request to a merchant.
 *
 * @param cls closure
 * @param hr HTTP response details
 */
typedef void
(*TALER_MERCHANT_PayCallback) (void *cls,
                               const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Information we need from the wallet for each coin when doing the
 * payment.
 */
struct TALER_MERCHANT_PayCoin
{
  /**
   * Denomination key with which the coin is signed
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Exchange’s unblinded signature of the coin
   */
  struct TALER_DenominationSignature denom_sig;

  /**
   * Overall value that coins of this @e denom_pub have.
   */
  struct TALER_Amount denom_value;

  /**
   * Coin's private key.
   */
  struct TALER_CoinSpendPrivateKeyP coin_priv;

  /**
   * Amount this coin contributes to (including fee).
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Amount this coin contributes to (without fee).
   */
  struct TALER_Amount amount_without_fee;

  /**
   * Fee the exchange charges for refunds of this coin.
   */
  struct TALER_Amount refund_fee;

  /**
   * URL of the exchange that issued @e coin_priv.
   */
  const char *exchange_url;

};


/**
 * Pay a merchant.  API for wallets that have the coin's private keys.
 *
 * @param ctx execution context
 * @param merchant_url base URL of the merchant
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig signature from the merchant over the original contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param pay_deadline maximum time limit to pay for this contract
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_wallet (struct GNUNET_CURL_Context *ctx,
                           const char *merchant_url,
                           const struct GNUNET_HashCode *h_contract,
                           const struct TALER_Amount *amount,
                           const struct TALER_Amount *max_fee,
                           const struct TALER_MerchantPublicKeyP *merchant_pub,
                           const struct TALER_MerchantSignatureP *merchant_sig,
                           struct GNUNET_TIME_Absolute timestamp,
                           struct GNUNET_TIME_Absolute refund_deadline,
                           struct GNUNET_TIME_Absolute pay_deadline,
                           const struct GNUNET_HashCode *h_wire,
                           const char *order_id,
                           unsigned int num_coins,
                           const struct TALER_MERCHANT_PayCoin *coins,
                           TALER_MERCHANT_PayCallback pay_cb,
                           void *pay_cb_cls);


/**
 * Entry in the array of refunded coins.
 */
struct TALER_MERCHANT_RefundEntry
{
  /**
   * Merchant signature affirming the refund.
   */
  struct TALER_MerchantSignatureP merchant_sig;

  /**
   * Public key of the refunded coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Refund transaction ID.
   */
  uint64_t rtransaction_id;
};


/**
 * Callbacks of this type are used to serve the result of submitting a
 * /pay request to a merchant.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param merchant_pub public key of the merchant
 * @param h_contract hash of the contract
 * @param num_refunds size of the @a res array, 0 on errors
 * @param res merchant signatures refunding coins, NULL on errors
 */
typedef void
(*TALER_MERCHANT_PayRefundCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const struct GNUNET_HashCode *h_contract,
  unsigned int num_refunds,
  const struct TALER_MERCHANT_RefundEntry *res);


/**
 * Run a payment abort operation, asking for refunds for coins
 * that were previously spend on a /pay that failed to go through.
 *
 * @param ctx execution context
 * @param merchant_url base URL of the merchant
 * @param h_wire hash of the merchant’s account details
 * @param h_contract hash of the contact of the merchant with the customer
 * @param transaction_id transaction id for the transaction between merchant and customer
 * @param amount total value of the contract to be paid to the merchant
 * @param max_fee maximum fee covered by the merchant (according to the contract)
 * @param merchant_pub the public key of the merchant (used to identify the merchant for refund requests)
 * @param merchant_sig signature from the merchant over the original contract
 * @param timestamp timestamp when the contract was finalized, must match approximately the current time of the merchant
 * @param refund_deadline date until which the merchant can issue a refund to the customer via the merchant (can be zero if refunds are not allowed)
 * @param pay_deadline maximum time limit to pay for this contract
 * @param num_coins number of coins used to pay
 * @param coins array of coins we use to pay
 * @param coin_sig the signature made with purpose #TALER_SIGNATURE_WALLET_COIN_DEPOSIT made by the customer with the coin’s private key.
 * @param payref_cb the callback to call when a reply for this request is available
 * @param payref_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_abort (struct GNUNET_CURL_Context *ctx,
                          const char *merchant_url,
                          const struct GNUNET_HashCode *h_contract,
                          const struct TALER_Amount *amount,
                          const struct TALER_Amount *max_fee,
                          const struct TALER_MerchantPublicKeyP *merchant_pub,
                          const struct TALER_MerchantSignatureP *merchant_sig,
                          struct GNUNET_TIME_Absolute timestamp,
                          struct GNUNET_TIME_Absolute refund_deadline,
                          struct GNUNET_TIME_Absolute pay_deadline,
                          const struct GNUNET_HashCode *h_wire,
                          const char *order_id,
                          unsigned int num_coins,
                          const struct TALER_MERCHANT_PayCoin *coins,
                          TALER_MERCHANT_PayRefundCallback payref_cb,
                          void *payref_cb_cls);


/**
 * Information we need from the frontend (ok, the frontend sends just JSON)
 * when forwarding a payment to the backend.
 */
struct TALER_MERCHANT_PaidCoin
{
  /**
   * Denomination key with which the coin is signed
   */
  struct TALER_DenominationPublicKey denom_pub;

  /**
   * Exchange’s unblinded signature of the coin
   */
  struct TALER_DenominationSignature denom_sig;

  /**
   * Overall value that coins of this @e denom_pub have.
   */
  struct TALER_Amount denom_value;

  /**
   * Coin's public key.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Coin's signature key.
   */
  struct TALER_CoinSpendSignatureP coin_sig;

  /**
   * Amount this coin contributes to (including fee).
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Amount this coin contributes to (without fee).
   */
  struct TALER_Amount amount_without_fee;

  /**
   * Fee the exchange charges for refunds of this coin.
   */
  struct TALER_Amount refund_fee;

  /**
   * What is the URL of the exchange that issued @a coin_pub?
   */
  const char *exchange_url;

};


/**
 * Pay a merchant.  API for frontends talking to backends. Here,
 * the frontend does not have the coin's private keys, but just
 * the public keys and signatures.  Note the sublte difference
 * in the type of @a coins compared to #TALER_MERCHANT_pay().
 *
 * @param ctx execution context
 * @param merchant_url base URL of the merchant
 * @param merchant_pub public key of the merchant
 * @param order_id which order should be paid
 * @param num_coins length of the @a coins array
 * @param coins array of coins to pay with
 * @param pay_cb the callback to call when a reply for this request is available
 * @param pay_cb_cls closure for @a pay_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_Pay *
TALER_MERCHANT_pay_frontend (
  struct GNUNET_CURL_Context *ctx,
  const char *merchant_url,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const char *order_id,
  unsigned int num_coins,
  const struct TALER_MERCHANT_PaidCoin *coins,
  TALER_MERCHANT_PayCallback pay_cb,
  void *pay_cb_cls);


/**
 * Cancel a /pay request.  Note that if you cancel a request like
 * this, you have no assurance that the request has not yet been
 * forwarded to the merchant. Thus, the payment may still succeed or
 * fail.  Re-issue the original /pay request to resume/retry and
 * obtain a definitive result, or /refresh the coins involved to
 * ensure that the merchant can no longer complete the payment.
 *
 * @param ph the payment request handle
 */
void
TALER_MERCHANT_pay_cancel (struct TALER_MERCHANT_Pay *ph);


/* ********************* /track/transfer *********************** */

/**
 * @brief Handle to a /track/transfer operation at a merchant's backend.
 */
struct TALER_MERCHANT_TrackTransferHandle;

/**
 * Information about the _total_ amount that was paid back
 * by the exchange for a given h_contract_terms, by _one_ wire
 * transfer.
 */
struct TALER_MERCHANT_TrackTransferDetails
{

  /**
   * Total amount paid back by the exchange.
   */
  struct TALER_Amount deposit_value;

  /**
   * Total amount of deposit fees.
   */
  struct TALER_Amount deposit_fee;

  /**
   * Order ID associated whit this payment.
   */
  const char *order_id;

};

/**
 * Callbacks of this type are used to work the result of submitting a /track/transfer request to a merchant
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param sign_key exchange key used to sign @a json, or NULL
 * @param h_wire hash of the wire transfer address the transfer went to, or NULL on error
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
typedef void
(*TALER_MERCHANT_TrackTransferCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  const struct TALER_ExchangePublicKeyP *sign_key,
  const struct GNUNET_HashCode *h_wire,
  const struct TALER_Amount *total_amount,
  unsigned int details_length,
  const struct TALER_MERCHANT_TrackTransferDetails *details);


/**
 * Request backend to return deposits associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_url base URL of the backend
 * @param wire_method wire method used for the wire transfer
 * @param wtid base32 string indicating a wtid
 * @param exchange base URL of the exchange in charge of returning the wanted information
 * @param track_transfer_cb the callback to call when a reply for this request is available
 * @param track_transfer_cb_cls closure for @a track_transfer_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransferHandle *
TALER_MERCHANT_track_transfer (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *wire_method,
  const struct TALER_WireTransferIdentifierRawP *wtid,
  const char *exchange_url,
  TALER_MERCHANT_TrackTransferCallback
  track_transfer_cb,
  void *track_transfer_cb_cls);


/**
 * Cancel a /track/transfer request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param co the deposit's tracking operation
 */
void
TALER_MERCHANT_track_transfer_cancel (
  struct TALER_MERCHANT_TrackTransferHandle *tdo);


/* ********************* /track/transaction *********************** */

/**
 * @brief Handle to a /track/transaction operation at a merchant's backend.
 */
struct TALER_MERCHANT_TrackTransactionHandle;

/**
 * Information about a coin aggregated in a wire transfer for a
 * /track/transaction response.
 */
struct TALER_MERCHANT_CoinWireTransfer
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
 * Callbacks of this type are used to work the result of submitting a
 * /track/transaction request to a merchant
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param ec taler-specific error code
 * @param json original json reply from the backend
 * @param num_transfers number of wire transfers the exchange used for the transaction
 * @param transfers details about each transfer and which coins are aggregated in it
*/
typedef void
(*TALER_MERCHANT_TrackTransactionCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Request backend to return deposits associated with a given wtid.
 *
 * @param ctx execution context
 * @param backend_url base URL of the backend
 * @param order_id which order should we trace
 * @param track_transaction_cb the callback to call when a reply for this request is available
 * @param track_transaction_cb_cls closure for @a track_transaction_cb
 * @return a handle for this request
 */
struct TALER_MERCHANT_TrackTransactionHandle *
TALER_MERCHANT_track_transaction (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *order_id,
  TALER_MERCHANT_TrackTransactionCallback track_transaction_cb,
  void *track_transaction_cb_cls);


/**
 * Cancel a /track/transaction request.  This function cannot be used
 * on a request handle if a response is already served for it.
 *
 * @param tdo the tracking request to cancel
 */
void
TALER_MERCHANT_track_transaction_cancel (
  struct TALER_MERCHANT_TrackTransactionHandle *tdo);

/* ********************* /history *********************** */

struct TALER_MERCHANT_HistoryOperation;

/**
 * Callback for a /history request. It's up to this function how
 * to render the array containing transactions details (FIXME link to
 * documentation)
 *
 * @param cls closure
 * @param hr HTTP response details
 */
typedef void
(*TALER_MERCHANT_HistoryOperationCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr);


/**
 * Issue a /history request to the backend.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param start return @a delta records starting from position @a start
 * @param delta return @a delta records starting from position @a start
 * @param date only transactions younger than/equals to date will be returned
 * @param history_cb callback which will work the response gotten from the backend
 * @param history_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_HistoryOperation *
TALER_MERCHANT_history (struct GNUNET_CURL_Context *ctx,
                        const char *backend_url,
                        unsigned long long start,
                        long long delta,
                        struct GNUNET_TIME_Absolute date,
                        TALER_MERCHANT_HistoryOperationCallback history_cb,
                        void *history_cb_cls);

/**
 * Issue a /history request to the backend.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param start return `delta` records starting from position `start`.
 * If given as zero, then no initial skip of `start` records is done.
 * @param delta return `delta` records starting from position `start`
 * @param date only transactions younger than/equals to date will be returned
 * @param history_cb callback which will work the response gotten from the backend
 * @param history_cb_cls closure to pass to @a history_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_HistoryOperation *
TALER_MERCHANT_history_default_start (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  long long delta,
  struct GNUNET_TIME_Absolute date,
  TALER_MERCHANT_HistoryOperationCallback history_cb,
  void *history_cb_cls);


/**
 * Cancel a pending /history request
 *
 * @param ho handle from the operation to cancel
 */
void
TALER_MERCHANT_history_cancel (struct TALER_MERCHANT_HistoryOperation *ho);


/* ********************** /tip-authorize ********************** */

/**
 * Handle for a /tip-authorize operation.
 */
struct TALER_MERCHANT_TipAuthorizeOperation;


/**
 * Callback for a /tip-authorize request.  Returns the result of
 * the operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param tip_id which tip ID should be used to pickup the tip
 * @param tip_uri URI for the tip
 */
typedef void
(*TALER_MERCHANT_TipAuthorizeCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  struct GNUNET_HashCode *tip_id,
  const char *tip_uri);


/**
 * Issue a /tip-authorize request to the backend.  Informs the backend
 * that a tip should be created.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param pickup_url frontend URL for where the tip can be picked up
 * @param next_url where the browser should proceed after picking up the tip
 * @param amount amount to be handed out as a tip
 * @param justification which justification should be stored (human-readable reason for the tip)
 * @param authorize_cb callback which will work the response gotten from the backend
 * @param authorize_cb_cls closure to pass to @a authorize_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipAuthorizeOperation *
TALER_MERCHANT_tip_authorize (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              const char *pickup_url,
                              const char *next_url,
                              const struct TALER_Amount *amount,
                              const char *justification,
                              TALER_MERCHANT_TipAuthorizeCallback authorize_cb,
                              void *authorize_cb_cls);


/**
 * Cancel a pending /tip-authorize request
 *
 * @param ta handle from the operation to cancel
 */
void
TALER_MERCHANT_tip_authorize_cancel (
  struct TALER_MERCHANT_TipAuthorizeOperation *ta);

/* ********************** /tip-pickup ************************* */


/**
 * Handle for a /tip-pickup operation.
 */
struct TALER_MERCHANT_TipPickupOperation;


/**
 * Callback for a /tip-pickup request.  Returns the result of the operation.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param num_sigs length of the @a reserve_sigs array, 0 on error
 * @param sigs array of signatures over the coins, NULL on error
 */
typedef void
(*TALER_MERCHANT_TipPickupCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  unsigned int num_sigs,
  const struct TALER_DenominationSignature *sigs);


/**
 * Information per planchet.
 */
struct TALER_MERCHANT_PlanchetData
{
  /**
   * Planchet secrets.
   */
  struct TALER_PlanchetSecretsP ps;

  /**
   * Denomination key desired.
   */
  const struct TALER_EXCHANGE_DenomPublicKey *pk;

};

/**
 * Issue a /tip-pickup request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param tip_id unique identifier for the tip
 * @param num_planches number of planchets provided in @a pds
 * @param pds array of planchet secrets to be signed into existence for the tip
 * @param pickup_cb callback which will work the response gotten from the backend
 * @param pickup_cb_cls closure to pass to @a pickup_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipPickupOperation *
TALER_MERCHANT_tip_pickup (struct GNUNET_CURL_Context *ctx,
                           const char *backend_url,
                           const struct GNUNET_HashCode *tip_id,
                           unsigned int num_planchets,
                           const struct TALER_MERCHANT_PlanchetData *pds,
                           TALER_MERCHANT_TipPickupCallback pickup_cb,
                           void *pickup_cb_cls);


/**
 * Cancel a pending /tip-pickup request
 *
 * @param tp handle from the operation to cancel
 */
void
TALER_MERCHANT_tip_pickup_cancel (struct TALER_MERCHANT_TipPickupOperation *tp);


/**
 * Handle for a low-level /tip-pickup operation (without unblinding).
 */
struct TALER_MERCHANT_TipPickup2Operation;

/**
 * A blind signature returned via tipping API.
 */

struct TALER_MERCHANT_BlindSignature
{
  /**
   * We use RSA.
   */
  const struct GNUNET_CRYPTO_RsaSignature *blind_sig;
};


/**
 * Callback for a /tip-pickup request.  Returns the result of the operation.
 * Note that the client MUST still do the unblinding of the @a blind_sigs.
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param num_blind_sigs length of the @a blind_sigs array, 0 on error
 * @param blind_sigs array of blind signatures over the planchets, NULL on error
 */
typedef void
(*TALER_MERCHANT_TipPickup2Callback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  unsigned int num_blind_sigs,
  const struct TALER_MERCHANT_BlindSignature *blind_sigs);


/**
 * Issue a /tip-pickup request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param tip_id unique identifier for the tip
 * @param num_planches number of planchets provided in @a planchets
 * @param planchets array of planchets to be signed into existence for the tip
 * @param pickup_cb callback which will work the response gotten from the backend
 * @param pickup_cb_cls closure to pass to @a pickup_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipPickup2Operation *
TALER_MERCHANT_tip_pickup2 (struct GNUNET_CURL_Context *ctx,
                            const char *backend_url,
                            const struct GNUNET_HashCode *tip_id,
                            unsigned int num_planchets,
                            struct TALER_PlanchetDetail *planchets,
                            TALER_MERCHANT_TipPickup2Callback pickup_cb,
                            void *pickup_cb_cls);


/**
 * Cancel a pending /tip-pickup request.
 *
 * @param tp handle from the operation to cancel
 */
void
TALER_MERCHANT_tip_pickup2_cancel (
  struct TALER_MERCHANT_TipPickup2Operation *tp);


/* ********************** /check-payment ************************* */


/**
 * Handle for a /check-payment operation.
 */
struct TALER_MERCHANT_CheckPaymentOperation;


/**
 * Callback to process a GET /check-payment request
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param paid #GNUNET_YES if the payment is settled, #GNUNET_NO if not
 *        settled, $GNUNET_SYSERR on error
 *        (note that refunded payments are returned as paid!)
 * @param refunded #GNUNET_YES if there is at least on refund on this payment,
 *        #GNUNET_NO if refunded, #GNUNET_SYSERR or error
 * @param refunded_amount amount that was refunded, NULL if there
 *        was no refund
 * @param taler_pay_uri the URI that instructs the wallets to process
 *                      the payment
 */
typedef void
(*TALER_MERCHANT_CheckPaymentCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  int paid,
  int refunded,
  struct TALER_Amount *refund_amount,
  const char *taler_pay_uri);


/**
 * Issue a /check-payment request to the backend.  Checks the status
 * of a payment.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id to identify the payment
 * @param session_id sesion id for the payment (or NULL if the payment is not bound to a session)
 * @param timeout timeout to use in long polling (how long may the server wait to reply
 *        before generating an unpaid response). Note that this is just provided to
 *        the server, we as client will block until the response comes back or until
 *        #TALER_MERCHANT_check_payment_cancel() is called.
 * @param check_payment_cb callback which will work the response gotten from the backend
 * @param check_payment_cb_cls closure to pass to @a check_payment_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_CheckPaymentOperation *
TALER_MERCHANT_check_payment (struct GNUNET_CURL_Context *ctx,
                              const char *backend_url,
                              const char *order_id,
                              const char *session_id,
                              struct GNUNET_TIME_Relative timeout,
                              TALER_MERCHANT_CheckPaymentCallback
                              check_payment_cb,
                              void *check_payment_cls);

/**
 * Cancel a GET /check-payment request.
 *
 * @param cpo handle to the request to be canceled
 */
void
TALER_MERCHANT_check_payment_cancel (
  struct TALER_MERCHANT_CheckPaymentOperation *cpo);


/* ********************** /tip-query ************************* */

/**
 * Handle for a /tip-query operation.
 */
struct TALER_MERCHANT_TipQueryOperation;


/**
 * Callback to process a GET /tip-query request
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param reserve_expiration when the tip reserve will expire
 * @param reserve_pub tip reserve public key
 * @param amount_authorized total amount authorized on tip reserve
 * @param amount_available total amount still available on tip reserve
 * @param amount_picked_up total amount picked up from tip reserve
 */
typedef void
(*TALER_MERCHANT_TipQueryCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  struct GNUNET_TIME_Absolute reserve_expiration,
  struct TALER_ReservePublicKeyP *reserve_pub,
  struct TALER_Amount *amount_authorized,
  struct TALER_Amount *amount_available,
  struct TALER_Amount *amount_picked_up);


/**
 * Cancel a GET /tip-query request.
 *
 * @param cph handle to the request to be canceled
 */
void
TALER_MERCHANT_tip_query_cancel (struct TALER_MERCHANT_TipQueryOperation *tqo);


/**
 * Issue a /tip-query request to the backend.  Informs the backend
 * that a customer wants to pick up a tip.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_TipQueryOperation *
TALER_MERCHANT_tip_query (struct GNUNET_CURL_Context *ctx,
                          const char *backend_url,
                          TALER_MERCHANT_TipQueryCallback query_cb,
                          void *query_cb_cls);


/**
 * Cancel a GET /tip-query request.
 *
 * @param tqo handle to the request to be canceled
 */
void
TALER_MERCHANT_tip_query_cancel (struct TALER_MERCHANT_TipQueryOperation *tqh);


/* ********************** /public/poll-payment ************************* */


/**
 * Handle for a /public/poll-payment operation.
 */
struct TALER_MERCHANT_PollPaymentOperation;


/**
 * Callback to process a GET /poll-payment request
 *
 * @param cls closure
 * @param hr HTTP response details
 * @param paid #GNUNET_YES if the payment is settled, #GNUNET_NO if not
 *        settled, $GNUNET_SYSERR on error
 *        (note that refunded payments are returned as paid!)
 * @param refunded #GNUNET_YES if there is at least on refund on this payment,
 *        #GNUNET_NO if refunded, #GNUNET_SYSERR or error
 * @param refunded_amount amount that was refunded, NULL if there
 *        was no refund
 * @param taler_pay_uri the URI that instructs the wallets to process
 *                      the payment
 */
typedef void
(*TALER_MERCHANT_PollPaymentCallback) (
  void *cls,
  const struct TALER_MERCHANT_HttpResponse *hr,
  int paid,
  int refunded,
  struct TALER_Amount *refund_amount,
  const char *taler_pay_uri);


/**
 * Issue a /poll-payment request to the backend.  Polls the status
 * of a payment.
 *
 * @param ctx execution context
 * @param backend_url base URL of the merchant backend
 * @param order_id order id to identify the payment
 * @param h_contract hash of the contract for @a order_id
 * @param session_id sesion id for the payment (or NULL if the payment is not bound to a session)
 * @param timeout timeout to use in long polling (how long may the server wait to reply
 *        before generating an unpaid response). Note that this is just provided to
 *        the server, we as client will block until the response comes back or until
 *        #TALER_MERCHANT_poll_payment_cancel() is called.
 * @param min_refund long poll for the service to approve a refund exceeding this value;
 *        use NULL to not wait for any refund (only for payment). Only makes sense
 *        with a non-zero @a timeout.
 * @param poll_payment_cb callback which will work the response gotten from the backend
 * @param poll_payment_cb_cls closure to pass to @a poll_payment_cb
 * @return handle for this operation, NULL upon errors
 */
struct TALER_MERCHANT_PollPaymentOperation *
TALER_MERCHANT_poll_payment (
  struct GNUNET_CURL_Context *ctx,
  const char *backend_url,
  const char *order_id,
  const struct GNUNET_HashCode *h_contract,
  const char *session_id,
  struct GNUNET_TIME_Relative timeout,
  const struct TALER_Amount *min_refund,
  TALER_MERCHANT_PollPaymentCallback poll_payment_cb,
  void *poll_payment_cls);


/**
 * Cancel a GET /public/poll-payment request.
 *
 * @param cpo handle to the request to be canceled
 */
void
TALER_MERCHANT_poll_payment_cancel (
  struct TALER_MERCHANT_PollPaymentOperation *cpo);


#endif  /* _TALER_MERCHANT_SERVICE_H */
