/*
  This file is part of TALER
  (C) 2014, 2015, 2016 INRIA

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
#include <gnunet/gnunet_postgres_lib.h>
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
 * Extract error code.
 *
 * @param res postgres result object with error details
 */
#define EXTRACT_DB_ERROR(res)                                         \
  PQresultErrorField(res, PG_DIAG_SQLSTATE)


/**
 * Log error from PostGres.
 *
 * @param kind log level to use
 * @param cmd command that failed
 * @param res postgres result object with error details
 */
#define PQSQL_strerror(kind, cmd, res)                                \
  GNUNET_log_from (kind, "merchantdb-postgres",                       \
                   "SQL %s failed at %s:%u with error: %s",           \
                   cmd, __FILE__, __LINE__, PQresultErrorMessage (res));


/**
 * Macro to run @a s SQL statement using #GNUNET_POSTGRES_exec()
 * and return with #GNUNET_SYSERR if the operation fails.
 *
 * @param pg context for running the statement
 * @param s SQL statement to run
 */
#define PG_EXEC(pg,s) do {                                            \
    if (GNUNET_OK != GNUNET_POSTGRES_exec (pg->conn, s))              \
    {                                                                 \
      GNUNET_break (0);                                               \
      return GNUNET_SYSERR;                                           \
    }                                                                 \
  } while (0)


/**
 * Macro to run @a s SQL statement using #GNUNET_POSTGRES_exec().
 * Ignore errors, they happen.
 *
 * @param pg context for running the statement
 * @param s SQL statement to run
 */
#define PG_EXEC_INDEX(pg,s) do {                                        \
    PGresult *result = PQexec (pg->conn, s);                            \
    PQclear (result);                                                   \
  } while (0)


/**
 * Prepare an SQL statement and log errors on failure.
 *
 * @param pg context for running the preparation
 * @param n name of the prepared statement
 * @param s SQL statement to run
 * @param c number of arguments @a s expects
 */
#define PG_PREPARE(pg,n,s,c) do {                                       \
    ExecStatusType status;                                              \
    PGresult *res = PQprepare (pg->conn, n, s, c, NULL);                \
    if ( (NULL == res) ||                                               \
         (PGRES_COMMAND_OK != (status = PQresultStatus (res))) )        \
    {                                                                   \
      if (NULL != res)                                                  \
      {                                                                 \
        PQSQL_strerror (GNUNET_ERROR_TYPE_ERROR, "PQprepare", res);     \
        PQclear (res);                                                  \
      }                                                                 \
      return GNUNET_SYSERR;                                             \
    }                                                                   \
    PQclear (res);                                                      \
  } while (0)


/**
 * Log a really unexpected PQ error.
 *
 * @param result PQ result object of the PQ operation that failed
 */
