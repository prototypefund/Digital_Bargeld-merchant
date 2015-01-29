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
#include <taler/taler_db_lib.h>
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
 * Initialise merchant tables
 *
 * @param conn the connection handle to postgres db.
 * @param tmp GNUNET_YES if the tables are to be made temporary i.e. their
 *          contents are dropped when the @a conn is closed
 * @return GNUNET_OK upon success; GNUNET_SYSERR upon failure
 */
int
MERCHANT_DB_initialise (PGconn *conn, int tmp)
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
                          "transaction_id SERIAL8 PRIMARY KEY,"
                          "amount INT4 NOT NULL,"
                          "amount_fraction INT4 NOT NULL,"
                          "description TEXT NOT NULL,"
                          "nounce BYTEA NOT NULL,"
                          "expiry INT8 NOT NULL,"
                          "product INT8 NOT NULL);"
                          "CREATE %1$s TABLE IF NOT EXISTS checkouts ("
                          "coin_pub BYTEA PRIMARY KEY,"
                          "transaction_id INT8 REFERENCES contracts(transaction_id),"
                          "amount INT4 NOT NULL,"
                          "amount_fraction INT4 NOT NULL,"
                          "coin_sig BYTEA NOT NULL);",
                          tmp_str);
  ret = GNUNET_POSTGRES_exec (conn, sql);
  (void) GNUNET_POSTGRES_exec (conn,
                               (GNUNET_OK == ret) ? "COMMIT;" : "ROLLBACK");
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
                    "(amount, amount_fraction, description,"
                    "nounce, expiry, product) VALUES"
                    "($1, $2, $3, $4, $5, $6)"
                    "RETURNING transaction_id",
                    6, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "get_contract_product",
                    "SELECT ("
                    "product"
                    ") FROM contracts "
                    "WHERE ("
                    "transaction_id=$1"
                    ")",
                    1, NULL)));
  EXITIF (PGRES_COMMAND_OK != (status = PQresultStatus(res)));
  PQclear (res);

  EXITIF (NULL == (res = PQprepare
                   (conn,
                    "checkout_create",
                    "INSERT INTO checkouts ("
                    "coin_pub,"
                    "transaction_id,"
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
                    "transaction_id IN ("
                    "SELECT (transaction_id) FROM checkouts "
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
 * @param desc descripition of the contract
 * @param nounce a random 64-bit nounce
 * @param product description to identify a product
 * @return -1 upon error; the serial id of the inserted contract upon success
 */
long long
MERCHANT_DB_contract_create (PGconn *conn,
                             struct GNUNET_TIME_Absolute expiry,
                             struct TALER_Amount *amount,
                             const char *desc,
                             uint64_t nounce,
                             uint64_t product)
{
  PGresult *res;
  uint64_t expiry_ms_nbo;
  uint32_t value_nbo;
  uint32_t fraction_nbo;
  uint64_t nounce_nbo;
  ExecStatusType status;
  long long id;
  struct TALER_DB_QueryParam params[] = {
    TALER_DB_QUERY_PARAM_PTR (&value_nbo),
    TALER_DB_QUERY_PARAM_PTR (&fraction_nbo),
    TALER_DB_QUERY_PARAM_PTR_SIZED (desc, strlen(desc)),
    TALER_DB_QUERY_PARAM_PTR (&nounce_nbo),
    TALER_DB_QUERY_PARAM_PTR (&expiry_ms_nbo),
    TALER_DB_QUERY_PARAM_PTR (&product),
    TALER_DB_QUERY_PARAM_END
  };
  struct TALER_DB_ResultSpec rs[] = {
    TALER_DB_RESULT_SPEC("transaction_id", &id),
    TALER_DB_RESULT_SPEC_END
  };

  expiry_ms_nbo = GNUNET_htonll (expiry.abs_value_us);
  value_nbo = htonl (amount->value);
  fraction_nbo = htonl (amount->fraction);
  nounce_nbo = GNUNET_htonll (nounce);
  product = GNUNET_htonll (product);
  res = TALER_DB_exec_prepared (conn, "contract_create", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_DB_extract_result (res, rs, 0));
  PQclear (res);
  return GNUNET_ntohll ((uint64_t) id);

 EXITIF_exit:
  PQclear (res);
  return -1;
}

long long
MERCHANT_DB_get_contract_product (PGconn *conn,
                                  uint64_t contract_id)
{
  PGresult *res;
  int64_t product;
  ExecStatusType status;
  struct TALER_DB_QueryParam params[] = {
    TALER_DB_QUERY_PARAM_PTR (&contract_id),
    TALER_DB_QUERY_PARAM_END
  };
  struct TALER_DB_ResultSpec rs[] = {
    TALER_DB_RESULT_SPEC("product", &product),
    TALER_DB_RESULT_SPEC_END
  };

  contract_id = GNUNET_htonll (contract_id);
  res = TALER_DB_exec_prepared (conn, "get_contract_product", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_DB_extract_result (res, rs, 0));
  PQclear (res);
  return GNUNET_ntohll ((uint64_t) product);

 EXITIF_exit:
  PQclear (res);
  return -1;
}

unsigned int
MERCHANT_DB_checkout_create (PGconn *conn,
                             struct GNUNET_CRYPTO_EddsaPublicKey *coin_pub,
                             uint64_t transaction_id,
                             struct TALER_Amount *amount,
                             struct GNUNET_CRYPTO_EddsaSignature *coin_sig)
{
  PGresult *res;
  ExecStatusType status;
  uint32_t value_nbo;
  uint32_t fraction_nbo;
  struct TALER_DB_QueryParam params[] = {
    TALER_DB_QUERY_PARAM_PTR (coin_pub),
    TALER_DB_QUERY_PARAM_PTR (&transaction_id),
    TALER_DB_QUERY_PARAM_PTR (&value_nbo),
    TALER_DB_QUERY_PARAM_PTR (&fraction_nbo),
    TALER_DB_QUERY_PARAM_PTR (coin_sig),
    TALER_DB_QUERY_PARAM_END
  };

  transaction_id = GNUNET_htonll (transaction_id);
  value_nbo = htonl (amount->value);
  fraction_nbo = htonl (amount->fraction);
  res = TALER_DB_exec_prepared (conn, "checkout_create", params);
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
                                  struct GNUNET_CRYPTO_EddsaPublicKey *coin_pub)
{
  PGresult *res;
  ExecStatusType status;
  uint64_t product;
  struct TALER_DB_QueryParam params[] = {
    TALER_DB_QUERY_PARAM_PTR (coin_pub),
    TALER_DB_QUERY_PARAM_END
  };
  struct TALER_DB_ResultSpec rs[] = {
    TALER_DB_RESULT_SPEC("product", &product),
    TALER_DB_RESULT_SPEC_END
  };

  product = -1;
  res = TALER_DB_exec_prepared (conn, "get_checkout_product", params);
  status = PQresultStatus (res);
  EXITIF (PGRES_TUPLES_OK != status);
  if (0 == PQntuples (res))
  {
    char *coin_pub_enc;
    coin_pub_enc = GNUNET_CRYPTO_eddsa_public_key_to_string (coin_pub);
    LOG_DEBUG ("Checkout not found for given coin: %s\n",
               coin_pub_enc);
    GNUNET_free (coin_pub_enc);
    goto EXITIF_exit;
  }
  EXITIF (1 != PQntuples (res));
  EXITIF (GNUNET_YES != TALER_DB_extract_result (res, rs, 0));
  PQclear (res);
  return GNUNET_ntohll ((uint64_t) product);

 EXITIF_exit:
  PQclear (res);
  return -1;
}
/* end of merchant-db.c */