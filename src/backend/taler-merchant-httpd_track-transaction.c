/*
  This file is part of TALER
  (C) 2014-2017 INRIA

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
 * Information about a wire transfer for a /track/transaction response.
 */
struct TransactionWireTransfer
{

  /**
   * Wire transfer identifier this struct is about.
   */
  struct TALER_WireTransferIdentifierRawP wtid;

  /**
   * When was this wire transfer executed?
   */
  struct GNUNET_TIME_Absolute execution_time;

  /**
   * Number of coins of the selected transaction that
   * is covered by this wire transfer.
   */
  unsigned int num_coins;

  /**
   * Information about the coins of the selected transaction
   * that are part of the wire transfer.
   */
  struct TALER_MERCHANT_CoinWireTransfer *coins;

  /**
   * URL of the exchange that executed the wire transfer.
   */
  char *exchange_url;
};


/**
 * Map containing all the known merchant instances
 */
extern struct GNUNET_CONTAINER_MultiHashMap *by_id_map;

/**
 * How long to wait before giving up processing with the exchange?
 */
#define TRACK_TIMEOUT (GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30))

/**
 * How often do we retry the simple INSERT database transaction?
 */
#define MAX_RETRIES 3


/**
 * Generate /track/transaction response.
 *
 * @param num_transfers how many wire transfers make up the transaction
 * @param transfers data on each wire transfer
 * @return MHD response object
 */
