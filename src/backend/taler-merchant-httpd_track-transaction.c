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
 * @file backend/taler-merchant-httpd_track-transaction.c
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
#include "taler-merchant-httpd_track-transaction.h"


/**
 * How long to wait before giving up processing with the exchange?
 */
#define TRACK_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))


/**
 * Context for a /track/transaction operation.
 */
struct TrackTransactionContext;


/**
 * Information we keep for each coin in a /track/transaction operation.
 */
struct TrackCoinContext
{
  /**
   * Kept in a DLL.
   */
  struct TrackCoinContext *next;

  /**
   * Kept in a DLL.
   */
  struct TrackCoinContext *prev;

  /**
   * Our Context for a /track/transaction operation.
   */
  struct TrackTransactionContext *tctx;

  /**
   * Public key of the coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Handle for the request to resolve the WTID for this coin.
   */
  struct TALER_EXCHANGE_DepositWtidHandle *dwh;

  /**
   * Wire transfer identifier for this coin.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Have we obtained the WTID for this coin yet?
   */
  int have_wtid;

};


/**
 * Context for a /track/transaction operation.
 */
struct TrackTransactionContext
{

  /**
   * This field MUST be first.
   * FIXME: Explain why!
   */
  struct TM_HandlerContext hc;

  /**
   * HTTP request we are handling.
   */
  struct MHD_Connection *connection;

  /**
   * Kept in a DLL.
   */
  struct TrackCoinContext *tcc_head;

  /**
   * Kept in a DLL.
   */
  struct TrackCoinContext *tcc_tail;

  /**
   * Exchange that was used for the transaction.
   */
  char *exchange_uri;

  /**
   * Wire transfer identifier we are currently looking up in @e wdh
   */
  struct TALER_WireTransferIdentifierRawP current_wtid;

  /**
   * Transaction this request is about.
   */
  uint64_t transaction_id;

  /**
   * Hash of wire details for the transaction.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Timestamp of the transaction.
   */
  struct GNUNET_TIME_Absolute timestamp;

  /**
   * Refund deadline for the transaction.
   */
  struct GNUNET_TIME_Absolute refund_deadline;

  /**
   * Total value of the transaction.
   */
  struct TALER_Amount total_amount;

  /**
   * Hash of the contract.
   */
  struct GNUNET_HashCode h_contract;

  /**
   * Task run on timeout.
   */
  struct GNUNET_SCHEDULER_Task *timeout_task;

  /**
   * Handle for operation to lookup /keys (and auditors) from
   * the exchange used for this transaction; NULL if no operation is
   * pending.
   */
  struct TMH_EXCHANGES_FindOperation *fo;

  /**
   * Handle to our exchange, once we found it.
   */
  struct TALER_EXCHANGE_Handle *eh;

  /**
   * Handle we use to resolve transactions for a given WTID.
   */
  struct TALER_EXCHANGE_WireDepositsHandle *wdh;

  /**
   * Response to return upon resume.
   */
  struct MHD_Response *response;

  /**
   * Response code to return upon resume.
   */
  unsigned int response_code;


};


/**
 * Free the @a tctx.
 *
 * @param tctx data to free
 */
static void
free_tctx (struct TrackTransactionContext *tctx)
{
  struct TrackCoinContext *tcc;

  while (NULL != (tcc = tctx->tcc_head))
  {
    GNUNET_CONTAINER_DLL_remove (tctx->tcc_head,
                                 tctx->tcc_tail,
                                 tcc);
    if (NULL != tcc->dwh)
    {
      TALER_EXCHANGE_deposit_wtid_cancel (tcc->dwh);
      tcc->dwh = NULL;
    }
    GNUNET_free (tcc);
  }
  if (NULL != tctx->wdh)
  {
    TALER_EXCHANGE_wire_deposits_cancel (tctx->wdh);
    tctx->wdh = NULL;
  }
  if (NULL != tctx->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (tctx->fo);
    tctx->fo = NULL;
  }
  if (NULL != tctx->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (tctx->timeout_task);
    tctx->timeout_task = NULL;
  }
  if (NULL != tctx->exchange_uri)
  {
    GNUNET_free (tctx->exchange_uri);
    tctx->exchange_uri = NULL;
  }
  GNUNET_free (tctx);
}


