/*
  This file is part of TALER
  (C) 2017-2020 Taler Systems SA

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
 * @file backend/taler-merchant-httpd_tip-pickup.c
 * @brief implementation of /tip-pickup handler
 * @author Christian Grothoff
 */
#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_mhd.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_tip-pickup.h"


/**
 * Details about a planchet that the customer wants to obtain
 * a withdrawal authorization.  This is the information that
 * will need to be sent to the exchange to obtain the blind
 * signature required to turn a planchet into a coin.
 */
struct PlanchetDetail
{

  /**
   * The complete withdraw request that we are building to sign.
   * Built incrementally during the processing of the request.
   */
  struct TALER_WithdrawRequestPS wr;

  /**
   * Blinded coin (see GNUNET_CRYPTO_rsa_blind()).  Note: is malloc()'ed!
   */
  char *coin_ev;

  /**
   * Number of bytes in @a coin_ev.
   */
  size_t coin_ev_size;

};


/**
 * Information we keep for individual calls
 * to requests that parse JSON, but keep no other state.
 */
struct PickupContext
{

  /**
   * This field MUST be first for handle_mhd_completion_callback() to work
   * when it treats this struct as a `struct TM_HandlerContext`.
   */
  struct TM_HandlerContext hc;

  /**
   * Placeholder for #TALER_MHD_parse_post_json() to keep its internal state.
   */

  void *json_parse_context;

  /**
   * URL of the exchange this tip uses.
   */
  char *exchange_url;

  /**
   * Operation we run to find the exchange (and get its /keys).
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Array of planchets of length @e planchets_len.
   */
  struct PlanchetDetail *planchets;

  /**
   * The connection we are processing.
   */
  struct MHD_Connection *connection;

  /**
   * Tip ID that was supplied by the client.
   */
  struct GNUNET_HashCode tip_id;

  /**
   * Unique identifier for the pickup operation, used to detect
   * duplicate requests (retries).
   */
  struct GNUNET_HashCode pickup_id;

  /**
   * Total value of the coins we are withdrawing.
   */
  struct TALER_Amount total;

  /**
   * Length of @e planchets.
   */
  unsigned int planchets_len;

  /**
   * Error code, #TALER_EC_NONE as long as all is fine.
   */
  enum TALER_ErrorCode ec;

  /**
   * HTTP status code to return in combination with @e ec
   * if @e ec is not #TALER_EC_NONE.
   */
  unsigned int response_code;

  /**
   * Human-readable error hint to return.
   * if @e ec is not #TALER_EC_NONE.
   */
  const char *error_hint;

};


/**
 * Custom cleanup routine for a `struct PickupContext`.
 *
 * @param hc the `struct PickupContext` to clean up.
 */
static void
pickup_cleanup (struct TM_HandlerContext *hc)
{
  struct PickupContext *pc = (struct PickupContext *) hc;

  if (NULL != pc->planchets)
  {
    for (unsigned int i = 0; i<pc->planchets_len; i++)
      GNUNET_free_non_null (pc->planchets[i].coin_ev);
    GNUNET_free (pc->planchets);
    pc->planchets = NULL;
  }
  if (NULL != pc->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (pc->fo);
    pc->fo = NULL;
  }
  TALER_MHD_parse_post_cleanup_callback (pc->json_parse_context);
  GNUNET_free_non_null (pc->exchange_url);
  GNUNET_free (pc);
}


/**
 * Prepare (and eventually execute) a pickup.  Computes
 * the "pickup ID" (by hashing the planchets and denomination keys),
 * resolves the denomination keys and calculates the total
 * amount to be picked up.  Then runs the pick up execution logic.
 *
 * @param connection MHD connection for sending the response
 * @param tip_id which tip are we picking up
 * @param pc pickup context
 * @return #MHD_YES upon success, #MHD_NO if
 *         the connection ought to be dropped
 */
