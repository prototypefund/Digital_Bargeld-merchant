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
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
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
 * Map containing all the known merchant instances
 */
extern struct GNUNET_CONTAINER_MultiHashMap *by_id_map;

/**
 * How long to wait before giving up processing with the exchange?
 */
#define TRACK_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))


/**
 * Context for a /track/transaction operation.
 */
struct TrackTransactionContext;

/**
 * Merchant instance being tracked
 */
struct MerchantInstance;

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
  struct TALER_EXCHANGE_TrackTransactionHandle *dwh;

  /**
   * Wire transfer identifier for this coin.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * Execution time of the wire transfer @e wtid.
   */
  struct GNUNET_TIME_Absolute execution_time;

  /**
   * Value of the coin including deposit fee.
   */
  struct TALER_Amount amount_with_fee;

  /**
   * Deposit fee for the coin.
   */
  struct TALER_Amount deposit_fee;

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
  struct TALER_EXCHANGE_TrackTransferHandle *wdh;

  /**
   * Response to return upon resume.
   */
  struct MHD_Response *response;

  /**
   * Wire transfer identifier we are currently looking up in @e wdh.
   */
  struct TALER_WireTransferIdentifierRawP current_wtid;

  /**
   * Execution time of the wire transfer we are currently looking up in @e wdh.
   */
  struct GNUNET_TIME_Absolute current_execution_time;

  /**
   * Hash of wire details for the transaction.
   */
  struct GNUNET_HashCode h_wire;

  /**
   * Hash of the contract.
   */
  struct GNUNET_HashCode h_contract;

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
   * Transaction this request is about.
   */
  uint64_t transaction_id;

  /**
   * Response code to return upon resume.
   */
  unsigned int response_code;

  /**
   * Which merchant instance is being tracked
   */
  struct MerchantInstance *mi;

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
      TALER_EXCHANGE_track_transaction_cancel (tcc->dwh);
      tcc->dwh = NULL;
    }
    GNUNET_free (tcc);
  }
  if (NULL != tctx->wdh)
  {
    TALER_EXCHANGE_track_transfer_cancel (tctx->wdh);
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
 * @param execution_time time when the exchange claims to have performed the wire transfer
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
                  struct GNUNET_TIME_Absolute execution_time,
                  const struct TALER_Amount *total_amount,
                  unsigned int details_length,
                  const struct TALER_TrackTransferDetails *details)
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
                                   tctx->current_execution_time,
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
      tcc->execution_time = tctx->current_execution_time;
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
  /* Continue tracing (will also handle case that we are done) */
  trace_coins (tctx);
}


/**
 * Closure for #proof_cb().
 */
struct ProofCheckContext
{
  /**
   * Proof returned from #proof_cb.  The reference counter was
   * increased for this reference and it must thus be freed.
   * NULL if we did not find any proof.  The JSON should
   * match the `TrackTransferResponse` of the exchange API
   * (https://api.taler.net/api-exchange.html#tracktransferresponse)
   */
  json_t *p_ret;

};


/**
 * Function called with information about a wire transfer identifier.
 * We actually never expect this to be called.
 *
 * @param cls closure with a `struct ProofCheckContext`
 * @param proof proof from exchange about what the wire transfer was for
 */