#define BREAK_DB_ERR(result) do {               \
    GNUNET_break (0);                           \
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,        \
                "Database failure: %s\n",       \
                PQresultErrorMessage (result)); \
  } while (0)


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

  PG_EXEC_INDEX (pg, "DROP TABLE merchant_transfers;");
  PG_EXEC_INDEX (pg, "DROP TABLE merchant_deposits;");
  PG_EXEC_INDEX (pg, "DROP TABLE merchant_transactions;");
  PG_EXEC_INDEX (pg, "DROP TABLE merchant_proofs;");
  PG_EXEC_INDEX (pg, "DROP TABLE merchant_proposal_data;");
  return GNUNET_OK;
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

  /* Setup tables */
  PG_EXEC (pg,
           "CREATE TABLE IF NOT EXISTS merchant_proposal_data ("
           "order_id VARCHAR NOT NULL"
           ",merchant_pub BYTEA NOT NULL"
           ",proposal_data BYTEA NOT NULL"
           ",h_proposal_data BYTEA NOT NULL"
           ",timestamp INT8 NOT NULL"
           ",row_id BIGSERIAL"
	   ",PRIMARY KEY (order_id, merchant_pub)"
           ");");

  PG_EXEC (pg,
           "CREATE TABLE IF NOT EXISTS merchant_transactions ("
           " h_proposal_data BYTEA NOT NULL"
           ",exchange_uri VARCHAR NOT NULL"
	   ",merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)"
           ",h_wire BYTEA NOT NULL CHECK (LENGTH(h_wire)=64)"
           ",timestamp INT8 NOT NULL"
           ",refund_deadline INT8 NOT NULL"
           ",total_amount_val INT8 NOT NULL"
           ",total_amount_frac INT4 NOT NULL"
           ",total_amount_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
	   ",PRIMARY KEY (h_proposal_data, merchant_pub)"
           ");");
  PG_EXEC (pg,
           "CREATE TABLE IF NOT EXISTS merchant_deposits ("
	   " h_proposal_data BYTEA NOT NULL"
	   ",merchant_pub BYTEA NOT NULL CHECK (LENGTH(merchant_pub)=32)"
	   ",FOREIGN KEY (h_proposal_data, merchant_pub) REFERENCES merchant_transactions (h_proposal_data, merchant_pub)"
           ",coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)"
           ",amount_with_fee_val INT8 NOT NULL"
           ",amount_with_fee_frac INT4 NOT NULL"
           ",amount_with_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
           ",deposit_fee_val INT8 NOT NULL"
           ",deposit_fee_frac INT4 NOT NULL"
           ",deposit_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL"
           ",signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)"
           ",exchange_proof BYTEA NOT NULL"
           ",PRIMARY KEY (h_proposal_data, coin_pub)"
           ");");
  PG_EXEC (pg,
           "CREATE TABLE IF NOT EXISTS merchant_proofs ("
           " exchange_uri VARCHAR NOT NULL"
           ",wtid BYTEA CHECK (LENGTH(wtid)=32)"
           ",execution_time INT8 NOT NULL"
           ",signkey_pub BYTEA NOT NULL CHECK (LENGTH(signkey_pub)=32)"
           ",proof BYTEA NOT NULL"
           ",PRIMARY KEY (wtid, exchange_uri)"
           ");");
  /* Note that h_proposal_data + coin_pub may actually be unknown to
     us, e.g. someone else deposits something for us at the exchange.
     Hence those cannot be foreign keys into deposits/transactions! */
  PG_EXEC (pg,
           "CREATE TABLE IF NOT EXISTS merchant_transfers ("
           " h_proposal_data BYTEA NOT NULL"
           ",coin_pub BYTEA NOT NULL CHECK (LENGTH(coin_pub)=32)"
           ",wtid BYTEA NOT NULL CHECK (LENGTH(wtid)=32)"
           ",PRIMARY KEY (h_proposal_data, coin_pub)"
           ");");
  PG_EXEC_INDEX (pg,
                 "CREATE INDEX IF NOT EXISTS merchant_transfers_by_coin"
                 " ON merchant_transfers (h_proposal_data, coin_pub)");
  PG_EXEC_INDEX (pg,
                 "CREATE INDEX IF NOT EXISTS merchant_transfers_by_wtid"
                 " ON merchant_transfers (wtid)");

  /* Setup prepared "INSERT" statements */
  PG_PREPARE (pg,
              "insert_transaction",
              "INSERT INTO merchant_transactions"
              "(h_proposal_data"
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
              9);
  PG_PREPARE (pg,
              "insert_deposit",
              "INSERT INTO merchant_deposits"
              "(h_proposal_data"
	      ",merchant_pub"
              ",coin_pub"
              ",amount_with_fee_val"
              ",amount_with_fee_frac"
              ",amount_with_fee_curr"
              ",deposit_fee_val"
              ",deposit_fee_frac"
              ",deposit_fee_curr"
              ",signkey_pub"
              ",exchange_proof) VALUES "
              "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)",
              11);
  PG_PREPARE (pg,
              "insert_transfer",
              "INSERT INTO merchant_transfers"
              "(h_proposal_data"
              ",coin_pub"
              ",wtid) VALUES "
              "($1, $2, $3)",
              3);
  PG_PREPARE (pg,
              "insert_proof",
              "INSERT INTO merchant_proofs"
              "(exchange_uri"
              ",wtid"
              ",execution_time"
              ",signkey_pub"
              ",proof) VALUES "
              "($1, $2, $3, $4, $5)",
              5);

  PG_PREPARE (pg,
              "insert_proposal_data",
              "INSERT INTO merchant_proposal_data"
              "(order_id"
              ",merchant_pub"
              ",timestamp"
              ",proposal_data"
              ",h_proposal_data)"
              " VALUES "
              "($1, $2, $3, $4, $5)",
              4);

  PG_PREPARE (pg,
              "find_proposal_data_from_hash",
              "SELECT"
              " proposal_data"
              " FROM merchant_proposal_data"
              " WHERE"
              " h_proposal_data=$1"
              " AND merchant_pub=$2",
              2);

  PG_PREPARE (pg,
              "find_proposal_data",
              "SELECT"
              " proposal_data"
              " FROM merchant_proposal_data"
              " WHERE"
              " order_id=$1"
              " AND merchant_pub=$2",
              2);

  PG_PREPARE (pg,
              "find_proposal_data_by_date",
              "SELECT"
              " proposal_data"
              ",order_id"
              ",row_id"
              " FROM merchant_proposal_data"
              " WHERE"
              " timestamp<$1"
              " AND merchant_pub=$2"
              " ORDER BY row_id DESC, timestamp DESC"
              " LIMIT $3",
              3);

  PG_PREPARE (pg,
              "find_proposal_data_by_date_and_range",
              "SELECT"
              " proposal_data"
              ",order_id"
              ",row_id"
              " FROM merchant_proposal_data"
              " WHERE"
              " timestamp<$1"
              " AND merchant_pub=$2"
              " AND row_id<$3"
              " ORDER BY row_id DESC, timestamp DESC"
              " LIMIT $4",
              4);

  PG_PREPARE (pg,
              "find_proposal_data_by_date_and_range_future",
              "SELECT"
              " proposal_data"
              ",order_id"
              ",row_id"
              " FROM merchant_proposal_data"
              " WHERE"
              " timestamp>$1"
              " AND merchant_pub=$2"
              " AND row_id>$3"
              " ORDER BY row_id DESC, timestamp DESC"
              " LIMIT $4",
              4);

  /* Setup prepared "SELECT" statements */
  PG_PREPARE (pg,
              "find_transaction",
              "SELECT"
              " exchange_uri"
              ",h_wire"
              ",timestamp"
              ",refund_deadline"
              ",total_amount_val"
              ",total_amount_frac"
              ",total_amount_curr"
              " FROM merchant_transactions"
              " WHERE h_proposal_data=$1"
	      " AND merchant_pub=$2",
              2);
  PG_PREPARE (pg,
              "find_deposits",
              "SELECT"
              " coin_pub"
              ",amount_with_fee_val"
              ",amount_with_fee_frac"
              ",amount_with_fee_curr"
              ",deposit_fee_val"
              ",deposit_fee_frac"
              ",deposit_fee_curr"
              ",exchange_proof"
              " FROM merchant_deposits"
              " WHERE h_proposal_data=$1"
              " AND merchant_pub=$2",
              2);
  PG_PREPARE (pg,
              "find_deposits_by_hash_and_coin",
              "SELECT"
              " amount_with_fee_val"
              ",amount_with_fee_frac"
              ",amount_with_fee_curr"
              ",deposit_fee_val"
              ",deposit_fee_frac"
              ",deposit_fee_curr"
              ",exchange_proof"
              " FROM merchant_deposits"
              " WHERE h_proposal_data=$1"
              " AND merchant_pub=$2"
              " AND coin_pub=$3",
              3);
  PG_PREPARE (pg,
              "find_transfers_by_hash",
              "SELECT"
              " coin_pub"
              ",wtid"
              ",merchant_proofs.execution_time"
              ",merchant_proofs.proof"
              " FROM merchant_transfers"
              "   JOIN merchant_proofs USING (wtid)"
              " WHERE h_proposal_data=$1",
              1);
  PG_PREPARE (pg,
              "find_deposits_by_wtid",
              "SELECT"
              " merchant_transfers.h_proposal_data"
              ",merchant_transfers.coin_pub"
              ",merchant_deposits.amount_with_fee_val"
              ",merchant_deposits.amount_with_fee_frac"
              ",merchant_deposits.amount_with_fee_curr"
              ",merchant_deposits.deposit_fee_val"
              ",merchant_deposits.deposit_fee_frac"
              ",merchant_deposits.deposit_fee_curr"
              ",merchant_deposits.exchange_proof"
              " FROM merchant_transfers"
              "   JOIN merchant_deposits"
              "     ON (merchant_deposits.h_proposal_data = merchant_transfers.h_proposal_data"
              "       AND"
              "         merchant_deposits.coin_pub = merchant_transfers.coin_pub)"
              " WHERE wtid=$1",
              1);
  PG_PREPARE (pg,
              "find_proof_by_wtid",
              "SELECT"
              " proof"
              " FROM merchant_proofs"
              " WHERE wtid=$1"
              "  AND exchange_uri=$2",
              2);
  return GNUNET_OK;
}