static int
run_pickup (struct MHD_Connection *connection,
            struct PickupContext *pc)
{
  struct TALER_ReservePrivateKeyP reserve_priv;
  struct TALER_ReservePublicKeyP reserve_pub;
  enum TALER_ErrorCode ec;
  json_t *sigs;

  if (TALER_EC_NONE != pc->ec)
  {
    return TALER_MHD_reply_with_error (connection,
                                       pc->response_code,
                                       pc->ec,
                                       pc->error_hint);
  }
  db->preflight (db->cls);
  ec = db->pickup_tip_TR (db->cls,
                          &pc->total,
                          &pc->tip_id,
                          &pc->pickup_id,
                          &reserve_priv);
  if (TALER_EC_NONE != ec)
  {
    unsigned int response_code;
    const char *human;

    switch (ec)
    {
    case TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN:
      response_code = MHD_HTTP_NOT_FOUND;
      human = "tip identifier not known to this service";
      break;
    case TALER_EC_TIP_PICKUP_NO_FUNDS:
      response_code = MHD_HTTP_CONFLICT;
      human = "withdrawn funds exceed amounts approved for tip";
      break;
    default:
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      human = "database failure";
      break;
    }
    return TALER_MHD_reply_with_error (connection,
                                       response_code,
                                       ec,
                                       human);
  }
  sigs = json_array ();
  if (NULL == sigs)
  {
    GNUNET_break (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_JSON_ALLOCATION_FAILURE,
                                       "could not create JSON array");
  }
  GNUNET_CRYPTO_eddsa_key_get_public (&reserve_priv.eddsa_priv,
                                      &reserve_pub.eddsa_pub);
  for (unsigned int i = 0; i<pc->planchets_len; i++)
  {
    struct PlanchetDetail *pd = &pc->planchets[i];
    struct TALER_ReserveSignatureP reserve_sig;

    pd->wr.reserve_pub = reserve_pub;
    GNUNET_CRYPTO_eddsa_sign (&reserve_priv.eddsa_priv,
                              &pd->wr,
                              &reserve_sig.eddsa_signature);
    if (0 !=
        json_array_append_new (sigs,
                               json_pack ("{s:o}",
                                          "reserve_sig",
                                          GNUNET_JSON_from_data_auto (
                                            &reserve_sig))))
    {
      GNUNET_break (0);
      json_decref (sigs);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_JSON_ALLOCATION_FAILURE,
                                         "could not add element to JSON array");
    }
  }
  return TALER_MHD_reply_json_pack (connection,
                                    MHD_HTTP_OK,
                                    "{s:o, s:o}",
                                    "reserve_pub",
                                    GNUNET_JSON_from_data_auto (
                                      &reserve_pub),
                                    "reserve_sigs", sigs);
}


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.
 *
 * @param cls closure with the `struct PickupContext`
 * @param eh handle to the exchange context
 * @param wire_fee current applicable wire fee for dealing with @a eh, NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 * @param ec error code, #TALER_EC_NONE on success
 * @param http_status the HTTP status we got from the exchange
 * @param error_reply the full reply from the exchange, NULL if
 *        the response was NOT in JSON or on success
 */
