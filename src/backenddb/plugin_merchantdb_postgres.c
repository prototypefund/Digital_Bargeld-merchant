/*
  This file is part of TALER
  (C) 2014--2020 Taler Systems SA

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
#include <taler/taler_mhd_lib.h>
#include "taler_merchantdb_plugin.h"

/**
 * How often do we re-try if we run into a DB serialization error?
 */
#define MAX_RETRIES 3


/**
 * Wrapper macro to add the currency from the plugin's state
 * when fetching amounts from the database.
 *
 * @param field name of the database field to fetch amount from
 * @param amountp[out] pointer to amount to set
 */
#define TALER_PQ_RESULT_SPEC_AMOUNT(field,amountp) \
  TALER_PQ_result_spec_amount (                    \
    field,pg->currency,amountp)

/**
 * Wrapper macro to add the currency from the plugin's state
 * when fetching amounts from the database.  NBO variant.
 *
 * @param field name of the database field to fetch amount from
 * @param amountp[out] pointer to amount to set
 */
#define TALER_PQ_RESULT_SPEC_AMOUNT_NBO(field, amountp) \
  TALER_PQ_result_spec_amount_nbo (                     \
    field,pg->currency,amountp)


/**
 * Wrapper macro to add the currency from the plugin's state
 * when fetching amounts from the database.
 *
 * @param field name of the database field to fetch amount from
 * @param amountp[out] pointer to amount to set
 */
#define TALER_PQ_RESULT_SPEC_AMOUNT(field,amountp) \
  TALER_PQ_result_spec_amount (                    \
    field,pg->currency,amountp)


/**
 * Type of the "cls" argument given to each of the functions in
 * our API.
 */
struct PostgresClosure
{

  /**
   * Postgres connection handle.
   */
  struct GNUNET_PQ_Context *conn;

  /**
   * Which currency do we deal in?
   */
  char *currency;

  /**
   * Directory with SQL statements to run to create tables.
   */
  char *sql_dir;

  /**
   * Underlying configuration.
   */
  const struct GNUNET_CONFIGURATION_Handle *cfg;

  /**
   * Name of the currently active transaction, NULL if none is active.
   */
  const char *transaction_name;

};


/* ********************* NEW API ************************** */


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
  char *load_path;

  GNUNET_asprintf (&load_path,
                   "%s%s",
                   pg->sql_dir,
                   "drop");
  GNUNET_PQ_run_sql (pg->conn,
                     load_path);
  GNUNET_free (load_path);
  return GNUNET_OK;
}


/**
 * Do a pre-flight check that we are not in an uncommitted transaction.
 * If we are, try to commit the previous transaction and output a warning.
 * Does not return anything, as we will continue regardless of the outcome.
 *
 * @param cls the `struct PostgresClosure` with the plugin-specific state
 */
static void
postgres_preflight (void *cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_ExecuteStatement es[] = {
    GNUNET_PQ_make_execute ("COMMIT"),
    GNUNET_PQ_EXECUTE_STATEMENT_END
  };

  if (NULL == pg->transaction_name)
    return; /* all good */
  if (GNUNET_OK ==
      GNUNET_PQ_exec_statements (pg->conn,
                                 es))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "BUG: Preflight check committed transaction `%s'!\n",
                pg->transaction_name);
  }
  else
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "BUG: Preflight check failed to commit transaction `%s'!\n",
                pg->transaction_name);
  }
  pg->transaction_name = NULL;
}


/**
 * Check that the database connection is still up.
 *
 * @param pg connection to check
 */
static void
check_connection (struct PostgresClosure *pg)
{
  GNUNET_PQ_reconnect_if_down (pg->conn);
}


/**
 * Start a transaction.
 *
 * @param cls the `struct PostgresClosure` with the plugin-specific state
 * @param name unique name identifying the transaction (for debugging),
 *             must point to a constant
 * @return #GNUNET_OK on success
 */
static int
postgres_start (void *cls,
                const char *name)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_ExecuteStatement es[] = {
    GNUNET_PQ_make_execute ("START TRANSACTION ISOLATION LEVEL SERIALIZABLE"),
    GNUNET_PQ_EXECUTE_STATEMENT_END
  };

  check_connection (pg);
  postgres_preflight (pg);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Starting merchant DB transaction\n");
  if (GNUNET_OK !=
      GNUNET_PQ_exec_statements (pg->conn,
                                 es))
  {
    TALER_LOG_ERROR ("Failed to start transaction\n");
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }
  pg->transaction_name = name;
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
  struct GNUNET_PQ_ExecuteStatement es[] = {
    GNUNET_PQ_make_execute ("ROLLBACK"),
    GNUNET_PQ_EXECUTE_STATEMENT_END
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Rolling back merchant DB transaction\n");
  GNUNET_break (GNUNET_OK ==
                GNUNET_PQ_exec_statements (pg->conn,
                                           es));
  pg->transaction_name = NULL;
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

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Committing merchant DB transaction\n");
  pg->transaction_name = NULL;
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "end_transaction",
                                             params);
}


/**
 * Context for lookup_instances().
 */
struct LookupInstancesContext
{
  /**
   * Function to call with the results.
   */
  TALER_MERCHANTDB_InstanceCallback cb;

  /**
   * Closure for @e cb.
   */
  void *cb_cls;

  /**
   * Database context.
   */
  struct PostgresClosure *pg;

  /**
   * Instance settings, valid only during find_instances_cb().
   */
  struct TALER_MERCHANTDB_InstanceSettings is;

  /**
   * Instance serial number, valid only during find_instances_cb().
   */
  uint64_t instance_serial;

  /**
   * Public key of the current instance, valid only during find_instances_cb().
   */
  struct TALER_MerchantPublicKeyP merchant_pub;

  /**
   * Set to the return value on errors.
   */
  enum GNUNET_DB_QueryStatus qs;

  /**
   * true if we only are interested in instances for which we have the private key.
   */
  bool active_only;
};


/**
 * We are processing an instances lookup and have the @a accounts.
 * Find the private key if possible, and invoke the callback.
 *
 * @param lic context we are handling
 * @param num_accounts length of @a accounts array
 * @param accounts information about accounts of the instance in @a lic
 */
static void
call_with_accounts (struct LookupInstancesContext *lic,
                    unsigned int num_accounts,
                    const struct TALER_MERCHANTDB_AccountDetails accounts[])
{
  struct PostgresClosure *pg = lic->pg;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_uint64 (&lic->instance_serial),
    GNUNET_PQ_query_param_end
  };
  struct TALER_MerchantPrivateKeyP merchant_priv;
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_auto_from_type ("merchant_priv",
                                          &merchant_priv),
    GNUNET_PQ_result_spec_end
  };

  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                 "lookup_instance_private_key",
                                                 params,
                                                 rs);
  if (qs < 0)
  {
    GNUNET_break (0);
    lic->qs = GNUNET_DB_STATUS_HARD_ERROR;
    return;
  }
  if ( (0 == qs) &&
       (lic->active_only) )
    return; /* skip, not interesting */
  lic->cb (lic->cb_cls,
           &lic->merchant_pub,
           (0 == qs) ? NULL : &merchant_priv,
           &lic->is,
           num_accounts,
           accounts);
}


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results about accounts.
 *
 * @param cls of type `struct FindInstancesContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
lookup_accounts_cb (void *cls,
                    PGresult *result,
                    unsigned int num_results)
{
  struct LookupInstancesContext *lic = cls;
  char *paytos[num_results];
  struct TALER_MERCHANTDB_AccountDetails accounts[num_results];

  for (unsigned int i = 0; i < num_results; i++)
  {
    uint8_t active;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("h_wire",
                                            &accounts[i].h_wire),
      GNUNET_PQ_result_spec_auto_from_type ("salt",
                                            &accounts[i].salt),
      GNUNET_PQ_result_spec_string ("payto_uri",
                                    &paytos[i]),
      GNUNET_PQ_result_spec_auto_from_type ("active",
                                            &active),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      lic->qs = GNUNET_DB_STATUS_HARD_ERROR;
      for (unsigned int j = 0; j < i; j++)
        GNUNET_free (paytos[j]);
      return;
    }
    accounts[i].active = (0 != active);
    accounts[i].payto_uri = paytos[i];
  }
  call_with_accounts (lic,
                      num_results,
                      accounts);
  for (unsigned int i = 0; i < num_results; i++)
    GNUNET_free (paytos[i]);
}


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results about instances.
 *
 * @param cls of type `struct FindInstancesContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
lookup_instances_cb (void *cls,
                     PGresult *result,
                     unsigned int num_results)
{
  struct LookupInstancesContext *lic = cls;
  struct PostgresClosure *pg = lic->pg;

  for (unsigned int i = 0; i < num_results; i++)
  {
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_uint64 ("merchant_serial",
                                    &lic->instance_serial),
      GNUNET_PQ_result_spec_auto_from_type ("merchant_pub",
                                            &lic->merchant_pub),
      GNUNET_PQ_result_spec_string ("merchant_id",
                                    &lic->is.id),
      GNUNET_PQ_result_spec_string ("merchant_name",
                                    &lic->is.name),
      TALER_PQ_result_spec_json ("address",
                                 &lic->is.address),
      TALER_PQ_result_spec_json ("jurisdiction",
                                 &lic->is.jurisdiction),
      TALER_PQ_RESULT_SPEC_AMOUNT ("default_max_deposit_fee",
                                   &lic->is.default_max_deposit_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("default_max_wire_fee",
                                   &lic->is.default_max_wire_fee),
      GNUNET_PQ_result_spec_uint32 ("default_wire_fee_amortization",
                                    &lic->is.default_wire_fee_amortization),
      GNUNET_PQ_result_spec_relative_time ("default_wire_transfer_delay",
                                           &lic->is.default_wire_transfer_delay),
      GNUNET_PQ_result_spec_relative_time ("default_pay_delay",
                                           &lic->is.default_pay_delay),
      GNUNET_PQ_result_spec_end
    };
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_uint64 (&lic->instance_serial),
      GNUNET_PQ_query_param_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      lic->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    lic->qs = GNUNET_PQ_eval_prepared_multi_select (lic->pg->conn,
                                                    "lookup_accounts",
                                                    params,
                                                    &lookup_accounts_cb,
                                                    lic);
    if (0 == lic->qs)
    {
      /* find_accounts_cb() did not run, still notify about the
         account-less instance! */
      call_with_accounts (lic,
                          0,
                          NULL);
    }
    GNUNET_PQ_cleanup_result (rs);
    if (0 > lic->qs)
      break;
  }
}


/**
 * Lookup all of the instances this backend has configured.
 *
 * @param cls closure
 * @param active_only only find 'active' instances
 * @param cb function to call on all instances found
 * @param cb_cls closure for @a cb
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_instances (void *cls,
                           bool active_only,
                           TALER_MERCHANTDB_InstanceCallback cb,
                           void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct LookupInstancesContext lic = {
    .cb = cb,
    .cb_cls = cb_cls,
    .active_only = active_only,
    .pg = pg
  };
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_end
  };
  enum GNUNET_DB_QueryStatus qs;

  check_connection (pg);
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
                                             "lookup_instances",
                                             params,
                                             &lookup_instances_cb,
                                             &lic);
  if (0 > lic.qs)
    return lic.qs;
  return qs;
}


/**
 * Insert information about an instance into our database.
 *
 * @param cls closure
 * @param merchant_pub public key of the instance
 * @param merchant_priv private key of the instance
 * @param is details about the instance
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_instance (void *cls,
                          const struct TALER_MerchantPublicKeyP *merchant_pub,
                          const struct TALER_MerchantPrivateKeyP *merchant_priv,
                          const struct TALER_MERCHANTDB_InstanceSettings *is)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_string (is->id),
    GNUNET_PQ_query_param_string (is->name),
    TALER_PQ_query_param_json (is->address),
    TALER_PQ_query_param_json (is->jurisdiction),
    TALER_PQ_query_param_amount (&is->default_max_deposit_fee),
    TALER_PQ_query_param_amount (&is->default_max_wire_fee),
    GNUNET_PQ_query_param_uint32 (&is->default_wire_fee_amortization),
    GNUNET_PQ_query_param_relative_time (
      &is->default_wire_transfer_delay),
    GNUNET_PQ_query_param_relative_time (&is->default_pay_delay),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_QueryParam params_priv[] = {
    GNUNET_PQ_query_param_auto_from_type (merchant_priv),
    GNUNET_PQ_query_param_string (is->id),
    GNUNET_PQ_query_param_end
  };
  enum GNUNET_DB_QueryStatus qs;

  check_connection (pg);
  qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                           "insert_instance",
                                           params);
  if (qs <= 0)
    return qs;
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_keys",
                                             params_priv);
}


/**
 * Insert information about an instance's account into our database.
 *
 * @param cls closure
 * @param id identifier of the instance
 * @param account_details details about the account
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_account (
  void *cls,
  const char *id,
  const struct
  TALER_MERCHANTDB_AccountDetails *account_details)
{
  struct PostgresClosure *pg = cls;
  uint8_t active = account_details->active;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (id),
    GNUNET_PQ_query_param_auto_from_type (&account_details->h_wire),
    GNUNET_PQ_query_param_auto_from_type (&account_details->salt),
    GNUNET_PQ_query_param_string (account_details->payto_uri),
    GNUNET_PQ_query_param_auto_from_type (&active),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_account",
                                             params);
}


/**
 * Delete private key of an instance from our database.
 *
 * @param cls closure
 * @param merchant_id identifier of the instance
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_delete_instance_private_key (
  void *cls,
  const char *merchant_id)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (merchant_id),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "delete_key",
                                             params);
}


/**
 * Purge an instance and all associated information from our database.
 * Highly likely to cause undesired data loss. Use with caution.
 *
 * @param cls closure
 * @param merchant_id identifier of the instance
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_purge_instance (void *cls,
                         const char *merchant_id)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (merchant_id),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "purge_instance",
                                             params);
}


/**
 * Update information about an instance into our database.
 *
 * @param cls closure
 * @param is details about the instance
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_update_instance (void *cls,
                          const struct TALER_MERCHANTDB_InstanceSettings *is)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (is->id),
    GNUNET_PQ_query_param_string (is->name),
    TALER_PQ_query_param_json (is->address),
    TALER_PQ_query_param_json (is->jurisdiction),
    TALER_PQ_query_param_amount (&is->default_max_deposit_fee),
    TALER_PQ_query_param_amount (&is->default_max_wire_fee),
    GNUNET_PQ_query_param_uint32 (&is->default_wire_fee_amortization),
    GNUNET_PQ_query_param_relative_time (
      &is->default_wire_transfer_delay),
    GNUNET_PQ_query_param_relative_time (&is->default_pay_delay),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "update_instance",
                                             params);
}


/**
 * Set an instance's account in our database to "inactive".
 *
 * @param cls closure
 * @param h_wire hash of the wire account to set to inactive
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_inactivate_account (void *cls,
                             const struct GNUNET_HashCode *h_wire)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_wire),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "inactivate_account",
                                             params);
}


/**
 * Context used for postgres_lookup_products().
 */