/**
 * Retrieve proposal data given its proposal data's hashcode
 *
 * @param cls closure
 * @param proposal_data where to store the retrieved proposal data
 * @param h_proposal_data proposal data's hashcode that will be used to
 * perform the lookup
 * @return #GNUNET_OK on success, #GNUNET_NO if no proposal is
 * found, #GNUNET_SYSERR upon error
 */
static int
postgres_find_proposal_data_from_hash (void *cls,
                                       json_t **proposal_data,
                                       const struct GNUNET_HashCode *h_proposal_data,
                                       const struct TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_proposal_data_from_hash",
                                    params);
  i = PQntuples (result);
  if (1 < i)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Mupltiple proposal data hash the same hashcode!\n");
    return GNUNET_SYSERR;
  }

  if (0 == i)
    return GNUNET_NO;

  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("proposal_data",
                               proposal_data),
    GNUNET_PQ_result_spec_end
  };
  if (GNUNET_OK !=
      GNUNET_PQ_extract_result (result,
                                rs,
                                0))
  {
    GNUNET_break (0);
    PQclear (result);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Retrieve proposal data given its order id.
 *
 * @param cls closure
 * @param proposal_data where to store the retrieved proposal data
 * @param order id order id used to perform the lookup
 * @return #GNUNET_OK on success, #GNUNET_NO if no proposal is
 * found, #GNUNET_SYSERR upon error
 */
static int
postgres_find_proposal_data (void *cls,
                             json_t **proposal_data,
                             const char *order_id,
                             const struct TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_proposal_data",
                                    params);
  i = PQntuples (result);
  if (1 < i)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Mupltiple proposal data share the same hashcode.\n");
    return GNUNET_SYSERR;
  }

  if (0 == i)
    return GNUNET_NO;

  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("proposal_data",
                               proposal_data),
    GNUNET_PQ_result_spec_end
  };
  if (GNUNET_OK !=
      GNUNET_PQ_extract_result (result,
                                rs,
                                0))
  {
    GNUNET_break (0);
    PQclear (result);
    return GNUNET_SYSERR;
  }

  return GNUNET_OK;
}