static void
exchange_found_cb (void *cls,
                   struct TALER_EXCHANGE_Handle *eh,
                   const struct TALER_Amount *wire_fee,
                   int exchange_trusted,
                   enum TALER_ErrorCode ec,
                   unsigned int http_status,
                   const json_t *error_reply)
{
  struct PickupContext *pc = cls;
  const struct TALER_EXCHANGE_Keys *keys;
  struct GNUNET_HashContext *hc;
  struct TALER_Amount total;
  int ae;

  pc->fo = NULL;
  MHD_resume_connection (pc->connection);
  if (NULL == eh)
  {
    // FIXME: #6014: forward error details!
    pc->ec = TALER_EC_TIP_PICKUP_EXCHANGE_DOWN;
    pc->error_hint = "failed to contact exchange, check URL";
    pc->response_code = MHD_HTTP_FAILED_DEPENDENCY;
    TMH_trigger_daemon ();
    return;
  }
  keys = TALER_EXCHANGE_get_keys (eh);
  if (NULL == keys)
  {
    // FIXME: #6014: forward error details!?
    pc->ec = TALER_EC_TIP_PICKUP_EXCHANGE_LACKED_KEYS;
    pc->error_hint =
      "could not obtain denomination keys from exchange, check URL";
    pc->response_code = MHD_HTTP_FAILED_DEPENDENCY;
    TMH_trigger_daemon ();
    return;
  }
  GNUNET_assert (0 != pc->planchets_len);
  ae = GNUNET_NO;
  memset (&total,
          0,
          sizeof (total));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Calculating tip amount over %u planchets!\n",
              pc->planchets_len);
  hc = GNUNET_CRYPTO_hash_context_start ();
  for (unsigned int i = 0; i<pc->planchets_len; i++)
  {
    struct PlanchetDetail *pd = &pc->planchets[i];
    const struct TALER_EXCHANGE_DenomPublicKey *dk;
    struct TALER_Amount amount_with_fee;

    dk = TALER_EXCHANGE_get_denomination_key_by_hash (keys,
                                                      &pd->wr.h_denomination_pub);
    if (NULL == dk)
    {
      pc->ec = TALER_EC_TIP_PICKUP_EXCHANGE_LACKED_KEY;
      pc->error_hint = "could not find matching denomination key";
      pc->response_code = MHD_HTTP_NOT_FOUND;
      GNUNET_CRYPTO_hash_context_abort (hc);
      TMH_trigger_daemon ();
      return;
    }
    GNUNET_CRYPTO_hash_context_read (hc,
                                     &pd->wr.h_denomination_pub,
                                     sizeof (struct GNUNET_HashCode));
    GNUNET_CRYPTO_hash_context_read (hc,
                                     pd->coin_ev,
                                     pd->coin_ev_size);
    if (GNUNET_OK !=
        TALER_amount_add (&amount_with_fee,
                          &dk->value,
                          &dk->fee_withdraw))
    {
      ae = GNUNET_YES;
    }
    if (0 == i)
    {
      total = amount_with_fee;
    }
    else
    {
      if (GNUNET_OK !=
          TALER_amount_add (&total,
                            &total,
                            &amount_with_fee))
      {
        ae = GNUNET_YES;
      }
    }
    TALER_amount_hton (&pd->wr.withdraw_fee,
                       &dk->fee_withdraw);
    TALER_amount_hton (&pd->wr.amount_with_fee,
                       &amount_with_fee);
  }
  GNUNET_CRYPTO_hash_context_finish (hc,
                                     &pc->pickup_id);
  if (GNUNET_YES == ae)
  {
    pc->ec = TALER_EC_TIP_PICKUP_EXCHANGE_AMOUNT_OVERFLOW;
    pc->error_hint = "error computing total value of the tip";
    pc->response_code = MHD_HTTP_BAD_REQUEST;
    TMH_trigger_daemon ();
    return;
  }
  pc->total = total;
  TMH_trigger_daemon ();
}


/**
 * Prepare (and eventually execute) a pickup. Finds the exchange
 * handle we need for #run_pickup().
 *
 * @param pc pickup context
 * @return #MHD_YES upon success, #MHD_NO if
 *         the connection ought to be dropped
 */
static int
prepare_pickup (struct PickupContext *pc)
{
  enum GNUNET_DB_QueryStatus qs;

  db->preflight (db->cls);
  qs = db->lookup_tip_by_id (db->cls,
                             &pc->tip_id,
                             &pc->exchange_url,
                             NULL, NULL, NULL, NULL);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    unsigned int response_code;
    enum TALER_ErrorCode ec;

    switch (qs)
    {
    case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
      ec = TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN;
      response_code = MHD_HTTP_NOT_FOUND;
      break;
    case GNUNET_DB_STATUS_SOFT_ERROR:
      ec = TALER_EC_TIP_PICKUP_DB_ERROR_SOFT;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    case GNUNET_DB_STATUS_HARD_ERROR:
      ec = TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    default:
      GNUNET_break (0);
      ec = TALER_EC_INTERNAL_LOGIC_ERROR;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    }
    return TALER_MHD_reply_with_error (pc->connection,
                                       response_code,
                                       ec,
                                       "Could not determine exchange URL for the given tip id");

  }
  pc->fo = TMH_EXCHANGES_find_exchange (pc->exchange_url,
                                        NULL,
                                        &exchange_found_cb,
                                        pc);
  if (NULL == pc->fo)
  {
    return TALER_MHD_reply_with_error (pc->connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                       "consult server logs");
  }
  MHD_suspend_connection (pc->connection);
  return MHD_YES;
}


