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
* @file merchant/tests/merchant-non-http-test.c
* @brief test for various merchant's capabilities
* @author Marcello Stanisci
*/

#include "platform.h"
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_json_lib.h>
#include "merchant_db.h"
#include <taler_merchant_lib.h>

PGconn *db_conn;

static int dry;
struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;
char *keyfile;
static int result;
static struct MERCHANT_WIREFORMAT_Sepa *wire;
static struct GNUNET_SCHEDULER_Task *shutdown_task;

extern
struct MERCHANT_WIREFORMAT_Sepa *
TALER_MERCHANT_parse_wireformat_sepa (const struct GNUNET_CONFIGURATION_Handle *cfg);

/**
 * Shutdown task (magically invoked when the application is being
 * quit)
 *
 * @param cls NULL
 * @param tc scheduler task context
 */
static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{

  if (NULL != db_conn)
    {
      MERCHANT_DB_disconnect (db_conn);
      db_conn = NULL;
    }
}

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)

{
  json_t *j_fake_contract;
  json_t *j_root;
  json_t *j_details;
  json_t *j_item;
  json_t *j_amount;
  json_t *j_tax_amount;
  json_t *j_item_price;
  json_t *j_teatax;
  json_t *j_id; // trans id
  json_t *j_pid; // product id
  json_t *j_quantity;
  json_t *j_delloc;
  json_t *j_merchant;
  json_t *j_merchant_jurisdiction;
  json_t *j_merchant_zipcode;
  json_t *j_lnames;
  json_t *j_deldate;
  char *str;
  char *desc;
  struct TALER_Amount amount;
  int64_t t_id;
  int64_t p_id;
  struct ContractNBO contract;
  struct GNUNET_TIME_Absolute deldate;

  db_conn = NULL;
  keyfile = NULL;
  privkey = NULL;
  wire = NULL;


  db_conn = MERCHANT_DB_connect (config);
  if (GNUNET_OK != MERCHANT_DB_initialize (db_conn, GNUNET_NO))
  {
    printf ("no db init'd\n");
    result = GNUNET_SYSERR;
  }
  if (GNUNET_OK != GNUNET_CONFIGURATION_get_value_filename (config,
                                                            "merchant",
                                                            "KEYFILE",
                                                            &keyfile))
  {
    printf ("no keyfile entry in cfg file\n");
    result = GNUNET_SYSERR;
  }  

  privkey = GNUNET_CRYPTO_eddsa_key_create_from_file (keyfile); 
  wire = TALER_MERCHANT_parse_wireformat_sepa (config);
  shutdown_task = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
                                                &do_shutdown, NULL);

  /**
  * 'Root' object of the contract, leaving some holes to bi filled
  * up by the merchant library.
  *
  */

  /* Amount */
  TALER_amount_get_zero ("KUDOS", &amount);
  j_amount = TALER_json_from_amount (&amount);

  /* Transaction ID*/
  t_id = (int32_t) GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK, UINT64_MAX);

  if (t_id < 0)
     j_id = json_integer ((-1) * t_id);
  else
     j_id = json_integer (t_id);

  /**
  * Holes:
  *
  * - 'h_wire' 
  * - 'timestamp'
  *
  */
  
  /**
  *
  * Preparing the 'details' sub-object: an array of 'item' objects
  * plus some juridical and delivery-aware informations
  *
  */
  
  /**
  *
  * Preparing a 'item' sub-object
  */
  
  /* Description */
  desc = "Fake purchase";

  /* Quantity: OPTIONAL FIELD */
  j_quantity = json_integer (3);

  /* item price: OPTIONAL FIELD*/
  j_item_price = TALER_json_from_amount (&amount);
  
  /* Product ID */
  p_id = (int32_t) GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_WEAK, UINT64_MAX);

  if (p_id < 0)
     j_pid = json_integer ((-1) * p_id);
  else
     j_pid = json_integer (p_id);

  /* Taxes: array of "tax_name" : {tax amount} */
  j_tax_amount = TALER_json_from_amount (&amount);
  j_teatax = json_pack ("{s:o}",
                        "teatax", j_tax_amount);

  if (NULL == (j_item = json_pack ("{s:s, s:I, s:o, s:I, s:[o]}",
                      "description", desc,
		      "quantity", json_integer_value (j_quantity),
		      "itemprice", j_item_price,
		      "product_id", json_integer_value (j_pid),
		      "taxes", j_teatax)))
  {
    printf ("error in packing [j_item: %p]\n", j_item);
    return;
  }
  
  /* End of 'item' object definition */

  printf ("[j_item address: %p]\n", j_item);

  /* Delivery date: OPTIONAL FIELD */
  deldate = GNUNET_TIME_absolute_add (GNUNET_TIME_absolute_get (),
                                      GNUNET_TIME_UNIT_WEEKS);
  j_deldate = TALER_json_from_abs (deldate);
  

  /* Delivery location: OPTIONAL FIELD */
  j_delloc = json_string ("MALTK"); /* just a 'tag' which points to some well defined location */
			

  /* Merchant jurisdiction: OPTIONAL FIELD (with its fields from 3rd to the last being optional)
  * for another optional field */
  j_merchant_zipcode = json_integer (9468);
  j_merchant_jurisdiction = json_pack ("{s:s, s:s, s:s, s:s, s:s, s:I}",
                                       "country", "Test Country",
				       "city", "Test City",
				       "state", "NA",
				       "region", "NA",
				       "province", "NA",
				       "ZIP code", json_integer_value (j_merchant_zipcode));

  /* Merchant details */
  j_merchant = json_pack ("{s:s, s:s, s:o}",
                          "address", "UALMP",
			  "name", "test merchant",
			  "jurisdiction", j_merchant_jurisdiction);


  /* L-names mapping */
  j_lnames = json_pack ("[{s:s}, {s:s}]",
                        "MALTK", "Test Address 1",
	                "UALMP", "Second Test Address");



  j_details = json_pack ("{s:o, s:o, s:o, s:o, s:o}",
                         "items", j_item,
			 "delivery date", j_deldate,
			 "delivery location", j_delloc,
			 "merchant", j_merchant,
			 "L-names", j_lnames);

  j_fake_contract = json_pack ("{s:o, s:I, s:o}",
                    "amount", j_amount,
		    "trans_id", json_integer_value (j_id),
		    "details", j_details);




  j_root = MERCHANT_handle_contract (j_fake_contract,
                            db_conn,
			    wire,
			    &contract);

  #if 1
  str = json_dumps (j_root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
  printf ("%s\n", str);
  return;
  #endif

}


/**
 * The main function of the test tool
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  
  static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    {'t', "temp", NULL,
     gettext_noop ("Use temporary database tables"), GNUNET_NO,
     &GNUNET_GETOPT_set_one, &dry},
     GNUNET_GETOPT_OPTION_END
    };
  

  if (GNUNET_OK !=
      GNUNET_PROGRAM_run (argc, argv,
                          "merchant-contract-test",
                          "Test for contracts mgmt",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;


 
}