/**
 * Insert proposal data and its hashcode into db
 *
 * @param cls closure
 * @param order_id identificator of the proposal being stored
 * @param proposal_data proposal data to store
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
 */
static int
postgres_insert_proposal_data (void *cls,
                               const char *order_id,
                               const struct TALER_MerchantPublicKeyP *merchant_pub,
                               struct GNUNET_TIME_Absolute timestamp,
                               const json_t *proposal_data)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  int ret;
  struct GNUNET_HashCode h_proposal_data;

  if (GNUNET_OK != TALER_JSON_hash (proposal_data,
                                    &h_proposal_data))
    return GNUNET_SYSERR;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_absolute_time (&timestamp),
    TALER_PQ_query_param_json (proposal_data),
    GNUNET_PQ_query_param_auto_from_type (&h_proposal_data),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "insert_proposal_data",
                                    params);

  /**
   * We don't treat a unique_violation (code '23505') error as
   * an actual error, since there is no problem if a frontend tries
   * to store twice the same proposal.  That is especially needed
   * when DB-less frontends perform replayed payments.
   */
  if (PGRES_COMMAND_OK != PQresultStatus (result)
      && (0 != memcmp ("23505",
                       EXTRACT_DB_ERROR (result),
                       5)))
  {
    ret = GNUNET_SYSERR;
    BREAK_DB_ERR (result);
  }
  else
  {
    ret = GNUNET_OK;
  }
  PQclear (result);
  return ret;
}

/**
 * Insert transaction data into the database.
 *
 * @param cls closure
 * @param h_proposal_data hashcode of the proposal data associated with the
 * transaction being stored
 * @param merchant_pub merchant's public key
 * @param exchange_uri URI of the exchange
 * @param h_wire hash of our wire details
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param total_amount total amount we receive for the contract after fees
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
 */
