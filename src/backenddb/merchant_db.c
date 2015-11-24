/*
  This file is part of TALER
  (C) 2014, 2015 Christian Grothoff (and other contributing authors)

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
 * @file merchant/merchant_db.c
 * @brief database helper functions used by the merchant
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_pq_lib.h>
#include "taler_merchantdb_lib.h"

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)



#define PQSQL_strerror(kind, cmd, res)                \
  GNUNET_log_from (kind, "merchant-db",                     \
                   "SQL %s failed at %s:%u with error: %s", \
                   cmd, __FILE__, __LINE__, PQresultErrorMessage (res));

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)


/**
 * Connect to postgresql database
 *
 * @param cfg the configuration handle
 * @return connection to the postgresql database; NULL upon error
 */
PGconn *
MERCHANT_DB_connect (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  return GNUNET_POSTGRES_connect (cfg, "merchant-db");
}


/**
 * Disconnect from the database
 *
 * @param conn database handle to close
 */
void
MERCHANT_DB_disconnect (PGconn *conn)
{
  PQfinish (conn);
}


/**
 * Initialize merchant tables
 *
 * @param conn the connection handle to postgres db.
 * @param tmp GNUNET_YES if the tables are to be made temporary i.e. their
 *          contents are dropped when the @a conn is closed
 * @return GNUNET_OK upon success; GNUNET_SYSERR upon failure
 */
int
MERCHANT_DB_initialize (PGconn *conn, int tmp)
{
  const char *tmp_str = (1 == tmp) ? "TEMPORARY" : "";
  char *sql;
  PGresult *res;
  ExecStatusType status;
  int ret;

  res = NULL;
  (void) GNUNET_asprintf (&sql,
                          "BEGIN TRANSACTION;"
                          "CREATE %1$s TABLE IF NOT EXISTS contracts ("
                          "contract_id INT8 PRIMARY KEY,"
			  "hash BYTEA NOT NULL,"
                          "amount INT8 NOT NULL,"
                          "amount_fraction INT4 NOT NULL,"
			  "amount_currency VARCHAR(" TALER_CURRENCY_LEN_STR ") NOT NULL,"
                          "description TEXT NOT NULL,"
                          "nounce INT8 NOT NULL,"
                          "timestamp INT8 NOT NULL,"
                          "expiry INT8 NOT NULL,"
                          "edate INT8 NOT NULL,"
                          "refund_deadline INT8 NOT NULL,"
                          "product INT8 NOT NULL);"
                          "CREATE %1$s TABLE IF NOT EXISTS checkouts ("
                          "coin_pub BYTEA PRIMARY KEY,"
                          "contract_id INT8 REFERENCES contracts(contract_id),"
                          "amount INT4 NOT NULL,"
                          "amount_fraction INT4 NOT NULL,"
                          "coin_sig BYTEA NOT NULL);"
			  "CREATE %1$s TABLE IF NOT EXISTS deposits ("
			  "dep_perm VARCHAR NOT NULL,"
			  "transaction_id INT8,"
			  "pending INT4 NOT NULL,"
			  "mint_url VARCHAR NOT NULL);",
                          tmp_str);
  ret = GNUNET_POSTGRES_exec (conn, sql);
  (void) GNUNET_POSTGRES_exec (conn,
                              (GNUNET_OK == ret) ? "COMMIT;" : "ROLLBACK;");
  GNUNET_free (sql);
  if (GNUNET_OK != ret)
    return ret;

  while (NULL != (res = PQgetResult (conn)))
  {
    status = PQresultStatus (res);
    PQclear (res);
  }

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "contract_create",
                    "INSERT INTO contracts"
                    "(contract_id, hash, timestamp, expiry, edate,"
		    "refund_deadline, amount, amount_fraction, amount_currency,"
		    "description, nounce, product) VALUES"
                    "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
                    12, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);


  /* Query aimed to get the contract's nounce and edate which will be
  both used for regenerating a 'wire' JSON object to insert into the
  deposit permission. Implicitly, this query will tell whether a contract
  was created or not */

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "get_contract_hash",
                    "SELECT "
                    "nounce, edate "
                    "FROM contracts "
                    "WHERE ("
                    "hash=$1"
                    ")",
                    1, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "get_contract_set",
                    "SELECT "
                    "contract_id, nounce, timestamp, edate, "
                    "refund_deadline FROM contracts "
                    "WHERE ("
                    "hash=$1"
                    ")",
                    1, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "store_deposit_permission",
                    "INSERT INTO deposits"
                    "(dep_perm, transaction_id, pending, mint_url) "
                    "VALUES ($1, $2, $3, $4);", 4, NULL)));

  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);


  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "update_deposit_permission",
                    "UPDATE deposits "
                    "SET pending = $1 "
                    "WHERE transaction_id = $2", 2, NULL)));

  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);


  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "get_contract_product",
                    "SELECT ("
                    "product"
                    ") FROM contracts "
                    "WHERE ("
                    "contract_id=$1"
                    ")",
                    1, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "checkout_create",
                    "INSERT INTO checkouts ("
                    "coin_pub,"
                    "contract_id,"
                    "amount,"
                    "amount_fraction,"
                    "coin_sig"
                    ") VALUES ("
                    "$1, $2, $3, $4, $5"
                    ")",
                    5, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus (res)));
  PQclear (res);

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "get_checkout_product",
                    "SELECT ("
                    "product"
                    ") FROM contracts "
                    "WHERE "
                    "contract_id IN ("
                    "SELECT (contract_id) FROM checkouts "
                    "WHERE coin_pub=$1"
                    ")",
                    1, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus (res)));
  PQclear (res);

  return GNUNET_OK;

 EXITIF_exit:
  if (NULL != res)
  {
    PQSQL_strerror (GNUNET_ERROR_TYPE_ERROR, "PQprepare", res);
    PQclear (res);
  }
  return GNUNET_SYSERR;
}