static void
proof_cb (void *cls,
          const json_t *proof)
{
  struct ProofCheckContext *pcc = cls;

  GNUNET_break (NULL == pcc->p_ret);
  pcc->p_ret = json_incref ((json_t *) proof);
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
 *        validated already), should be a `TrackTransactionResponse`
 *        from the exchange API
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
  struct ProofCheckContext pcc;

  tcc->dwh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    /* Transaction not resolved for one of the coins, report error!
       We keep the status code for #MHD_HTTP_ACCEPTED, but box all the
       others (as #MHD_HTTP_ACCEPTED is not an error). */
    resume_track_transaction_with_response
      (tcc->tctx,
       (MHD_HTTP_ACCEPTED == http_status)
       ? MHD_HTTP_ACCEPTED
       : MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack ("{s:I, s:O}",
                                    "exchange_status", (json_int_t) http_status,
                                    "details", json));
    return;
  }
  tctx->current_wtid = *wtid;
  tctx->current_execution_time = execution_time;
  pcc.p_ret = NULL;
  if (GNUNET_YES ==
      db->find_proof_by_wtid (db->cls,
                              tctx->exchange_uri,
                              wtid,
                              &proof_cb,
                              NULL))
  {
    GNUNET_break_op (0);
    resume_track_transaction_with_response
      (tcc->tctx,
       MHD_HTTP_CONFLICT,
       TMH_RESPONSE_make_json_pack ("{s:s, s:O, s:o, s:o}",
                                    "error", "conflicting transfer data from exchange",
                                    "transaction_tracking_claim", json,
                                    "wtid_tracking_claim", pcc.p_ret,
                                    "coin_pub", GNUNET_JSON_from_data_auto (&tcc->coin_pub)));
    return;
  }
  tctx->wdh = TALER_EXCHANGE_track_transfer (tctx->eh,
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
  unsigned int num_wtid;

  GNUNET_assert (NULL != tctx->eh);
  for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)
    if (GNUNET_YES != tcc->have_wtid)
      break;
  if (NULL != tcc)
  {
    /* we are not done requesting WTIDs, do the next one */
    tcc->dwh = TALER_EXCHANGE_track_transaction (tctx->eh,
                                                 &tctx->mi->privkey,
                                                 &tctx->h_wire,
                                                 &tctx->h_contract,
                                                 &tcc->coin_pub,
                                                 tctx->transaction_id,
                                                 &wtid_cb,
                                                 tcc);
    return;
  }
  /* We have obtained all WTIDs, now prepare the response */
  num_wtid = 0;
  /* count how many disjoint wire transfer identifiers there are;
     note that there should only usually be one, so while this
     is worst-case O(n^2), in pracitce this is O(n) */
  for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)
  {
    struct TrackCoinContext *tcc2;
    int found = GNUNET_NO;

    for (tcc2 = tctx->tcc_head; tcc2 != tcc; tcc2 = tcc2->next)
    {
      if (0 == memcmp (&tcc->wtid,
                       &tcc2->wtid,
                       sizeof (struct TALER_WireTransferIdentifierRawP)))
      {
        found = GNUNET_YES;
        break;
      }
    }
    if (GNUNET_NO == found)
      num_wtid++;
  }

  {
    /* on-stack allocation is fine, as the number of coins and the
       number of wire-transfers per-transaction is expected to be tiny. */
    struct MHD_Response *resp;
    struct TALER_MERCHANT_TransactionWireTransfer wts[num_wtid];
    unsigned int wtid_off;

    wtid_off = 0;
    for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)
    {
      struct TrackCoinContext *tcc2;
      int found = GNUNET_NO;

      for (tcc2 = tctx->tcc_head; tcc2 != tcc; tcc2 = tcc2->next)
      {
        if (0 == memcmp (&tcc->wtid,
                         &tcc2->wtid,
                         sizeof (struct TALER_WireTransferIdentifierRawP)))
        {
          found = GNUNET_YES;
          break;
        }
      }
      if (GNUNET_NO == found)
      {
        unsigned int num_coins;
        struct TALER_MERCHANT_TransactionWireTransfer *wt;

        wt = &wts[wtid_off++];
        wt->wtid = tcc->wtid;
        wt->execution_time = tcc->execution_time;
        /* count number of coins with this wtid */
        num_coins = 0;
        for (tcc2 = tctx->tcc_head; NULL != tcc2; tcc2 = tcc2->next)
        {
          if (0 == memcmp (&wt->wtid,
                           &tcc2->wtid,
                           sizeof (struct TALER_WireTransferIdentifierRawP)))
            num_coins++;
        }
        /* initialize coins array */
        wt->num_coins = num_coins;
        wt->coins = GNUNET_new_array (num_coins,
                                      struct TALER_MERCHANT_CoinWireTransfer);
        num_coins = 0;
        for (tcc2 = tctx->tcc_head; NULL != tcc2; tcc2 = tcc2->next)
        {
          if (0 == memcmp (&wt->wtid,
                           &tcc2->wtid,
                           sizeof (struct TALER_WireTransferIdentifierRawP)))
          {
            struct TALER_MERCHANT_CoinWireTransfer *coin = &wt->coins[num_coins++];

            coin->coin_pub = tcc2->coin_pub;
            coin->amount_with_fee = tcc2->amount_with_fee;
            coin->deposit_fee = tcc2->deposit_fee;
          }
        }
      } /* GNUNET_NO == found */
    } /* for all tcc */
    GNUNET_assert (wtid_off == num_wtid);

    resp = TMH_RESPONSE_make_track_transaction_ok (num_wtid,
                                                   wts);
    for (wtid_off=0;wtid_off < num_wtid; wtid_off++)
      GNUNET_free (wts[wtid_off].coins);
    resume_track_transaction_with_response (tctx,
                                            MHD_HTTP_OK,
                                            resp);
  } /* end of scope for 'wts' and 'resp' */
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
 * @param merchant's public key
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
		const struct TALER_MerchantPublicKeyP *merchant_pub,
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
 * Information about the wire transfer corresponding to
 * a deposit operation.  Note that it is in theory possible
 * that we have a @a transaction_id and @a coin_pub in the
 * result that do not match a deposit that we know about,
 * for example because someone else deposited funds into
 * our account.
 *
 * @param cls closure
 * @param transaction_id ID of the contract
 * @param coin_pub public key of the coin
 * @param wtid identifier of the wire transfer in which the exchange
 *             send us the money for the coin deposit
 * @param execution_time when was the wire transfer executed?
 * @param exchange_proof proof from exchange about what the deposit was for
 *             NULL if we have not asked for this signature
 */