static int
postgres_store_transaction (void *cls,
                            const struct GNUNET_HashCode *h_proposal_data,
			    const struct TALER_MerchantPublicKeyP *merchant_pub,
                            const char *exchange_uri,
                            const struct GNUNET_HashCode *h_wire,
                            struct GNUNET_TIME_Absolute timestamp,
                            struct GNUNET_TIME_Absolute refund,
                            const struct TALER_Amount *total_amount)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  int ret;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_string (exchange_uri),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (h_wire),
    GNUNET_PQ_query_param_absolute_time (&timestamp),
    GNUNET_PQ_query_param_absolute_time (&refund),
    TALER_PQ_query_param_amount (total_amount),
    GNUNET_PQ_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Storing transaction with h_proposal_data '%s'\n",
              GNUNET_h2s (h_proposal_data));

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "insert_transaction",
                                    params);
  if (PGRES_COMMAND_OK != PQresultStatus (result))
  {
    ret = GNUNET_SYSERR;
    BREAK_DB_ERR (result);
  }
  else
  {
    ret = GNUNET_OK;
  }
  PQclear (result);
  return ret;
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
 * @param signkey_pub public key used by the exchange for @a exchange_proof
 * @param exchange_proof proof from exchange that coin was accepted
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
 */
static int
postgres_store_deposit (void *cls,
                        const struct GNUNET_HashCode *h_proposal_data,
                        const struct TALER_MerchantPublicKeyP *merchant_pub,
                        const struct TALER_CoinSpendPublicKeyP *coin_pub,
                        const struct TALER_Amount *amount_with_fee,
                        const struct TALER_Amount *deposit_fee,
                        const struct TALER_ExchangePublicKeyP *signkey_pub,
                        const json_t *exchange_proof)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  int ret;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    TALER_PQ_query_param_amount (amount_with_fee),
    TALER_PQ_query_param_amount (deposit_fee),
    GNUNET_PQ_query_param_auto_from_type (signkey_pub),
    TALER_PQ_query_param_json (exchange_proof),
    GNUNET_PQ_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "storing payment for h_proposal_data '%s'\n",
              GNUNET_h2s (h_proposal_data));
  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "insert_deposit",
                                    params);
  if (PGRES_COMMAND_OK != PQresultStatus (result))
  {
    ret = GNUNET_SYSERR;
    BREAK_DB_ERR (result);
  }
  else
  {
    ret = GNUNET_OK;
  }
  PQclear (result);
  return ret;
}


/**
 * Insert mapping of @a coin_pub and @a h_proposal_data to
 * corresponding @a wtid.
 *
 * @param cls closure
 * @param h_proposal_data hashcode of the proposal data paid by @a coin_pub
 * @param coin_pub public key of the coin
 * @param wtid identifier of the wire transfer in which the exchange
 *             send us the money for the coin deposit
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
 */
static int
postgres_store_coin_to_transfer (void *cls,
                                 const struct GNUNET_HashCode *h_proposal_data,
                                 const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                 const struct TALER_WireTransferIdentifierRawP *wtid)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  int ret;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "insert_transfer",
                                    params);
  if (PGRES_COMMAND_OK != PQresultStatus (result))
  {
    ret = GNUNET_SYSERR;
    BREAK_DB_ERR (result);
  }
  else
  {
    ret = GNUNET_OK;
  }
  PQclear (result);
  return ret;
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
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
 */
static int
postgres_store_transfer_to_proof (void *cls,
                                  const char *exchange_uri,
                                  const struct TALER_WireTransferIdentifierRawP *wtid,
                                  struct GNUNET_TIME_Absolute execution_time,
                                  const struct TALER_ExchangePublicKeyP *signkey_pub,
                                  const json_t *exchange_proof)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  int ret;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (exchange_uri),
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_absolute_time (&execution_time),
    GNUNET_PQ_query_param_auto_from_type (signkey_pub),
    TALER_PQ_query_param_json (exchange_proof),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "insert_proof",
                                    params);
  if (PGRES_COMMAND_OK != PQresultStatus (result))
  {
    ret = GNUNET_SYSERR;
    BREAK_DB_ERR (result);
  }
  else
  {
    ret = GNUNET_OK;
  }
  PQclear (result);
  return ret;
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
 * @return GNUNET_YES, GNUNET_NO, GNUNET_SYSERR according to the
 * query being successful, unsuccessful, or generated errors.
 */