struct LookupProductsContext
{
  /**
   * Function to call with the results.
   */
  TALER_MERCHANTDB_ProductsCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Internal result.
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results about products.
 *
 * @param[in,out] cls of type `struct LookupProductsContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
lookup_products_cb (void *cls,
                    PGresult *result,
                    unsigned int num_results)
{
  struct LookupProductsContext *plc = cls;

  for (unsigned int i = 0; i < num_results; i++)
  {
    char *product_id;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_string ("product_id",
                                    &product_id),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      plc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    plc->cb (plc->cb_cls,
             product_id);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Lookup all of the products the given instance has configured.
 *
 * @param cls closure
 * @param instance_id instance to lookup products for
 * @param cb function to call on all products found
 * @param cb_cls closure for @a cb
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_products (void *cls,
                          const char *instance_id,
                          TALER_MERCHANTDB_ProductsCallback cb,
                          void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct LookupProductsContext plc = {
    .cb = cb,
    .cb_cls = cb_cls
  };
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_end
  };
  enum GNUNET_DB_QueryStatus qs;

  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
                                             "lookup_products",
                                             params,
                                             &lookup_products_cb,
                                             &plc);
  if (0 != plc.qs)
    return plc.qs;
  return qs;
}


/**
 * Lookup details about a particular product.
 *
 * @param cls closure
 * @param instance_id instance to lookup products for
 * @param product_id product to lookup
 * @param[out] pd set to the product details on success, can be NULL
 *             (in that case we only want to check if the product exists)
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_product (void *cls,
                         const char *instance_id,
                         const char *product_id,
                         struct TALER_MERCHANTDB_ProductDetails *pd)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (product_id),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_string ("description",
                                  &pd->description),
    TALER_PQ_result_spec_json ("description_i18n",
                               &pd->description_i18n),
    GNUNET_PQ_result_spec_string ("unit",
                                  &pd->unit),
    TALER_PQ_RESULT_SPEC_AMOUNT ("price",
                                 &pd->price),
    TALER_PQ_result_spec_json ("taxes",
                               &pd->taxes),
    GNUNET_PQ_result_spec_uint64 ("total_stock",
                                  &pd->total_stock),
    GNUNET_PQ_result_spec_uint64 ("total_sold",
                                  &pd->total_sold),
    GNUNET_PQ_result_spec_uint64 ("total_lost",
                                  &pd->total_lost),
    TALER_PQ_result_spec_json ("image",
                               &pd->image),
    TALER_PQ_result_spec_json ("address",
                               &pd->address),
    GNUNET_PQ_result_spec_absolute_time ("next_restock",
                                         &pd->next_restock),
    GNUNET_PQ_result_spec_end
  };
  struct GNUNET_PQ_ResultSpec rs_null[] = {
    GNUNET_PQ_result_spec_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "lookup_product",
                                                   params,
                                                   (NULL == pd)
                                                   ? rs_null
                                                   : rs);
}


/**
 * Delete information about a product.  Note that the transaction must
 * enforce that no stocks are currently locked.
 *
 * @param cls closure
 * @param instance_id instance to delete product of
 * @param product_id product to delete
 * @return DB status code, #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS
 *           if locks prevent deletion OR product unknown
 */
static enum GNUNET_DB_QueryStatus
postgres_delete_product (void *cls,
                         const char *instance_id,
                         const char *product_id)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (product_id),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "delete_product",
                                             params);
}


/**
 * Insert details about a particular product.
 *
 * @param cls closure
 * @param instance_id instance to insert product for
 * @param product_id product identifier of product to insert
 * @param pd the product details to insert
 * @return database result code
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_product (void *cls,
                         const char *instance_id,
                         const char *product_id,
                         const struct TALER_MERCHANTDB_ProductDetails *pd)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (product_id),
    GNUNET_PQ_query_param_string (pd->description),
    TALER_PQ_query_param_json (pd->description_i18n),
    GNUNET_PQ_query_param_string (pd->unit),
    TALER_PQ_query_param_json (pd->image),
    TALER_PQ_query_param_json (pd->taxes),
    TALER_PQ_query_param_amount (&pd->price),
    GNUNET_PQ_query_param_uint64 (&pd->total_stock),
    TALER_PQ_query_param_json (pd->address),
    GNUNET_PQ_query_param_absolute_time (&pd->next_restock),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_product",
                                             params);
}


/**
 * Update details about a particular product. Note that the
 * transaction must enforce that the sold/stocked/lost counters
 * are not reduced (i.e. by expanding the WHERE clause on the existing
 * values).
 *
 * @param cls closure
 * @param instance_id instance to lookup products for
 * @param product_id product to lookup
 * @param[out] pd set to the product details on success, can be NULL
 *             (in that case we only want to check if the product exists)
 *             total_sold in @a pd is ignored, total_lost must not
 *             exceed total_stock minus the existing total_sold;
 *             total_sold and total_stock must be larger or equal to
 *             the existing value;
 * @return database result code, #GNUNET_DB_SUCCESS_NO_RESULTS if the
 *         non-decreasing constraints are not met *or* if the product
 *         does not yet exist.
 */
static enum GNUNET_DB_QueryStatus
postgres_update_product (void *cls,
                         const char *instance_id,
                         const char *product_id,
                         const struct TALER_MERCHANTDB_ProductDetails *pd)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id), /* $1 */
    GNUNET_PQ_query_param_string (product_id),
    GNUNET_PQ_query_param_string (pd->description),
    TALER_PQ_query_param_json (pd->description_i18n),
    GNUNET_PQ_query_param_string (pd->unit),
    TALER_PQ_query_param_json (pd->image), /* $6 */
    TALER_PQ_query_param_json (pd->taxes),
    TALER_PQ_query_param_amount (&pd->price),
    GNUNET_PQ_query_param_uint64 (&pd->total_stock),
    GNUNET_PQ_query_param_uint64 (&pd->total_lost), /* $11 */
    TALER_PQ_query_param_json (pd->address),
    GNUNET_PQ_query_param_absolute_time (&pd->next_restock),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "update_product",
                                             params);
}


/**
 * Lock stocks of a particular product. Note that the transaction must
 * enforce that the "stocked-sold-lost >= locked" constraint holds.
 *
 * @param cls closure
 * @param instance_id instance to lookup products for
 * @param product_id product to lookup
 * @param uuid the UUID that holds the lock
 * @param quantity how many units should be locked
 * @param expiration_time when should the lock expire
 * @return database result code, #GNUNET_DB_SUCCESS_NO_RESULTS if the
 *         product is unknown OR if there insufficient stocks remaining
 */
static enum GNUNET_DB_QueryStatus
postgres_lock_product (void *cls,
                       const char *instance_id,
                       const char *product_id,
                       const struct GNUNET_Uuid *uuid,
                       uint32_t quantity,
                       struct GNUNET_TIME_Absolute expiration_time)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (product_id),
    GNUNET_PQ_query_param_auto_from_type (uuid),
    GNUNET_PQ_query_param_uint32 (&quantity),
    GNUNET_PQ_query_param_absolute_time (&expiration_time),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "lock_product",
                                             params);
}


/**
 * Delete information about an order.  Note that the transaction must
 * enforce that the order is not awaiting payment anymore.
 *
 * @param cls closure
 * @param instance_id instance to delete order of
 * @param order_id order to delete
 * @return DB status code, #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS
 *           if pending payment prevents deletion OR order unknown
 */
static enum GNUNET_DB_QueryStatus
postgres_delete_order (void *cls,
                       const char *instance_id,
                       const char *order_id)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_absolute_time (&now),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "delete_order",
                                             params);
}


/**
 * Retrieve order given its @a order_id and the @a instance_id.
 *
 * @param cls closure
 * @param instance_id instance to obtain order of
 * @param order id order id used to perform the lookup
 * @param[out] contract_terms where to store the retrieved contract terms,
 *             NULL to only test if the order exists
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_order (void *cls,
                       const char *instance_id,
                       const char *order_id,
                       json_t **contract_terms)
{
  struct PostgresClosure *pg = cls;
  json_t *j;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("contract_terms",
                               &j),
    GNUNET_PQ_result_spec_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finding contract term, order_id: '%s', instance_id: '%s'.\n",
              order_id,
              instance_id);
  check_connection (pg);
  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                 "find_order",
                                                 params,
                                                 rs);
  if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
  {
    if (NULL != contract_terms)
      *contract_terms = j;
    else
      json_decref (j);
  }
  else
  {
    /* just to be safe: NULL it */
    if (NULL != contract_terms)
      *contract_terms = NULL;
  }
  return qs;
}


/**
 * Context used for postgres_lookup_orders().
 */
struct LookupOrdersContext
{
  /**
   * Function to call with the results.
   */
  TALER_MERCHANTDB_OrdersCallback cb;

  /**
   * Closure for @a cb.
   */
  void *cb_cls;

  /**
   * Internal result.
   */
  enum GNUNET_DB_QueryStatus qs;
};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results about orders.
 *
 * @param[in,out] cls of type `struct LookupOrdersContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
lookup_orders_cb (void *cls,
                  PGresult *result,
                  unsigned int num_results)
{
  struct LookupOrdersContext *plc = cls;

  for (unsigned int i = 0; i < num_results; i++)
  {
    char *order_id;
    uint64_t order_serial;
    struct GNUNET_TIME_Absolute ts;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_string ("order_id",
                                    &order_id),
      GNUNET_PQ_result_spec_uint64 ("order_serial",
                                    &order_serial),
      GNUNET_PQ_result_spec_absolute_time ("creation_time",
                                           &ts),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      plc->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    plc->cb (plc->cb_cls,
             order_id,
             order_serial,
             ts);
    GNUNET_PQ_cleanup_result (rs);
  }
}


/**
 * Retrieve orders given the @a instance_id.
 *
 * @param cls closure
 * @param instance_id instance to obtain order of
 * @param of filter to apply when looking up orders
 * @param[out] contract_terms where to store the retrieved contract terms,
 *             NULL to only test if the order exists
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_orders (void *cls,
                        const char *instance_id,
                        const struct TALER_MERCHANTDB_OrderFilter *of,
                        TALER_MERCHANTDB_OrdersCallback cb,
                        void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct LookupOrdersContext plc = {
    .cb = cb,
    .cb_cls = cb_cls
  };
  uint64_t limit = (of->delta > 0) ? of->delta : -of->delta;
  uint8_t paid;
  uint8_t refunded;
  uint8_t wired;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_uint64 (&limit),
    GNUNET_PQ_query_param_uint64 (&of->start_row),
    GNUNET_PQ_query_param_absolute_time (&of->date),
    GNUNET_PQ_query_param_auto_from_type (&paid),
    GNUNET_PQ_query_param_auto_from_type (&refunded),
    GNUNET_PQ_query_param_auto_from_type (&wired),
    GNUNET_PQ_query_param_end
  };
  enum GNUNET_DB_QueryStatus qs;
  char stmt[128];

  paid = (TALER_MERCHANTDB_YNA_YES == of->paid);
  refunded = (TALER_MERCHANTDB_YNA_YES == of->paid);
  wired = (TALER_MERCHANTDB_YNA_YES == of->paid);
  /* painfully many cases..., note that "_xxx" being present in 'stmt' merely
     means that we filter by that variable, the value we filter for is
     computed above */
  GNUNET_snprintf (stmt,
                   sizeof (stmt),
                   "lookup_orders_%s%s%s%s",
                   (of->delta > 0) ? "inc" : "dec",
                   (TALER_MERCHANTDB_YNA_ALL == of->paid) ? "" : "_paid",
                   (TALER_MERCHANTDB_YNA_ALL == of->refunded) ? "" :
                   "_refunded",
                   (TALER_MERCHANTDB_YNA_ALL == of->wired) ? "" : "_wired");
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
                                             stmt,
                                             params,
                                             &lookup_orders_cb,
                                             &plc);
  if (0 != plc.qs)
    return plc.qs;
  return qs;
}


