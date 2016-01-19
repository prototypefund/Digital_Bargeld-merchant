/*
  This file is part of TALER
  (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/plugin_merchantdb_postgres.c
 * @brief database helper functions for postgres used by the merchant
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_postgres_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_pq_lib.h>
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


#define PQSQL_strerror(kind, cmd, res)                \
  GNUNET_log_from (kind, "merchant-db",                     \
                   "SQL %s failed at %s:%u with error: %s", \
                   cmd, __FILE__, __LINE__, PQresultErrorMessage (res));


/**
 * Initialize merchant tables
 *
 * @param cls closure our `struct Plugin`
 * @param tmp #GNUNET_YES if the tables are to be made temporary i.e. their
 *          contents are dropped when the database connection is closed
 * @return #GNUNET_OK upon success; #GNUNET_SYSERR upon failure
 */
static int
postgres_initialize (void *cls,
                     int tmp)
{
  struct PostgresClosure *pg = cls;
  const char *tmp_str = (1 == tmp) ? "TEMPORARY" : "";
  char *sql;
  PGresult *res;
  ExecStatusType status;
  int ret;

  GNUNET_asprintf (&sql,
                   "CREATE %1$s TABLE IF NOT EXISTS payments ("
                   "h_contract BYTEA NOT NULL,"
                   "h_wire BYTEA NOT NULL,"
                   "transaction_id INT8," /*WARNING: this column used to be primary key, but that wrong since multiple coins belong to the same id*/
                   "timestamp INT8 NOT NULL,"
                   "refund_deadline INT8 NOT NULL,"
                   "amount_without_fee_val INT8 NOT NULL,"
                   "amount_without_fee_frac INT4 NOT NULL,"
                   "amount_without_fee_curr VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL,"
                   "coin_pub BYTEA NOT NULL,"
                   "mint_proof BYTEA NOT NULL);",
                   tmp_str);
  ret = GNUNET_POSTGRES_exec (pg->conn,
                              sql);
  GNUNET_free (sql);
  if (GNUNET_OK != ret)
    return ret;
  if ( (NULL == (res = PQprepare (pg->conn,
                                  "insert_payment",
                                  "INSERT INTO payments"
                                  "(h_contract"
                                  ",h_wire"
                                  ",transaction_id"
                                  ",timestamp"
                                  ",refund_deadline"
                                  ",amount_without_fee_val"
                                  ",amount_without_fee_frac"
                                  ",amount_without_fee_curr"
                                  ",coin_pub"
                                  ",mint_proof) VALUES "
                                  "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                                  10, NULL))) ||
       (PGRES_COMMAND_OK != (status = PQresultStatus(res))) )
  {
    if (NULL != res)
    {
      PQSQL_strerror (GNUNET_ERROR_TYPE_ERROR, "PQprepare", res);
      PQclear (res);
    }
    return GNUNET_SYSERR;
  }
  if ( (NULL == (res = PQprepare (pg->conn,
                                  "check_payment",
                                  "SELECT * "
				  "FROM payments "
				  "WHERE transaction_id=$1",
                                  1, NULL))) ||
       (PGRES_COMMAND_OK != (status = PQresultStatus(res))) )
  {
    if (NULL != res)
    {
      PQSQL_strerror (GNUNET_ERROR_TYPE_ERROR, "PQprepare", res);
      PQclear (res);
    }
    return GNUNET_SYSERR;
  }

  PQclear (res);
  return GNUNET_OK;
}


/**
 * Insert payment confirmation from the mint into the database.
 *
 * @param cls our plugin handle
 * @param h_contract hash of the contract
 * @param h_wire hash of our wire details
 * @param transaction_id of the contract
 * @param timestamp time of the confirmation
 * @param refund refund deadline
 * @param amount_without_fee amount the mint will deposit
 * @param coin_pub public key of the coin
 * @param mint_proof proof from the mint that coin was accepted
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon error
 */
static int
postgres_store_payment (void *cls,
                        const struct GNUNET_HashCode *h_contract,
                        const struct GNUNET_HashCode *h_wire,
                        uint64_t transaction_id,
                        struct GNUNET_TIME_Absolute timestamp,
                        struct GNUNET_TIME_Absolute refund,
                        const struct TALER_Amount *amount_without_fee,
                        const struct TALER_CoinSpendPublicKeyP *coin_pub,
                        json_t *mint_proof)
{
  struct PostgresClosure *pg = cls;
  PGresult *res;
  ExecStatusType status;

  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_auto_from_type (h_contract),
    TALER_PQ_query_param_auto_from_type (h_wire),
    TALER_PQ_query_param_uint64 (&transaction_id),
    TALER_PQ_query_param_absolute_time (&timestamp),
    TALER_PQ_query_param_absolute_time (&refund),
    TALER_PQ_query_param_amount (amount_without_fee),
    TALER_PQ_query_param_auto_from_type (coin_pub),
    TALER_PQ_query_param_json (mint_proof),
    TALER_PQ_query_param_end
  };

  res = TALER_PQ_exec_prepared (pg->conn,
                                "insert_payment",
                                params);
  status = PQresultStatus (res);

  if (PGRES_COMMAND_OK != status)
  {
    const char *sqlstate;

    sqlstate = PQresultErrorField (res, PG_DIAG_SQLSTATE);
    if (NULL == sqlstate)
    {
      /* very unexpected... */
      GNUNET_break (0);
      PQclear (res);
      return GNUNET_SYSERR;
    }
    /* 40P01: deadlock, 40001: serialization failure */
    if ( (0 == strcmp (sqlstate,
                       "23505")))
    {
      /* Primary key violation */
      PQclear (res);
      return GNUNET_NO;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Database commit failure: %s\n",
                sqlstate);
    PQclear (res);
    return GNUNET_SYSERR;
  }

  PQclear (res);
  return GNUNET_OK;
}

/**
 * Check whether a payment has already been stored
 *
 * @param cls our plugin handle
 * @param transaction_id the transaction id to search into
 * the db
 *
 * @return GNUNET_OK if found, GNUNET_NO if not, GNUNET_SYSERR
 * upon error
 */
static int
postgres_check_payment(void *cls,
                       uint64_t transaction_id)
{
  struct PostgresClosure *pg = cls;
  PGresult *res;
  ExecStatusType status;

  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_uint64 (&transaction_id),
    TALER_PQ_query_param_end
  };
  res = TALER_PQ_exec_prepared (pg->conn,
                                "check_payment",
				params);
  
  status = PQresultStatus (res);
  if (PGRES_TUPLES_OK != status)
  {
    const char *sqlstate;

    sqlstate = PQresultErrorField (res, PG_DIAG_SQLSTATE);
    if (NULL == sqlstate)
    {
      /* very unexpected... */
      GNUNET_break (0);
      PQclear (res);
      return GNUNET_SYSERR;
    }

    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Could not check if contract %llu is in DB: %s\n",
                transaction_id,
		sqlstate);
    PQclear (res);
    return GNUNET_SYSERR;
  }
  /* count rows */
  if (PQntuples (res) > 0)
    return GNUNET_OK;
  return GNUNET_NO;



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

  pg = GNUNET_new (struct PostgresClosure);
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
  pg->conn = GNUNET_POSTGRES_connect (cfg, "merchantdb-postgres");
  plugin = GNUNET_new (struct TALER_MERCHANTDB_Plugin);
  plugin->cls = pg;
  plugin->initialize = &postgres_initialize;
  plugin->store_payment = &postgres_store_payment;
  plugin->check_payment = &postgres_check_payment;

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
