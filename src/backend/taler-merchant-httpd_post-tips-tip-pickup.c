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
 * Information we keep per tip pickup request.
 */
struct PickupContext;


/**
 * Details about a planchet that the customer wants to obtain
 * a withdrawal authorization.  This is the information that
 * will need to be sent to the exchange to obtain the blind
 * signature required to turn a planchet into a coin.
 */
struct PlanchetDetail
{

  /**
   * Hash of the denomination public key requested for this planchet.
   */
  struct GNUNET_HashCode h_denom_pub;

  /**
   * Pickup context this planchet belongs to.
   */
  struct PickupContext *pc;

  /**
   * Handle to withdraw operation with the exchange.
   */
  struct TALER_EXCHANGE_Withdraw2Handle *wh;

  /**
   * Blind signature to return, or NULL if not available.
   */
  json_t *blind_sig;

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
 * Information we keep per tip pickup request.
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
   * Kept in a DLL while suspended.
   */
  struct PickupContext *next;

  /**
   * Kept in a DLL while suspended.
   */
  struct PickupContext *prev;

  /**
   * URL of the exchange this tip uses.
   */
  char *exchange_url;

  /**
   * Operation we run to find the exchange (and get its /keys).
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Handle to the exchange (set after exchange_found_cb()).
   */
  struct TALER_EXCHANGE_Handle *eh;

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
   * Message to return.
   */
  struct MHD_Response *response;

  /**
   * HTTP status code to return in combination with @e response.
   */
  unsigned int response_code;

  /**
   * Set to #GNUNET_YES if @e connection was suspended.
   */
  int suspended;

};


/**
 * Kept in a DLL while suspended.
 */
static struct PickupContext *pc_head;

/**
 * Kept in a DLL while suspended.
 */
static struct PickupContext *pc_tail;


/**
 * We are shutting down, force resuming all suspended pickup operations.
 */
void
MH_force_tip_pickup_resume ()
{
  struct PickupContext *pc;

  while (NULL != (pc = pc_head))
  {
    GNUNET_assert (GNUNET_YES == pc->suspended);
    GNUNET_CONTAINER_DLL_remove (pc_head,
                                 pc_tail,
                                 pc);
    MHD_resume_connection (pc->connection);
  }
}


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
    {
      struct PlanchetDetail *pd = &pc->planchets[i];
      GNUNET_free_non_null (pd->coin_ev);
      if (NULL != pd->wh)
      {
        TALER_EXCHANGE_withdraw2_cancel (pd->wh);
        pd->wh = NULL;
      }
      if (NULL != pd->blind_sig)
      {
        json_decref (pd->blind_sig);
        pd->blind_sig = NULL;
      }
    }
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
  if (NULL != pc->response)
  {
    MHD_destroy_response (pc->response);
    pc->response = NULL;
  }
  GNUNET_free (pc);
}


/**
 * Resume processing of a suspended @a pc.
 *
 * @param pc a suspended pickup operation
 */
static void
resume_pc (struct PickupContext *pc)
{
  for (unsigned int i = 0; i<pc->planchets_len; i++)
  {
    struct PlanchetDetail *pd = &pc->planchets[i];

    if (NULL != pd->wh)
    {
      TALER_EXCHANGE_withdraw2_cancel (pc->planchets[i].wh);
      pc->planchets[i].wh = NULL;
    }
  }
  GNUNET_assert (GNUNET_YES == pc->suspended);
  GNUNET_CONTAINER_DLL_remove (pc_head,
                               pc_tail,
                               pc);
  MHD_resume_connection (pc->connection);
  TMH_trigger_daemon ();
}


/**
 * Function called with the result of our attempt to withdraw
 * the coin for a tip.
 *
 * @param cls closure
 * @param hr HTTP response data
 * @param blind_sig blind signature over the coin, NULL on error
 */
static void
withdraw_cb (void *cls,
             const struct TALER_EXCHANGE_HttpResponse *hr,
             const struct GNUNET_CRYPTO_RsaSignature *blind_sig)
{
  struct PlanchetDetail *pd = cls;
  struct PickupContext *pc = pd->pc;
  json_t *ja;

  pd->wh = NULL;
  if (NULL == blind_sig)
  {
    pc->response_code = MHD_HTTP_FAILED_DEPENDENCY;
    pc->response = TALER_MHD_make_json_pack (
      (NULL != hr->reply)
      ? "{s:s, s:I, s:I, s:I, s:O}"
      : "{s:s, s:I, s:I, s:I}",
      "hint", "failed to withdraw coin from exchange",
      "code", (json_int_t) TALER_EC_TIP_PICKUP_WITHDRAW_FAILED_AT_EXCHANGE,
      "exchange_http_status", (json_int_t) hr->http_status,
      "exchange_code", (json_int_t) hr->ec,
      "exchange_reply", hr->reply);
    resume_pc (pc);
    return;
  }
  /* FIXME: persisit blind_sig in our database!?
     (or at least _all_ of them once we have them all?) */
  pd->blind_sig = GNUNET_JSON_from_rsa_signature (blind_sig);
  GNUNET_assert (NULL != pd->blind_sig);
  for (unsigned int i = 0; i<pc->planchets_len; i++)
    if (NULL != pc->planchets[i].wh)
      return;
  /* All done, build final response */
  ja = json_array ();
  GNUNET_assert (NULL != ja);
  for (unsigned int i = 0; i<pc->planchets_len; i++)
  {
    struct PlanchetDetail *pd = &pc->planchets[i];

    GNUNET_assert (0 ==
                   json_array_append_new (ja,
                                          json_pack ("{s:o}",
                                                     "blind_sig",
                                                     pd->blind_sig)));
    pd->blind_sig = NULL;
  }
  pc->response_code = MHD_HTTP_OK;
  pc->response = TALER_MHD_make_json_pack ("{s:o}",
                                           "blind_sigs",
                                           ja);
  resume_pc (pc);
}