/**
 * Insert order into the DB.
 *
 * @param cls closure
 * @param instance_id identifies the instance responsible for the order
 * @param order_id alphanumeric string that uniquely identifies the proposal
 * @param pay_deadline how long does the customer have to pay for the order
 * @param contract_terms proposal data to store
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_order (void *cls,
                       const char *instance_id,
                       const char *order_id,
                       struct GNUNET_TIME_Absolute pay_deadline,
                       const json_t *contract_terms)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_absolute_time (&pay_deadline),
    GNUNET_PQ_query_param_absolute_time (&now),
    TALER_PQ_query_param_json (contract_terms),
    GNUNET_PQ_query_param_end
  };

  now = GNUNET_TIME_absolute_get ();
  (void) GNUNET_TIME_round_abs (&now);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "inserting order: order_id: %s, instance_id: %s.\n",
              order_id,
              instance_id);
  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_order",
                                             params);
}


/**
 * Release an inventory lock by UUID. Releases ALL stocks locked under
 * the given UUID.
 *
 * @param cls closure
 * @param uuid the UUID to release locks for
 * @return transaction status,
 *   #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS means there are no locks under @a uuid
 *   #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT indicates success
 */
static enum GNUNET_DB_QueryStatus
postgres_unlock_inventory (void *cls,
                           const struct GNUNET_Uuid *uuid)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (uuid),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "unlock_inventory",
                                             params);
}


/**
 * Lock inventory stock to a particular order.
 *
 * @param cls closure
 * @param instance_id identifies the instance responsible for the order
 * @param order_id alphanumeric string that uniquely identifies the order
 * @param product_id uniquely identifies the product to be locked
 * @param quantity how many units should be locked to the @a order_id
 * @return transaction status,
 *   #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS means there are insufficient stocks
 *   #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT indicates success
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_order_lock (void *cls,
                            const char *instance_id,
                            const char *order_id,
                            const char *product_id,
                            uint32_t quantity)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_string (product_id),
    GNUNET_PQ_query_param_uint32 (&quantity),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_order_lock",
                                             params);
}


/**
 * Retrieve contract terms given its @a order_id
 *
 * @param cls closure
 * @param instance_id instance's identifier
 * @param order_id order_id used to lookup.
 * @param[out] contract_terms where to store the result, NULL to only check for existence
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_contract_terms (void *cls,
                                const char *instance_id,
                                const char *order_id,
                                json_t **contract_terms)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("contract_terms",
                               contract_terms),
    GNUNET_PQ_result_spec_end
  };

  *contract_terms = NULL;
  check_connection (pg);
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "lookup_contract_terms",
                                                   params,
                                                   rs);
}


/**
 * Store contract terms given its @a order_id. Note that some attributes are
 * expected to be calculated inside of the function, like the hash of the
 * contract terms (to be hashed), the creation_time and pay_deadline (to be
 * obtained from the merchant_orders table). The "session_id" should be
 * initially set to the empty string.  The "fulfillment_url" and "refund_deadline"
 * must be extracted from @a contract_terms.
 *
 * @param cls closure
 * @param instance_id instance's identifier
 * @param order_id order_id used to store
 * @param contract_terms contract to store
 * @return transaction status, #GNUNET_DB_STATUS_HARD_ERROR if @a contract_terms
 *          is malformed
 */
static enum GNUNET_DB_QueryStatus
postgres_insert_contract_terms (void *cls,
                                const char *instance_id,
                                const char *order_id,
                                json_t *contract_terms)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_TIME_Absolute pay_deadline;
  struct GNUNET_TIME_Absolute refund_deadline;
  const char *fulfillment_url;
  struct GNUNET_HashCode h_contract_terms;

  if (GNUNET_OK !=
      TALER_JSON_hash (contract_terms,
                       &h_contract_terms))
  {
    GNUNET_break (0);
    return GNUNET_SYSERR;
  }

  {
    struct GNUNET_JSON_Specification spec[] = {
      GNUNET_JSON_spec_string ("fulfillment_url",
                               &fulfillment_url),
      GNUNET_JSON_spec_absolute_time ("pay_deadline",
                                      &pay_deadline),
      GNUNET_JSON_spec_absolute_time ("refund_deadline",
                                      &refund_deadline),
      GNUNET_JSON_spec_end ()
    };
    enum GNUNET_GenericReturnValue res;

    res = TALER_MHD_parse_json_data (NULL,
                                     contract_terms,
                                     spec);
    if (GNUNET_OK != res)
    {
      GNUNET_break (0);
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
  }

  check_connection (pg);
  {
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_string (instance_id),
      GNUNET_PQ_query_param_string (order_id),
      TALER_PQ_query_param_json (contract_terms),
      GNUNET_PQ_query_param_auto_from_type (&h_contract_terms),
      GNUNET_PQ_query_param_absolute_time (&pay_deadline),
      GNUNET_PQ_query_param_absolute_time (&refund_deadline),
      GNUNET_PQ_query_param_string (fulfillment_url),
      GNUNET_PQ_query_param_end
    };

    return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                               "insert_contract_terms",
                                               params);
  }
}


/**
 * Delete information about a contract. Note that the transaction must
 * enforce that the contract is not awaiting payment anymore AND was not
 * paid, or is past the legal expiration.
 *
 * @param cls closure
 * @param instance_id instance to delete order of
 * @param order_id order to delete
 * @param legal_expiration how long do we need to keep (paid) contracts on
 *          file for legal reasons (i.e. taxation)
 * @return DB status code, #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS
 *           if locks prevent deletion OR order unknown
 */
static enum GNUNET_DB_QueryStatus
postgres_delete_contract_terms (void *cls,
                                const char *instance_id,
                                const char *order_id,
                                struct GNUNET_TIME_Relative legal_expiration)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (instance_id),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_relative_time (&legal_expiration),
    GNUNET_PQ_query_param_absolute_time (&now),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "delete_contract_terms",
                                             params);
}


/* ********************* OLD API ************************** */

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
postgres_find_contract_terms_from_hash (
  void *cls,
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

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "find_contract_terms_from_hash",
                                                   params,
                                                   rs);
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
postgres_find_paid_contract_terms_from_hash (void *cls,
                                             json_t **contract_terms,
                                             const struct
                                             GNUNET_HashCode *h_contract_terms,
                                             const struct
                                             TALER_MerchantPublicKeyP *
                                             merchant_pub)
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

  /* no preflight check here, runs in its own transaction from
     caller (in /pay case) */
  check_connection (pg);
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "find_paid_contract_terms_from_hash",
                                                   params,
                                                   rs);
}


/**
 * Mark contract terms as paid.  Needed by /history as only paid
 * contracts must be shown.
 *
 * NOTE: we can't get the list of (paid) contracts from the
 * transactions table because it lacks contract_terms plain JSON.  In
 * facts, the protocol doesn't allow to store contract_terms in
 * transactions table, as /pay handler doesn't receive this data (only
 * /proposal does).
 *
 * @param cls closure
 * @param h_contract_terms hash of the contract that is now paid
 * @param merchant_pub merchant's public key
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_mark_proposal_paid (void *cls,
                             const struct GNUNET_HashCode *h_contract_terms,
                             const struct
                             TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };

  TALER_LOG_DEBUG ("Marking proposal paid, h_contract_terms: '%s',"
                   " merchant_pub: '%s'\n",
                   GNUNET_h2s (h_contract_terms),
                   TALER_B2S (merchant_pub));
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "mark_proposal_paid",
                                             params);
}


/**
 * Store the order ID that was used to pay for a resource within a session.
 *
 * @param cls closure
 * @param session_id session id
 * @param fulfillment_url URL that canonically identifies the resource
 *        being paid for
 * @param order_id the order ID that was used when paying for the resource URL
 * @param merchant_pub public key of the merchant, identifying the instance
 * @return transaction status
 */
enum GNUNET_DB_QueryStatus
postgres_insert_session_info (void *cls,
                              const char *session_id,
                              const char *fulfillment_url,
                              const char *order_id,
                              const struct
                              TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;

  struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (session_id),
    GNUNET_PQ_query_param_string (fulfillment_url),
    GNUNET_PQ_query_param_string (order_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_absolute_time (&now),
    GNUNET_PQ_query_param_end
  };

  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_session_info",
                                             params);
}


/**
 * Retrieve the order ID that was used to pay for a resource within a session.
 *
 * @param cls closure
 * @param[out] order_id where to store the order ID that was used when
 *             paying for the resource URL
 * @param session_id session id
 * @param fulfillment_url URL that canonically identifies the resource
 *        being paid for
 * @param merchant_pub public key of the merchant, identifying the instance
 * @return transaction status
 */
