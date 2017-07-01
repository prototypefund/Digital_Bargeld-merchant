/*
  This file is part of TALER
  (C) 2014, 2015, 2016, 2017 INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/plugin_merchantdb_postgres.c
 * @brief database helper functions for postgres used by the merchant
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_pq_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_pq_lib.h>
#include <taler/taler_json_lib.h>
#include "taler_merchantdb_plugin.h"


/**
 * Type of the "cls" argument given to each of the functions in
 * our API.
 */
struct PostgresClosure
{

  /**
   * Postgres connection handle.
   */
  PGconn *conn;

};


/**
 * Drop merchant tables
 *
 * @param cls closure our `struct Plugin`
 * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
 */
static int
postgres_drop_tables (void *cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_ExecuteStatement es[] = {
    GNUNET_PQ_make_try_execute ("DROP TABLE merchant_transfers;"),
    GNUNET_PQ_make_try_execute ("DROP TABLE merchant_deposits;"),
    GNUNET_PQ_make_try_execute ("DROP TABLE merchant_transactions;"),
    GNUNET_PQ_make_try_execute ("DROP TABLE merchant_proofs;"),
    GNUNET_PQ_make_try_execute ("DROP TABLE merchant_contract_terms;"),
    GNUNET_PQ_make_try_execute ("DROP TABLE merchant_refunds;"),
    GNUNET_PQ_EXECUTE_STATEMENT_END
  };

  return GNUNET_PQ_exec_statements (pg->conn,
                                    es);
}

/**
 * Start a transaction.
 *
 * @param cls the `struct PostgresClosure` with the plugin-specific state
 * @return #GNUNET_OK on success
 */