/**
 * Update the pending column of a deposit permission
 * @param conn handle to DB
 * @param transaction_id identification number of the deposit to
 * update
 * @param pending true if still pending, false otherwise (i.e. the
 * mint did respond something)
 * @return #GNUNET_OK if successful, #GNUNET_SYSERR upon errors
 */
uint32_t
MERCHANT_DB_update_deposit_permission (PGconn *conn,
                                       uint64_t transaction_id,
				       unsigned int pending)
{
  PGresult *res;
  ExecStatusType status;

  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_uint32 (&pending),
    TALER_PQ_query_param_uint64 (&transaction_id),
    TALER_PQ_query_param_end
  };

  res = TALER_PQ_exec_prepared (conn, "update_deposit_permission", params);
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
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Database commit failure: %s\n",
                sqlstate);
    PQclear (res);
    return GNUNET_SYSERR;
  }
}


/**
 * Store a deposit permission in DB. To be mainly used if /deposit should
 * be retried; also, the merchant can benefit from this information in case
 * he needs to later investigate about some @a transaction_id.
 *
 * @param conn DB handle
 * @param transaction_id identification number of this payment (which is the
 * same id of the related contract)
 * @param pending if true, this payment got to a persistent state
 * @param which mint is to get this deposit permission
 * @return #GNUNET_OK if successful, #GNUNET_SYSERR upon errors
 */
uint32_t
MERCHANT_DB_store_deposit_permission (PGconn *conn,
                                      const char *deposit_permission,
				      uint64_t transaction_id,
				      unsigned int pending,
				      const char *mint_url)
{
  PGresult *res;
  ExecStatusType status;
  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_fixed_size (deposit_permission, strlen (deposit_permission)),
    TALER_PQ_query_param_uint64 (&transaction_id),
    TALER_PQ_query_param_uint32 (&pending),
    TALER_PQ_query_param_fixed_size (mint_url, strlen (mint_url)),
    TALER_PQ_query_param_end
  };

  res = TALER_PQ_exec_prepared (conn, "store_deposit_permission", params);
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
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
		  "Inserting same transaction id twice\n");
      /* Primary key violation */
      PQclear (res);
      return GNUNET_SYSERR;
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
 * Insert a contract record into the database and if successfull
 * return the serial number of the inserted row.
 *
 * @param conn the database connection
 * @param timestamp the timestamp of this contract
 * @param expiry the time when the contract will expire
 * @param edate when the merchant wants to receive the wire transfer
 * corresponding to this deal (this value is also a field inside the
 * 'wire' JSON format)
 * @param refund deadline until which the merchant can return the paid
 * amount
 * @param amount the taler amount corresponding to the contract
 * @param hash of the stringified JSON corresponding to this contract
 * @param c_id contract's id
 * @param desc descripition of the contract
 * @param nounce a random 64-bit nounce
 * @param product description to identify a product
 * @return #GNUNET_OK on success, #GNUNET_NO if attempting to insert an
 * already inserted @a c_id, #GNUNET_SYSERR for other errors.
 */