enum GNUNET_DB_QueryStatus
postgres_find_session_info (void *cls,
                            char **order_id,
                            const char *session_id,
                            const char *fulfillment_url,
                            const struct TALER_MerchantPublicKeyP *merchant_pub)
{
  struct PostgresClosure *pg = cls;

  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (fulfillment_url),
    GNUNET_PQ_query_param_string (session_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_string ("order_id",
                                  order_id),
    GNUNET_PQ_result_spec_end
  };
  // We don't clean up the result spec since we want
  // to keep around the memory for order_id.
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "find_session_info",
                                                   params,
                                                   rs);
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
 * @param wire_fee wire fee changed by the exchange
 * @param signkey_pub public key used by the exchange for @a exchange_proof
 * @param exchange_proof proof from exchange that coin was accepted
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_store_deposit (void *cls,
                        const struct GNUNET_HashCode *h_contract_terms,
                        const struct TALER_MerchantPublicKeyP *merchant_pub,
                        const struct TALER_CoinSpendPublicKeyP *coin_pub,
                        const char *exchange_url,
                        const struct TALER_Amount *amount_with_fee,
                        const struct TALER_Amount *deposit_fee,
                        const struct TALER_Amount *refund_fee,
                        const struct TALER_Amount *wire_fee,
                        const struct TALER_ExchangePublicKeyP *signkey_pub,
                        const json_t *exchange_proof)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_string (exchange_url),
    TALER_PQ_query_param_amount (amount_with_fee),
    TALER_PQ_query_param_amount (deposit_fee),
    TALER_PQ_query_param_amount (refund_fee),
    TALER_PQ_query_param_amount (wire_fee),
    GNUNET_PQ_query_param_auto_from_type (signkey_pub),
    TALER_PQ_query_param_json (exchange_proof),
    GNUNET_PQ_query_param_end
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Storing payment for h_contract_terms `%s', coin_pub: `%s', amount_with_fee: %s\n",
              GNUNET_h2s (h_contract_terms),
              TALER_B2S (coin_pub),
              TALER_amount2s (amount_with_fee));
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Merchant pub is `%s'\n",
              TALER_B2S (merchant_pub));
  check_connection (pg);
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
                                 const struct
                                 TALER_CoinSpendPublicKeyP *coin_pub,
                                 const struct
                                 TALER_WireTransferIdentifierRawP *wtid)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_transfer",
                                             params);
}


/**
 * Insert wire transfer confirmation from the exchange into the database.
 *
 * @param cls closure
 * @param exchange_url URL of the exchange
 * @param wtid identifier of the wire transfer
 * @param execution_time when was @a wtid executed
 * @param signkey_pub public key used by the exchange for @a exchange_proof
 * @param exchange_proof proof from exchange about what the deposit was for
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_store_transfer_to_proof (void *cls,
                                  const char *exchange_url,
                                  const struct
                                  TALER_WireTransferIdentifierRawP *wtid,
                                  struct GNUNET_TIME_Absolute execution_time,
                                  const struct
                                  TALER_ExchangePublicKeyP *signkey_pub,
                                  const json_t *exchange_proof)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_string (exchange_url),
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_absolute_time (&execution_time),
    GNUNET_PQ_query_param_auto_from_type (signkey_pub),
    TALER_PQ_query_param_json (exchange_proof),
    GNUNET_PQ_query_param_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_proof",
                                             params);
}


/**
 * Closure for #find_tip_authorizations_cb().
 */
struct GetAuthorizedTipAmountContext
{
  /**
   * Total authorized amount.
   */
  struct TALER_Amount authorized_amount;

  /**
   * Transaction status code to set.
   */
  enum GNUNET_DB_QueryStatus qs;

  /**
   * Plugin context.
   */
  struct PostgresClosure *pg;

};


/**
 * Function to be called with the results of a SELECT statement
 * that has returned @a num_results results.
 *
 * @param cls of type `struct GetAuthorizedTipAmountContext *`
 * @param result the postgres result
 * @param num_result the number of results in @a result
 */
static void
find_tip_authorizations_cb (void *cls,
                            PGresult *result,
                            unsigned int num_results)
{
  struct GetAuthorizedTipAmountContext *ctx = cls;
  struct PostgresClosure *pg = ctx->pg;
  unsigned int i;

  for (i = 0; i < num_results; i++)
  {
    struct TALER_Amount amount;
    char *just;
    json_t *extra;
    struct GNUNET_HashCode h;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_string ("justification",
                                    &just),
      GNUNET_PQ_result_spec_auto_from_type ("tip_id",
                                            &h),
      TALER_PQ_RESULT_SPEC_AMOUNT ("amount",
                                   &amount),
      TALER_PQ_result_spec_json ("extra",
                                 &extra),
      GNUNET_PQ_result_spec_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    if (0 == i)
    {
      ctx->authorized_amount = amount;
    }
    else
    {
      if (0 >
          TALER_amount_add (&ctx->authorized_amount,
                            &ctx->authorized_amount,
                            &amount))
      {
        GNUNET_break (0);
        GNUNET_PQ_cleanup_result (rs);
        ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
        return;
      }
    }
    GNUNET_PQ_cleanup_result (rs);
  }

  if (0 == i)
  {
    ctx->qs = GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
  }
  else
  {
    /* one aggregated result */
    ctx->qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  }
}


/**
 * Get the total amount of authorized tips for a tipping reserve.
 *
 * @param cls closure, typically a connection to the db
 * @param reserve_priv which reserve to check
 * @param[out] authorzed_amount amount we've authorized so far for tips
 * @return transaction status, usually
 *      #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT for success
 *      #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if the reserve_priv
 *      does not identify a known tipping reserve
 */
static enum GNUNET_DB_QueryStatus
postgres_get_authorized_tip_amount (void *cls,
                                    const struct
                                    TALER_ReservePrivateKeyP *reserve_priv,
                                    struct TALER_Amount *authorized_amount)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (reserve_priv),
    GNUNET_PQ_query_param_end
  };
  enum GNUNET_DB_QueryStatus qs;
  struct GetAuthorizedTipAmountContext ctx = {
    .pg = pg
  };

  check_connection (pg);
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
                                             "find_tip_authorizations",
                                             params,
                                             &find_tip_authorizations_cb,
                                             &ctx);
  if (0 >= qs)
    return qs;
  *authorized_amount = ctx.authorized_amount;
  return ctx.qs;
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
   * Plugin context.
   */
  struct PostgresClosure *pg;

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
  struct PostgresClosure *pg = fpc->pg;

  for (unsigned int i = 0; i<num_results; i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    struct TALER_Amount refund_fee;
    struct TALER_Amount wire_fee;
    json_t *exchange_proof;
    char *exchange_url;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      GNUNET_PQ_result_spec_string ("exchange_url",
                                    &exchange_url),
      TALER_PQ_RESULT_SPEC_AMOUNT ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("deposit_fee",
                                   &deposit_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("refund_fee",
                                   &refund_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("wire_fee",
                                   &wire_fee),
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
             exchange_url,
             &amount_with_fee,
             &deposit_fee,
             &refund_fee,
             &wire_fee,
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
    .cb_cls = cb_cls,
    .pg = pg
  };
  enum GNUNET_DB_QueryStatus qs;

  /* no preflight check here, run in its own transaction by the
     caller! */
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Finding payment for h_contract_terms '%s'\n",
              GNUNET_h2s (h_contract_terms));
  check_connection (pg);
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
   * Plugin context.
   */
  struct PostgresClosure *pg;

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
  struct PostgresClosure *pg = fpc->pg;

  for (unsigned int i = 0; i<num_results; i++)
  {
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    struct TALER_Amount refund_fee;
    struct TALER_Amount wire_fee;
    char *exchange_url;
    json_t *exchange_proof;
    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_RESULT_SPEC_AMOUNT ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("deposit_fee",
                                   &deposit_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("refund_fee",
                                   &refund_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("wire_fee",
                                   &wire_fee),
      GNUNET_PQ_result_spec_string ("exchange_url",
                                    &exchange_url),
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
             exchange_url,
             &amount_with_fee,
             &deposit_fee,
             &refund_fee,
             &wire_fee,
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
                                         const struct
                                         GNUNET_HashCode *h_contract_terms,
                                         const struct
                                         TALER_MerchantPublicKeyP *merchant_pub,
                                         const struct
                                         TALER_CoinSpendPublicKeyP *coin_pub,
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
    .pg = pg,
    .h_contract_terms = h_contract_terms,
    .coin_pub = coin_pub
  };
  enum GNUNET_DB_QueryStatus qs;

  check_connection (pg);
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

  for (unsigned int i = 0; i<num_results; i++)
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

  check_connection (pg);
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
   * Plugin context.
   */
  struct PostgresClosure *pg;

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
  struct PostgresClosure *pg = fdc->pg;

  for (unsigned int i = 0; i<num_results; i++)
  {
    struct GNUNET_HashCode h_contract_terms;
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct TALER_Amount deposit_fee;
    struct TALER_Amount refund_fee;
    struct TALER_Amount wire_fee;
    char *exchange_url;
    json_t *exchange_proof;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("h_contract_terms",
                                            &h_contract_terms),
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_RESULT_SPEC_AMOUNT ("amount_with_fee",
                                   &amount_with_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("deposit_fee",
                                   &deposit_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("refund_fee",
                                   &refund_fee),
      TALER_PQ_RESULT_SPEC_AMOUNT ("wire_fee",
                                   &wire_fee),
      GNUNET_PQ_result_spec_string ("exchange_url",
                                    &exchange_url),
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
             exchange_url,
             &amount_with_fee,
             &deposit_fee,
             &refund_fee,
             &wire_fee,
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
                                const struct
                                TALER_WireTransferIdentifierRawP *wtid,
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
    .cb_cls = cb_cls,
    .pg = pg
  };
  enum GNUNET_DB_QueryStatus qs;

  check_connection (pg);
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
   * Plugin context.
   */
  struct PostgresClosure *pg;

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
  struct PostgresClosure *pg = grc->pg;

  for (unsigned int i = 0; i<num_results; i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    uint64_t rtransaction_id;
    struct TALER_Amount refund_amount;
    struct TALER_Amount refund_fee;
    char *reason;
    char *exchange_url;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      GNUNET_PQ_result_spec_string ("exchange_url",
                                    &exchange_url),
      GNUNET_PQ_result_spec_uint64 ("rtransaction_id",
                                    &rtransaction_id),
      TALER_PQ_RESULT_SPEC_AMOUNT ("refund_amount",
                                   &refund_amount),
      TALER_PQ_RESULT_SPEC_AMOUNT ("refund_fee",
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
             exchange_url,
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
postgres_get_refunds_from_contract_terms_hash (
  void *cls,
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
    .rc_cls = rc_cls,
    .pg = pg
  };
  enum GNUNET_DB_QueryStatus qs;

  /* no preflight check here, run in transaction by caller! */
  TALER_LOG_DEBUG ("Looking for refund %s + %s\n",
                   GNUNET_h2s (h_contract_terms),
                   TALER_B2S (merchant_pub));
  check_connection (pg);
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
 * Obtain refund proofs associated with a refund operation on a
 * coin.
 *
 * @param cls closure, typically a connection to the db
 * @param merchant_pub public key of the merchant instance
 * @param h_contract_terms hash code of the contract
 * @param coin_pub public key of the coin
 * @param rtransaction_id identificator of the refund
 * @param[out] exchange_pub public key of the exchange affirming the refund
 * @param[out] exchange_sig signature of the exchange affirming the refund
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_get_refund_proof (
  void *cls,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct TALER_CoinSpendPublicKeyP *coin_pub,
  uint64_t rtransaction_id,
  struct TALER_ExchangePublicKeyP *exchange_pub,
  struct TALER_ExchangeSignatureP *exchange_sig)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_uint64 (&rtransaction_id),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_auto_from_type ("exchange_sig",
                                          exchange_sig),
    GNUNET_PQ_result_spec_auto_from_type ("exchange_pub",
                                          exchange_pub),
    GNUNET_PQ_result_spec_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "get_refund_proof",
                                                   params,
                                                   rs);
}


/**
 * Store refund proofs associated with a refund operation on a
 * coin.
 *
 * @param cls closure, typically a connection to the db
 * @param merchant_pub public key of the merchant instance
 * @param h_contract_terms hash code of the contract
 * @param coin_pub public key of the coin
 * @param rtransaction_id identificator of the refund
 * @param exchange_pub public key of the exchange affirming the refund
 * @param exchange_sig signature of the exchange affirming the refund
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_put_refund_proof (
  void *cls,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct TALER_CoinSpendPublicKeyP *coin_pub,
  uint64_t rtransaction_id,
  const struct TALER_ExchangePublicKeyP *exchange_pub,
  const struct TALER_ExchangeSignatureP *exchange_sig)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_uint64 (&rtransaction_id),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_auto_from_type (exchange_sig),
    GNUNET_PQ_query_param_auto_from_type (exchange_pub),
    GNUNET_PQ_query_param_end
  };

  TALER_LOG_DEBUG ("Inserting refund proof %s + %s\n",
                   GNUNET_h2s (h_contract_terms),
                   TALER_B2S (coin_pub));
  check_connection (pg);
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_refund_proof",
                                             params);
}


/**
 * Insert a refund row into merchant_refunds.  Not meant to be exported
 * in the db API.
 *
 * @param cls closure, typically a connection to the db
 * @param merchant_pub merchant instance public key
 * @param h_contract_terms hashcode of the contract related to this refund
 * @param coin_pub public key of the coin giving the (part of) refund
 * @param reason human readable explanation behind the refund
 * @param refund how much this coin is refunding
 */
static enum GNUNET_DB_QueryStatus
insert_refund (void *cls,
               const struct TALER_MerchantPublicKeyP *merchant_pub,
               const struct GNUNET_HashCode *h_contract_terms,
               const struct TALER_CoinSpendPublicKeyP *coin_pub,
               const char *reason,
               const struct TALER_Amount *refund)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (coin_pub),
    GNUNET_PQ_query_param_string (reason),
    TALER_PQ_query_param_amount (refund),
    GNUNET_PQ_query_param_end
  };

  TALER_LOG_DEBUG ("Inserting refund %s + %s\n",
                   GNUNET_h2s (h_contract_terms),
                   TALER_B2S (merchant_pub));

  check_connection (pg);
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
postgres_store_wire_fee_by_exchange (
  void *cls,
  const struct
  TALER_MasterPublicKeyP *exchange_pub,
  const struct
  GNUNET_HashCode *h_wire_method,
  const struct TALER_Amount *wire_fee,
  const struct TALER_Amount *closing_fee,
  struct GNUNET_TIME_Absolute start_date,
  struct GNUNET_TIME_Absolute end_date,
  const struct
  TALER_MasterSignatureP *exchange_sig)
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

  /* no preflight check here, run in its own transaction by the caller */
  check_connection (pg);
  GNUNET_log (GNUNET_ERROR_TYPE_INFO,
              "Storing wire fee for %s starting at %s of %s\n",
              TALER_B2S (exchange_pub),
              GNUNET_STRINGS_absolute_time_to_string (start_date),
              TALER_amount2s (wire_fee));
  return GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_wire_fee",
                                             params);
}


/**
 * Obtain information about wire fees charged by an exchange,
 * including signature (so we have proof).
 *
 * @param cls closure
 * @param exchange_pub public key of the exchange
 * @param h_wire_method hash of wire method
 * @param contract_date date of the contract to use for the lookup
 * @param[out] wire_fee wire fee charged
 * @param[out] closing_fee closing fee charged (irrelevant for us,
 *              but needed to check signature)
 * @param[out] start_date start of fee being used
 * @param[out] end_date end of fee being used
 * @param[out] exchange_sig signature of exchange over fee structure
 * @return transaction status code
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_wire_fee (void *cls,
                          const struct TALER_MasterPublicKeyP *exchange_pub,
                          const struct GNUNET_HashCode *h_wire_method,
                          struct GNUNET_TIME_Absolute contract_date,
                          struct TALER_Amount *wire_fee,
                          struct TALER_Amount *closing_fee,
                          struct GNUNET_TIME_Absolute *start_date,
                          struct GNUNET_TIME_Absolute *end_date,
                          struct TALER_MasterSignatureP *exchange_sig)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (exchange_pub),
    GNUNET_PQ_query_param_auto_from_type (h_wire_method),
    GNUNET_PQ_query_param_absolute_time (&contract_date),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_RESULT_SPEC_AMOUNT ("wire_fee",
                                 wire_fee),
    TALER_PQ_RESULT_SPEC_AMOUNT ("closing_fee",
                                 closing_fee),
    GNUNET_PQ_result_spec_absolute_time ("start_date",
                                         start_date),
    GNUNET_PQ_result_spec_absolute_time ("end_date",
                                         end_date),
    GNUNET_PQ_result_spec_auto_from_type ("exchange_sig",
                                          exchange_sig),
    GNUNET_PQ_result_spec_end
  };

  check_connection (pg);
  return GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "lookup_wire_fee",
                                                   params,
                                                   rs);
}


/**
 * Closure for #process_refund_cb.
 */
struct FindRefundContext
{

  /**
   * Plugin context.
   */
  struct PostgresClosure *pg;

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
  struct PostgresClosure *pg = ictx->pg;

  for (unsigned int i = 0; i<num_results; i++)
  {
    /* Sum up existing refunds */
    struct TALER_Amount acc;
    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_RESULT_SPEC_AMOUNT ("refund_amount",
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
    if (0 >
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
 * Closure for #process_deposits_for_refund_cb.
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
  struct PostgresClosure *pg = ctx->pg;
  struct TALER_Amount current_refund;
  struct TALER_Amount deposit_refund[GNUNET_NZL (num_results)];
  struct TALER_CoinSpendPublicKeyP deposit_coin_pubs[GNUNET_NZL (num_results)];
  struct TALER_Amount deposit_amount_with_fee[GNUNET_NZL (num_results)];

  GNUNET_assert (GNUNET_OK ==
                 TALER_amount_get_zero (ctx->refund->currency,
                                        &current_refund));

  /* Pass 1:  Collect amount of existing refunds into current_refund.
   * Also store existing refunded amount for each deposit in deposit_refund. */
  for (unsigned int i = 0; i<num_results; i++)
  {
    struct TALER_CoinSpendPublicKeyP coin_pub;
    struct TALER_Amount amount_with_fee;
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_auto_from_type ("coin_pub",
                                            &coin_pub),
      TALER_PQ_RESULT_SPEC_AMOUNT ("amount_with_fee",
                                   &amount_with_fee),
      GNUNET_PQ_result_spec_end
    };
    struct FindRefundContext ictx = {
      .err = GNUNET_OK,
      .pg = pg
    };
    enum GNUNET_DB_QueryStatus ires;
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (&coin_pub),
      GNUNET_PQ_query_param_end
    };

    if (GNUNET_OK !=
        GNUNET_PQ_extract_result (result,
                                  rs,
                                  i))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    GNUNET_assert (GNUNET_OK ==
                   TALER_amount_get_zero (ctx->refund->currency,
                                          &ictx.refunded_amount));
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
    deposit_refund[i] = ictx.refunded_amount;
    deposit_amount_with_fee[i] = amount_with_fee;
    deposit_coin_pubs[i] = coin_pub;
    if (0 >
        TALER_amount_add (&current_refund,
                          &current_refund,
                          &ictx.refunded_amount))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Existing refund for coin %s is %s\n",
                TALER_B2S (&coin_pub),
                TALER_amount2s (&ictx.refunded_amount));
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Total existing refund is %s\n",
              TALER_amount2s (&current_refund));

  /* stop immediately if we are 'done' === amount already
   * refunded.  */
  if (0 >= TALER_amount_cmp (ctx->refund,
                             &current_refund))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                "Existing refund of %s at or above requested refund. Finished early.\n",
                TALER_amount2s (&current_refund));
    return;
  }

  /* Phase 2:  Try to increase current refund until it matches desired refund */
  for (unsigned int i = 0; i<num_results; i++)
  {
    const struct TALER_Amount *increment;
    struct TALER_Amount left;
    struct TALER_Amount remaining_refund;

    /* How much of the coin is left after the existing refunds? */
    if (0 >
        TALER_amount_subtract (&left,
                               &deposit_amount_with_fee[i],
                               &deposit_refund[i]))
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
                  TALER_B2S (&deposit_coin_pubs[i]));
      continue;
    }

    /* How much of the refund is still to be paid back? */
    if (0 >
        TALER_amount_subtract (&remaining_refund,
                               ctx->refund,
                               &current_refund))
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

    if (0 >
        TALER_amount_add (&current_refund,
                          &current_refund,
                          increment))
    {
      GNUNET_break (0);
      ctx->qs = GNUNET_DB_STATUS_HARD_ERROR;
      return;
    }

    /* actually run the refund */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Coin %s deposit amount is %s\n",
                TALER_B2S (&deposit_coin_pubs[i]),
                TALER_amount2s (&deposit_amount_with_fee[i]));
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Coin %s refund will be incremented by %s\n",
                TALER_B2S (&deposit_coin_pubs[i]),
                TALER_amount2s (increment));
    {
      enum GNUNET_DB_QueryStatus qs;

      if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT !=
          (qs = insert_refund (ctx->pg,
                               ctx->merchant_pub,
                               ctx->h_contract_terms,
                               &deposit_coin_pubs[i],
                               ctx->reason,
                               increment)))
      {
        GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
        ctx->qs = qs;
        return;
      }
    }

    /* stop immediately if we are done */
    if (0 == TALER_amount_cmp (ctx->refund,
                               &current_refund))
      return;
  }

  /**
   * We end up here if not all of the refund has been covered.
   * Although this should be checked as the business should never
   * issue a refund bigger than the contract's actual price, we cannot
   * rely upon the frontend being correct.
   *///
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "The refund of %s is bigger than the order's value\n",
              TALER_amount2s (ctx->refund));
  ctx->qs = GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
}


/**
 * Function called when some backoffice staff decides to award or
 * increase the refund on an existing contract.  This function
 * MUST be called from within a transaction scope setup by the
 * caller as it executes multiple SQL statements (NT).
 *
 * @param cls closure
 * @param h_contract_terms
 * @param merchant_pub merchant's instance public key
 * @param refund maximum refund to return to the customer for this contract
 * @param reason 0-terminated UTF-8 string giving the reason why the customer
 *               got a refund (free form, business-specific)
 * @return transaction status
 *        #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if @a refund is ABOVE the amount we
 *        were originally paid and thus the transaction failed;
 *        #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT if the request is valid,
 *        regardless of whether it actually increased the refund beyond
 *        what was already refunded (idempotency!)
 */
static enum GNUNET_DB_QueryStatus
postgres_increase_refund_for_contract_NT (
  void *cls,
  const struct GNUNET_HashCode *h_contract_terms,
  const struct TALER_MerchantPublicKeyP *merchant_pub,
  const struct TALER_Amount *refund,
  const char *reason)
{
  struct PostgresClosure *pg = cls;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (h_contract_terms),
    GNUNET_PQ_query_param_auto_from_type (merchant_pub),
    GNUNET_PQ_query_param_end
  };
  struct InsertRefundContext ctx = {
    .pg = pg,
    .qs = GNUNET_DB_STATUS_SUCCESS_ONE_RESULT,
    .refund = refund,
    .reason = reason,
    .h_contract_terms = h_contract_terms,
    .merchant_pub = merchant_pub
  };

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Asked to refund %s on contract %s\n",
              TALER_amount2s (refund),
              GNUNET_h2s (h_contract_terms));
  qs = GNUNET_PQ_eval_prepared_multi_select (pg->conn,
                                             "find_deposits",
                                             params,
                                             &process_deposits_for_refund_cb,
                                             &ctx);
  switch (qs)
  {
  case GNUNET_DB_STATUS_SUCCESS_NO_RESULTS:
    /* never paid, means we clearly cannot refund anything */
    return GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
  case GNUNET_DB_STATUS_SOFT_ERROR:
  case GNUNET_DB_STATUS_HARD_ERROR:
    return qs;
  default:
    /* Got one or more deposits */
    return ctx.qs;
  }
}


/**
 * Lookup proof information about a wire transfer.
 *
 * @param cls closure
 * @param exchange_url from which exchange are we looking for proof
 * @param wtid wire transfer identifier for the search
 * @param cb function to call with proof data
 * @param cb_cls closure for @a cb
 * @return transaction status
 */
static enum GNUNET_DB_QueryStatus
postgres_find_proof_by_wtid (void *cls,
                             const char *exchange_url,
                             const struct
                             TALER_WireTransferIdentifierRawP *wtid,
                             TALER_MERCHANTDB_ProofCallback cb,
                             void *cb_cls)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (wtid),
    GNUNET_PQ_query_param_string (exchange_url),
    GNUNET_PQ_query_param_end
  };
  json_t *proof;
  struct GNUNET_PQ_ResultSpec rs[] = {
    TALER_PQ_result_spec_json ("proof",
                               &proof),
    GNUNET_PQ_result_spec_end
  };
  enum GNUNET_DB_QueryStatus qs;

  check_connection (pg);
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
 * Add @a credit to a reserve to be used for tipping.  Note that
 * this function does not actually perform any wire transfers to
 * credit the reserve, it merely tells the merchant backend that
 * a reserve was topped up.  This has to happen before tips can be
 * authorized.
 *
 * @param cls closure, typically a connection to the db
 * @param reserve_priv which reserve is topped up or created
 * @param credit_uuid unique identifier for the credit operation
 * @param credit how much money was added to the reserve
 * @param expiration when does the reserve expire?
 * @return transaction status, usually
 *      #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT for success
 *      #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if @a credit_uuid already known
 */
static enum GNUNET_DB_QueryStatus
postgres_enable_tip_reserve_TR (void *cls,
                                const struct
                                TALER_ReservePrivateKeyP *reserve_priv,
                                const struct GNUNET_HashCode *credit_uuid,
                                const struct TALER_Amount *credit,
                                struct GNUNET_TIME_Absolute expiration)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_TIME_Absolute old_expiration;
  struct TALER_Amount old_balance;
  enum GNUNET_DB_QueryStatus qs;
  struct GNUNET_TIME_Absolute new_expiration;
  struct TALER_Amount new_balance;
  unsigned int retries;

  retries = 0;
  check_connection (pg);
RETRY:
  if (MAX_RETRIES < ++retries)
    return GNUNET_DB_STATUS_SOFT_ERROR;
  if (GNUNET_OK !=
      postgres_start (pg,
                      "enable tip reserve"))
  {
    GNUNET_break (0);
    return GNUNET_DB_STATUS_HARD_ERROR;
  }

  /* ensure that credit_uuid is new/unique */
  {
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (credit_uuid),
      GNUNET_PQ_query_param_auto_from_type (reserve_priv),
      GNUNET_PQ_query_param_end
    };

    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_end
    };
    qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "lookup_tip_credit_uuid",
                                                   params,
                                                   rs);
    if (0 > qs)
    {
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
      postgres_rollback (pg);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
        goto RETRY;
      return qs;
    }
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS != qs)
    {
      /* UUID already exists, we are done! */
      postgres_rollback (pg);
      return GNUNET_DB_STATUS_SUCCESS_NO_RESULTS;
    }
  }

  {
    struct GNUNET_TIME_Absolute now;
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (reserve_priv),
      GNUNET_PQ_query_param_auto_from_type (credit_uuid),
      GNUNET_PQ_query_param_absolute_time (&now),
      TALER_PQ_query_param_amount (credit),
      GNUNET_PQ_query_param_end
    };

    now = GNUNET_TIME_absolute_get ();
    (void) GNUNET_TIME_round_abs (&now);
    qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_tip_credit_uuid",
                                             params);
    if (0 > qs)
    {
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
      postgres_rollback (pg);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
        goto RETRY;
      return qs;
    }
  }

  /* Obtain existing reserve balance */
  {
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (reserve_priv),
      GNUNET_PQ_query_param_end
    };
    struct GNUNET_PQ_ResultSpec rs[] = {
      GNUNET_PQ_result_spec_absolute_time ("expiration",
                                           &old_expiration),
      TALER_PQ_RESULT_SPEC_AMOUNT ("balance",
                                   &old_balance),
      GNUNET_PQ_result_spec_end
    };

    qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "lookup_tip_reserve_balance",
                                                   params,
                                                   rs);
  }
  if (0 > qs)
  {
    GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
    postgres_rollback (pg);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      goto RETRY;
    return qs;
  }
  if ( (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs) &&
       (GNUNET_TIME_absolute_get_remaining (old_expiration).rel_value_us > 0) )
  {
    new_expiration = GNUNET_TIME_absolute_max (old_expiration,
                                               expiration);
    if (0 >
        TALER_amount_add (&new_balance,
                          credit,
                          &old_balance))
    {
      GNUNET_break (0);
      postgres_rollback (pg);
      return GNUNET_DB_STATUS_HARD_ERROR;
    }
  }
  else
  {
    if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_INFO,
                  "Old reserve balance of %s had expired at %s, not carrying it over!\n",
                  TALER_amount2s (&old_balance),
                  GNUNET_STRINGS_absolute_time_to_string (old_expiration));
    }
    new_expiration = expiration;
    new_balance = *credit;
  }

  {
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (reserve_priv),
      GNUNET_PQ_query_param_absolute_time (&new_expiration),
      TALER_PQ_query_param_amount (&new_balance),
      GNUNET_PQ_query_param_end
    };
    const char *stmt;

    stmt = (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
           ? "update_tip_reserve_balance"
           : "insert_tip_reserve_balance";
    qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             stmt,
                                             params);
    if (0 > qs)
    {
      GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
      postgres_rollback (pg);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
        goto RETRY;
      return qs;
    }
  }
  qs = postgres_commit (pg);
  if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
    return GNUNET_DB_STATUS_SUCCESS_ONE_RESULT;
  GNUNET_break (GNUNET_DB_STATUS_SOFT_ERROR == qs);
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    goto RETRY;
  return qs;
}


/**
 * Authorize a tip over @a amount from reserve @a reserve_priv.  Remember
 * the authorization under @a tip_id for later, together with the
 * @a justification.
 *
 * @param cls closure, typically a connection to the db
 * @param justification why was the tip approved
 * @param extra extra data for the customer's wallet
 * @param amount how high is the tip (with fees)
 * @param reserve_priv which reserve is debited
 * @param exchange_url which exchange manages the tip
 * @param[out] expiration set to when the tip expires
 * @param[out] tip_id set to the unique ID for the tip
 * @return taler error code
 *      #TALER_EC_TIP_AUTHORIZE_RESERVE_EXPIRED if the reserve is known but has expired
 *      #TALER_EC_TIP_AUTHORIZE_RESERVE_UNKNOWN if the reserve is not known
 *      #TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS if the reserve has insufficient funds left
 *      #TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR on hard DB errors
 *      #TALER_EC_TIP_AUTHORIZE_DB_SOFT_ERROR on soft DB errors (client should retry)
 *      #TALER_EC_NONE upon success
 */
static enum TALER_ErrorCode
postgres_authorize_tip_TR (void *cls,
                           const char *justification,
                           const json_t *extra,
                           const struct TALER_Amount *amount,
                           const struct TALER_ReservePrivateKeyP *reserve_priv,
                           const char *exchange_url,
                           struct GNUNET_TIME_Absolute *expiration,
                           struct GNUNET_HashCode *tip_id)
{
  struct PostgresClosure *pg = cls;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (reserve_priv),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_TIME_Absolute old_expiration;
  struct TALER_Amount old_balance;
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_absolute_time ("expiration",
                                         &old_expiration),
    TALER_PQ_RESULT_SPEC_AMOUNT ("balance",
                                 &old_balance),
    GNUNET_PQ_result_spec_end
  };
  enum GNUNET_DB_QueryStatus qs;
  struct TALER_Amount new_balance;
  unsigned int retries;

  retries = 0;
  check_connection (pg);
RETRY:
  if (MAX_RETRIES < ++retries)
    return TALER_EC_TIP_AUTHORIZE_DB_SOFT_ERROR;
  if (GNUNET_OK !=
      postgres_start (pg,
                      "authorize tip"))
  {
    GNUNET_break (0);
    return TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR;
  }
  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                 "lookup_tip_reserve_balance",
                                                 params,
                                                 rs);
  if (0 >= qs)
  {
    /* reserve unknown */
    postgres_rollback (pg);
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      goto RETRY;
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
      return TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS;
    return TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR;
  }
  if (0 == GNUNET_TIME_absolute_get_remaining (old_expiration).rel_value_us)
  {
    /* reserve expired, can't be used */
    postgres_rollback (pg);
    return TALER_EC_TIP_AUTHORIZE_RESERVE_EXPIRED;
  }
  if (0 >
      TALER_amount_subtract (&new_balance,
                             &old_balance,
                             amount))
  {
    /* insufficient funds left in reserve */
    postgres_rollback (pg);
    return TALER_EC_TIP_AUTHORIZE_INSUFFICIENT_FUNDS;
  }
  /* Update reserve balance */
  {
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (reserve_priv),
      GNUNET_PQ_query_param_absolute_time (&old_expiration),
      TALER_PQ_query_param_amount (&new_balance),
      GNUNET_PQ_query_param_end
    };

    qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "update_tip_reserve_balance",
                                             params);
    if (0 > qs)
    {
      postgres_rollback (pg);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
        goto RETRY;
      return TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR;
    }
  }
  /* Generate and store tip ID */
  *expiration = old_expiration;
  GNUNET_CRYPTO_hash_create_random (GNUNET_CRYPTO_QUALITY_STRONG,
                                    tip_id);
  {
    struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (reserve_priv),
      GNUNET_PQ_query_param_auto_from_type (tip_id),
      GNUNET_PQ_query_param_string (exchange_url),
      GNUNET_PQ_query_param_string (justification),
      TALER_PQ_query_param_json (extra),
      GNUNET_PQ_query_param_absolute_time (&now),
      TALER_PQ_query_param_amount (amount), /* overall amount */
      TALER_PQ_query_param_amount (amount), /* amount left */
      GNUNET_PQ_query_param_end
    };

    (void) GNUNET_TIME_round_abs (&now);
    qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                             "insert_tip_justification",
                                             params);
    if (0 > qs)
    {
      postgres_rollback (pg);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
        goto RETRY;
      return TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR;
    }
  }
  qs = postgres_commit (pg);
  if (0 <= qs)
    return TALER_EC_NONE; /* success! */
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    goto RETRY;
  return TALER_EC_TIP_AUTHORIZE_DB_HARD_ERROR;
}


/**
 * Find out tip authorization details associated with @a tip_id
 *
 * @param cls closure, typically a connection to the d
 * @param tip_id the unique ID for the tip
 * @param[out] exchange_url set to the URL of the exchange (unless NULL)
 * @param[out] extra extra data to pass to the wallet (unless NULL)
 * @param[out] amount set to the authorized amount (unless NULL)
 * @param[out] amount_left set to the amount left (unless NULL)
 * @param[out] timestamp set to the timestamp of the tip authorization (unless NULL)
 * @return transaction status, usually
 *      #GNUNET_DB_STATUS_SUCCESS_ONE_RESULT for success
 *      #GNUNET_DB_STATUS_SUCCESS_NO_RESULTS if @a credit_uuid already known
 */
static enum GNUNET_DB_QueryStatus
postgres_lookup_tip_by_id (void *cls,
                           const struct GNUNET_HashCode *tip_id,
                           char **exchange_url,
                           json_t **extra,
                           struct TALER_Amount *amount,
                           struct TALER_Amount *amount_left,
                           struct GNUNET_TIME_Absolute *timestamp)
{
  struct PostgresClosure *pg = cls;
  char *res_exchange_url;
  json_t *res_extra;
  struct TALER_Amount res_amount;
  struct TALER_Amount res_amount_left;
  struct GNUNET_TIME_Absolute res_timestamp;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (tip_id),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_string ("exchange_url",
                                  &res_exchange_url),
    GNUNET_PQ_result_spec_absolute_time ("timestamp",
                                         &res_timestamp),
    TALER_PQ_result_spec_json ("extra",
                               &res_extra),
    TALER_PQ_RESULT_SPEC_AMOUNT ("amount",
                                 &res_amount),
    TALER_PQ_RESULT_SPEC_AMOUNT ("left",
                                 &res_amount_left),
    GNUNET_PQ_result_spec_end
  };
  enum GNUNET_DB_QueryStatus qs;

  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                 "find_tip_by_id",
                                                 params,
                                                 rs);
  if (0 >= qs)
  {
    if (NULL != exchange_url)
      *exchange_url = NULL;
    return qs;
  }
  if (NULL != exchange_url)
    *exchange_url = strdup (res_exchange_url);
  if (NULL != amount)
    *amount = res_amount;
  if (NULL != amount_left)
    *amount_left = res_amount_left;
  if (NULL != timestamp)
    *timestamp = res_timestamp;
  if (NULL != extra)
  {
    json_incref (res_extra);
    *extra = res_extra;
  }
  GNUNET_PQ_cleanup_result (rs);
  return qs;
}


/**
 * Pickup a tip over @a amount using pickup id @a pickup_id.
 *
 * @param cls closure, typically a connection to the db
 * @param amount how high is the amount picked up (with fees)
 * @param tip_id the unique ID from the tip authorization
 * @param pickup_id the unique ID identifying the pick up operation
 *        (to allow replays, hash over the coin envelope and denomination key)
 * @param[out] reserve_priv which reserve key to use to sign
 * @return taler error code
 *      #TALER_EC_TIP_PICKUP_ID_UNKNOWN if @a tip_id is unknown
 *      #TALER_EC_TIP_PICKUP_NO_FUNDS if @a tip_id has insufficient funds left
 *      #TALER_EC_TIP_PICKUP_DB_ERROR_HARD on hard database errors
 *      #TALER_EC_TIP_PICKUP_AMOUNT_CHANGED if @a amount is different for known @a pickup_id
 *      #TALER_EC_TIP_PICKUP_DB_ERROR_SOFT on soft database errors (client should retry)
 *      #TALER_EC_NONE upon success (@a reserve_priv was set)
 */
static enum TALER_ErrorCode
postgres_pickup_tip_TR (void *cls,
                        const struct TALER_Amount *amount,
                        const struct GNUNET_HashCode *tip_id,
                        const struct GNUNET_HashCode *pickup_id,
                        struct TALER_ReservePrivateKeyP *reserve_priv)
{
  struct PostgresClosure *pg = cls;
  struct TALER_Amount left_amount;
  struct GNUNET_PQ_QueryParam params[] = {
    GNUNET_PQ_query_param_auto_from_type (tip_id),
    GNUNET_PQ_query_param_end
  };
  struct GNUNET_PQ_ResultSpec rs[] = {
    GNUNET_PQ_result_spec_auto_from_type ("reserve_priv",
                                          reserve_priv),
    TALER_PQ_RESULT_SPEC_AMOUNT ("left",
                                 &left_amount),
    GNUNET_PQ_result_spec_end
  };
  enum GNUNET_DB_QueryStatus qs;
  unsigned int retries;

  retries = 0;
  check_connection (pg);
RETRY:
  if (MAX_RETRIES < ++retries)
    return TALER_EC_TIP_PICKUP_DB_ERROR_SOFT;
  if (GNUNET_OK !=
      postgres_start (pg,
                      "pickup tip"))
  {
    GNUNET_break (0);
    return TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
  }
  qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                 "lookup_reserve_by_tip_id",
                                                 params,
                                                 rs);
  if (0 >= qs)
  {
    /* tip ID unknown */
    memset (reserve_priv,
            0,
            sizeof (*reserve_priv));
    postgres_rollback (pg);
    if (GNUNET_DB_STATUS_SUCCESS_NO_RESULTS == qs)
      return TALER_EC_TIP_PICKUP_TIP_ID_UNKNOWN;
    if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
      goto RETRY;
    return TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
  }

  /* Check if pickup_id already exists */
  {
    struct TALER_Amount existing_amount;
    struct GNUNET_PQ_QueryParam params[] = {
      GNUNET_PQ_query_param_auto_from_type (pickup_id),
      GNUNET_PQ_query_param_auto_from_type (tip_id),
      GNUNET_PQ_query_param_end
    };
    struct GNUNET_PQ_ResultSpec rs[] = {
      TALER_PQ_RESULT_SPEC_AMOUNT ("amount",
                                   &existing_amount),
      GNUNET_PQ_result_spec_end
    };

    qs = GNUNET_PQ_eval_prepared_singleton_select (pg->conn,
                                                   "lookup_amount_by_pickup",
                                                   params,
                                                   rs);
    if (0 > qs)
    {
      /* DB error */
      memset (reserve_priv,
              0,
              sizeof (*reserve_priv));
      postgres_rollback (pg);
      if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
        goto RETRY;
      return TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
    }
    if (GNUNET_DB_STATUS_SUCCESS_ONE_RESULT == qs)
    {
      if (0 !=
          TALER_amount_cmp (&existing_amount,
                            amount))
      {
        GNUNET_break_op (0);
        postgres_rollback (pg);
        return TALER_EC_TIP_PICKUP_AMOUNT_CHANGED;
      }
      postgres_commit (pg);
      return TALER_EC_NONE; /* we are done! */
    }
  }

  /* Calculate new balance */
  {
    struct TALER_Amount new_left;

    if (0 >
        TALER_amount_subtract (&new_left,
                               &left_amount,
                               amount))
    {
      /* attempt to take more tips than the tipping amount */
      GNUNET_break_op (0);
      memset (reserve_priv,
              0,
              sizeof (*reserve_priv));
      postgres_rollback (pg);
      return TALER_EC_TIP_PICKUP_NO_FUNDS;
    }

    /* Update DB: update balance */
    {
      struct GNUNET_PQ_QueryParam params[] = {
        GNUNET_PQ_query_param_auto_from_type (tip_id),
        TALER_PQ_query_param_amount (&new_left),
        GNUNET_PQ_query_param_end
      };

      qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                               "update_tip_balance",
                                               params);
      if (0 > qs)
      {
        postgres_rollback (pg);
        memset (reserve_priv,
                0,
                sizeof (*reserve_priv));
        if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
          goto RETRY;
        return TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
      }
    }

    /* Update DB: remember pickup_id */
    {
      struct GNUNET_PQ_QueryParam params[] = {
        GNUNET_PQ_query_param_auto_from_type (tip_id),
        GNUNET_PQ_query_param_auto_from_type (pickup_id),
        TALER_PQ_query_param_amount (amount),
        GNUNET_PQ_query_param_end
      };

      qs = GNUNET_PQ_eval_prepared_non_select (pg->conn,
                                               "insert_pickup_id",
                                               params);
      if (0 > qs)
      {
        postgres_rollback (pg);
        memset (reserve_priv,
                0,
                sizeof (*reserve_priv));
        if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
          goto RETRY;
        return TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
      }
    }
  }
  qs = postgres_commit (pg);
  if (0 <= qs)
    return TALER_EC_NONE; /* success  */
  if (GNUNET_DB_STATUS_SOFT_ERROR == qs)
    goto RETRY;
  return TALER_EC_TIP_PICKUP_DB_ERROR_HARD;
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
  const struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  struct PostgresClosure *pg;
  struct TALER_MERCHANTDB_Plugin *plugin;
  struct GNUNET_PQ_PreparedStatement ps[] = {
    GNUNET_PQ_make_prepare ("end_transaction",
                            "COMMIT",
                            0),
    /* for call_with_accounts(), part of postgres_lookup_instances() */
    GNUNET_PQ_make_prepare ("lookup_instance_private_key",
                            "SELECT"
                            " merchant_priv"
                            " FROM merchant_keys"
                            " WHERE merchant_serial=$1",
                            1),
    /* for find_instances_cb(), part of postgres_lookup_instances() */
    GNUNET_PQ_make_prepare ("lookup_accounts",
                            "SELECT"
                            " h_wire"
                            ",salt"
                            ",payto_uri"
                            ",active"
                            " FROM merchant_accounts"
                            " WHERE merchant_serial=$1",
                            1),
    /* for postgres_lookup_instances() */
    GNUNET_PQ_make_prepare ("lookup_instances",
                            "SELECT"
                            " merchant_serial"
                            ",merchant_pub"
                            ",merchant_id"
                            ",merchant_name"
                            ",address"
                            ",jurisdiction"
                            ",default_max_deposit_fee_val"
                            ",default_max_deposit_fee_frac"
                            ",default_max_wire_fee_val"
                            ",default_max_wire_fee_frac"
                            ",default_wire_fee_amortization"
                            ",default_wire_transfer_delay"
                            ",default_pay_delay"
                            " FROM merchant_instances",
                            0),
    /* for postgres_insert_instance() */
    GNUNET_PQ_make_prepare ("insert_instance",
                            "INSERT INTO merchant_instances"
                            "(merchant_pub"
                            ",merchant_id"
                            ",merchant_name"
                            ",address"
                            ",jurisdiction"
                            ",default_max_deposit_fee_val"
                            ",default_max_deposit_fee_frac"
                            ",default_max_wire_fee_val"
                            ",default_max_wire_fee_frac"
                            ",default_wire_fee_amortization"
                            ",default_wire_transfer_delay"
                            ",default_pay_delay)"
                            "VALUES"
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
                            12),
    /* for postgres_insert_instance() */
    GNUNET_PQ_make_prepare ("insert_keys",
                            "INSERT INTO merchant_keys"
                            "(merchant_priv"
                            ",merchant_serial)"
                            " SELECT $1, merchant_serial"
                            " FROM merchant_instances"
                            " WHERE merchant_id=$2",
                            2),
    /* for postgres_insert_account() */
    GNUNET_PQ_make_prepare ("insert_account",
                            "INSERT INTO merchant_accounts"
                            "(merchant_serial"
                            ",h_wire"
                            ",salt"
                            ",payto_uri"
                            ",active)"
                            " SELECT merchant_serial, $2, $3, $4, $5"
                            " FROM merchant_instances"
                            " WHERE merchant_id=$1",
                            5),
    /* for postgres_delete_instance_private_key() */
    GNUNET_PQ_make_prepare ("delete_key",
                            "DELETE FROM merchant_keys"
                            " USING merchant_instances"
                            " WHERE merchant_keys.merchant_serial"
                            "   = merchant_instances.merchant_serial"
                            " AND merchant_instances.merchant_id = $1",
                            1),
    /* for postgres_purge_instance() */
    GNUNET_PQ_make_prepare ("purge_keys",
                            "DELETE FROM merchant_instances"
                            " WHERE merchant_instances.merchant_id = $1",
                            1),
    /* for postgres_update_instance() */
    GNUNET_PQ_make_prepare ("update_instance",
                            "UPDATE merchant_instances SET"
                            " merchant_name=$2"
                            ",address=$3"
                            ",jurisdiction=$4"
                            ",default_max_deposit_fee_val=$5"
                            ",default_max_deposit_fee_frac=$6"
                            ",default_max_wire_fee_val=$7"
                            ",default_max_wire_fee_frac=$8"
                            ",default_wire_fee_amortization=$9"
                            ",default_wire_transfer_delay=$10"
                            ",default_pay_delay=$11"
                            " WHERE merchant_id = $1",
                            11),
    /* for postgres_inactivate_account() */
    GNUNET_PQ_make_prepare ("inactivate_account",
                            "UPDATE merchant_accounts SET"
                            " active=FALSE"
                            " WHERE h_wire = $1",
                            1),
    /* for postgres_lookup_products() */
    GNUNET_PQ_make_prepare ("lookup_products",
                            "SELECT"
                            " product_id"
                            " FROM merchant_inventory"
                            " JOIN merchant_instances"
                            "   USING (merchant_serial)"
                            " WHERE merchant_instances.merchant_id=$1",
                            1),
    /* for postgres_lookup_product() */
    GNUNET_PQ_make_prepare ("lookup_product",
                            "SELECT"
                            " description"
                            ",description_i18n"
                            ",unit"
                            ",price_val"
                            ",price_frac"
                            ",taxes"
                            ",total_stock"
                            ",total_sold"
                            ",total_lost"
                            ",image"
                            ",merchant_inventory.address"
                            ",next_restock"
                            " FROM merchant_inventory"
                            " JOIN merchant_instances"
                            "   USING (merchant_serial)"
                            " WHERE merchant_instances.merchant_id=$1"
                            "   AND merchant_inventory.product_id=$2",
                            2),
    /* for postgres_delete_product() */
    GNUNET_PQ_make_prepare ("delete_product",
                            "DELETE"
                            " FROM merchant_inventory"
                            " WHERE merchant_inventory.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND merchant_inventory.product_id=$2"
                            "   AND product_serial NOT IN "
                            "     (SELECT product_serial FROM merchant_order_locks)"
                            "   AND product_serial NOT IN "
                            "     (SELECT product_serial FROM merchant_inventory_locks)",
                            2),
    /* for postgres_insert_product() */
    GNUNET_PQ_make_prepare ("insert_product",
                            "INSERT INTO merchant_inventory"
                            "(merchant_serial"
                            ",product_id"
                            ",description"
                            ",description_i18n"
                            ",unit"
                            ",image"
                            ",taxes"
                            ",price_val"
                            ",price_frac"
                            ",total_stock"
                            ",address"
                            ",next_restock)"
                            " SELECT merchant_serial,"
                            " $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12"
                            " FROM merchant_instances"
                            " WHERE merchant_id=$1",
                            12),
    /* for postgres_update_product() */
    GNUNET_PQ_make_prepare ("update_product",
                            "UPDATE merchant_inventory SET"
                            " description=$3"
                            ",description_i18n=$4"
                            ",unit=$5"
                            ",image=$6"
                            ",taxes=$7"
                            ",price_val=$8"
                            ",price_frac=$9"
                            ",total_stock=$10"
                            ",total_lost=$11"
                            ",address=$12"
                            ",next_restock=$13"
                            " WHERE merchant_serial="
                            "   (SELECT merchant_serial"
                            "      FROM merchant_instances"
                            "      WHERE merchant_id=$1)"
                            "   AND product_id=$2"
                            "   AND total_stock <= $10"
                            "   AND $10 - total_sold >= $11"
                            "   AND total_lost <= $11",
                            13),

    /* for postgres_lock_product() */
    GNUNET_PQ_make_prepare ("lock_product",
                            "WITH ps AS"
                            "  (SELECT product_serial"
                            "   FROM merchant_inventory"
                            "   WHERE product_id=$2"
                            "     AND merchant_serial="
                            "     (SELECT merchant_serial"
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1))"
                            "INSERT INTO merchant_inventory_locks"
                            "(product_serial"
                            ",lock_uuid"
                            ",total_locked"
                            ",expiration)"
                            " SELECT product_serial, $3, $4, $5"
                            "   FROM merchant_inventory"
                            "   JOIN ps USING (product_serial)"
                            "   WHERE "
                            "     total_stock - total_sold - total_lost - $4 >= "
                            "     (SELECT SUM(total_locked)"
                            "        FROM merchant_inventory_locks"
                            "        WHERE product_serial=ps.product_serial) + "
                            "     (SELECT SUM(total_locked)"
                            "        FROM merchant_order_locks"
                            "        WHERE product_serial=ps.product_serial)",
                            5),
    /* for postgres_delete_order() */
    GNUNET_PQ_make_prepare ("delete_order",
                            "DELETE"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND merchant_orders.order_id=$2"
                            "   AND pay_deadline < $3",
                            3),
    /* for postgres_lookup_order() */
    GNUNET_PQ_make_prepare ("lookup_order",
                            "SELECT"
                            " contract_terms"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND merchant_orders.order_id=$2",
                            2),
    /* for postgres_lookup_orders() */
    GNUNET_PQ_make_prepare ("lookup_orders_inc",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_paid",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_refunded",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_paid_refunded",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_paid_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_refunded_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_inc_paid_refunded_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial > $3"
                            "   AND"
                            "    creation_time > $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2)"
                            " ORDER BY order_serial ASC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_paid",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_refunded",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_paid_refunded",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_paid_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($6)" /* otherwise $6 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_refunded_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            ",BOOL($5)" /* otherwise $5 is unused and Postgres unhappy */
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    GNUNET_PQ_make_prepare ("lookup_orders_dec_paid_refunded_wired",
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_orders"
                            " WHERE merchant_orders.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    NOT BOOL($5)" /* unclaimed orders are never paid */
                            "   AND"
                            "    NOT BOOL ($6)"/* unclaimed orders are never refunded */
                            "   AND"
                            "    NOT BOOL ($7)" /* unclaimed orders are never wired */
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            "UNION " /* union ensures elements are distinct! */
                            "(SELECT"
                            " order_id"
                            ",order_serial"
                            ",creation_time"
                            " FROM merchant_contract_terms"
                            " WHERE merchant_contract_terms.merchant_serial="
                            "     (SELECT merchant_serial "
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND"
                            "    order_serial < $3"
                            "   AND"
                            "    creation_time < $4"
                            "   AND"
                            "    BOOL($5) = paid"
                            "   AND"
                            "    BOOL($6) = (order_serial IN"
                            "     (SELECT order_serial "
                            "      FROM merchant_refunds))"
                            "   AND"
                            "    BOOL($7) = wired"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2)"
                            " ORDER BY order_serial DESC"
                            " LIMIT $2",
                            7),
    /* for postgres_insert_order() */
    GNUNET_PQ_make_prepare ("insert_order",
                            "INSERT INTO merchant_orders"
                            "(merchant_serial"
                            ",order_id"
                            ",pay_deadline"
                            ",creation_time"
                            ",contract_terms)"
                            " SELECT merchant_serial,"
                            " $2, $3, $4, $5"
                            " FROM merchant_instances"
                            " WHERE merchant_id=$1",
                            5),
    /* for postgres_unlock_inventory() */
    GNUNET_PQ_make_prepare ("unlock_inventory",
                            "DELETE"
                            " FROM merchant_inventory_locks"
                            " WHERE lock_uuid=$1",
                            1),
    /* for postgres_insert_order_lock() */
    GNUNET_PQ_make_prepare ("insert_order_lock",
                            "WITH tmp AS"
                            "  (SELECT "
                            "      product_serial"
                            "     ,merchant_serial"
                            "     ,total_stock"
                            "     ,total_sold"
                            "     ,total_lost"
                            "   FROM merchant_inventory"
                            "   WHERE product_id=$3"
                            "     AND merchant_serial="
                            "     (SELECT merchant_serial"
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1))"
                            " INSERT INTO merchant_order_locks"
                            " (product_serial"
                            " ,total_locked"
                            " ,order_serial)"
                            " SELECT tmp.product_serial, $4, order_serial"
                            "   FROM merchant_orders"
                            "   JOIN tmp USING(merchant_serial)"
                            "   WHERE order_id=$2 AND"
                            "     tmp.total_stock - tmp.total_sold - tmp.total_lost - $4 >= "
                            "     (SELECT SUM(total_locked)"
                            "        FROM merchant_inventory_locks"
                            "        WHERE product_serial=tmp.product_serial) + "
                            "     (SELECT SUM(total_locked)"
                            "        FROM merchant_order_locks"
                            "        WHERE product_serial=tmp.product_serial)",
                            4),
    /* for postgres_lookup_contract_terms() */
    GNUNET_PQ_make_prepare ("lookup_contract_terms",
                            "SELECT contract_terms"
                            " FROM merchant_contract_terms"
                            " WHERE order_id=$2"
                            "   AND merchant_serial="
                            "     (SELECT merchant_serial"
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)",
                            2),
    /* for postgres_insert_contract_terms() */
    GNUNET_PQ_make_prepare ("insert_contract_terms",
                            "INSERT INTO merchant_contract_terms"
                            "(order_serial"
                            ",merchant_serial"
                            ",order_id"
                            ",contract_terms"
                            ",h_contract_terms"
                            ",creation_time"
                            ",pay_deadline"
                            ",refund_deadline"
                            ",fulfillment_url)"
                            "SELECT"
                            " order_serial"
                            ",merchant_serial"
                            ",order_id"
                            ",$3"  /* contract_terms */
                            ",$4"  /* h_contract_terms */
                            ",creation_time"
                            ",$5" /* pay_deadline */
                            ",$6" /* refund_deadline */
                            ",$7" /* fulfillment_url */
                            "FROM merchant_orders"
                            " WHERE order_id=$2"
                            "   AND merchant_serial="
                            "     (SELECT merchant_serial"
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)",
                            7),
    /* for postgres_delete_contract_terms() */
    GNUNET_PQ_make_prepare ("delete_contract_terms",
                            "DELETE FROM merchant_contract_terms"
                            " WHERE order_id=$2"
                            "   AND merchant_serial="
                            "     (SELECT merchant_serial"
                            "        FROM merchant_instances"
                            "        WHERE merchant_id=$1)"
                            "   AND ( ( (pay_deadline < $4) AND"
                            "           (NOT paid) ) OR"
                            "         (creation_time + $3 > $4) )",
                            4),

    /* OLD API: */

#if 0
    GNUNET_PQ_make_prepare ("insert_deposit",
                            "INSERT INTO merchant_deposits"
                            "(h_contract_terms"
                            ",merchant_pub"
                            ",coin_pub"
                            ",exchange_url"
                            ",amount_with_fee_val"
                            ",amount_with_fee_frac"
                            ",deposit_fee_val"
                            ",deposit_fee_frac"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",wire_fee_val"
                            ",wire_fee_frac"
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
                            ") VALUES"
                            "($1, $2, $3, $4, $5, $6)",
                            6),
    GNUNET_PQ_make_prepare ("insert_proof",
                            "INSERT INTO merchant_proofs"
                            "(exchange_url"
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
                            5),
    GNUNET_PQ_make_prepare ("insert_session_info",
                            "INSERT INTO merchant_session_info"
                            "(session_id"
                            ",fulfillment_url"
                            ",order_id"
                            ",merchant_pub"
                            ",timestamp)"
                            " VALUES "
                            "($1, $2, $3, $4, $5)",
                            5),
    GNUNET_PQ_make_prepare ("mark_proposal_paid",
                            "UPDATE merchant_contract_terms SET"
                            " paid=TRUE"
                            " WHERE h_contract_terms=$1"
                            " AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("insert_wire_fee",
                            "INSERT INTO exchange_wire_fees"
                            "(exchange_pub"
                            ",h_wire_method"
                            ",wire_fee_val"
                            ",wire_fee_frac"
                            ",closing_fee_val"
                            ",closing_fee_frac"
                            ",start_date"
                            ",end_date"
                            ",exchange_sig)"
                            " VALUES "
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                            9),
    GNUNET_PQ_make_prepare ("lookup_wire_fee",
                            "SELECT"
                            " wire_fee_val"
                            ",wire_fee_frac"
                            ",closing_fee_val"
                            ",closing_fee_frac"
                            ",start_date"
                            ",end_date"
                            ",exchange_sig"
                            " FROM exchange_wire_fees"
                            " WHERE exchange_pub=$1"
                            "   AND h_wire_method=$2"
                            "   AND start_date <= $3"
                            "   AND end_date > $3",
                            1),
    GNUNET_PQ_make_prepare ("find_contract_terms_from_hash",
                            "SELECT"
                            " contract_terms"
                            " FROM merchant_contract_terms"
                            " WHERE h_contract_terms=$1"
                            "   AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_paid_contract_terms_from_hash",
                            "SELECT"
                            " contract_terms"
                            " FROM merchant_contract_terms"
                            " WHERE h_contract_terms=$1"
                            "   AND merchant_pub=$2"
                            "   AND paid=TRUE",
                            2),
    GNUNET_PQ_make_prepare ("find_refunds",
                            "SELECT"
                            " refund_amount_val"
                            ",refund_amount_frac"
                            " FROM merchant_refunds"
                            " WHERE coin_pub=$1",
                            1),
    GNUNET_PQ_make_prepare ("find_session_info",
                            "SELECT"
                            " order_id"
                            " FROM merchant_session_info"
                            " WHERE"
                            " fulfillment_url=$1"
                            " AND session_id=$2"
                            " AND merchant_pub=$3",
                            2),
    GNUNET_PQ_make_prepare ("find_refunds_from_contract_terms_hash",
                            "SELECT"
                            " coin_pub"
                            ",merchant_deposits.exchange_url"
                            ",rtransaction_id"
                            ",refund_amount_val"
                            ",refund_amount_frac"
                            ",merchant_deposits.refund_fee_val"
                            ",merchant_deposits.refund_fee_frac"
                            ",reason"
                            " FROM merchant_refunds"
                            "   JOIN merchant_deposits USING (merchant_pub, coin_pub)"
                            " WHERE merchant_refunds.merchant_pub=$1"
                            "   AND merchant_refunds.h_contract_terms=$2",
                            2),
    GNUNET_PQ_make_prepare ("get_refund_proof",
                            "SELECT"
                            " exchange_pub"
                            ",exchange_sig"
                            " FROM merchant_refund_proofs"
                            " WHERE"
                            " h_contract_terms=$1"
                            " AND merchant_pub=$2"
                            " AND coin_pub=$3"
                            " AND rtransaction_id=$4",
                            4),
    GNUNET_PQ_make_prepare ("insert_refund_proof",
                            "INSERT INTO merchant_refund_proofs"
                            "(rtransaction_id"
                            ",merchant_pub"
                            ",h_contract_terms"
                            ",coin_pub"
                            ",exchange_sig"
                            ",exchange_pub)"
                            " VALUES "
                            "($1, $2, $3, $4, $5, $6)",
                            6),
    GNUNET_PQ_make_prepare ("find_deposits",
                            "SELECT"
                            " coin_pub"
                            ",exchange_url"
                            ",amount_with_fee_val"
                            ",amount_with_fee_frac"
                            ",deposit_fee_val"
                            ",deposit_fee_frac"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",wire_fee_val"
                            ",wire_fee_frac"
                            ",exchange_proof"
                            " FROM merchant_deposits"
                            " WHERE h_contract_terms=$1"
                            " AND merchant_pub=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_deposits_by_hash_and_coin",
                            "SELECT"
                            " amount_with_fee_val"
                            ",amount_with_fee_frac"
                            ",deposit_fee_val"
                            ",deposit_fee_frac"
                            ",refund_fee_val"
                            ",refund_fee_frac"
                            ",wire_fee_val"
                            ",wire_fee_frac"
                            ",exchange_url"
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
                            ",merchant_deposits.deposit_fee_val"
                            ",merchant_deposits.deposit_fee_frac"
                            ",merchant_deposits.refund_fee_val"
                            ",merchant_deposits.refund_fee_frac"
                            ",merchant_deposits.wire_fee_val"
                            ",merchant_deposits.wire_fee_frac"
                            ",merchant_deposits.exchange_url"
                            ",merchant_deposits.exchange_proof"
                            " FROM merchant_transfers"
                            "   JOIN merchant_deposits"
                            "     USING (h_contract_terms,coin_pub)"
                            " WHERE wtid=$1",
                            1),
    GNUNET_PQ_make_prepare ("find_proof_by_wtid",
                            "SELECT"
                            " proof"
                            " FROM merchant_proofs"
                            " WHERE wtid=$1"
                            "  AND exchange_url=$2",
                            2),
    GNUNET_PQ_make_prepare ("lookup_tip_reserve_balance",
                            "SELECT"
                            " expiration"
                            ",balance_val"
                            ",balance_frac"
                            " FROM merchant_tip_reserves"
                            " WHERE reserve_priv=$1",
                            1),
    GNUNET_PQ_make_prepare ("find_tip_authorizations",
                            "SELECT"
                            " amount_val"
                            ",amount_frac"
                            ",justification"
                            ",extra"
                            ",tip_id"
                            " FROM merchant_tips"
                            " WHERE reserve_priv=$1",
                            1),
    GNUNET_PQ_make_prepare ("update_tip_reserve_balance",
                            "UPDATE merchant_tip_reserves SET"
                            " expiration=$2"
                            ",balance_val=$3"
                            ",balance_frac=$4"
                            " WHERE reserve_priv=$1",
                            4),
    GNUNET_PQ_make_prepare ("insert_tip_reserve_balance",
                            "INSERT INTO merchant_tip_reserves"
                            "(reserve_priv"
                            ",expiration"
                            ",balance_val"
                            ",balance_frac"
                            ") VALUES "
                            "($1, $2, $3, $4)",
                            4),
    GNUNET_PQ_make_prepare ("insert_tip_justification",
                            "INSERT INTO merchant_tips"
                            "(reserve_priv"
                            ",tip_id"
                            ",exchange_url"
                            ",justification"
                            ",extra"
                            ",timestamp"
                            ",amount_val"
                            ",amount_frac"
                            ",left_val"
                            ",left_frac"
                            ") VALUES "
                            "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
                            10),
    GNUNET_PQ_make_prepare ("lookup_reserve_by_tip_id",
                            "SELECT"
                            " reserve_priv"
                            ",left_val"
                            ",left_frac"
                            " FROM merchant_tips"
                            " WHERE tip_id=$1",
                            1),
    GNUNET_PQ_make_prepare ("lookup_amount_by_pickup",
                            "SELECT"
                            " amount_val"
                            ",amount_frac"
                            " FROM merchant_tip_pickups"
                            " WHERE pickup_id=$1"
                            " AND tip_id=$2",
                            2),
    GNUNET_PQ_make_prepare ("find_tip_by_id",
                            "SELECT"
                            " exchange_url"
                            ",extra"
                            ",timestamp"
                            ",amount_val"
                            ",amount_frac"
                            ",left_val"
                            ",left_frac"
                            " FROM merchant_tips"
                            " WHERE tip_id=$1",
                            1),
    GNUNET_PQ_make_prepare ("update_tip_balance",
                            "UPDATE merchant_tips SET"
                            " left_val=$2"
                            ",left_frac=$3"
                            " WHERE tip_id=$1",
                            3),
    GNUNET_PQ_make_prepare ("insert_pickup_id",
                            "INSERT INTO merchant_tip_pickups"
                            "(tip_id"
                            ",pickup_id"
                            ",amount_val"
                            ",amount_frac"
                            ") VALUES "
                            "($1, $2, $3, $4)",
                            4),
    GNUNET_PQ_make_prepare ("insert_tip_credit_uuid",
                            "INSERT INTO merchant_tip_reserve_credits"
                            "(reserve_priv"
                            ",credit_uuid"
                            ",timestamp"
                            ",amount_val"
                            ",amount_frac"
                            ") VALUES "
                            "($1, $2, $3, $4, $5)",
                            5),
    GNUNET_PQ_make_prepare ("lookup_tip_credit_uuid",
                            "SELECT 1 "
                            "FROM merchant_tip_reserve_credits "
                            "WHERE credit_uuid=$1 AND reserve_priv=$2",
                            2),
#endif
    GNUNET_PQ_PREPARED_STATEMENT_END
  };

  pg = GNUNET_new (struct PostgresClosure);
  pg->cfg = cfg;
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_get_value_filename (cfg,
                                               "merchantdb-postgres",
                                               "SQL_DIR",
                                               &pg->sql_dir))
  {
    GNUNET_log_config_missing (GNUNET_ERROR_TYPE_ERROR,
                               "merchantdb-postgres",
                               "SQL_DIR");
    GNUNET_free (pg);
    return NULL;
  }
  pg->conn = GNUNET_PQ_connect_with_cfg (cfg,
                                         "merchantdb-postgres",
                                         "merchant-",
                                         NULL,
                                         ps);
  if (NULL == pg->conn)
  {
    GNUNET_free (pg->sql_dir);
    GNUNET_free (pg);
    return NULL;
  }
  if (GNUNET_OK !=
      TALER_config_get_currency (cfg,
                                 &pg->currency))
  {
    GNUNET_PQ_disconnect (pg->conn);
    GNUNET_free (pg->sql_dir);
    GNUNET_free (pg);
    return NULL;
  }
  plugin = GNUNET_new (struct TALER_MERCHANTDB_Plugin);
  plugin->cls = pg;
  plugin->drop_tables = &postgres_drop_tables;
  plugin->preflight = &postgres_preflight;
  plugin->start = &postgres_start;
  plugin->rollback = &postgres_rollback;
  plugin->commit = &postgres_commit;
  plugin->lookup_instances = &postgres_lookup_instances;
  plugin->insert_instance = &postgres_insert_instance;
  plugin->insert_account = &postgres_insert_account;
  plugin->delete_instance_private_key = &postgres_delete_instance_private_key;
  plugin->purge_instance = &postgres_purge_instance;
  plugin->update_instance = &postgres_update_instance;
  plugin->inactivate_account = &postgres_inactivate_account;
  plugin->lookup_products = &postgres_lookup_products;
  plugin->lookup_product = &postgres_lookup_product;
  plugin->delete_product = &postgres_delete_product;
  plugin->insert_product = &postgres_insert_product;
  plugin->update_product = &postgres_update_product;
  plugin->lock_product = &postgres_lock_product;
  plugin->delete_order = &postgres_delete_order;
  plugin->lookup_order = &postgres_lookup_order;
  plugin->lookup_orders = &postgres_lookup_orders;
  plugin->insert_order = &postgres_insert_order;
  plugin->unlock_inventory = &postgres_unlock_inventory;
  plugin->insert_order_lock = &postgres_insert_order_lock;
  plugin->lookup_contract_terms = &postgres_lookup_contract_terms;
  plugin->insert_contract_terms = &postgres_insert_contract_terms;
  plugin->delete_contract_terms = &postgres_delete_contract_terms;
  /* OLD API: */
  plugin->find_contract_terms_from_hash =
    &postgres_find_contract_terms_from_hash;
  plugin->find_paid_contract_terms_from_hash =
    &postgres_find_paid_contract_terms_from_hash;
  plugin->store_deposit = &postgres_store_deposit;
  plugin->store_coin_to_transfer = &postgres_store_coin_to_transfer;
  plugin->store_transfer_to_proof = &postgres_store_transfer_to_proof;
  plugin->store_wire_fee_by_exchange = &postgres_store_wire_fee_by_exchange;
  plugin->find_payments_by_hash_and_coin =
    &postgres_find_payments_by_hash_and_coin;
  plugin->find_payments = &postgres_find_payments;
  plugin->find_transfers_by_hash = &postgres_find_transfers_by_hash;
  plugin->find_deposits_by_wtid = &postgres_find_deposits_by_wtid;
  plugin->find_proof_by_wtid = &postgres_find_proof_by_wtid;
  plugin->get_authorized_tip_amount = &postgres_get_authorized_tip_amount;
  plugin->get_refunds_from_contract_terms_hash =
    &postgres_get_refunds_from_contract_terms_hash;
  plugin->lookup_wire_fee = &postgres_lookup_wire_fee;
  plugin->increase_refund_for_contract_NT =
    &postgres_increase_refund_for_contract_NT;
  plugin->get_refund_proof = &postgres_get_refund_proof;
  plugin->put_refund_proof = &postgres_put_refund_proof;
  plugin->mark_proposal_paid = &postgres_mark_proposal_paid;
  plugin->insert_session_info = &postgres_insert_session_info;
  plugin->find_session_info = &postgres_find_session_info;
  plugin->enable_tip_reserve_TR = &postgres_enable_tip_reserve_TR;
  plugin->authorize_tip_TR = &postgres_authorize_tip_TR;
  plugin->lookup_tip_by_id = &postgres_lookup_tip_by_id;
  plugin->pickup_tip_TR = &postgres_pickup_tip_TR;

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

  GNUNET_PQ_disconnect (pg->conn);
  GNUNET_free (pg->sql_dir);
  GNUNET_free (pg->currency);
  GNUNET_free (pg);
  GNUNET_free (plugin);
  return NULL;
}


/* end of plugin_merchantdb_postgres.c */
