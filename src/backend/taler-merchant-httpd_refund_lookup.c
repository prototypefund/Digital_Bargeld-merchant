/*
  This file is part of TALER
  (C) 2014-2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Affero General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file backend/taler-merchant-httpd_refund_lookup.c
 * @brief refund handling logic
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_exchange_service.h>
#include "taler-merchant-httpd.h"
#include "taler-merchant-httpd_exchanges.h"
#include "taler-merchant-httpd_refund.h"

/**
 * How often do we retry DB transactions on serialization failures?
 */
#define MAX_RETRIES 5

/**
 * Information we keep for each coin to be refunded.
 */
struct CoinRefund
{

  /**
   * Kept in a DLL.
   */
  struct CoinRefund *next;

  /**
   * Kept in a DLL.
   */
  struct CoinRefund *prev;

  /**
   * Request to connect to the target exchange.
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Handle for the refund operation with the exchange.
   */
  struct TALER_EXCHANGE_RefundHandle *rh;

  /**
   * PRD this operation is part of.
   */
  struct ProcessRefundData *prd;

  /**
   * URL of the exchange for this @e coin_pub.
   */
  char *exchange_url;

  /**
   * Coin to refund.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Refund transaction ID to use.
   */
  uint64_t rtransaction_id;

  /**
   * Amount to refund.
   */
  struct TALER_Amount refund_amount;

  /**
   * Applicable refund transaction fee.
   */
  struct TALER_Amount refund_fee;

  /**
   * Public key of the exchange affirming the refund.
   */
  struct TALER_ExchangePublicKeyP exchange_pub;

  /**
   * Signature of the exchange affirming the refund.
   */
  struct TALER_ExchangeSignatureP exchange_sig;

  /**
   * HTTP status from the exchange, #MHD_HTTP_OK if
   * @a exchange_pub and @a exchange_sig are valid.
   */
  unsigned int exchange_status;

  /**
   * HTTP error code from the exchange.
   */
  enum TALER_ErrorCode exchange_code;

  /**
   * Fully reply from the exchange, only possibly set if
   * we got a JSON reply and a non-#MHD_HTTP_OK error code
   */
  json_t *exchange_reply;

};


/**
 * Closure for #process_refunds_cb.
 */
struct ProcessRefundData
{
  /**
   * Must be first for #handle_mhd_completion_callback() cleanup
   * logic to work.
   */
  struct TM_HandlerContext hc;

  /**
   * Hashed version of contract terms.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * DLL of (suspended) requests.
   */
  struct ProcessRefundData *next;

  /**
   * DLL of (suspended) requests.
   */
  struct ProcessRefundData *prev;

  /**
   * Head of DLL of coin refunds for this request.
   */
  struct CoinRefund *cr_head;

  /**
   * Tail of DLL of coin refunds for this request.
   */
  struct CoinRefund *cr_tail;

  /**
   * Both public and private key are needed by the callback
   */
  const struct MerchantInstance *merchant;

  /**
   * Connection we are handling.
   */
  struct MHD_Connection *connection;

  /**
   * Did we suspend @a connection?
   */
  int suspended;

  /**
   * Return code: #TALER_EC_NONE if successful.
   */
  enum TALER_ErrorCode ec;
};


/**
 * HEad of DLL of (suspended) requests.
 */
static struct ProcessRefundData *prd_head;

/**
 * Tail of DLL of (suspended) requests.
 */
static struct ProcessRefundData *prd_tail;


/**
 * Clean up memory in @a cls, the connection was closed.
 *
 * @param cls a `struct ProcessRefundData` to clean up.
 */
static void
cleanup_prd (struct TM_HandlerContext *cls)
{
  struct ProcessRefundData *prd = (struct ProcessRefundData *) cls;
  struct CoinRefund *cr;

  while (NULL != (cr = prd->cr_head))
  {
    GNUNET_CONTAINER_DLL_remove (prd->cr_head,
                                 prd->cr_tail,
                                 cr);
    if (NULL != cr->fo)
    {
      TMH_EXCHANGES_find_exchange_cancel (cr->fo);
      cr->fo = NULL;
    }
    if (NULL != cr->rh)
    {
      TALER_EXCHANGE_refund_cancel (cr->rh);
      cr->rh = NULL;
    }
    if (NULL != cr->exchange_reply)
    {
      json_decref (cr->exchange_reply);
      cr->exchange_reply = NULL;
    }
    GNUNET_free (cr->exchange_url);
    GNUNET_free (cr);
  }
  GNUNET_free (prd);
}