uint32_t
MERCHANT_DB_contract_create (PGconn *conn,
                             const struct GNUNET_TIME_Absolute timestamp,
                             const struct GNUNET_TIME_Absolute expiry,
			     struct GNUNET_TIME_Absolute edate,
			     struct GNUNET_TIME_Absolute refund,
                             const struct TALER_Amount *amount,
			     const struct GNUNET_HashCode *h_contract,
			     uint64_t c_id,
                             const char *desc,
                             uint64_t nounce,
                             uint64_t product)
{
  PGresult *res;
#if 0
  uint64_t expiry_ms_nbo;
  uint64_t value_nbo;
  uint32_t fraction_nbo;
  uint64_t nounce_nbo;
#endif
  ExecStatusType status;

  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_uint64 (&c_id),
    TALER_PQ_query_param_fixed_size (h_contract, sizeof (struct GNUNET_HashCode)),
    TALER_PQ_query_param_absolute_time (&timestamp),
    TALER_PQ_query_param_absolute_time (&expiry),
    TALER_PQ_query_param_absolute_time (&edate),
    TALER_PQ_query_param_absolute_time (&refund),
    TALER_PQ_query_param_amount (amount),
    /* A *string* is being put in the following statement,
    though the column is declared as *blob*. Will this be
    liked by the DB ? */
    TALER_PQ_query_param_fixed_size (desc, strlen (desc)),
    TALER_PQ_query_param_uint64 (&nounce),
    TALER_PQ_query_param_uint64 (&product),
    TALER_PQ_query_param_end
  };

  /* NOTE: the statement is prepared by MERCHANT_DB_initialize function */
  res = TALER_PQ_exec_prepared (conn, "contract_create", params);
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


long long
MERCHANT_DB_get_contract_product (PGconn *conn,
                                  uint64_t contract_id)
{
  PGresult *res;
  uint64_t product;
  ExecStatusType status;
  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_uint64 (&contract_id),
    TALER_PQ_query_param_end
  };
  struct TALER_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_uint64 ("product", &product),
    TALER_PQ_result_spec_end
  };

  contract_id = GNUNET_htonll (contract_id);
  res = TALER_PQ_exec_prepared (conn, "get_contract_product", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_PQ_extract_result (res, rs, 0));
  PQclear (res);
  return GNUNET_ntohll ((uint64_t) product);

 EXITIF_exit:
  PQclear (res);
  return -1;
}


unsigned int
MERCHANT_DB_checkout_create (PGconn *conn,
                             struct GNUNET_CRYPTO_rsa_PublicKey *coin_pub,
                             uint64_t transaction_id,
                             struct TALER_Amount *amount,
                             struct GNUNET_CRYPTO_rsa_Signature *coin_sig)
{
  PGresult *res;
  ExecStatusType status;
  uint32_t value_nbo;
  uint32_t fraction_nbo;
  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_rsa_public_key (coin_pub),
    TALER_PQ_query_param_uint64 (&transaction_id),
    TALER_PQ_query_param_uint32 (&value_nbo),
    TALER_PQ_query_param_uint32 (&fraction_nbo),
    TALER_PQ_query_param_rsa_signature (coin_sig),
    TALER_PQ_query_param_end
  };

  transaction_id = GNUNET_htonll (transaction_id);
  value_nbo = htonl (amount->value);
  fraction_nbo = htonl (amount->fraction);
  res = TALER_PQ_exec_prepared (conn, "checkout_create", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_COMMAND_OK != status);
  PQclear (res);
  return GNUNET_OK;

 EXITIF_exit:
  PQclear (res);
  return GNUNET_SYSERR;
}