/**
 * Custom cleanup routine for a `struct TrackTransactionContext`.
 *
 * @param hc the `struct PayContext` to clean up.
 */
static void
track_transaction_cleanup (struct TM_HandlerContext *hc)
{
  struct TrackTransactionContext *tctx = (struct TrackTransactionContext *) hc;

  free_tctx (tctx);
}


/**
 * Resume the given /track/transaction operation and send the given
 * response.  Stores the response in the @a tctx and signals MHD to
 * resume the connection.  Also ensures MHD runs immediately.
 *
 * @param tctx transaction tracking context
 * @param response_code response code to use
 * @param response response data to send back
 */
static void
resume_track_transaction_with_response (struct TrackTransactionContext *tctx,
                                        unsigned int response_code,
                                        struct MHD_Response *response)
{
  tctx->response_code = response_code;
  tctx->response = response;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /track/transaction handling as exchange interaction is done (%u)\n",
              response_code);
  if (NULL != tctx->timeout_task)
  {
    GNUNET_SCHEDULER_cancel (tctx->timeout_task);
    tctx->timeout_task = NULL;
  }
  MHD_resume_connection (tctx->connection);
  TMH_trigger_daemon (); /* we resumed, kick MHD */
}


/**
 * This function is called to trace the wire transfers for
 * all of the coins of the transaction of the @a tctx.  Once
 * we have traced all coins, we build the response.
 *
 * @param tctx track context with established connection to exchange
 */
static void
trace_coins (struct TrackTransactionContext *tctx);


/**
 * Function called with detailed wire transfer data, including all
 * of the coin transactions that were combined into the wire transfer.
 *
 * We now store this information.  Then we check if we still have
 * any coins of the original wire transfer not taken care of.
 *
 * @param cls closure
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param exchange_pub public key of the exchange used for signing
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
wire_deposits_cb (void *cls,
                  unsigned int http_status,
                  const struct TALER_ExchangePublicKeyP *exchange_pub,
                  const json_t *json,
                  const struct GNUNET_HashCode *h_wire,
                  const struct TALER_Amount *total_amount,
                  unsigned int details_length,
                  const struct TALER_WireDepositDetails *details)
{
  struct TrackTransactionContext *tctx = cls;
  struct TrackCoinContext *tcc;
  unsigned int i;

  tctx->wdh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    resume_track_transaction_with_response
      (tctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack ("{s:I, s:O}",
                                    "exchange_status", (json_int_t) http_status,
                                    "details", json));
    return;
  }
  if (GNUNET_OK !=
      db->store_transfer_to_proof (db->cls,
                                   tctx->exchange_uri,
                                   &tctx->current_wtid,
                                   exchange_pub,
                                   json))
  {
    /* Not good, but not fatal either, log error and continue */
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to store transfer-to-proof mapping in DB\n");
  }
  for (tcc=tctx->tcc_head;NULL != tcc;tcc=tcc->next)
  {
    if (GNUNET_YES == tcc->have_wtid)
      continue;
    for (i=0;i<details_length;i++)
    {
      if (0 != memcmp (&details[i].coin_pub,
                       &tcc->coin_pub,
                       sizeof (struct TALER_CoinSpendPublicKeyP)))
        continue;
      tcc->wtid = tctx->current_wtid;
      tcc->have_wtid = GNUNET_YES;
      if (GNUNET_OK !=
          db->store_coin_to_transfer (db->cls,
                                      tctx->transaction_id,
                                      &tcc->coin_pub,
                                      &tctx->current_wtid))
      {
        /* Not good, but not fatal either, log error and continue */
        GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                    "Failed to store coin-to-transfer mapping in DB\n");
      }
    }
  }
  /* Continue traceing (will also handle case that we are done) */
  trace_coins (tctx);
}