static int
postgres_find_proposal_data_history (void *cls,
                                     const char *order_id,
                                     const struct TALER_MerchantPublicKeyP *merchant_pub,
                                     TALER_MERCHANTDB_ProposalDataCallback cb,
                                     void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;
  json_t *proposal_data;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_proposal_data",
                                    params);
  i = PQntuples (result);
  if (1 < i)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Mupltiple proposal data share the same hashcode.\n");
    return GNUNET_SYSERR;
  }

  if (0 == i)
    return GNUNET_NO;

  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("proposal_data",
                               &proposal_data),
    GNUNET_PQ_result_spec_end
  };
  if (GNUNET_OK !=
      GNUNET_PQ_extract_result (result,
                                rs,
                                0))
  {
    GNUNET_break (0);
    PQclear (result);
    return GNUNET_SYSERR;
  }

  cb (cb_cls,
      order_id,
      0,
      proposal_data);

  PQclear (result);
  return GNUNET_OK;

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
 * usage is to firstly call `find_proposal_data_by_date`, so that you get
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
 * @return numer of found tuples, #GNUNET_SYSERR upon error
 */
static int
postgres_find_proposal_data_by_date_and_range (void *cls,
                                               struct GNUNET_TIME_Absolute date,
                                               const struct TALER_MerchantPublicKeyP *merchant_pub,
                                               unsigned int start,
                                               unsigned int nrows,
                                               unsigned int future,
                                               TALER_MERCHANTDB_ProposalDataCallback cb,
                                               void *cb_cls)
{
  uint64_t s64 = start;
  uint64_t r64 = nrows;
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int n;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_absolute_time (&date),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_uint64 (&s64),
    GNUNET_PQ_query_param_uint64 (&r64),
    GNUNET_PQ_query_param_end
  };

  if (GNUNET_YES == future)
    result = GNUNET_PQ_exec_prepared (pg->conn,
                                      "find_proposal_data_by_date_and_range_future",
                                      params);
  else
    result = GNUNET_PQ_exec_prepared (pg->conn,
                                      "find_proposal_data_by_date_and_range",
                                      params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if ( (0 == (n = PQntuples (result))) ||
       (NULL == cb) )
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "No records found.\n");
    PQclear (result);
    return n;
  }
  for (unsigned int i = 0; i < n; i++)
  {
    char *order_id;
    json_t *proposal_data;
    uint64_t row_id;

    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_string ("order_id",
                                    &order_id),
      TALER_PQ_result_spec_json ("proposal_data",
                                 &proposal_data),
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
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        order_id,
        row_id,
        proposal_data);

    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return n;
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
 * @return numer of found tuples, #GNUNET_SYSERR upon error
 */
static int
postgres_find_proposal_data_by_date (void *cls,
                                     struct GNUNET_TIME_Absolute date,
                                     const struct TALER_MerchantPublicKeyP *merchant_pub,
                                     unsigned int nrows,
                                     TALER_MERCHANTDB_ProposalDataCallback cb,
                                     void *cb_cls)
{

  uint64_t r64 = nrows;
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int n;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_absolute_time (&date),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_uint64 (&r64),
    GNUNET_PQ_query_param_end
  };
  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_proposal_data_by_date",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == (n = PQntuples (result)) || NULL == cb)
  {
    PQclear (result);
    return n;
  }
  for (i = 0; i < n; i++)
  {
    char *order_id;
    json_t *proposal_data;
    uint64_t row_id;

    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_string ("order_id",
                                    &order_id),
      TALER_PQ_result_spec_json ("proposal_data",
                                 &proposal_data),
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
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        order_id,
        row_id,
        proposal_data);

    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return n;
}

/**
 * Find information about a transaction.
 *
 * @param cls our plugin handle
 * @param h_proposal_data value used to perform the lookup
 * @param merchant_pub merchant's public key
 * @param cb function to call with transaction data
 * @param cb_cls closure for @a cb
 * @return #GNUNET_OK if found, #GNUNET_NO if not, #GNUNET_SYSERR
 *         upon error
 */
