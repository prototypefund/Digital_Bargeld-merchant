/*
  This file is part of TALER
  (C) 2014 Christian Grothoff (and other contributing authors)

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
#include "merchant_db.h"


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
                          "expiry INT8 NOT NULL,"
                          "product INT8 NOT NULL);"
                          "CREATE %1$s TABLE IF NOT EXISTS checkouts ("
                          "coin_pub BYTEA PRIMARY KEY,"
                          "contract_id INT8 REFERENCES contracts(contract_id),"
                          "amount INT4 NOT NULL,"
                          "amount_fraction INT4 NOT NULL,"
                          "coin_sig BYTEA NOT NULL);",
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
                    "(contract_id, hash, amount, amount_fraction, amount_currency,"
		    "description, nounce, expiry, product) VALUES"
                    "($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                    9, NULL)));
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
 * Inserts a contract record into the database and if successfull returns the
 * serial number of the inserted row.
 *
 * @param conn the database connection
 * @param expiry the time when the contract will expire
 * @param amount the taler amount corresponding to the contract
 * @param hash of the stringified JSON corresponding to this contract
 * @param c_id contract's id
 * @param desc descripition of the contract
 * @param nounce a random 64-bit nounce
 * @param product description to identify a product
 * @return GNUNET_OK on success, GNUNET_SYSERR upon error
 */

uint32_t
MERCHANT_DB_contract_create (PGconn *conn,
                             const struct GNUNET_TIME_Absolute *expiry,
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

  /* ported. To be tested/compiled */
  struct TALER_PQ_QueryParam params[] = {
    TALER_PQ_query_param_uint64 (&c_id), 
    TALER_PQ_query_param_fixed_size (h_contract, sizeof (struct GNUNET_HashCode)),
    TALER_PQ_query_param_amount (amount),
    /* a *string* is being put in the following statement,
    though the API talks about a *blob*. Will this be liked by
    the DB ? */
    // the following inserts a string as a blob. Will Taler provide a param-from-string helper?
    TALER_PQ_query_param_fixed_size (desc, strlen (desc)),
    TALER_PQ_query_param_uint64 (&nounce), 
    TALER_PQ_query_param_absolute_time (expiry),
    TALER_PQ_query_param_uint64 (&product),
    TALER_PQ_query_param_end
  };
  
  /* NOTE: the statement is prepared by MERCHANT_DB_initialize function */
  res = TALER_PQ_exec_prepared (conn, "contract_create", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_COMMAND_OK != status);
  PQclear (res);
  return GNUNET_OK;

 EXITIF_exit:
  PQclear (res);
  return GNUNET_SYSERR;
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
/* end of merchant-db.c */