static int
postgres_start (void *cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  ExecStatusType ex;

  result = PQexec (pg->conn,
                   "START TRANSACTION ISOLATION LEVEL SERIALIZABLE");
  if (PGRES_COMMAND_OK !=
      (ex = PQresultStatus (result)))
  {
    TALER_LOG_ERROR ("Failed to start transaction (%s): %s\n",
                     PQresStatus (ex),
                     PQerrorMessage (pg->conn));
    GNUNET_break (0);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  PQclear (result);
  return GNUNET_OK;
}


/**
 * Roll back the current transaction of a database connection.
 *
 * @param cls the `struct PostgresClosure` with the plugin-specific state
 * @return #GNUNET_OK on success
 */
static void
postgres_rollback (void *cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;

  result = PQexec (pg->conn,
                   "ROLLBACK");
  GNUNET_break (PGRES_COMMAND_OK ==
                PQresultStatus (result));
  PQclear (result);
}


/**
 * Commit the current transaction of a database connection.
 *
 * @param cls the `struct PostgresClosure` with the plugin-specific state
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
postgres_commit (void *cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_end
  };

  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
					     "end_transaction",
					     params);
}


/**
 * Initialize merchant tables
 *
 * @param cls closure our `struct Plugin`
 * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
 */
static int
postgres_initialize (void *cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_ExecuteStatement es[] = {
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS merchant_contract_terms ("
                            "order_id VARCHAR NOT NULL"
                            ",merchant_pub BYTEA NOT NULL"
                            ",contract_terms BYTEA NOT NULL"
                            ",h_contract_terms BYTEA NOT NULL"
                            ",timestamp INT8 NOT NULL"
                            ",row_id BIGSERIAL"
                            ",PRIMARY KEY (order_id, merchant_pub)"
			    ",UNIQUE (h_contract_terms, merchant_pub)"
                            ");"),
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS merchant_refunds ("
                            " rtransaction_id BIGSERIAL"
                            ",merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)"
                            ",h_contract_terms BYTEA NOT NULL"
                            ",coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)"
                            ",reason VARCHAR NOT NULL"
                            ",refund_amount_val INT8 NOT NULL"
                            ",refund_amount_frac INT4 NOT NULL"
                            ",refund_amount_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ",refund_fee_val INT8 NOT NULL"
                            ",refund_fee_frac INT4 NOT NULL"
                            ",refund_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ");"),
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS merchant_transactions ("
                            " h_contract_terms BYTEA NOT NULL"
                            ",exchange_uri VARCHAR NOT NULL"
                            ",merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)"
                            ",h_wire BYTEA NOT NULL CHECK (LENGTH(h_wire)=64)"
                            ",timestamp INT8 NOT NULL"
                            ",refund_deadline INT8 NOT NULL"
                            ",total_amount_val INT8 NOT NULL"
                            ",total_amount_frac INT4 NOT NULL"
                            ",total_amount_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ",PRIMARY KEY (h_contract_terms, merchant_pub)"
                            ");"),
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS merchant_deposits ("
                            " h_contract_terms BYTEA NOT NULL"
                            ",merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)"
                            ",FOREIGN KEY (h_contract_terms, merchant_pub) REFERENCES merchant_transactions (h_contract_terms, merchant_pub)"
                            ",coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)"
                            ",amount_with_fee_val INT8 NOT NULL"
                            ",amount_with_fee_frac INT4 NOT NULL"
                            ",amount_with_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ",deposit_fee_val INT8 NOT NULL"
                            ",deposit_fee_frac INT4 NOT NULL"
                            ",deposit_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ",refund_fee_val INT8 NOT NULL"
                            ",refund_fee_frac INT4 NOT NULL"
                            ",refund_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ",signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)"
                            ",exchange_proof BYTEA NOT NULL"
                            ",PRIMARY KEY (h_contract_terms, coin_pub)"
                            ");"),
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS merchant_proofs ("
                          " exchange_uri VARCHAR NOT NULL"
                          ",wtid BYTEA CHECK (LENGTH(wtid)=32)"
                          ",execution_time INT8 NOT NULL"
                          ",signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)"
                          ",proof BYTEA NOT NULL"
                          ",PRIMARY KEY (wtid, exchange_uri)"
                          ");"),
    /* Note that h_contract_terms + coin_pub may actually be unknown to
       us, e.g. someone else deposits something for us at the exchange.
       Hence those cannot be foreign keys into deposits/transactions! */
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS merchant_transfers ("
                            " h_contract_terms BYTEA NOT NULL"
                            ",coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)"
                            ",wtid BYTEA NOT NULL CHECK (LENGTH(wtid)=32)"
                            ",PRIMARY KEY (h_contract_terms, coin_pub)"
                            ");"),
    GNUNET_PQ_make_try_execute ("CREATE INDEX IF NOT EXISTS merchant_transfers_by_coin"
                                " ON merchant_transfers (h_contract_terms, coin_pub)"),
    GNUNET_PQ_make_try_execute ("CREATE INDEX IF NOT EXISTS merchant_transfers_by_wtid"
                                " ON merchant_transfers (wtid)"),
    GNUNET_PQ_make_execute ("CREATE TABLE IF NOT EXISTS exchange_wire_fees ("
			    " exchange_pub BYTEA NOT NULL CHECK (length(exchange_pub)=32)"
			    ",h_wire_method BYTEA NOT NULL CHECK (length(h_wire_method)=64)"
                            ",wire_fee_val INT8 NOT NULL"
                            ",wire_fee_frac INT4 NOT NULL"
                            ",wire_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
                            ",closing_fee_val INT8 NOT NULL"
                            ",closing_fee_frac INT4 NOT NULL"
                            ",closing_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
			    ",start_date INT8 NOT NULL"
			    ",end_date INT8 NOT NULL"
			    ",exchange_sig BYTEA NOT NULL CHECK (length(exchange_sig)=64)"
			    ",PRIMARY KEY (exchange_pub,h_wire_method,start_date,end_date)"
			    ");"),
    GNUNET_PQ_EXECUTE_STATEMENT_END
  };
  struct GNUNET_PQ_PreparedStatement ps[] = {
    GNUNET_PQ_make_prepare ("insert_transaction",
                            "INSERT INTO merchant_transactions"
                            "(h_contract_terms"
                            ",exchange_uri"
                            ",merchant_pub"
                            ",h_wire"
                            ",timestamp"
                            ",refund_deadline"
                            ",total_amount_val"
                            ",total_amount_frac"
                            ",total_amount_curr"
                            ") VALUES "
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                            9),
    GNUNET_PQ_make_prepare ("insert_deposit",
                            "INSERT INTO merchant_deposits"
                            "(h_contract_terms"
                            ",merchant_pub"
                            ",coin_pub"
                            ",amount_with_fee_val"
                            ",amount_with_fee_frac"
                            ",amount_with_fee_curr"
                            ",deposit_fee_val"
                            ",deposit_fee_frac"
                            ",deposit_fee_curr"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",refund_fee_curr"
                            ",signkey_pub"
                            ",exchange_proof) VALUES "
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)",
                            14),
    GNUNET_PQ_make_prepare ("insert_transfer",
                            "INSERT INTO merchant_transfers"
                            "(h_contract_terms"
                            ",coin_pub"
                            ",wtid) VALUES "
                            "($1, $2, $3)",
                            3),
    GNUNET_PQ_make_prepare ("insert_refund",
                            "INSERT INTO merchant_refunds"
                            "(merchant_pub"
                            ",h_contract_terms"
                            ",coin_pub"
                            ",reason"
                            ",refund_amount_val"
                            ",refund_amount_frac"
                            ",refund_amount_curr"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",refund_fee_curr"
                            ") VALUES"
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                            10),
    GNUNET_PQ_make_prepare ("insert_proof",
                            "INSERT INTO merchant_proofs"
                            "(exchange_uri"
                            ",wtid"
                            ",execution_time"
                            ",signkey_pub"
                            ",proof) VALUES "
                            "($1, $2, $3, $4, $5)",
                            5),
    GNUNET_PQ_make_prepare ("insert_contract_terms",
                            "INSERT INTO merchant_contract_terms"
                            "(order_id"
                            ",merchant_pub"
                            ",timestamp"
                            ",contract_terms"
                            ",h_contract_terms)"
                            " VALUES "
                            "($1, $2, $3, $4, $5)",
                            4),
    GNUNET_PQ_make_prepare ("insert_wire_fee",
                            "INSERT INTO exchange_wire_fees"
                            "(exchange_pub"
                            ",h_wire_method"
                            ",wire_fee_val"
                            ",wire_fee_frac"
                            ",wire_fee_curr"
                            ",closing_fee_val"
                            ",closing_fee_frac"
                            ",closing_fee_curr"
                            ",start_date"
                            ",end_data"
                            ",exchange_sig)"
                            " VALUES "
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)",
                            11),
    GNUNET_PQ_make_prepare ("find_contract_terms_from_hash",
                            "SELECT"
                            " contract_terms"
                            " FROM merchant_contract_terms"
                            " WHERE"
                            " h_contract_terms=$1"
                            " AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("end_transaction",
                            "COMMIT",
                            0),

    /*NOTE: minimal version, to be expanded on a needed basis*/
    GNUNET_PQ_make_prepare ("find_refunds",
                            "SELECT"
                            " refund_amount_val"
                            ",refund_amount_frac"
                            ",refund_amount_curr"
                            " FROM merchant_refunds"
                            " WHERE coin_pub=$1",
                            1),
    GNUNET_PQ_make_prepare ("find_contract_terms",
                            "SELECT"
                            " contract_terms"
                            " FROM merchant_contract_terms"
                            " WHERE"
                            " order_id=$1"
                            " AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_contract_terms_by_date",
                            "SELECT"
                            " contract_terms"
                            ",order_id"
                            ",row_id"
                            " FROM merchant_contract_terms"
                            " WHERE"
                            " timestamp<$1"
                            " AND merchant_pub=$2"
                            " ORDER BY row_id DESC, timestamp DESC"
                            " LIMIT $3",
                            3),
    GNUNET_PQ_make_prepare ("find_refunds_from_contract_terms_hash",
                            "SELECT"
			    " coin_pub"
			    ",rtransaction_id"
			    ",refund_amount_val"
			    ",refund_amount_frac"
			    ",refund_amount_curr"
			    ",refund_fee_val"
			    ",refund_fee_frac"
			    ",refund_fee_curr"
			    ",reason"
			    " FROM merchant_refunds"
                            " WHERE merchant_pub=$1"
                            " AND h_contract_terms=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_contract_terms_by_date_and_range",
                            "SELECT"
                            " contract_terms"
                            ",order_id"
                            ",row_id"
                            " FROM merchant_contract_terms"
                            " WHERE"
                            " timestamp<$1"
                            " AND merchant_pub=$2"
                            " AND row_id<$3"
                            " ORDER BY row_id DESC, timestamp DESC"
                            " LIMIT $4",
                            4),
    GNUNET_PQ_make_prepare ("find_contract_terms_by_date_and_range_future",
                            "SELECT"
                            " contract_terms"
                            ",order_id"
                            ",row_id"
                            " FROM merchant_contract_terms"
                            " WHERE"
                            " timestamp>$1"
                            " AND merchant_pub=$2"
                            " AND row_id>$3"
                            " ORDER BY row_id DESC, timestamp DESC"
                            " LIMIT $4",
                            4),
    GNUNET_PQ_make_prepare ("find_transaction",
                            "SELECT"
                            " exchange_uri"
                            ",h_wire"
                            ",timestamp"
                            ",refund_deadline"
                            ",total_amount_val"
                            ",total_amount_frac"
                            ",total_amount_curr"
                            " FROM merchant_transactions"
                            " WHERE h_contract_terms=$1"
                            " AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_deposits",
                            "SELECT"
                            " coin_pub"
                            ",amount_with_fee_val"
                            ",amount_with_fee_frac"
                            ",amount_with_fee_curr"
                            ",deposit_fee_val"
                            ",deposit_fee_frac"
                            ",deposit_fee_curr"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",refund_fee_curr"
                            ",exchange_proof"
                            " FROM merchant_deposits"
                            " WHERE h_contract_terms=$1"
                            " AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_deposits_by_hash_and_coin",
                            "SELECT"
                            " amount_with_fee_val"
                            ",amount_with_fee_frac"
                            ",amount_with_fee_curr"
                            ",deposit_fee_val"
                            ",deposit_fee_frac"
                            ",deposit_fee_curr"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",refund_fee_curr"
                            ",exchange_proof"
                            " FROM merchant_deposits"
                            " WHERE h_contract_terms=$1"
                            " AND merchant_pub=$2"
                            " AND coin_pub=$3",
                            3),
    GNUNET_PQ_make_prepare ("find_transfers_by_hash",
                            "SELECT"
                            " coin_pub"
                            ",wtid"
                            ",merchant_proofs.execution_time"
                            ",merchant_proofs.proof"
                            " FROM merchant_transfers"
                            "   JOIN merchant_proofs USING (wtid)"
                            " WHERE h_contract_terms=$1",
                            1),
    GNUNET_PQ_make_prepare ("find_deposits_by_wtid",
                            "SELECT"
                            " merchant_transfers.h_contract_terms"
                            ",merchant_transfers.coin_pub"
                            ",merchant_deposits.amount_with_fee_val"
                            ",merchant_deposits.amount_with_fee_frac"
                            ",merchant_deposits.amount_with_fee_curr"
                            ",merchant_deposits.deposit_fee_val"
                            ",merchant_deposits.deposit_fee_frac"
                            ",merchant_deposits.deposit_fee_curr"
                            ",merchant_deposits.refund_fee_val"
                            ",merchant_deposits.refund_fee_frac"
                            ",merchant_deposits.refund_fee_curr"
                            ",merchant_deposits.exchange_proof"
                            " FROM merchant_transfers"
                            "   JOIN merchant_deposits"
                            "     ON (merchant_deposits.h_contract_terms = merchant_transfers.h_contract_terms"
                            "       AND"
                            "         merchant_deposits.coin_pub = merchant_transfers.coin_pub)"
                            " WHERE wtid=$1",
                            1),
    GNUNET_PQ_make_prepare ("find_proof_by_wtid",
                            "SELECT"
                            " proof"
                            " FROM merchant_proofs"
                            " WHERE wtid=$1"
                            "  AND exchange_uri=$2",
                            2),
    GNUNET_PQ_PREPARED_STATEMENT_END
  };

  if (GNUNET_OK !=
      GNUNET_PQ_exec_statements (pg->conn,
                                 es))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  if (GNUNET_OK !=
      GNUNET_PQ_prepare_statements (pg->conn,
                                    ps))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  return GNUNET_OK;
}