/**
 * Parse the given @a planchet into the @a pd.
 *
 * @param connection connection to use for error reporting
 * @param planchet planchet data in JSON format
 * @param[out] pd where to store the binary data
 * @return #GNUNET_OK upon success, #GNUNET_NO if a response
 *    was generated, #GNUNET_SYSERR to drop the connection
 */
static int
parse_planchet (struct MHD_Connection *connection,
                const json_t *planchet,
                struct PlanchetDetail *pd)
{
  int ret;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("denom_pub_hash",
                                 &pd->wr.h_denomination_pub),
    GNUNET_JSON_spec_varsize ("coin_ev",
                              (void **) &pd->coin_ev,
                              &pd->coin_ev_size),
    GNUNET_JSON_spec_end ()
  };

  ret = TALER_MHD_parse_json_data (connection,
                                   planchet,
                                   spec);
  if (GNUNET_OK != ret)
    return ret;
  pd->wr.purpose.purpose = htonl (TALER_SIGNATURE_WALLET_RESERVE_WITHDRAW);
  pd->wr.purpose.size = htonl (sizeof (struct TALER_WithdrawRequestPS));
  GNUNET_CRYPTO_hash (pd->coin_ev,
                      pd->coin_ev_size,
                      &pd->wr.h_coin_envelope);
  return ret;
}


/**
 * Manages a POST /tip-pickup call, checking that the tip is authorized,
 * and if so, returning the withdrawal permissions.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_tip_pickup (struct TMH_RequestHandler *rh,
                       struct MHD_Connection *connection,
                       void **connection_cls,
                       const char *upload_data,
                       size_t *upload_data_size,
                       struct MerchantInstance *mi)
{
  int res;
  struct GNUNET_HashCode tip_id;
  json_t *planchets;
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("tip_id",
                                 &tip_id),
    GNUNET_JSON_spec_json ("planchets",
                           &planchets),
    GNUNET_JSON_spec_end ()
  };
  struct PickupContext *pc;
  json_t *root;

  if (NULL == *connection_cls)
  {
    pc = GNUNET_new (struct PickupContext);
    pc->hc.cc = &pickup_cleanup;
    pc->connection = connection;
    *connection_cls = pc;
  }
  else
  {
    pc = *connection_cls;
  }
  if (NULL != pc->planchets)
  {
    /* This actually happens when we are called much later
       after an exchange /keys' request to obtain the DKs
       (and not for each request). */
    return run_pickup (connection,
                       pc);
  }
  res = TALER_MHD_parse_post_json (connection,
                                   &pc->json_parse_context,
                                   upload_data,
                                   upload_data_size,
                                   &root);
  if (GNUNET_SYSERR == res)
    return MHD_NO;
  /* the POST's body has to be further fetched */
  if ( (GNUNET_NO == res) ||
       (NULL == root) )
    return MHD_YES;

  res = TALER_MHD_parse_json_data (connection,
                                   root,
                                   spec);
  if (GNUNET_YES != res)
  {
    GNUNET_break_op (0);
    json_decref (root);
    return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
  }
  pc->planchets_len = json_array_size (planchets);
  if (pc->planchets_len > 1024)
  {
    GNUNET_JSON_parse_free (spec);
    json_decref (root);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_TIP_PICKUP_EXCHANGE_TOO_MANY_PLANCHETS,
                                       "limit of 1024 planchets exceeded by request");
  }
  if (0 == pc->planchets_len)
  {
    GNUNET_JSON_parse_free (spec);
    json_decref (root);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "no planchets specified");
  }
  db->preflight (db->cls);
  pc->planchets = GNUNET_new_array (pc->planchets_len,
                                    struct PlanchetDetail);
  for (unsigned int i = 0; i<pc->planchets_len; i++)
  {
    if (GNUNET_OK !=
        (res = parse_planchet (connection,
                               json_array_get (planchets,
                                               i),
                               &pc->planchets[i])))
    {
      GNUNET_JSON_parse_free (spec);
      json_decref (root);
      return (GNUNET_NO == res) ? MHD_YES : MHD_NO;
    }
  }
  pc->tip_id = tip_id;
  res = prepare_pickup (pc);
  GNUNET_JSON_parse_free (spec);
  json_decref (root);
  return res;
}