long long
MERCHANT_DB_get_checkout_product (PGconn *conn,
                                  struct GNUNET_CRYPTO_rsa_PublicKey *coin_pub)
{
  PGresult *res;
  ExecStatusType status;
  uint64_t product;
  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_rsa_public_key (coin_pub),
    TALER_PQ_query_param_end
  };
  struct TALER_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_uint64 ("product", &product),
    TALER_PQ_result_spec_end
  };

  product = -1;
  res = TALER_PQ_exec_prepared (conn, "get_checkout_product", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  if (0 == PQntuples (res))
  {
    TALER_LOG_DEBUG ("Checkout not found for given coin");
    goto EXITIF_exit;
  }
  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_PQ_extract_result (res, rs, 0));
  PQclear (res);
  return GNUNET_ntohll ((uint64_t) product);

 EXITIF_exit:
  PQclear (res);
  return -1;
}


/**
 * The query gets a contract's nounce and edate used to reproduce
 * a 'wire' JSON object. This function is also useful to check whether
 * a claimed contract existed or not.
 *
 * @param conn handle to the DB
 * @param h_contract the parameter for the row to match against
 * @param nounce where to store the found nounce
 * @param edate where to store the found edate
 * @return #GNUNET_OK on success, #GNUNET_SYSERR upon errors
 */
uint32_t
MERCHANT_DB_get_contract_values (PGconn *conn,
                                 const struct GNUNET_HashCode *h_contract,
                                 uint64_t *nounce,
				 struct GNUNET_TIME_Absolute *edate)
{
  PGresult *res;
  ExecStatusType status;

  struct TALER_PQ_QueryParam params[] = {
      TALER_PQ_query_param_fixed_size (h_contract, sizeof (struct GNUNET_HashCode)),
      TALER_PQ_query_param_end
  };
  struct TALER_PQ_ResultSpec rs[] = {
   TALER_PQ_result_spec_uint64 ("nounce", nounce),
   TALER_PQ_result_spec_absolute_time ("edate", edate),
   TALER_PQ_result_spec_end
  };

  res = TALER_PQ_exec_prepared (conn, "get_contract_hash", params);

  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  if (0 == PQntuples (res))
  {
    TALER_LOG_DEBUG ("Contract not found");
    goto EXITIF_exit;
  }

  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_PQ_extract_result (res, rs, 0));
  PQclear (res);
  return GNUNET_OK;

  EXITIF_exit:
  PQclear (res);
  return GNUNET_SYSERR;
}


/**
 * Get a set of values representing a contract. This function is meant
 * to obsolete the '_get_contract_values' version.
 *
 * @param h_contract the hashcode of this contract
 * @param contract_handle where to store the results
 * @raturn GNUNET_OK in case of success, GNUNET_SYSERR
 * upon errors
 */
uint32_t
MERCHANT_DB_get_contract_handle (PGconn *conn,
                                 const struct GNUNET_HashCode *h_contract,
				 struct MERCHANT_contract_handle *contract_handle)
{
  struct MERCHANT_contract_handle ch;
  PGresult *res;
  ExecStatusType status;

  struct TALER_PQ_QueryParam params[] = {
      TALER_PQ_query_param_fixed_size (h_contract, sizeof (struct GNUNET_HashCode)),
      TALER_PQ_query_param_end
  };

  struct TALER_PQ_ResultSpec rs[] = {
   TALER_PQ_result_spec_uint64 ("nounce", &ch.nounce),
   TALER_PQ_result_spec_absolute_time ("edate", &ch.edate),
   TALER_PQ_result_spec_absolute_time ("timestamp", &ch.timestamp),
   TALER_PQ_result_spec_absolute_time ("refund_deadline", &ch.refund_deadline),
   TALER_PQ_result_spec_uint64 ("contract_id", &ch.contract_id),
   TALER_PQ_result_spec_end
  };

  res = TALER_PQ_exec_prepared (conn, "get_contract_set", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  if (0 == PQntuples (res))
  {
    TALER_LOG_DEBUG ("Contract not found");
    goto EXITIF_exit;
  }

  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_PQ_extract_result (res, rs, 0));
  *contract_handle = ch;
  PQclear (res);
  return GNUNET_OK;

  EXITIF_exit:
  PQclear (res);
  return GNUNET_SYSERR;
}