/**
 * Retrieve proposal data given its proposal data's hashcode
 *
 * @param cls closure
 * @param contract_terms where to store the retrieved proposal data
 * @param h_contract_terms proposal data's hashcode that will be used to
 * perform the lookup
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_contract_terms_from_hash (void *cls,
                                       json_t **contract_terms,
                                       const struct GNUNET_HashCode *h_contract_terms,
                                       const struct TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("contract_terms",
                               contract_terms),
    GNUNET_PQ_result_spec_end
  };

  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
						   "find_contract_terms_from_hash",
						   params,
						   rs);
}


/**
 * Retrieve proposal data given its order id.
 *
 * @param cls closure
 * @param[out] contract_terms where to store the retrieved contract terms
 * @param order id order id used to perform the lookup
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_contract_terms (void *cls,
                              json_t **contract_terms,
                              const char *order_id,
                              const struct TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("contract_terms",
                               contract_terms),
    GNUNET_PQ_result_spec_end
  };

  *contract_terms = NULL;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finding contract term, order_id: '%s', merchant_pub: '%s'.\n",
              order_id,
              TALER_B2S (merchant_pub));
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
						   "find_contract_terms",
						   params,
						   rs);
}


/**
 * Insert proposal data and its hashcode into db
 *
 * @param cls closure
 * @param order_id identificator of the proposal being stored
 * @param contract_terms proposal data to store
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_contract_terms (void *cls,
                               const char *order_id,
                               const struct TALER_MerchantPublicKeyP *merchant_pub,
                               struct GNUNET_TIME_Absolute timestamp,
                               const json_t *contract_terms)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_HashCode h_contract_terms;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_absolute_time (&timestamp),
    TALER_PQ_query_param_json (contract_terms),
    GNUNET_PQ_query_param_auto_from_type (&h_contract_terms),
    GNUNET_PQ_query_param_end
  };
  
  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
		       &h_contract_terms))
    return GNUNET_SYSERR;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "inserting contract_terms: order_id: %s, merchant_pub: %s, h_contract_terms: %s.\n",
              order_id,
              TALER_B2S (merchant_pub),
              GNUNET_h2s (&h_contract_terms));
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
					     "insert_contract_terms",
					     params);
}


/**
 * Insert transaction data into the database.
 *
 * @param cls closure
 * @param h_contract_terms hashcode of the proposal data associated with the
 * transaction being stored
 * @param merchant_pub merchant's public key
 * @param exchange_uri URI of the exchange
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_store_transaction (void *cls,
                            const struct GNUNET_HashCode *h_contract_terms,
			    const struct TALER_MerchantPublicKeyP *merchant_pub,
                            const char *exchange_uri,
                            const struct GNUNET_HashCode *h_wire,
                            struct GNUNET_TIME_Absolute timestamp,
                            struct GNUNET_TIME_Absolute refund,
                            const struct TALER_Amount *total_amount)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_string (exchange_uri),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (h_wire),
    GNUNET_PQ_query_param_absolute_time (&timestamp),
    GNUNET_PQ_query_param_absolute_time (&refund),
    TALER_PQ_query_param_amount (total_amount),
    GNUNET_PQ_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Storing transaction with h_contract_terms '%s', merchant_pub '%s'.\n",
              GNUNET_h2s (h_contract_terms),
              TALER_B2S (merchant_pub));
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
					     "insert_transaction",
					     params);
}


/**
 * Insert payment confirmation from the exchange into the database.
 *
 * @param cls closure
 * @param order_id identificator of the proposal associated with this revenue
 * @param merchant_pub merchant's public key
 * @param coin_pub public key of the coin
 * @param amount_with_fee amount the exchange will deposit for this coin
 * @param deposit_fee fee the exchange will charge for this coin
 * @param refund_fee fee the exchange will charge for refunding this coin
 * @param signkey_pub public key used by the exchange for @a exchange_proof
 * @param exchange_proof proof from exchange that coin was accepted
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_store_deposit (void *cls,
                        const struct GNUNET_HashCode *h_contract_terms,
                        const struct TALER_MerchantPublicKeyP *merchant_pub,
                        const struct TALER_CoinSpendPublicKeyP *coin_pub,
                        const struct TALER_Amount *amount_with_fee,
                        const struct TALER_Amount *deposit_fee,
                        const struct TALER_Amount *refund_fee,
                        const struct TALER_ExchangePublicKeyP *signkey_pub,
                        const json_t *exchange_proof)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    TALER_PQ_query_param_amount (amount_with_fee),
    TALER_PQ_query_param_amount (deposit_fee),
    TALER_PQ_query_param_amount (refund_fee),
    GNUNET_PQ_query_param_auto_from_type (signkey_pub),
    TALER_PQ_query_param_json (exchange_proof),
    GNUNET_PQ_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "storing payment for h_contract_terms '%s'"
              ", coin_pub: %s, amount_with_fee: %s, merchant_pub: %s\n",
              GNUNET_h2s (h_contract_terms),
              TALER_B2S (coin_pub),
              TALER_amount_to_string (amount_with_fee),
              TALER_B2S (merchant_pub));
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
					     "insert_deposit",
					     params);
}


/**
 * Insert mapping of @a coin_pub and @a h_contract_terms to
 * corresponding @a wtid.
 *
 * @param cls closure
 * @param h_contract_terms hashcode of the proposal data paid by @a coin_pub
 * @param coin_pub public key of the coin
 * @param wtid identifier of the wire transfer in which the exchange
 *             send us the money for the coin deposit
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_store_coin_to_transfer (void *cls,
                                 const struct GNUNET_HashCode *h_contract_terms,
                                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                 const struct TALER_WireTransferIdentifierRawP *wtid)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_end
  };

  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
					     "insert_transfer",
					     params);
}


/**
 * Insert wire transfer confirmation from the exchange into the database.
 *
 * @param cls closure
 * @param exchange_uri URI of the exchange
 * @param wtid identifier of the wire transfer
 * @param execution_time when was @a wtid executed
 * @param signkey_pub public key used by the exchange for @a exchange_proof
 * @param exchange_proof proof from exchange about what the deposit was for
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_store_transfer_to_proof (void *cls,
                                  const char *exchange_uri,
                                  const struct TALER_WireTransferIdentifierRawP *wtid,
                                  struct GNUNET_TIME_Absolute execution_time,
                                  const struct TALER_ExchangePublicKeyP *signkey_pub,
                                  const json_t *exchange_proof)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (exchange_uri),
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_absolute_time (&execution_time),
    GNUNET_PQ_query_param_auto_from_type (signkey_pub),
    TALER_PQ_query_param_json (exchange_proof),
    GNUNET_PQ_query_param_end
  };

  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
					     "insert_proof",
					     params);
}


/**
 * Lookup for a proposal, respecting the signature used by the
 * /history's db methods.
 *
 * @param cls db plugin handle
 * @param order_id order id used to search for the proposal data
 * @param merchant_pub public key of the merchant using this method
 * @param cb the callback
 * @param cb_cls closure to pass to the callback
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_contract_terms_history (void *cls,
                                     const char *order_id,
                                     const struct TALER_MerchantPublicKeyP *merchant_pub,
                                     TALER_MERCHANTDB_ProposalDataCallback cb,
                                     void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  json_t *contract_terms;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("contract_terms",
                               &contract_terms),
    GNUNET_PQ_result_spec_end
  };

  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
						 "find_contract_terms",
						 params,
						 rs);
  if (qs <= 0)
    return qs;
  cb (cb_cls,
      order_id,
      0,
      contract_terms);
  GNUNET_PQ_cleanup_result (rs);
  return qs;
}


/**
 * Closure for #find_contracts_cb().
 */