static struct MHD_Response *
make_track_transaction_ok (unsigned int num_transfers,
			   const struct TransactionWireTransfer *transfers)
{
  struct MHD_Response *ret;
  json_t *j_transfers;
  struct TALER_Amount sum;

  j_transfers = json_array ();
  for (unsigned int i=0;i<num_transfers;i++)
  {
    const struct TransactionWireTransfer *transfer = &transfers[i];

    sum = transfer->coins[0].amount_with_fee;
    for (unsigned int j=1;j<transfer->num_coins;j++)
    {
      const struct TALER_MERCHANT_CoinWireTransfer *coin = &transfer->coins[j];

      GNUNET_assert (GNUNET_SYSERR !=
		     TALER_amount_add (&sum,
				       &sum,
				       &coin->amount_with_fee));
    }

    GNUNET_assert (0 ==
                   json_array_append_new (j_transfers,
                                          json_pack ("{s:s, s:o, s:o, s:o}",
                                                     "exchange", transfer->exchange_url,
                                                     "wtid", GNUNET_JSON_from_data_auto (&transfer->wtid),
                                                     "execution_time", GNUNET_JSON_from_time_abs (transfer->execution_time),
                                                     "amount", TALER_JSON_from_amount (&sum))));
  }
  ret = TMH_RESPONSE_make_json (j_transfers);
  json_decref (j_transfers);
  return ret;
}


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
   * Our context for a /track/transaction operation.
   */
  struct TrackTransactionContext *tctx;

  /**
   * Public key of the coin.
   */
  struct TALER_CoinSpendPublicKeyP coin_pub;

  /**
   * Exchange that was used for the transaction.
   */
  char *exchange_url;

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
   * URL of the exchange we currently have in @e eh.
   */
  const char *current_exchange;

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
  const char *transaction_id;

  /**
   *  Proposal's hashcode.
   */
  struct GNUNET_HashCode h_contract_terms;

  /**
   * Response code to return upon resume.
   */
  unsigned int response_code;

  /**
   * Which merchant instance is being tracked
   */
  struct MerchantInstance *mi;

  /**
   * Set to negative values in #coin_cb() if we encounter
   * a database problem.
   */
  enum GNUNET_DB_QueryStatus qs;

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
    if (NULL != tcc->exchange_url)
    {
      GNUNET_free (tcc->exchange_url);
      tcc->exchange_url = NULL;
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
 * @param ec taler-specific error code
 * @param exchange_pub public key of the exchange used for signing
 * @param json original json reply (may include signatures, those have then been
 *        validated already)
 * @param execution_time time when the exchange claims to have performed the wire transfer
 * @param wtid extracted wire transfer identifier, or NULL if the exchange could
 *             not provide any (set only if @a http_status is #MHD_HTTP_OK)
 * @param total_amount total amount of the wire transfer, or NULL if the exchange could
 *             not provide any @a wtid (set only if @a http_status is #MHD_HTTP_OK)
 * @param wire_fee wire fee that was charged by the exchange
 * @param details_length length of the @a details array
 * @param details array with details about the combined transactions
 */
static void
wire_deposits_cb (void *cls,
                  unsigned int http_status,
		  enum TALER_ErrorCode ec,
                  const struct TALER_ExchangePublicKeyP *exchange_pub,
                  const json_t *json,
                  const struct GNUNET_HashCode *h_wire,
                  struct GNUNET_TIME_Absolute execution_time,
                  const struct TALER_Amount *total_amount,
                  const struct TALER_Amount *wire_fee,
                  unsigned int details_length,
                  const struct TALER_TrackTransferDetails *details)
{
  struct TrackTransactionContext *tctx = cls;
  enum GNUNET_DB_QueryStatus qs;

  tctx->wdh = NULL;
  if (MHD_HTTP_OK != http_status)
  {
    GNUNET_break_op (0);
    resume_track_transaction_with_response
      (tctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack ("{s:I, s:I, s:I, s:O}",
				    "code", (json_int_t) TALER_EC_TRACK_TRANSACTION_WIRE_TRANSFER_TRACE_ERROR,
                                    "exchange-http-status", (json_int_t) http_status,
				    "exchange-code", (json_int_t) ec,
                                    "details", json));
    return;
  }
  for (unsigned int i=0;i<MAX_RETRIES;i++)
  {
    qs = db->store_transfer_to_proof (db->cls,
				      tctx->current_exchange,
				      &tctx->current_wtid,
				      tctx->current_execution_time,
				      exchange_pub,
				      json);
    if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
      break;
  }
  if (0 > qs)
  {
    /* Not good, but not fatal either, log error and continue */
    /* Special report if retries insufficient */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to store transfer-to-proof mapping in DB\n");
  }
  for (struct TrackCoinContext *tcc=tctx->tcc_head;
       NULL != tcc;
       tcc=tcc->next)
  {
    if (GNUNET_YES == tcc->have_wtid)
      continue;
    for (unsigned int d=0;d<details_length;d++)
    {

      if (0 == memcmp (&details[d].coin_pub,
                       &tcc->coin_pub,
                       sizeof (struct TALER_CoinSpendPublicKeyP)))
      {
        tcc->wtid = tctx->current_wtid;
        tcc->execution_time = tctx->current_execution_time;
        tcc->have_wtid = GNUNET_YES;
      }

      for (unsigned int i=0;i<MAX_RETRIES;i++)
      {
	qs = db->store_coin_to_transfer (db->cls,
					 &details[d].h_contract_terms,
					 &details[d].coin_pub,
					 &tctx->current_wtid);
	if (GNUNET_DB_STATUS_SOFT_ERROR != qs)
	  break;
      }
      if (0 > qs)
      {
	/* Not good, but not fatal either, log error and continue */
	/* Special report if retries insufficient */
	GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
	/* Always report on hard error as well to enable diagnostics */
	GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
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
 * This function takes the wtid from the coin being tracked
 * and _track_ it against the exchange.  This way, we know
 * all the other coins which were aggregated together with
 * this one.  This way we save further HTTP requests to track
 * the other coins.
 *
 * @param cls closure with a `struct TrackCoinContext`
 * @param http_status HTTP status code we got, 0 on exchange protocol violation
 * @param ec taler-specific error code, #TALER_EC_NONE on success
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
	 enum TALER_ErrorCode ec,
         const struct TALER_ExchangePublicKeyP *exchange_pub,
         const json_t *json,
         const struct TALER_WireTransferIdentifierRawP *wtid,
         struct GNUNET_TIME_Absolute execution_time,
         const struct TALER_Amount *coin_contribution)
{
  struct TrackCoinContext *tcc = cls;
  struct TrackTransactionContext *tctx = tcc->tctx;
  struct ProofCheckContext pcc;
  enum GNUNET_DB_QueryStatus qs;

  tcc->dwh = NULL;

  if (MHD_HTTP_OK != http_status)
  {
    if (MHD_HTTP_ACCEPTED == http_status)
    {
      resume_track_transaction_with_response
        (tcc->tctx,
         MHD_HTTP_ACCEPTED,
         /* Return verbatim what the exchange said.  */
         TMH_RESPONSE_make_json (json));

      return;
    }

    /* Transaction not resolved for one of the
       coins, report error! */
    resume_track_transaction_with_response
      (tcc->tctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack
         ("{s:I, s:I, s:I, s:O}",
          "code",
          (json_int_t) TALER_EC_TRACK_TRANSACTION_COIN_TRACE_ERROR,
          "exchange-http-status",
          (json_int_t) http_status,
          "exchange-code",
          (json_int_t) ec,
          "details",
          json));

    return;
  }
  tctx->current_wtid = *wtid;
  tctx->current_execution_time = execution_time;

  pcc.p_ret = NULL;
  /* attempt to find this wtid's track from our database,
     Will make pcc.p_ret point to a "proof", if one exists. */
  qs = db->find_proof_by_wtid (db->cls,
			       tctx->current_exchange,
			       wtid,
			       &proof_cb,
			       &pcc);
  if (0 > qs)
  {
    /* Simple select queries should not
       cause serialization issues */
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    /* Always report on hard error as well to enable diagnostics */
    GNUNET_break (GNUNET_DB_STATUS_HARD_ERROR == qs);
    resume_track_transaction_with_response
      (tcc->tctx,
       MHD_HTTP_INTERNAL_SERVER_ERROR,
       TMH_RESPONSE_make_error
         (TALER_EC_TRACK_TRANSACTION_DB_FETCH_FAILED,
	  "Fail to query database about proofs"));
    return;
  }

  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    /* How come this wtid was already stored into the
       database and _not all_ of its coins were already
       tracked? Inconsistent state (! At least regarding
       what the exchange tells us) */
    GNUNET_break_op (0);
    resume_track_transaction_with_response
      (tcc->tctx,
       MHD_HTTP_FAILED_DEPENDENCY,
       TMH_RESPONSE_make_json_pack ("{s:I, s:s, s:O, s:o, s:o}",
				    "code", (json_int_t) TALER_EC_TRACK_TRANSACTION_CONFLICTING_REPORTS,
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
 * We have obtained all WTIDs, now prepare the response
 *
 * @param tctx handle for the operation
 */
static void
generate_response (struct TrackTransactionContext *tctx)
{
  struct TrackCoinContext *tcc;
  unsigned int num_wtid;

  num_wtid = 0;
  /* count how many disjoint wire transfer identifiers there are;
     note that there should only usually be one, so while this
     is worst-case O(n^2), in pracitce this is O(n) */
  for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)
  {
    int found = GNUNET_NO;

    for (struct TrackCoinContext *tcc2 = tctx->tcc_head;
	 tcc2 != tcc;
	 tcc2 = tcc2->next)
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
    struct TransactionWireTransfer wts[num_wtid];
    unsigned int wtid_off;

    wtid_off = 0;
    for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)
    {
      int found = GNUNET_NO;

      for (struct TrackCoinContext *tcc2 = tctx->tcc_head;
	   tcc2 != tcc;
	   tcc2 = tcc2->next)
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
        struct TransactionWireTransfer *wt;

        wt = &wts[wtid_off++];
        wt->wtid = tcc->wtid;
	wt->exchange_url = tcc->exchange_url;
        wt->execution_time = tcc->execution_time;
        /* count number of coins with this wtid */
        num_coins = 0;
        for (struct TrackCoinContext *tcc2 = tctx->tcc_head;
	     NULL != tcc2;
	     tcc2 = tcc2->next)
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
        for (struct TrackCoinContext *tcc2 = tctx->tcc_head;
	     NULL != tcc2;
	     tcc2 = tcc2->next)
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

    resp = make_track_transaction_ok (num_wtid,
				      wts);
    for (wtid_off=0;wtid_off < num_wtid; wtid_off++)
      GNUNET_free (wts[wtid_off].coins);
    resume_track_transaction_with_response (tctx,
                                            MHD_HTTP_OK,
                                            resp);
  } /* end of scope for 'wts' and 'resp' */
}


/**
 * Find the exchange to trace the next coin(s).
 *
 * @param tctx operation context
 */
static void
find_exchange (struct TrackTransactionContext *tctx);


/**
 * This function is called to 'trace the wire transfers'
 * (true?) for all of the coins of the transaction of the @a tctx.
 * Once we have traced all coins, we build the response.
 *
 * @param tctx track context with established connection to exchange
 */
static void
trace_coins (struct TrackTransactionContext *tctx)
{
  struct TrackCoinContext *tcc;

  /* Make sure we are connected to the exchange. */
  GNUNET_assert (NULL != tctx->eh);

  for (tcc = tctx->tcc_head; NULL != tcc; tcc = tcc->next)

    /* How come one does't have wtid? */
    if (GNUNET_YES != tcc->have_wtid)
      break;

  if (NULL != tcc)
  {
    if (0 != strcmp (tcc->exchange_url,
		     tctx->current_exchange))
    {
      /* exchange changed, find matching one first! */
      tctx->eh = NULL;
      tctx->current_exchange = NULL;
      find_exchange (tctx);
      return;
    }
    /* we are not done requesting WTIDs from the current
       exchange; do the next one */
    tcc->dwh = TALER_EXCHANGE_track_transaction (tctx->eh,
                                                 &tctx->mi->privkey,
                                                 &tctx->h_wire,
                                                 &tctx->h_contract_terms,
                                                 &tcc->coin_pub,
                                                 &wtid_cb,
                                                 tcc);
    return;
  }
  tctx->current_exchange = NULL;
  tctx->eh = NULL;
  generate_response (tctx);
}


/**
 * Function called with the result of our exchange lookup.
 * Merely provide the execution context to the routine actually
 * tracking the coin.
 *
 * @param cls the `struct TrackTransactionContext`
 * @param eh NULL if exchange was not found to be acceptable
 * @param wire_fee NULL (we did not specify a wire method)
 * @param exchange_trusted #GNUNET_YES if this exchange is trusted by config
 */
static void
process_track_transaction_with_exchange (void *cls,
                                         struct TALER_EXCHANGE_Handle *eh,
                                         const struct TALER_Amount *wire_fee,
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
                                          TMH_RESPONSE_make_error (TALER_EC_PAY_EXCHANGE_TIMEOUT,
									    "exchange not reachable"));
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
	     const struct GNUNET_HashCode *h_contract_terms,
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
 * Responsible to get the current coin wtid and store it into its state.
 *
 * @param cls closure
 * @param transaction_id of the contract
 * @param coin_pub public key of the coin
 * @param exchange_url URL of exchange that issued @a coin_pub
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param refund_fee fee the exchange will charge for refunding this coin
 * @param exchange_proof proof from exchange that coin was accepted
 */
static void
coin_cb (void *cls,
         const struct GNUNET_HashCode *h_contract_terms,
         const struct TALER_CoinSpendPublicKeyP *coin_pub,
	 const char *exchange_url,
         const struct TALER_Amount *amount_with_fee,
         const struct TALER_Amount *deposit_fee,
         const struct TALER_Amount *refund_fee,
         const struct TALER_Amount *wire_fee,
         const json_t *exchange_proof)
{
  struct TrackTransactionContext *tctx = cls;
  struct TrackCoinContext *tcc;
  enum GNUNET_DB_QueryStatus qs;

  tcc = GNUNET_new (struct TrackCoinContext);
  tcc->tctx = tctx;
  tcc->coin_pub = *coin_pub;
  tcc->exchange_url = GNUNET_strdup (exchange_url);

  tcc->amount_with_fee = *amount_with_fee;
  tcc->deposit_fee = *deposit_fee;
  GNUNET_CONTAINER_DLL_insert (tctx->tcc_head,
                               tctx->tcc_tail,
                               tcc);

  /* find all those <coin, wtid> pairs associated to
     this contract term's hash code.  The callback
     will then set the wtid for the "current coin"
     context. */
  qs = db->find_transfers_by_hash (db->cls,
				   h_contract_terms,
				   &transfer_cb,
				   tcc);
  if (0 > qs)
  {
    GNUNET_break (0);
    tctx->qs = qs;
  }
}


/**
 * Find the exchange to trace the next coin(s).
 *
 * @param tctx operation context
 */
static void
find_exchange (struct TrackTransactionContext *tctx)
{
  struct TrackCoinContext *tcc = tctx->tcc_head;

  while ( (NULL != tcc) &&
	  (GNUNET_YES == tcc->have_wtid) )
    tcc = tcc->next;
  if (NULL != tcc)
  {
    tctx->current_exchange = tcc->exchange_url;
    tctx->fo = TMH_EXCHANGES_find_exchange (tctx->current_exchange,
					    NULL,
					    &process_track_transaction_with_exchange,
					    tctx);

  }
  else
  {
    generate_response (tctx);
  }
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
  const char *order_id;
  const char *instance;
  int ret;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_HashCode h_instance;
  struct json_t *contract_terms;
  char *last_session_id;

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
  order_id = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "order_id");
  if (NULL == order_id)
    return TMH_RESPONSE_reply_arg_missing (connection,
                                           TALER_EC_PARAMETER_MISSING,
					   "order_id");
  instance = MHD_lookup_connection_value (connection,
                                          MHD_GET_ARGUMENT_KIND,
                                          "instance");
  if (NULL == instance)
    instance = "default";

  GNUNET_CRYPTO_hash (instance,
                      strlen (instance),
                      &h_instance);

  tctx->mi = GNUNET_CONTAINER_multihashmap_get (by_id_map,
                                                &h_instance);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Tracking on behalf of instance '%s'\n",
              instance);

  if (NULL == tctx->mi)
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TRACK_TRANSACTION_INSTANCE_UNKNOWN,
					 "unknown instance");
  
  /* Map order id to contract terms; the objective is to get
     the contract term's hashcode so as to retrieve all the
     coins which have been deposited for it. */
  qs = db->find_contract_terms (db->cls,
				&contract_terms,
                                &last_session_id,
				order_id,
				&tctx->mi->pubkey);
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_TRACK_TRANSACTION_DB_FETCH_TRANSACTION_ERROR,
                                              "Database error finding contract terms");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_PROPOSAL_LOOKUP_NOT_FOUND,
					 "Given order_id doesn't map to any proposal");

  GNUNET_free (last_session_id);

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &tctx->h_contract_terms))
  {
    json_decref (contract_terms);
    return TMH_RESPONSE_reply_internal_error (connection,
					      TALER_EC_INTERNAL_LOGIC_ERROR,
                                              "Failed to hash contract terms");
  }

  {
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                      &tctx->refund_deadline),
      GNUNET_JSON_spec_absolute_time ("timestamp",
                                      &tctx->timestamp),
      TALER_JSON_spec_amount ("amount",
                              &tctx->total_amount),
      GNUNET_JSON_spec_fixed_auto ("H_wire",
                                   &tctx->h_wire),
      GNUNET_JSON_spec_end()
    };

    if (GNUNET_YES !=
        GNUNET_JSON_parse (contract_terms,
                           spec,
                           NULL, NULL))
    {
      GNUNET_break (0);
      GNUNET_JSON_parse_free (spec);
      json_decref (contract_terms);
      return TMH_RESPONSE_reply_internal_error (connection,
                                                TALER_EC_INTERNAL_LOGIC_ERROR,
                                                "Failed to parse contract terms from DB");
    }
    json_decref (contract_terms);
  }

  tctx->qs = GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
  db->preflight (db->cls);

  /* Find coins which have been deposited for this contract,
     and retrieve the wtid for each one. */
  qs = db->find_payments (db->cls,
			  &tctx->h_contract_terms,
			  &tctx->mi->pubkey,
			  &coin_cb,
			  tctx);
  if ( (0 > qs) ||
       (0 > tctx->qs) )
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != qs);
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR != tctx->qs);
    return TMH_RESPONSE_reply_internal_error (connection,
                                              TALER_EC_TRACK_TRANSACTION_DB_FETCH_PAYMENT_ERROR,
					      "Database error: failed to find payment data");
  }
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
  {
    return TMH_RESPONSE_reply_not_found (connection,
					 TALER_EC_TRACK_TRANSACTION_DB_NO_DEPOSITS_ERROR,
                                         "deposit data not found");
  }
  *connection_cls = tctx;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Suspending /track/transaction handling while working with the exchange\n");
  MHD_suspend_connection (connection);
  tctx->timeout_task
    = GNUNET_SCHEDULER_add_delayed (TRACK_TIMEOUT,
				    &handle_track_transaction_timeout,
				    tctx);
  find_exchange (tctx);
  return MHD_YES;
}


/* end of taler-merchant-httpd_track-transaction.c */