/**
 * Prepare (and eventually execute) a pickup.  Computes
 * the "pickup ID" (by hashing the planchets and denomination keys),
 * resolves the denomination keys and calculates the total
 * amount to be picked up.  Then runs the pick up execution logic.
 *
 * @param pc pickup context
 */
static void
run_pickup (struct PickupContext *pc)
{
  struct TALER_ReservePrivateKeyP reserve_priv;
  enum TALER_ErrorCode ec;

  db->preflight (db->cls);
  ec = db->pickup_tip_TR (db->cls,
                          &pc->total,
                          &pc->tip_id,
                          &pc->pickup_id,
                          &reserve_priv);
  if (TALER_EC_NONE != ec)
  {
    const char *human;

    switch (ec)
    {
    case TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN:
      pc->response_code = MHD_HTTP_NOT_FOUND;
      human = "tip identifier not known to this service";
      break;
    case TALER_EC_TIP_PICKUP_NO_FUNDS:
      pc->response_code = MHD_HTTP_CONFLICT;
      human = "withdrawn funds exceed amounts approved for tip";
      break;
    default:
      pc->response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      human = "database failure";
      break;
    }
    pc->response = TALER_MHD_make_error (ec,
                                         human);
    resume_pc (pc);
    return;
  }
  for (unsigned int i = 0; i<pc->planchets_len; i++)
  {
    struct PlanchetDetail *pd = &pc->planchets[i];
    struct TALER_PlanchetDetail pdx = {
      .denom_pub_hash = pd->h_denom_pub,
      .coin_ev = pd->coin_ev,
      .coin_ev_size = pd->coin_ev_size,
    };

    pd->wh = TALER_EXCHANGE_withdraw2 (pc->eh,
                                       &pdx,
                                       &reserve_priv,
                                       &withdraw_cb,
                                       pd);
    if (NULL == pd->wh)
    {
      GNUNET_break (0);
      pc->response_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
      pc->response = TALER_MHD_make_error (TALER_EC_TIP_PICKUP_WITHDRAW_FAILED,
                                           "could not inititate withdrawal");
      resume_pc (pc);
      return;
    }
  }
}


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.
 *
 * @param cls closure with the `struct PickupContext`
 * @param hr HTTP response details
 * @param eh handle to the exchange context
 * @param wire_fee current applicable wire fee for dealing with @a eh, NULL if not available
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
exchange_found_cb (void *cls,
                   const struct TALER_EXCHANGE_HttpResponse *hr,
                   struct TALER_EXCHANGE_Handle *eh,
                   const struct TALER_Amount *wire_fee,
                   int exchange_trusted)
{
  struct PickupContext *pc = cls;
  const struct TALER_EXCHANGE_Keys *keys;
  struct TALER_Amount total;
  int ae;

  pc->fo = NULL;
  if (NULL == eh)
  {
    pc->response_code = MHD_HTTP_FAILED_DEPENDENCY;
    pc->response = TALER_MHD_make_json_pack (
      (NULL != hr->reply)
      ? "{s:s, s:I, s:I, s:I, s:O}"
      : "{s:s, s:I, s:I, s:I}",
      "hint", "failed to contact exchange, check URL",
      "code", (json_int_t) TALER_EC_TIP_PICKUP_EXCHANGE_DOWN,
      "exchange_http_status", (json_int_t) hr->http_status,
      "exchange_code", (json_int_t) hr->ec,
      "exchange_reply", hr->reply);
    resume_pc (pc);
    return;
  }
  keys = TALER_EXCHANGE_get_keys (eh);
  if (NULL == keys)
  {
    pc->response_code = MHD_HTTP_FAILED_DEPENDENCY;
    pc->response = TALER_MHD_make_json_pack (
      (NULL != hr->reply)
      ? "{s:s, s:I, s:I, s:I, s:O}"
      : "{s:s, s:I, s:I, s:I}",
      "hint", "could not obtain denomination keys from exchange, check URL",
      "code", (json_int_t) TALER_EC_TIP_PICKUP_EXCHANGE_LACKED_KEYS,
      "exchange_http_status", (json_int_t) hr->http_status,
      "exchange_code", (json_int_t) hr->ec,
      "exchange_reply", hr->reply);
    resume_pc (pc);
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
  {
    struct GNUNET_HashContext *hc;

    hc = GNUNET_CRYPTO_hash_context_start ();
    for (unsigned int i = 0; i<pc->planchets_len; i++)
    {
      struct PlanchetDetail *pd = &pc->planchets[i];
      struct TALER_Amount amount_with_fee;
      const struct TALER_EXCHANGE_DenomPublicKey *dk;

      dk = TALER_EXCHANGE_get_denomination_key_by_hash (keys,
                                                        &pd->h_denom_pub);
      if (NULL == dk)
      {
        pc->response_code = MHD_HTTP_NOT_FOUND;
        pc->response
          = TALER_MHD_make_json_pack (
              "{s:s, s:I}"
              "hint",
              "could not find matching denomination key",
              "code",
              (json_int_t) TALER_EC_TIP_PICKUP_EXCHANGE_LACKED_KEY);
        resume_pc (pc);
        GNUNET_CRYPTO_hash_context_abort (hc);
        return;
      }
      GNUNET_CRYPTO_hash_context_read (hc,
                                       &pd->h_denom_pub,
                                       sizeof (struct GNUNET_HashCode));
      GNUNET_CRYPTO_hash_context_read (hc,
                                       pd->coin_ev,
                                       pd->coin_ev_size);
      if (0 >
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
        if (0 >
            TALER_amount_add (&total,
                              &total,
                              &amount_with_fee))
        {
          ae = GNUNET_YES;
        }
      }
    }
    GNUNET_CRYPTO_hash_context_finish (hc,
                                       &pc->pickup_id);
  }
  if (GNUNET_YES == ae)
  {
    pc->response_code = MHD_HTTP_BAD_REQUEST;
    pc->response
      = TALER_MHD_make_json_pack (
          "{s:s, s:I}"
          "hint",
          "error computing total value of the tip",
          "code",
          (json_int_t) TALER_EC_TIP_PICKUP_EXCHANGE_AMOUNT_OVERFLOW);
    resume_pc (pc);
    return;
  }
  pc->eh = eh;
  pc->total = total;
  run_pickup (pc);
}


/**
 * Prepare (and eventually execute) a pickup. Finds the exchange
 * handle we need for #run_pickup().
 *
 * @param pc pickup context
 * @return #MHD_YES upon success, #MHD_NO if
 *         the connection ought to be dropped
 */
static MHD_RESULT
prepare_pickup (struct PickupContext *pc)
{
  enum GNUNET_DB_QueryStatus qs;

  db->preflight (db->cls);
  /* FIXME: do not pass NULL's, *do* get the
     data from the DB, we may be able to avoid
     most of the processing if we already have
     a valid result! */
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
                                        GNUNET_NO,
                                        &exchange_found_cb,
                                        pc);
  if (NULL == pc->fo)
  {
    return TALER_MHD_reply_with_error (pc->connection,
                                       MHD_HTTP_INTERNAL_SERVER_ERROR,
                                       TALER_EC_INTERNAL_INVARIANT_FAILURE,
                                       "consult server logs");
  }
  /* continued asynchronously in exchange_found_cb() */
  GNUNET_assert (GNUNET_NO == pc->suspended);
  pc->suspended = GNUNET_YES;
  GNUNET_CONTAINER_DLL_insert (pc_head,
                               pc_tail,
                               pc);
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
static enum GNUNET_GenericReturnValue
parse_planchet (struct MHD_Connection *connection,
                const json_t *planchet,
                struct PlanchetDetail *pd)
{
  struct GNUNET_JSON_Specification spec[] = {
    GNUNET_JSON_spec_fixed_auto ("denom_pub_hash",
                                 &pd->h_denom_pub),
    GNUNET_JSON_spec_varsize ("coin_ev",
                              (void **) &pd->coin_ev,
                              &pd->coin_ev_size),
    GNUNET_JSON_spec_end ()
  };

  return TALER_MHD_parse_json_data (connection,
                                    planchet,
                                    spec);
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
MHD_RESULT
MH_handler_tip_pickup (struct TMH_RequestHandler *rh,
                       struct MHD_Connection *connection,
                       void **connection_cls,
                       const char *upload_data,
                       size_t *upload_data_size,
                       struct MerchantInstance *mi)
{
  enum GNUNET_GenericReturnValue res;
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
  if (NULL != pc->response)
  {
    MHD_RESULT ret;

    ret = MHD_queue_response (connection,
                              pc->response_code,
                              pc->response);
    MHD_destroy_response (pc->response);
    pc->response = NULL;
    return ret;
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
                                       "per request limit of 1024 planchets exceeded");
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
    pc->planchets[i].pc = pc;
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
  {
    MHD_RESULT ret;

    ret = prepare_pickup (pc);
    GNUNET_JSON_parse_free (spec);
    json_decref (root);
    return ret;
  }
}