struct FindContractsContext
{
  /**
   * Function to call on each result.
   */
  TALER_MERCHANTDB_ProposalDataCallback cb;

  /**
   * Closure for @e cb.
   */
  void *cb_cls;

  /**
   * Transaction status code to set.
   */ 
  enum GNUNET_DB_QueryStatus qs;
};

  
/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct FindContractsContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
find_contracts_cb (void *cls,
		   PGresult *result,
		   unsigned int num_results)
{
  struct FindContractsContext *fcctx = cls;
  
  for (unsigned int i = 0; i < num_results; i++)
  {
    char *order_id;
    json_t *contract_terms;
    uint64_t row_id;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_string ("order_id",
                                    &order_id),
      TALER_PQ_result_spec_json ("contract_terms",
                                 &contract_terms),
      GNUNET_PQ_result_spec_uint64 ("row_id",
                                    &row_id),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      fcctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    fcctx->qs = i + 1;
    fcctx->cb (fcctx->cb_cls,
	       order_id,
	       row_id,
	       contract_terms);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Return proposals whose timestamp are older than `date`.
 * Among those proposals, only those ones being between the
 * start-th and (start-nrows)-th record are returned.  The rows
 * are sorted having the youngest first.
 *
 * @param cls our plugin handle.
 * @param date only results older than this date are returned.
 * @param merchant_pub instance's public key; only rows related to this
 * instance are returned.
 * @param start only rows with serial id less than start are returned.
 * In other words, you lower `start` to get older records. The tipical
 * usage is to firstly call `find_contract_terms_by_date`, so that you get
 * the `nrows` youngest records. The oldest of those records will tell you
 * from which timestamp and `start` you can query the DB in order to get
 * furtherly older records, and so on. Alternatively, you can use always
 * the same timestamp and just go behind in history by tuning `start`.
 * @param nrows only nrows rows are returned.
 * @param future if set to GNUNET_YES, retrieves rows younger than `date`.
 * This is tipically used to show live updates on the merchant's backoffice
 * Web interface.
 * @param cb function to call with transaction data, can be NULL.
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_contract_terms_by_date_and_range (void *cls,
						struct GNUNET_TIME_Absolute date,
						const struct TALER_MerchantPublicKeyP *merchant_pub,
						uint64_t start,
						uint64_t nrows,
						int future,
						TALER_MERCHANTDB_ProposalDataCallback cb,
						void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_absolute_time (&date),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_uint64 (&start),
    GNUNET_PQ_query_param_uint64 (&nrows),
    GNUNET_PQ_query_param_end
  };
  const char *stmt;
  enum GNUNET_DB_QueryStatus qs;
  struct FindContractsContext fcctx = {
    .cb = cb,
    .cb_cls = cb_cls
  };

  if (GNUNET_YES == future)
    stmt = "find_contract_terms_by_date_and_range_future";
  else
    stmt = "find_contract_terms_by_date_and_range";
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     stmt,
					     params,
					     &find_contracts_cb,
					     &fcctx);
  if (0 >= qs)
    return qs;
  return fcctx.qs;
}