/**
 * Function called with detailed wire transfer data.
 * We were trying to find out in which wire transfer one of the
 * coins was involved in. Now we know. What we do now is first
 * obtain the inverse: all other coins of that wire transfer,
 * which is what we prefer to store.
 *
 * @param cls closure with a `struct TrackCoinContext`
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param exchange_pub public key of the exchange used for signing @a json
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param wtid wire transfer identifier used by the exchange, NULL if exchange did not
 *                  yet execute the transaction
 * @param execution_time actual or planned execution time for the wire transfer
 * @param coin_contribution contribution to the @a total_amount of the deposited coin (may be NULL)
 */
static void
wtid_cb (void *cls,
         unsigned int http_status,
         const struct TALER_ExchangePublicKeyP *exchange_pub,
         const json_t *json,
         const struct TALER_WireTransferIdentifierRawP *wtid,
         struct GNUNET_TIME_Absolute execution_time,
         const struct TALER_Amount *coin_contribution)
{
  struct TrackCoinContext *tcc = cls;
  struct TrackTransactionContext *tctx = tcc->tctx;

  tcc->dwh = NULL;
  tctx->current_wtid = *wtid;
  tctx->wdh = TALER_EXCHANGE_wire_deposits (tctx->eh,
                                            wtid,
                                            &wire_deposits_cb,
                                            tctx);
}


/**
 * This function is called to trace the wire transfers for
 * all of the coins of the transaction of the @a tctx.  Once
 * we have traced all coins, we build the response.
 *
 * @param tctx track context with established connection to exchange
 */
static void
trace_coins (struct TrackTransactionContext *tctx)
{
  struct TrackCoinContext *tcc;

  GNUNET_assert (NULL != tctx->eh);
  for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)
    if (GNUNET_YES != tcc->have_wtid)
      break;
  if (NULL == tcc)
  {
#if ALL_COINS_DONE_FIXME
    generate_response ();
    resume_track_transaction_with_response (tctx,
                                            MHD_HTTP_OK,
                                            response);
#endif
    return;
  }
  tcc->dwh = TALER_EXCHANGE_deposit_wtid (tctx->eh,
                                          &privkey,
                                          &tctx->h_wire,
                                          &tctx->h_contract,
                                          &tcc->coin_pub,
                                          tctx->transaction_id,
                                          &wtid_cb,
                                          tcc);
}


/**
 * Function called with the result of our exchange lookup.
 *
 * @param cls the `struct TrackTransactionContext`
 * @param eh NULL if exchange was not found to be acceptable
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_track_transaction_with_exchange (void *cls,
                                         struct TALER_EXCHANGE_Handle *eh,
                                         int exchange_trusted)
{
  struct TrackTransactionContext *tctx = cls;

  tctx->fo = NULL;
  tctx->eh = eh;
  trace_coins (tctx);
}


/**
 * Handle a timeout for the processing of the track transaction request.
 *
 * @param cls closure
 */
static void
handle_track_transaction_timeout (void *cls)
{
  struct TrackTransactionContext *tctx = cls;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Resuming /track/transaction with error after timeout\n");
  tctx->timeout_task = NULL;

  if (NULL != tctx->fo)
  {
    TMH_EXCHANGES_find_exchange_cancel (tctx->fo);
    tctx->fo = NULL;
  }
  resume_track_transaction_with_response (tctx,
                                          MHD_HTTP_SERVICE_UNAVAILABLE,
                                          TMH_RESPONSE_make_internal_error ("exchange not reachable"));
}


/**
 * Function called with information about a transaction.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param exchange_uri URI of the exchange
 * @param h_contract hash of the contract
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 */
static void
transaction_cb (void *cls,
                uint64_t transaction_id,
                const char *exchange_uri,
                const struct GNUNET_HashCode *h_contract,
                const struct GNUNET_HashCode *h_wire,
                struct GNUNET_TIME_Absolute timestamp,
                struct GNUNET_TIME_Absolute refund,
                const struct TALER_Amount *total_amount)
{
  struct TrackTransactionContext *tctx = cls;

  tctx->transaction_id = transaction_id;
  tctx->exchange_uri = GNUNET_strdup (exchange_uri);
  tctx->h_contract = *h_contract;
  tctx->h_wire = *h_wire;
  tctx->timestamp = timestamp;
  tctx->refund_deadline = refund;
  tctx->total_amount = *total_amount;
}