/**
 * Check if @a prd has sub-activities still pending.
 *
 * @param prd request to check
 * @return #GNUNET_YES if activities are still pending
 */
static int
prd_pending (struct ProcessRefundData *prd)
{
  int pending = GNUNET_NO;

  for (struct CoinRefund *cr = prd->cr_head;
       NULL != cr;
       cr = cr->next)
  {
    if ( (NULL != cr->fo) ||
         (NULL != cr->rh) )
    {
      pending = GNUNET_YES;
      break;
    }
  }
  return pending;
}


/**
 * Check if @a prd is ready to be resumed, and if so, do it.
 *
 * @param prd refund request to be possibly ready
 */
static void
check_resume_prd (struct ProcessRefundData *prd)
{
  if (prd_pending (prd))
    return;
  GNUNET_CONTAINER_DLL_remove (prd_head,
                               prd_tail,
                               prd);
  GNUNET_assert (prd->suspended);
  prd->suspended = GNUNET_NO;
  MHD_resume_connection (prd->connection);
  TMH_trigger_daemon ();
}


/**
 * Callbacks of this type are used to serve the result of submitting a
 * refund request to an exchange.
 *
 * @param cls a `struct CoinRefund`
 * @param hr HTTP response data
 * @param exchange_pub exchange key used to sign refund confirmation
 * @param exchange_sig exchange's signature over refund
 */
static void
refund_cb (void *cls,
           const struct TALER_EXCHANGE_HttpResponse *hr,
           const struct TALER_ExchangePublicKeyP *exchange_pub,
           const struct TALER_ExchangeSignatureP *exchange_sig)
{
  struct CoinRefund *cr = cls;

  cr->rh = NULL;
  cr->exchange_status = hr->http_status;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Exchange refund status for coin %s is %u\n",
              TALER_B2S (&cr->coin_pub),
              hr->http_status);
  if (MHD_HTTP_OK != hr->http_status)
  {
    cr->exchange_code = hr->ec;
    cr->exchange_reply = json_incref ((json_t*) hr->reply);
  }
  else
  {
    enum GNUNET_DB_QueryStatus qs;

    cr->exchange_pub = *exchange_pub;
    cr->exchange_sig = *exchange_sig;
    qs = db->put_refund_proof (db->cls,
                               &cr->prd->merchant->pubkey,
                               &cr->prd->h_contract_terms,
                               &cr->coin_pub,
                               cr->rtransaction_id,
                               exchange_pub,
                               exchange_sig);
    if (0 >= qs)
    {
      /* generally, this is relatively harmless for the merchant, but let's at
         least log this. */
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Failed to persist exchange response to /refund in database: %d\n",
                  qs);
    }
  }
  check_resume_prd (cr->prd);
}


/**
 * Function called with the result of a #TMH_EXCHANGES_find_exchange()
 * operation.
 *
 * @param cls a `struct CoinRefund *`
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
  struct CoinRefund *cr = cls;

  cr->fo = NULL;
  if (TALER_EC_NONE == ec)
  {
    cr->rh = TALER_EXCHANGE_refund (eh,
                                    &cr->refund_amount,
                                    &cr->refund_fee,
                                    &cr->prd->h_contract_terms,
                                    &cr->coin_pub,
                                    cr->rtransaction_id,
                                    &cr->prd->merchant->privkey,
                                    &refund_cb,
                                    cr);
    return;
  }
  cr->exchange_status = http_status;
  cr->exchange_code = ec;
  cr->exchange_reply = json_incref ((json_t*) error_reply);
  check_resume_prd (cr->prd);
}


/**
 * Function called with information about a refund.
 * It is responsible for packing up the data to return.
 *
 * @param cls closure
 * @param coin_pub public coin from which the refund comes from
 * @param exchange_url URL of the exchange that issued @a coin_pub
 * @param rtransaction_id identificator of the refund
 * @param reason human-readable explanation of the refund
 * @param refund_amount refund amount which is being taken from @a coin_pub
 * @param refund_fee cost of this refund operation
 */