/**
 * Return proposals whose timestamp are older than `date`.
 * The rows are sorted having the youngest first.
 *
 * @param cls our plugin handle.
 * @param date only results older than this date are returned.
 * @param merchant_pub instance's public key; only rows related to this
 * instance are returned.
 * @param nrows at most nrows rows are returned.
 * @param cb function to call with transaction data, can be NULL.
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_contract_terms_by_date (void *cls,
				      struct GNUNET_TIME_Absolute date,
				      const struct TALER_MerchantPublicKeyP *merchant_pub,
				      uint64_t nrows,
				      TALER_MERCHANTDB_ProposalDataCallback cb,
				      void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_absolute_time (&date),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_uint64 (&nrows),
    GNUNET_PQ_query_param_end
  };
  enum GNUNET_DB_QueryStatus qs;
  struct FindContractsContext fcctx = {
    .cb = cb,
    .cb_cls = cb_cls
  };

  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_contract_terms_by_date",
					     params,
					     &find_contracts_cb,
					     &fcctx);
  if (0 >= qs)
    return qs;
  return fcctx.qs;
}


/**
 * Find information about a transaction.
 *
 * @param cls our plugin handle
 * @param h_contract_terms value used to perform the lookup
 * @param merchant_pub merchant's public key
 * @param cb function to call with transaction data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_transaction (void *cls,
                           const struct GNUNET_HashCode *h_contract_terms,
			   const struct TALER_MerchantPublicKeyP *merchant_pub,
                           TALER_MERCHANTDB_TransactionCallback cb,
                           void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  char *exchange_uri;
  struct GNUNET_HashCode h_wire;
  struct GNUNET_TIME_Absolute timestamp;
  struct GNUNET_TIME_Absolute refund_deadline;
  struct TALER_Amount total_amount;
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_string ("exchange_uri",
				  &exchange_uri),
    GNUNET_PQ_result_spec_auto_from_type ("h_wire",
					  &h_wire),
    GNUNET_PQ_result_spec_absolute_time ("timestamp",
					 &timestamp),
    GNUNET_PQ_result_spec_absolute_time ("refund_deadline",
					 &refund_deadline),
    TALER_PQ_result_spec_amount ("total_amount",
				 &total_amount),
    GNUNET_PQ_result_spec_end
  };
  enum GNUNET_DB_QueryStatus qs;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finding transaction for h_contract_terms '%s', merchant_pub: '%s'.\n",
              GNUNET_h2s (h_contract_terms),
              TALER_B2S (merchant_pub));

  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
						 "find_transaction",
						 params,
						 rs);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    cb (cb_cls,
	merchant_pub,
        exchange_uri,
        h_contract_terms,
        &h_wire,
        timestamp,
        refund_deadline,
        &total_amount);
    GNUNET_PQ_cleanup_result (rs);
  }
  return qs;
}


/**
 * Closure for #find_payments_cb().
 */
struct FindPaymentsContext
{
  /**
   * Function to call with results.
   */
  TALER_MERCHANTDB_CoinDepositCallback cb;
  
  /**
   * Closure for @e cls.
   */
  void *cb_cls;

  /** 
   * Contract term hash used for the search.
   */
  const struct GNUNET_HashCode *h_contract_terms;
  
  /**
   * Transaction status (set).
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct FindPaymentsContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
find_payments_cb (void *cls,
		  PGresult *result,
		  unsigned int num_results)
{
  struct FindPaymentsContext *fpc = cls;
  
  for (unsigned int i=0;i<num_results;i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    struct TALER_Amount refund_fee;
    json_t *exchange_proof;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("deposit_fee",
                                   &deposit_fee),
      TALER_PQ_result_spec_amount ("refund_fee",
                                   &refund_fee),
      TALER_PQ_result_spec_json ("exchange_proof",
                                 &exchange_proof),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      fpc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    fpc->qs = i + 1;
    fpc->cb (fpc->cb_cls,
	     fpc->h_contract_terms,
	     &coin_pub,
	     &amount_with_fee,
	     &deposit_fee,
	     &refund_fee,
	     exchange_proof);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Lookup information about coin payments by proposal data hash
 * (and @a merchant_pub)
 *
 * @param cls closure
 * @param h_contract_terms key for the search
 * @param merchant_pub merchant's public key
 * @param cb function to call with payment data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_payments (void *cls,
                        const struct GNUNET_HashCode *h_contract_terms,
		        const struct TALER_MerchantPublicKeyP *merchant_pub,
                        TALER_MERCHANTDB_CoinDepositCallback cb,
                        void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  struct FindPaymentsContext fpc = {
    .h_contract_terms = h_contract_terms,
    .cb = cb,
    .cb_cls = cb_cls
  };
  enum GNUNET_DB_QueryStatus qs;
  
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finding payment for h_contract_terms '%s'\n",
              GNUNET_h2s (h_contract_terms));
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_deposits",
					     params,
					     &find_payments_cb,
					     &fpc);
  if (qs <= 0)
    return qs;
  return fpc.qs;
}


/**
 * Closure for #find_payments_by_coin_cb().
 */
struct FindPaymentsByCoinContext
{
  /**
   * Function to call with results.
   */
  TALER_MERCHANTDB_CoinDepositCallback cb;
  
  /**
   * Closure for @e cls.
   */
  void *cb_cls;

  /**
   * Coin we are looking for.
   */ 
  const struct TALER_CoinSpendPublicKeyP *coin_pub;

  /**
   * Hash of the contract we are looking for.
   */ 
  const struct GNUNET_HashCode *h_contract_terms;
  
  /**
   * Transaction status (set).
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct FindPaymentsByCoinContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
find_payments_by_coin_cb (void *cls,
			  PGresult *result,
			  unsigned int num_results)
{
  struct FindPaymentsByCoinContext *fpc = cls;
  
  for (unsigned int i=0;i<num_results;i++)
  {
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    struct TALER_Amount refund_fee;
    json_t *exchange_proof;
    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("deposit_fee",
                                   &deposit_fee),
      TALER_PQ_result_spec_amount ("refund_fee",
                                   &refund_fee),
      TALER_PQ_result_spec_json ("exchange_proof",
                                 &exchange_proof),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      fpc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    fpc->qs = i + 1;
    fpc->cb (fpc->cb_cls,
	     fpc->h_contract_terms,
	     fpc->coin_pub,
	     &amount_with_fee,
	     &deposit_fee,
	     &refund_fee,
	     exchange_proof);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Retrieve information about a deposited coin.
 *
 * @param cls closure
 * @param h_contract_terms hashcode of the proposal data paid by @a coin_pub
 * @param merchant_pub merchant's public key.
 * @param coin_pub coin's public key used for the search
 * @param cb function to call with payment data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_payments_by_hash_and_coin (void *cls,
                                         const struct GNUNET_HashCode *h_contract_terms,
                                         const struct TALER_MerchantPublicKeyP *merchant_pub,
                                         const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                         TALER_MERCHANTDB_CoinDepositCallback cb,
                                         void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_end
  };
  struct FindPaymentsByCoinContext fpc = {
    .cb = cb,
    .cb_cls = cb_cls,
    .h_contract_terms = h_contract_terms,
    .coin_pub = coin_pub
  };
  enum GNUNET_DB_QueryStatus qs;

  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_deposits_by_hash_and_coin",
					     params,
					     &find_payments_by_coin_cb,
					     &fpc);
  if (0 >= qs)
    return qs;
  return fpc.qs;
}


/**
 * Closure for #find_transfers_cb().
 */
struct FindTransfersContext
{
  /**
   * Function to call on results.
   */
  TALER_MERCHANTDB_TransferCallback cb;

  /**
   * Closure for @e cb.
   */
  void *cb_cls;

  /** 
   * Hash of the contract we are looking under.
   */
  const struct GNUNET_HashCode *h_contract_terms;
  