static int
postgres_find_transaction (void *cls,
                           const struct GNUNET_HashCode *h_proposal_data,
			   const struct TALER_MerchantPublicKeyP *merchant_pub,
                           TALER_MERCHANTDB_TransactionCallback cb,
                           void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finding transaction for h_proposal_data '%s'\n",
              GNUNET_h2s (h_proposal_data));

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_transaction",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == PQntuples (result))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Could NOT find transaction for h_proposal_data '%s'\n",
                GNUNET_h2s (h_proposal_data));

    PQclear (result);
    return GNUNET_NO;
  }
  if (1 != PQntuples (result))
  {
    GNUNET_break (0);
    PQclear (result);
    return GNUNET_SYSERR;
  }

  {
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

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  0))
    {
      GNUNET_break (0);
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
	merchant_pub,
        exchange_uri,
        h_proposal_data,
        &h_wire,
        timestamp,
        refund_deadline,
        &total_amount);
    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return GNUNET_OK;
}


/**
 * Lookup information about coin payments by proposal data hash
 * (and @a merchant_pub)
 *
 * @param cls closure
 * @param h_proposal_data key for the search
 * @param merchant_pub merchant's public key
 * @param cb function to call with payment data
 * @param cb_cls closure for @a cb
 * @return #GNUNET_OK on success, #GNUNET_NO if transaction Id is unknown,
 *         #GNUNET_SYSERR on hard errors
 */
static int
postgres_find_payments (void *cls,
                        const struct GNUNET_HashCode *h_proposal_data,
		        const struct TALER_MerchantPublicKeyP *merchant_pub,
                        TALER_MERCHANTDB_CoinDepositCallback cb,
                        void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "finding payment for h_proposal_data '%s'\n",
              GNUNET_h2s (h_proposal_data));
  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_deposits",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == PQntuples (result))
  {
    PQclear (result);
    return GNUNET_NO;
  }

  for (i=0;i<PQntuples (result);i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    json_t *exchange_proof;

    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("deposit_fee",
                                   &deposit_fee),
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
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        h_proposal_data,
        &coin_pub,
        &amount_with_fee,
        &deposit_fee,
        exchange_proof);
    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return GNUNET_OK;

  GNUNET_break (0);
  return GNUNET_SYSERR;
}


/**
 * Retrieve information about a deposited coin.
 *
 * @param cls closure
 * @param h_proposal_data hashcode of the proposal data paid by @a coin_pub
 * @param merchant_pub merchant's public key.
 * @param coin_pub coin's public key used for the search
 * @param cb function to call with payment data
 * @param cb_cls closure for @a cb
 * @return #GNUNET_OK on success, #GNUNET_NO if transaction Id is unknown,
 *         #GNUNET_SYSERR on hard errors
 */
static int
postgres_find_payments_by_hash_and_coin (void *cls,
                                         const struct GNUNET_HashCode *h_proposal_data,
                                         const struct TALER_MerchantPublicKeyP *merchant_pub,
                                         const struct TALER_CoinSpendPublicKeyP *coin_pub,
                                         TALER_MERCHANTDB_CoinDepositCallback cb,
                                         void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_end
  };

  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_deposits_by_hash_and_coin",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == PQntuples (result))
  {
    PQclear (result);
    return GNUNET_NO;
  }

  for (i=0;i<PQntuples (result);i++)
  {
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    json_t *exchange_proof;

    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("deposit_fee",
                                   &deposit_fee),
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
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        h_proposal_data,
        coin_pub,
        &amount_with_fee,
        &deposit_fee,
        exchange_proof);
    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return GNUNET_OK;

  GNUNET_break (0);
  return GNUNET_SYSERR;
}


/**
 * Lookup information about a transfer by @a h_proposal_data.  Note
 * that in theory there could be multiple wire transfers for a
 * single @a h_proposal_data, as the transaction may have involved
 * multiple coins and the coins may be spread over different wire
 * transfers.
 *
 * @param cls closure
 * @param h_proposal_data key for the search
 * @param cb function to call with transfer data
 * @param cb_cls closure for @a cb
 * @return #GNUNET_OK on success, #GNUNET_NO if transaction Id is unknown,
 *         #GNUNET_SYSERR on hard errors
 */
static int
postgres_find_transfers_by_hash (void *cls,
                                 const struct GNUNET_HashCode *h_proposal_data,
                                 TALER_MERCHANTDB_TransferCallback cb,
                                 void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_proposal_data),
    GNUNET_PQ_query_param_end
  };
  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_transfers_by_hash",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == PQntuples (result))
  {
    PQclear (result);
    return GNUNET_NO;
  }

  for (i=0;i<PQntuples (result);i++)
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
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        h_proposal_data,
        &coin_pub,
        &wtid,
        execution_time,
        proof);
    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return GNUNET_OK;
}