/**
 * Function called with information about a coin that was deposited.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
coin_cb (void *cls,
         uint64_t transaction_id,
         const struct TALER_CoinSpendPublicKeyP *coin_pub,
         const struct TALER_Amount *amount_with_fee,
         const struct TALER_Amount *deposit_fee,
         const json_t *exchange_proof)
{
  struct TrackTransactionContext *tctx = cls;
  struct TrackCoinContext *tcc;

  tcc = GNUNET_new (struct TrackCoinContext);
  tcc->tctx = tctx;
  tcc->coin_pub = *coin_pub;
  GNUNET_CONTAINER_DLL_insert (tctx->tcc_head,
                               tctx->tcc_tail,
                               tcc);
}


/**
 * Handle a "/track/transaction" request.
 *
 * @param rh context of the handler
 * @param connection the MHD connection to handle
 * @param[in,out] connection_cls the connection's closure (can be updated)
 * @param upload_data upload data
 * @param[in,out] upload_data_size number of bytes (left) in @a upload_data
 * @return MHD result code
 */
int
MH_handler_track_transaction (struct TMH_RequestHandler *rh,
                              struct MHD_Connection *connection,
                              void **connection_cls,
                              const char *upload_data,
                              size_t *upload_data_size)
{
  struct TrackTransactionContext *tctx;
  unsigned long long transaction_id;
  const char *str;
  int ret;

  if (NULL == *connection_cls)
  {
    tctx = GNUNET_new (struct TrackTransactionContext);
    tctx->hc.cc = &track_transaction_cleanup;
    tctx->connection = connection;
    *connection_cls = tctx;
  }
  else
  {
    /* not first call, recover state */
    tctx = *connection_cls;
  }

  if (0 != tctx->response_code)
  {
    /* We are *done* processing the request, just queue the response (!) */
    if (UINT_MAX == tctx->response_code)
    {
      GNUNET_break (0);
      return MHD_NO; /* hard error */
    }
    ret = MHD_queue_response (connection,
                              tctx->response_code,
                              tctx->response);
    if (NULL != tctx->response)
    {
      MHD_destroy_response (tctx->response);
      tctx->response = NULL;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Queueing response (%u) for /track/transaction (%s).\n",
                (unsigned int) tctx->response_code,
                ret ? "OK" : "FAILED");
    return ret;
  }

  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "id");
  if (NULL == str)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "id argument missing");
  if (1 !=
      sscanf (str,
              "%llu",
              &transaction_id))
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "id argument must be a number");

  ret = db->find_transaction_by_id (db->cls,
                                    transaction_id,
                                    &transaction_cb,
                                    tctx);
  if (GNUNET_NO == ret)
  {
    return TMH_RESPONSE_reply_not_found (connection,
                                         "id");
  }
  if ( (GNUNET_SYSERR == ret) ||
       (tctx->transaction_id != (uint64_t) transaction_id) ||
       (NULL == tctx->exchange_uri) )
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "Database error");
  }
  ret = db->find_payments_by_id (db->cls,
                                 transaction_id,
                                 &coin_cb,
                                 tctx);
  if (GNUNET_SYSERR == ret)
  {
    GNUNET_break (0);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              "Database error");
  }
  if (GNUNET_NO == ret)
  {
    return TMH_RESPONSE_reply_not_found (connection,
                                         "deposits");
  }
  *connection_cls = tctx;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /track/transaction handling while working with the exchange\n");
  MHD_suspend_connection (connection);
  tctx->fo = TMH_EXCHANGES_find_exchange (tctx->exchange_uri,
                                          &process_track_transaction_with_exchange,
                                          tctx);

  tctx->timeout_task = GNUNET_SCHEDULER_add_delayed (TRACK_TIMEOUT,
                                                     &handle_track_transaction_timeout,
                                                     tctx);
  return MHD_NO;
}


/* end of taler-merchant-httpd_contract.c */