  /**
   * Transaction status (set).
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct FindTransfersContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
find_transfers_cb (void *cls,
		   PGresult *result,
		   unsigned int num_results)
{
  struct FindTransfersContext *ftc = cls;

  for (unsigned int i=0;i<PQntuples (result);i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_WireTransferIdentifierRawP wtid;
    struct GNUNET_TIME_Absolute execution_time;
    json_t *proof;

    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      GNUNET_PQ_result_spec_auto_from_type ("wtid",
                                            &wtid),
      GNUNET_PQ_result_spec_absolute_time ("execution_time",
                                           &execution_time),
      TALER_PQ_result_spec_json ("proof",
                                 &proof),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      ftc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    ftc->qs = i + 1;
    ftc->cb (ftc->cb_cls,
	     ftc->h_contract_terms,
	     &coin_pub,
	     &wtid,
	     execution_time,
	     proof);
    GNUNET_PQ_cleanup_result (rs);
  }
}
  

/**
 * Lookup information about a transfer by @a h_contract_terms.  Note
 * that in theory there could be multiple wire transfers for a
 * single @a h_contract_terms, as the transaction may have involved
 * multiple coins and the coins may be spread over different wire
 * transfers.
 *
 * @param cls closure
 * @param h_contract_terms key for the search
 * @param cb function to call with transfer data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_transfers_by_hash (void *cls,
                                 const struct GNUNET_HashCode *h_contract_terms,
                                 TALER_MERCHANTDB_TransferCallback cb,
                                 void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_end
  };
  struct FindTransfersContext ftc = {
    .h_contract_terms = h_contract_terms,
    .cb = cb,
    .cb_cls = cb_cls
  };
  enum GNUNET_DB_QueryStatus qs;
  
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_transfers_by_hash",
					     params,
					     &find_transfers_cb,
					     &ftc);
  if (0 >= qs)
    return qs;
  return ftc.qs;
}


/**
 * Closure for #find_deposits_cb().
 */
struct FindDepositsContext
{

  /**
   * Function to call for each result.
   */ 
  TALER_MERCHANTDB_CoinDepositCallback cb;

  /**
   * Closure for @e cb.
   */
  void *cb_cls;
  
  /**
   * Transaction status (set).
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct FindDepositsContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
find_deposits_cb (void *cls,
		  PGresult *result,
		  unsigned int num_results)
{
  struct FindDepositsContext *fdc = cls;

  for (unsigned int i=0;i<PQntuples (result);i++)
  {
    struct GNUNET_HashCode h_contract_terms;
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    struct TALER_Amount refund_fee;
    json_t *exchange_proof;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("h_contract_terms",
                                            &h_contract_terms),
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("deposit_fee",
                                   &deposit_fee),
      TALER_PQ_result_spec_amount ("refund_fee",
                                   &refund_fee),
      TALER_PQ_result_spec_json ("exchange_proof",
                                 &exchange_proof),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      fdc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    fdc->qs = i + 1;
    fdc->cb (fdc->cb_cls,
	     &h_contract_terms,
	     &coin_pub,
	     &amount_with_fee,
	     &deposit_fee,
	     &refund_fee,
	     exchange_proof);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Lookup information about a coin deposits by @a wtid.
 *
 * @param cls closure
 * @param wtid wire transfer identifier to find matching transactions for
 * @param cb function to call with payment data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_deposits_by_wtid (void *cls,
                                const struct TALER_WireTransferIdentifierRawP *wtid,
                                TALER_MERCHANTDB_CoinDepositCallback cb,
                                void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_end
  };
  struct FindDepositsContext fdc = {
    .cb = cb,
    .cb_cls = cb_cls
  };
  enum GNUNET_DB_QueryStatus qs;

  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_deposits_by_wtid",
					     params,
					     &find_deposits_cb,
					     &fdc);
  if (0 >= qs)
    return qs;
  return fdc.qs;
}


/**
 * Closure for #get_refunds_cb().
 */
struct GetRefundsContext
{
  /**
   * Function to call for each refund.
   */
  TALER_MERCHANTDB_RefundCallback rc;

  /**
   * Closure for @e rc.
   */ 
  void *rc_cls;

  /**
   * Transaction result.
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct GetRefundsContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
get_refunds_cb (void *cls,
		PGresult *result,
		unsigned int num_results)
{
  struct GetRefundsContext *grc = cls;
  
  for (unsigned int i=0;i<num_results;i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    uint64_t rtransaction_id;
    struct TALER_Amount refund_amount;
    struct TALER_Amount refund_fee;
    char *reason;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      GNUNET_PQ_result_spec_uint64 ("rtransaction_id",
                                    &rtransaction_id),
      TALER_PQ_result_spec_amount ("refund_amount",
                                   &refund_amount),
      TALER_PQ_result_spec_amount ("refund_fee",
                                   &refund_fee),
      GNUNET_PQ_result_spec_string ("reason",
                                    &reason),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      grc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    grc->qs = i + 1;
    grc->rc (grc->rc_cls,
	     &coin_pub,
	     rtransaction_id,
	     reason,
	     &refund_amount,
	     &refund_fee);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Obtain refunds associated with a contract.
 *
 * @param cls closure, typically a connection to the db
 * @param merchant_pub public key of the merchant instance
 * @param h_contract_terms hash code of the contract
 * @param rc function to call for each coin on which there is a refund
 * @param rc_cls closure for @a rc
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_get_refunds_from_contract_terms_hash (void *cls,
                                               const struct TALER_MerchantPublicKeyP *merchant_pub,
                                               const struct GNUNET_HashCode *h_contract_terms,
                                               TALER_MERCHANTDB_RefundCallback rc,
                                               void *rc_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_end
  };
  struct GetRefundsContext grc = {
    .rc = rc,
    .rc_cls = rc_cls
  };
  enum GNUNET_DB_QueryStatus qs;

  TALER_LOG_DEBUG ("Looking for refund %s + %s\n",
                   GNUNET_h2s (h_contract_terms),
                   TALER_B2S (merchant_pub));

  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_refunds_from_contract_terms_hash",
					     params,
					     &get_refunds_cb,
					     &grc);
  if (0 >= qs)
    return qs; 
  return grc.qs; 
}


/**
 * Insert a refund row into merchant_refunds.  Not meant to be exported
 * in the db API.
 *
 * @param cls closure, tipically a connection to the db
 * @param merchant_pub merchant instance public key
 * @param h_contract_terms hashcode of the contract related to this refund
 * @param coin_pub public key of the coin giving the (part of) refund
 * @param reason human readable explaination behind the refund
 * @param refund how much this coin is refunding
 * @param refund_fee refund fee for this coin
 */
enum GNUNET_DB_QueryStatus
insert_refund (void *cls,
               const struct TALER_MerchantPublicKeyP *merchant_pub,
               const struct GNUNET_HashCode *h_contract_terms,
               const struct TALER_CoinSpendPublicKeyP *coin_pub,
               const char *reason,
               const struct TALER_Amount *refund,
               const struct TALER_Amount *refund_fee)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_string (reason),
    TALER_PQ_query_param_amount (refund),
    TALER_PQ_query_param_amount (refund_fee),
    GNUNET_PQ_query_param_end
  };