static void
transfer_cb (void *cls,
             uint64_t transaction_id,
             const struct TALER_CoinSpendPublicKeyP *coin_pub,
             const struct TALER_WireTransferIdentifierRawP *wtid,
             struct GNUNET_TIME_Absolute execution_time,
             const json_t *exchange_proof)
{
  struct TrackCoinContext *tcc = cls;

  if (0 != memcmp (coin_pub,
                   &tcc->coin_pub,
                   sizeof (struct TALER_CoinSpendPublicKeyP)))
    return;
  tcc->wtid = *wtid;
  tcc->execution_time = execution_time;
  tcc->have_wtid = GNUNET_YES;
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
  tcc->amount_with_fee = *amount_with_fee;
  tcc->deposit_fee = *deposit_fee;
  GNUNET_CONTAINER_DLL_insert (tctx->tcc_head,
                               tctx->tcc_tail,
                               tcc);
  GNUNET_break (GNUNET_SYSERR !=
                db->find_transfers_by_id (db->cls,
                                          transaction_id,
                                          &transfer_cb,
                                          tcc));
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
  const char *receiver;
  int ret;
  struct GNUNET_HashCode h_receiver;

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
  if ( (NULL != tctx->fo) ||
       (NULL != tctx->eh) )
  {
    /* likely old MHD version */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Not sure why we are here, should be suspended\n");
    return MHD_YES; /* still work in progress */
  }
  str = MHD_lookup_connection_value (connection,
                                     MHD_GET_ARGUMENT_KIND,
                                     "id");
  if (NULL == str)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "id argument missing");
  receiver = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "receiver");
  if (NULL == receiver)
    receiver = "default";
  GNUNET_CRYPTO_hash (receiver,
                      strlen (receiver),
                      &h_receiver);
  tctx->mi = GNUNET_CONTAINER_multihashmap_get (by_id_map,
                                                &h_receiver);
  if (NULL == tctx->mi)
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "unknown receiver");
  if (1 !=
      sscanf (str,
              "%llu",
              &transaction_id))
    return TMH_RESPONSE_reply_bad_request (connection,
                                           "id argument must be a number");

  ret = db->find_transaction (db->cls,
                              transaction_id,
			      &tctx->mi->pubkey,
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
  ret = db->find_payments (db->cls,
                           transaction_id,
                           &tctx->mi->pubkey,
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
  return MHD_YES;
}


/* end of taler-merchant-httpd_track-transaction.c */