/**
 * Lookup information about a coin deposits by @a wtid.
 *
 * @param cls closure
 * @param wtid wire transfer identifier to find matching transactions for
 * @param cb function to call with payment data
 * @param cb_cls closure for @a cb
 * @return #GNUNET_OK on success, #GNUNET_NO if transaction Id is unknown,
 *         #GNUNET_SYSERR on hard errors
 */
static int
postgres_find_deposits_by_wtid (void *cls,
                                const struct TALER_WireTransferIdentifierRawP *wtid,
                                TALER_MERCHANTDB_CoinDepositCallback cb,
                                void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;
  unsigned int i;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_end
  };
  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_deposits_by_wtid",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == PQntuples (result))
  {
    PQclear (result);
    return GNUNET_NO;
  }

  for (i=0;i<PQntuples (result);i++)
  {
    struct GNUNET_HashCode h_proposal_data;
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    json_t *exchange_proof;

    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("h_proposal_data",
                                            &h_proposal_data),
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_result_spec_amount ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_result_spec_amount ("deposit_fee",
                                   &deposit_fee),
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
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        &h_proposal_data,
        &coin_pub,
        &amount_with_fee,
        &deposit_fee,
        exchange_proof);
    GNUNET_PQ_cleanup_result (rs);
  }
  PQclear (result);
  return GNUNET_OK;
}


/**
 * Lookup proof information about a wire transfer.
 *
 * @param cls closure
 * @param exchange_uri from which exchange are we looking for proof
 * @param wtid wire transfer identifier for the search
 * @param cb function to call with proof data
 * @param cb_cls closure for @a cb
 * @return #GNUNET_OK on success, #GNUNET_NO if transaction Id is unknown,
 *         #GNUNET_SYSERR on hard errors
 */
static int
postgres_find_proof_by_wtid (void *cls,
                             const char *exchange_uri,
                             const struct TALER_WireTransferIdentifierRawP *wtid,
                             TALER_MERCHANTDB_ProofCallback cb,
                             void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  PGresult *result;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_string (exchange_uri),
    GNUNET_PQ_query_param_end
  };
  result = GNUNET_PQ_exec_prepared (pg->conn,
                                    "find_proof_by_wtid",
                                    params);
  if (PGRES_TUPLES_OK != PQresultStatus (result))
  {
    BREAK_DB_ERR (result);
    PQclear (result);
    return GNUNET_SYSERR;
  }
  if (0 == PQntuples (result))
  {
    PQclear (result);
    return GNUNET_NO;
  }
  if (1 != PQntuples (result))
  {
    GNUNET_break (0);
    PQclear (result);
    return GNUNET_SYSERR;
  }

  {
    json_t *proof;

    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_result_spec_json ("proof",
                                 &proof),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  0))
    {
      GNUNET_break (0);
      PQclear (result);
      return GNUNET_SYSERR;
    }
    cb (cb_cls,
        proof);
    GNUNET_PQ_cleanup_result (rs);
  }

  PQclear (result);
  return GNUNET_OK;
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
  pg->conn = GNUNET_POSTGRES_connect (cfg, "merchantdb-postgres");
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
  plugin->find_transaction = &postgres_find_transaction;
  plugin->find_payments_by_hash_and_coin = &postgres_find_payments_by_hash_and_coin;
  plugin->find_payments = &postgres_find_payments;
  plugin->find_transfers_by_hash = &postgres_find_transfers_by_hash;
  plugin->find_deposits_by_wtid = &postgres_find_deposits_by_wtid;
  plugin->find_proof_by_wtid = &postgres_find_proof_by_wtid;
  plugin->insert_proposal_data = &postgres_insert_proposal_data;
  plugin->find_proposal_data = &postgres_find_proposal_data;
  plugin->find_proposal_data_history = &postgres_find_proposal_data_history;
  plugin->find_proposal_data_by_date = &postgres_find_proposal_data_by_date;
  plugin->find_proposal_data_by_date_and_range = &postgres_find_proposal_data_by_date_and_range;
  plugin->find_proposal_data_from_hash = &postgres_find_proposal_data_from_hash;

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