  TALER_LOG_DEBUG ("Inserting refund %s + %s\n",
                   GNUNET_h2s (h_contract_terms),
                   TALER_B2S (merchant_pub));
  
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_refund",
                                             params);
}


/**
 * Store information about wire fees charged by an exchange,
 * including signature (so we have proof).
 *
 * @param cls closure
 * @paramm exchange_pub public key of the exchange
 * @param h_wire_method hash of wire method
 * @param wire_fee wire fee charged
 * @param closing_fee closing fee charged (irrelevant for us,
 *              but needed to check signature)
 * @param start_date start of fee being used
 * @param end_date end of fee being used
 * @param exchange_sig signature of exchange over fee structure
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
postgres_store_wire_fee_by_exchange (void *cls,
				     const struct TALER_MasterPublicKeyP *exchange_pub,
				     const struct GNUNET_HashCode *h_wire_method,
				     const struct TALER_Amount *wire_fee,
				     const struct TALER_Amount *closing_fee,
				     struct GNUNET_TIME_Absolute start_date,
				     struct GNUNET_TIME_Absolute end_date,
				     const struct TALER_MasterSignatureP *exchange_sig)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (exchange_pub),
    GNUNET_PQ_query_param_auto_from_type (h_wire_method),
    TALER_PQ_query_param_amount (wire_fee),
    TALER_PQ_query_param_amount (closing_fee),
    GNUNET_PQ_query_param_absolute_time (&start_date),
    GNUNET_PQ_query_param_absolute_time (&end_date),
    GNUNET_PQ_query_param_auto_from_type (exchange_sig),
    GNUNET_PQ_query_param_end
  };

  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_wire_fee",
                                             params);  
}


/**
 * Closure for #process_refund_cb.
 */
struct FindRefundContext
{
  /**
   * Updated to reflect total amount refunded so far.
   */
  struct TALER_Amount refunded_amount;

  /**
   * Set to #GNUNET_SYSERR on hard errors.
   */
  int err;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls closure, our `struct FindRefundContext`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
process_refund_cb (void *cls,
                   PGresult *result,
                   unsigned int num_results)
{
  struct FindRefundContext *ictx = cls;

  for (unsigned int i=0; i<num_results; i++)
  {
    /* Sum up existing refunds */
    struct TALER_Amount acc;
    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_result_spec_amount ("refund_amount",
                                   &acc),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      ictx->err = GNUNET_SYSERR;
      return;
    }
    if (GNUNET_SYSERR ==
        TALER_amount_add (&ictx->refunded_amount,
                          &ictx->refunded_amount,
                          &acc))
    {
      GNUNET_break (0);
      ictx->err = GNUNET_SYSERR;
      return;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Found refund of %s\n",
                TALER_amount2s (&acc));
  }
}


/**
 * Closure for #process_deposits_cb.
 */
struct InsertRefundContext
{
  /**
   * Used to provide a connection to the db
   */
  struct PostgresClosure *pg;

  /**
   * Amount to which increase the refund for this contract
   */
  const struct TALER_Amount *refund;

  /**
   * Merchant instance public key
   */
  const struct TALER_MerchantPublicKeyP *merchant_pub;

  /**
   * Hash code representing the contract
   */
  const struct GNUNET_HashCode *h_contract_terms;

  /**
   * Human-readable reason behind this refund
   */
  const char *reason;

  /**
   * Transaction status code.
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls closure, our `struct InsertRefundContext`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
process_deposits_for_refund_cb (void *cls,
				PGresult *result,
				unsigned int num_results)
{
  struct InsertRefundContext *ctx = cls;
  struct TALER_Amount previous_refund;

  TALER_amount_get_zero (ctx->refund->currency,
                         &previous_refund);

  for (unsigned int i=0;i<num_results;i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount refund_fee;
    struct FindRefundContext ictx;
    enum GNUNET_DB_QueryStatus ires;
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (&coin_pub),
      GNUNET_PQ_query_param_end
    };
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("refund_fee",
                                   &refund_fee),
      GNUNET_PQ_result_spec_end
    };
    struct TALER_Amount left;
    struct TALER_Amount remaining_refund;
    const struct TALER_Amount *increment;

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Processing refund for coin %s, deposit value was %s\n",
                TALER_B2S (&coin_pub),
                TALER_amount2s (&amount_with_fee));
    TALER_amount_get_zero (amount_with_fee.currency,
                           &ictx.refunded_amount);
    ictx.err = GNUNET_OK; /* no error so far */
    ires = GNUNET_PQ_eval_prepared_multi_select (ctx->pg->conn,
                                                 "find_refunds",
                                                 params,
                                                 &process_refund_cb,
                                                 &ictx);
    if ( (GNUNET_OK != ictx.err) ||
         (GNUNET_DB_STATUS_HARD_ERROR == ires) )
    { 
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    if (GNUNET_DB_STATUS_SOFT_ERROR == ires)
    {
      ctx->qs = GNUNET_DB_STATUS_SOFT_ERROR;
      return;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Processing refund for coin %s total up to %s\n",
                TALER_B2S (&coin_pub),
                TALER_amount2s (&ictx.refunded_amount));
    
    /**
     * Sum coin's contribution for refunding to the total (previously
     * awarded) refund.  If this value will exceed the current awarded
     * refund, we'll return #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS.
     */
    if (GNUNET_SYSERR ==
	TALER_amount_add (&previous_refund,
			  &previous_refund,
			  &ictx.refunded_amount))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    if (0 >= TALER_amount_cmp (ctx->refund,
			       &previous_refund))
    {
      /* refund <= refunded_amount; nothing to do! */ 
      ctx->qs = GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
      return;
    }

    /* How much of the coin is left after the existing refunds? */
    if (GNUNET_SYSERR ==
	TALER_amount_subtract (&left, 
			       &amount_with_fee,
			       &ictx.refunded_amount))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    if ( (0 == left.value) &&
	 (0 == left.fraction) )
    {
      /* coin was fully refunded, move to next coin */
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		  "Coin %s fully refunded, moving to next coin\n",
		  TALER_B2S (&coin_pub));
      continue;
    }

    /* How much of the refund is left? */
    if (GNUNET_SYSERR ==
	TALER_amount_subtract (&remaining_refund, 
			       ctx->refund,
			       &previous_refund))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    /* By how much will we increase the refund for this coin? */
    if (0 >= TALER_amount_cmp (&remaining_refund,
			       &left))
    {
      /* remaining_refund <= left */
      increment = &remaining_refund;
    }
    else
    {
      increment = &left;
    }
    
    if (GNUNET_SYSERR ==
	TALER_amount_add (&previous_refund,
			  &previous_refund,
			  increment))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    
    /* Subtract from refund what has already been awarded */
    if (GNUNET_SYSERR ==
	TALER_amount_subtract (&remaining_refund,
			       ctx->refund,
			       &ictx.refunded_amount))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    /* actually run the refund */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"Coin %s refund will be incremented by %s\n",
		TALER_B2S (&coin_pub),
		TALER_amount2s (increment));
    {
      enum GNUNET_DB_QueryStatus qs;
      
      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
	  (qs = insert_refund (ctx->pg,
			       ctx->merchant_pub,
			       ctx->h_contract_terms,
			       &coin_pub,
			       ctx->reason,
			       increment,
			       &refund_fee)))
      {
	GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
	ctx->qs = qs;
	return;    
      }
      