/**
 * Manages a GET /tip-pickup call, checking that the tip is authorized,
 * and if so, returning the withdrawal permissions.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @param mi merchant backend instance, never NULL
 * @return MHD result code
 */
int
MH_handler_tip_pickup_get (struct TMH_RequestHandler *rh,
                           struct MHD_Connection *connection,
                           void **connection_cls,
                           const char *upload_data,
                           size_t *upload_data_size,
                           struct MerchantInstance *mi)
{
  const char *tip_id_str;
  char *exchange_url;
  json_t *extra;
  struct GNUNET_HashCode tip_id;
  struct TALER_Amount tip_amount;
  struct TALER_Amount tip_amount_left;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute timestamp_expire;
  int ret;
  int qs;

  tip_id_str = MHD_lookup_connection_value (connection,
                                            MHD_GET_ARGUMENT_KIND,
                                            "tip_id");

  if (NULL == tip_id_str)
  {
    /* tip_id is required but missing */
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MISSING,
                                       "tip_id required");
  }

  if (GNUNET_OK !=
      GNUNET_CRYPTO_hash_from_string (tip_id_str,
                                      &tip_id))
  {
    /* tip_id has wrong encoding */
    GNUNET_break_op (0);
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_BAD_REQUEST,
                                       TALER_EC_PARAMETER_MALFORMED,
                                       "tip_id malformed");
  }

  db->preflight (db->cls);
  qs = db->lookup_tip_by_id (db->cls,
                             &tip_id,
                             &exchange_url,
                             &extra,
                             &tip_amount,
                             &tip_amount_left,
                             &timestamp);

  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
  {
    unsigned int response_code;
    enum TALER_ErrorCode ec;

    switch (qs)
    {
    case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
      ec = TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN;
      response_code = MHD_HTTP_NOT_FOUND;
      break;
    case GNUNET_DB_STATUS_SOFT_ERROR:
      ec = TALER_EC_TIP_PICKUP_DB_ERROR_SOFT;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    case GNUNET_DB_STATUS_HARD_ERROR:
      ec = TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    default:
      GNUNET_break (0);
      ec = TALER_EC_INTERNAL_LOGIC_ERROR;
      response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      break;
    }
    return TALER_MHD_reply_with_error (connection,
                                       response_code,
                                       ec,
                                       "Could not determine exchange URL for the given tip id");
  }

  timestamp_expire = GNUNET_TIME_absolute_add (timestamp,
                                               GNUNET_TIME_UNIT_DAYS);

  ret = TALER_MHD_reply_json_pack (
    connection,
    MHD_HTTP_OK,
    "{s:s, s:o, s:o, s:o, s:o, s:o}",
    "exchange_url", exchange_url,
    "amount", TALER_JSON_from_amount (&tip_amount),
    "amount_left", TALER_JSON_from_amount (&tip_amount_left),
    "stamp_created", GNUNET_JSON_from_time_abs (timestamp),
    "stamp_expire", GNUNET_JSON_from_time_abs (timestamp_expire),
    "extra", extra);

  GNUNET_free (exchange_url);
  json_decref (extra);
  return ret;
}