static void
process_refunds_cb (void *cls,
                    const struct TALER_CoinSpendPublicKeyP *coin_pub,
                    const char *exchange_url,
                    uint64_t rtransaction_id,
                    const char *reason,
                    const struct TALER_Amount *refund_amount,
                    const struct TALER_Amount *refund_fee)
{
  struct ProcessRefundData *prd = cls;
  struct CoinRefund *cr;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Found refund of %s for coin %s with reason `%s' in database\n",
              TALER_B2S (coin_pub),
              TALER_amount2s (refund_amount),
              reason);
  cr = GNUNET_new (struct CoinRefund);
  cr->exchange_url = GNUNET_strdup (exchange_url);
  cr->prd = prd;
  cr->coin_pub = *coin_pub;
  cr->rtransaction_id = rtransaction_id;
  cr->refund_amount = *refund_amount;
  cr->refund_fee = *refund_fee;
  GNUNET_CONTAINER_DLL_insert (prd->cr_head,
                               prd->cr_tail,
                               cr);
}


/**
 * Force resuming all suspended refund lookups, needed during shutdown.
 */
void
MH_force_refund_resume (void)
{
  struct ProcessRefundData *prd;

  while (NULL != (prd = prd_head))
  {
    GNUNET_CONTAINER_DLL_remove (prd_head,
                                 prd_tail,
                                 prd);
    GNUNET_assert (prd->suspended);
    prd->suspended = GNUNET_NO;
    MHD_resume_connection (prd->connection);
  }
}


/**
 * Return refund situation about a contract.
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
MH_handler_refund_lookup (struct TMH_RequestHandler *rh,
                          struct MHD_Connection *connection,
                          void **connection_cls,
                          const char *upload_data,
                          size_t *upload_data_size,
                          struct MerchantInstance *mi)
{
  struct ProcessRefundData *prd;
  const char *order_id;
  json_t *contract_terms;
  enum GNUNET_DB_QueryStatus qs;

  prd = *connection_cls;
  if (NULL == prd)
  {
    order_id = MHD_lookup_connection_value (connection,
                                            MHD_GET_ARGUMENT_KIND,
                                            "order_id");
    if (NULL == order_id)
    {
      GNUNET_break_op (0);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_BAD_REQUEST,
                                         TALER_EC_PARAMETER_MISSING,
                                         "order_id");
    }

    /* Convert order id to h_contract_terms */
    contract_terms = NULL;
    db->preflight (db->cls);
    qs = db->find_contract_terms (db->cls,
                                  &contract_terms,
                                  order_id,
                                  &mi->pubkey);
    if (0 > qs)
    {
      /* single, read-only SQL statements should never cause
         serialization problems */
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
      /* Always report on hard error as well to enable diagnostics */
      GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_REFUND_LOOKUP_DB_ERROR,
                                         "database error looking up order_id from merchant_contract_terms table");
    }

    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Unknown order id given: `%s'\n",
                  order_id);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_NOT_FOUND,
                                         TALER_EC_REFUND_ORDER_ID_UNKNOWN,
                                         "order_id not found in database");
    }

    prd = GNUNET_new (struct ProcessRefundData);
    if (GNUNET_OK !=
        TALER_JSON_hash (contract_terms,
                         &prd->h_contract_terms))
    {
      GNUNET_break (0);
      json_decref (contract_terms);
      GNUNET_free (prd);
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_INTERNAL_LOGIC_ERROR,
                                         "Could not hash contract terms");
    }
    json_decref (contract_terms);
    prd->hc.cc = &cleanup_prd;
    prd->merchant = mi;
    prd->ec = TALER_EC_NONE;
    prd->connection = connection;
    *connection_cls = prd;

    for (unsigned int i = 0; i<MAX_RETRIES; i++)
    {
      qs = db->get_refunds_from_contract_terms_hash (db->cls,
                                                     &mi->pubkey,
                                                     &prd->h_contract_terms,
                                                     &process_refunds_cb,
                                                     prd);
      if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
        break;
    }
    if (0 > qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Database hard error on refunds_from_contract_terms_hash lookup: %s\n",
                  GNUNET_h2s (&prd->h_contract_terms));
      return TALER_MHD_reply_with_error (connection,
                                         MHD_HTTP_INTERNAL_SERVER_ERROR,
                                         TALER_EC_REFUND_LOOKUP_DB_ERROR,
                                         "Failed to lookup refunds for contract");
    }

    /* Now launch exchange interactions, unless we already have the
       response in the database! */
    for (struct CoinRefund *cr = prd->cr_head;
         NULL != cr;
         cr = cr->next)
    {
      enum GNUNET_DB_QueryStatus qs;

      qs = db->get_refund_proof (db->cls,
                                 &cr->prd->merchant->pubkey,
                                 &cr->prd->h_contract_terms,
                                 &cr->coin_pub,
                                 cr->rtransaction_id,
                                 &cr->exchange_pub,
                                 &cr->exchange_sig);
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != qs)
      {
        /* We need to talk to the exchange */
        cr->fo = TMH_EXCHANGES_find_exchange (cr->exchange_url,
                                              NULL,
                                              GNUNET_NO,
                                              &exchange_found_cb,
                                              cr);
      }
    }
  }

  /* Check if there are still exchange operations pending */
  if (GNUNET_YES == prd_pending (prd))
  {
    if (! prd->suspended)
    {
      prd->suspended = GNUNET_YES;
      MHD_suspend_connection (connection);
      GNUNET_CONTAINER_DLL_insert (prd_head,
                                   prd_tail,
                                   prd);
    }
    return MHD_YES;   /* we're still talking to the exchange */
  }

  /* All operations done, build final response */
  if (NULL == prd->cr_head)
  {
    /* There ARE no refunds scheduled, bitch */
    return TALER_MHD_reply_with_error (connection,
                                       MHD_HTTP_NOT_FOUND,
                                       TALER_EC_REFUND_LOOKUP_NO_REFUND,
                                       "This contract is not currently eligible for refunds");
  }

  {
    json_t *ra;

    ra = json_array ();
    GNUNET_assert (NULL != ra);
    for (struct CoinRefund *cr = prd->cr_head;
         NULL != cr;
         cr = cr->next)
    {
      GNUNET_assert (
        0 ==
        json_array_append_new (
          ra,
          (MHD_HTTP_OK != cr->exchange_status)
          ? json_pack ((NULL != cr->exchange_reply)
                       ? "{s:o,s:o,s:o,s:I,s:I,s:I,s:O}"
                       : "{s:o,s:o,s:o,s:I,s:I:s:I}",
                       "coin_pub",
                       GNUNET_JSON_from_data_auto (&cr->coin_pub),
                       "refund_amount",
                       TALER_JSON_from_amount (&cr->refund_amount),
                       "refund_fee",
                       TALER_JSON_from_amount (&cr->refund_fee),
                       "exchange_http_status",
                       (json_int_t) cr->exchange_status,
                       "rtransaction_id",
                       (json_int_t) cr->rtransaction_id,
                       "exchange_code",
                       (json_int_t) cr->exchange_code,
                       "exchange_reply",
                       cr->exchange_reply)
          : json_pack ("{s:o,s:o,s:o,s:I,s:I,s:o,s:o}",
                       "coin_pub",
                       GNUNET_JSON_from_data_auto (&cr->coin_pub),
                       "refund_amount",
                       TALER_JSON_from_amount (&cr->refund_amount),
                       "refund_fee",
                       TALER_JSON_from_amount (&cr->refund_fee),
                       "exchange_http_status",
                       (json_int_t) cr->exchange_status,
                       "rtransaction_id",
                       (json_int_t) cr->rtransaction_id,
                       "exchange_pub",
                       GNUNET_JSON_from_data_auto (&cr->exchange_pub),
                       "exchange_sig",
                       GNUNET_JSON_from_data_auto (&cr->exchange_sig)
                       )));
    }
    return TALER_MHD_reply_json_pack (
      connection,
      MHD_HTTP_OK,
      "{s:o, s:o, s:o}",
      "refunds",
      ra,
      "merchant_pub",
      GNUNET_JSON_from_data_auto (&mi->pubkey),
      "h_contract_terms",
      GNUNET_JSON_from_data_auto (&prd->h_contract_terms));
  }
}


/* end of taler-merchant-httpd_refund_lookup.c */