      /* stop immediately if we are done */
      if (0 == TALER_amount_cmp (ctx->refund,
				 &previous_refund))
	return;
    }
  }

  /**
   * We end up here if nto all of the refund has been covered.
   * Although this should be checked as the business should never
   * issue a refund bigger than the contract's actual price, we cannot
   * rely upon the frontend being correct.
   */
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
	      "The refund of %s is bigger than the order's value\n",
	      TALER_amount2s (ctx->refund));
  ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
}


/**
 * Function called when some backoffice staff decides to award or
 * increase the refund on an existing contract.
 *
 * @param cls closure
 * @param h_contract_terms
 * @param merchant_pub merchant's instance public key
 * @param refund maximum refund to return to the customer for this contract
 * @param reason 0-terminated UTF-8 string giving the reason why the customer
 *               got a refund (free form, business-specific)
 * @return transaction status
 *         #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT if the refund is accepted
 *         #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if the refund cannot be issued: this can happen for two
 *               reasons: the issued refund is not greater of the previous refund,
 *               or the coins don't have enough amount left to pay for this refund.
 */
static enum GNUNET_DB_QueryStatus
postgres_increase_refund_for_contract (void *cls,
                                       const struct GNUNET_HashCode *h_contract_terms,
                                       const struct TALER_MerchantPublicKeyP *merchant_pub,
                                       const struct TALER_Amount *refund,
                                       const char *reason)
{
  struct PostgresClosure *pg = cls;
  struct InsertRefundContext ctx;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };

  if (GNUNET_OK !=
      postgres_start (cls))
  {
    GNUNET_break (0);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }
  ctx.pg = pg;
  ctx.qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  ctx.refund = refund;
  ctx.reason = reason;
  ctx.h_contract_terms = h_contract_terms;
  ctx.merchant_pub = merchant_pub;
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
					     "find_deposits",
					     params,
					     &process_deposits_for_refund_cb,
					     &ctx);
  switch (qs)
  {
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unknown contract: %s (merchant_pub: %s), no refund possible\n",
                GNUNET_h2s (h_contract_terms),
                TALER_B2S (merchant_pub));
    postgres_rollback (cls);
    return qs;
  case GNUNET_DB_STATUS_SOFT_ERROR:
  case GNUNET_DB_STATUS_HARD_ERROR:
    postgres_rollback (cls);
    return qs;
  default:
    /* Got one or more deposits */
    if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT != ctx.qs)
    {
      postgres_rollback (cls);
      return ctx.qs;
    }
    qs = postgres_commit (cls);
    if (0 > qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                  "Failed to commit transaction increasing refund\n");
      return qs;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
		"Committed refund transaction\n");
    return ctx.qs;
  }
}


/**
 * Lookup proof information about a wire transfer.
 *
 * @param cls closure
 * @param exchange_uri from which exchange are we looking for proof
 * @param wtid wire transfer identifier for the search
 * @param cb function to call with proof data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_proof_by_wtid (void *cls,
                             const char *exchange_uri,
                             const struct TALER_WireTransferIdentifierRawP *wtid,
                             TALER_MERCHANTDB_ProofCallback cb,
                             void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_string (exchange_uri),
    GNUNET_PQ_query_param_end
  };
  json_t *proof;
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("proof",
			       &proof),
    GNUNET_PQ_result_spec_end
  };
  enum GNUNET_DB_QueryStatus qs;

  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
						 "find_proof_by_wtid",
						 params,
						 rs);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    cb (cb_cls,
        proof);
    GNUNET_PQ_cleanup_result (rs);
  }
  return qs;
}


/**
 * Initialize Postgres database subsystem.
 *
 * @param cls a configuration instance
 * @return NULL on error, otherwise a `struct TALER_MERCHANTDB_Plugin`
 */
void *
libtaler_plugin_merchantdb_postgres_init (void *cls)
{
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  struct PostgresClosure *pg;
  struct TALER_MERCHANTDB_Plugin *plugin;
  const char *ec;

  pg = GNUNET_new (struct PostgresClosure);
  ec = getenv ("TALER_MERCHANTDB_POSTGRES_CONFIG");
  if (NULL != ec)
  {
    GNUNET_CONFIGURATION_set_value_string (cfg,
                                           "merchantdb-postgres",
                                           "CONFIG",
                                           ec);
  }
  else
  {
    if (GNUNET_OK !=
        GNUNET_CONFIGURATION_have_value (cfg,
                                         "merchantdb-postgres",
                                         "CONFIG"))
    {
      GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                                 "merchantdb-postgres",
                                 "CONFIG");
      return NULL;
    }
  }
  pg->conn = GNUNET_PQ_connect_with_cfg (cfg,
                                         "merchantdb-postgres");
  if (NULL == pg->conn)
  {
    GNUNET_free (pg);
    return NULL;
  }
  plugin = GNUNET_new (struct TALER_MERCHANTDB_Plugin);
  plugin->cls = pg;
  plugin->drop_tables = &postgres_drop_tables;
  plugin->initialize = &postgres_initialize;
  plugin->store_transaction = &postgres_store_transaction;
  plugin->store_deposit = &postgres_store_deposit;
  plugin->store_coin_to_transfer = &postgres_store_coin_to_transfer;
  plugin->store_transfer_to_proof = &postgres_store_transfer_to_proof;
  plugin->store_wire_fee_by_exchange = &postgres_store_wire_fee_by_exchange;
  plugin->find_transaction = &postgres_find_transaction;
  plugin->find_payments_by_hash_and_coin = &postgres_find_payments_by_hash_and_coin;
  plugin->find_payments = &postgres_find_payments;
  plugin->find_transfers_by_hash = &postgres_find_transfers_by_hash;
  plugin->find_deposits_by_wtid = &postgres_find_deposits_by_wtid;
  plugin->find_proof_by_wtid = &postgres_find_proof_by_wtid;
  plugin->insert_contract_terms = &postgres_insert_contract_terms;
  plugin->find_contract_terms = &postgres_find_contract_terms;
  plugin->find_contract_terms_history = &postgres_find_contract_terms_history;
  plugin->find_contract_terms_by_date = &postgres_find_contract_terms_by_date;
  plugin->find_contract_terms_by_date_and_range = &postgres_find_contract_terms_by_date_and_range;
  plugin->find_contract_terms_from_hash = &postgres_find_contract_terms_from_hash;
  plugin->get_refunds_from_contract_terms_hash = &postgres_get_refunds_from_contract_terms_hash;
  plugin->increase_refund_for_contract = postgres_increase_refund_for_contract;

  return plugin;
}


/**
 * Shutdown Postgres database subsystem.
 *
 * @param cls a `struct TALER_MERCHANTDB_Plugin`
 * @return NULL (always)
 */
void *
libtaler_plugin_merchantdb_postgres_done (void *cls)
{
  struct TALER_MERCHANTDB_Plugin *plugin = cls;
  struct PostgresClosure *pg = plugin->cls;

  PQfinish (pg->conn);
  GNUNET_free (pg);
  GNUNET_free (plugin);
  return NULL;
}

/* end of plugin_merchantdb_postgres.c */
