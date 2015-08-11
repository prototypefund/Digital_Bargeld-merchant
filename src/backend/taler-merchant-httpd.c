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
* @file merchant/backend/taler-merchant-httpd.c
* @brief HTTP serving layer mainly intended to communicate with the frontend
* @author Marcello Stanisci
*/

#include "platform.h"
#include <microhttpd.h>
#include <jansson.h>
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_mint_service.h>
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_mhd.h"
#include "taler-mint-httpd_admin.h"
#include "taler-mint-httpd_deposit.h"
#include "taler-mint-httpd_withdraw.h"
#include "taler-mint-httpd_refresh.h"
#include "taler-mint-httpd_keystate.h"
#include "taler-mint-httpd_responses.h"
#include "merchant.h"
#include "merchant_db.h"

extern struct MERCHANT_WIREFORMAT_Sepa *
TALER_MERCHANT_parse_wireformat_sepa (const struct GNUNET_CONFIGURATION_Handle *cfg);

/**
 * Shorthand for exit jumps.
 */
#define EXITIF(cond)                                              \
  do {                                                            \
    if (cond) { GNUNET_break (0); goto EXITIF_exit; }             \
  } while (0)

/**
 * Macro to round microseconds to seconds in GNUNET_TIME_* structs.
 */
#define ROUND_TO_SECS(name,us_field) name.us_field -= name.us_field % (1000 * 1000)

/**
 * Our hostname
 */
static char *hostname;

/**
 * The port we are running on
 */
static long long unsigned port;

/**
 * Merchant's private key
 */
struct GNUNET_CRYPTO_EddsaPrivateKey *privkey;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
 * Connection handle to the our database
 */
PGconn *db_conn;

/**
 * Which currency is used by this mint?
 * (verbatim copy from mint's code, just to make this
 * merchant's source compile)
 */
char *TMH_mint_currency_string;

/* As above */
struct TALER_MINTDB_Plugin *TMH_plugin;


/**
 * As above, though the merchant does need some form of
 * configuration
 */
struct GNUNET_CONFIGURATION_Handle *cfg;


/**
 * As above
 */
int TMH_test_mode;


/**
 * As above
 */
char *TMH_mint_directory;


/**
 * As above
 */
struct GNUNET_CRYPTO_EddsaPublicKey TMH_master_public_key;

/**
 * As above
 */
char *TMH_expected_wire_format;

/**
 * Shutdown task identifier
 */
static struct GNUNET_SCHEDULER_Task *shutdown_task;

/**
 * Our wireformat
 */
static struct MERCHANT_WIREFORMAT_Sepa *wire;

/**
 * Hash of the wireformat
 */
static struct GNUNET_HashCode h_wire;

/**
 * Should we do a dry run where temporary tables are used for storing the data.
 */
static int dry;

/**
 * Global return code
 */
static int result;

GNUNET_NETWORK_STRUCT_BEGIN

struct Contract
{
  /**
   * The signature of the merchant for this contract
   */
  struct GNUNET_CRYPTO_EddsaSignature sig;

  /**
   * Purpose header for the signature over contract
   */
  struct GNUNET_CRYPTO_EccSignaturePurpose purpose;

  /**
   * The transaction identifier
   */
  char m[13];

  /**
   * Expiry time
   */
  struct GNUNET_TIME_AbsoluteNBO t;

  /**
   * The invoice amount
   */
  struct TALER_AmountNBO amount;

  /**
   * The hash of the preferred wire format + nounce
   */
  struct GNUNET_HashCode h_wire;

  /**
   * The contract data
   */
  char a[];
};

GNUNET_NETWORK_STRUCT_END

/**
 * Mint context
 */
static struct TALER_MINT_Context *mctx;

/**
 * Context information of the mints we trust
 */
struct Mint
{
  /**
   * Public key of this mint
   */
  struct GNUNET_CRYPTO_EddsaPublicKey pubkey;

  /**
   * Connection handle to this mint
   */
  struct TALER_MINT_Handle *conn;
};

/**
 * Hashmap to store the mint context information
 */
static struct GNUNET_CONTAINER_MultiPeerMap *mints_map;

/**
* Return the given message to the other end of connection
* @msg (0-terminated) message to show
* @param connection a MHD connection
* @param resp where to store the response for the calling function
* @return HTTP status code reflecting the operation outcome
*
*/
 
static unsigned int
generate_message (struct MHD_Response **resp, const char *msg) // this parameter was preceded by a '_' in its original file. Why?
{
 
  unsigned int ret;

  *resp = MHD_create_response_from_buffer (strlen (msg), (void *) msg,
                                           MHD_RESPMEM_PERSISTENT);
  ret = 200;
  return ret;


}

/**
* Generate the 'hello world' response
* @param connection a MHD connection
* @param resp where to store the response for the calling function
* @return HTTP status code reflecting the operation outcome
*
*/
 
static unsigned int
generate_hello (struct MHD_Response **resp) // this parameter was preceded by a '_' in its original file. Why?
{
 
  const char *hello = "Hello customer\n";
  unsigned int ret;

  *resp = MHD_create_response_from_buffer (strlen (hello), (void *) hello,
                                           MHD_RESPMEM_PERSISTENT);
  ret = 200;
  return ret;


}

/**
 * Callback for catching serious error conditions from MHD.
 *
 * @param cls user specified value
 * @param file where the error occured
 * @param line where the error occured
 * @param reason error detail, may be NULL
 */
static void
mhd_panic_cb (void *cls,
              const char *file,
              unsigned int line,
              const char *reason)
{
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
              "MHD panicked at %s:%u: %s",
              file, line, reason);
  result = GNUNET_SYSERR;
  GNUNET_SCHEDULER_shutdown ();
}

/**
* Manage a non 200 HTTP status. I.e. it shows a 'failure' page to
* the client
* @param connection the channel thorugh which send the message
* @status the HTTP status to examine
* @return GNUNET_OK on successful message sending, GNUNET_SYSERR upon error
*
*/

static int
failure_resp (struct MHD_Connection *connection, unsigned int status)
{
  static char page_MHD_HTTP_NOT_FOUND[]="\
<!DOCTYPE html>                                         \
<html><title>Resource not found</title><body><center>   \
<h3>The resource you are looking for is not found.</h3> \
</center></body></html>";
  static char page_MHD_HTTP_BAD_REQUEST[]="\
<!DOCTYPE html>                                         \
<html><title>Bad request</title><body><center>   \
<h3>Malformed POSTed JSON.</h3> \
</center></body></html>";
static char page_MHD_HTTP_METHOD_NOT_ALLOWED[]="\
<!DOCTYPE html>                                         \
<html><title>Method NOT allowe</title><body><center>   \
<h3>ONLY POSTs are allowed.</h3> \
</center></body></html>";
  static char page_MHD_HTTP_INTERNAL_SERVER_ERROR[]="\
<!DOCTYPE html> <html><title>Internal Server Error</title><body><center> \
<h3>The server experienced an internal error and hence cannot serve your \
request</h3></center></body></html>";
  struct MHD_Response *resp;
  char *page;
  size_t size;
#define PAGE(number) \
  do {page=page_ ## number; size=sizeof(page_ ## number)-1;} while(0)

  GNUNET_assert (MHD_HTTP_BAD_REQUEST <= status);
  resp = NULL;
  switch (status)
  {
  case MHD_HTTP_NOT_FOUND :
    PAGE(MHD_HTTP_NOT_FOUND);
    break;
  case MHD_HTTP_BAD_REQUEST:
    PAGE(MHD_HTTP_BAD_REQUEST);
    break;
  case MHD_HTTP_METHOD_NOT_ALLOWED:
    PAGE(MHD_HTTP_METHOD_NOT_ALLOWED);
    break;
  default:
    status = MHD_HTTP_INTERNAL_SERVER_ERROR;
  case MHD_HTTP_INTERNAL_SERVER_ERROR:
    PAGE(MHD_HTTP_INTERNAL_SERVER_ERROR);
  }
#undef PAGE

  EXITIF (NULL == (resp = MHD_create_response_from_buffer (size,
                                                           page,
                                                           MHD_RESPMEM_PERSISTENT)));
  EXITIF (MHD_YES != MHD_queue_response (connection, status, resp));
  MHD_destroy_response (resp);
  return GNUNET_OK;

 EXITIF_exit:
  if (NULL != resp)
    MHD_destroy_response (resp);
  return GNUNET_SYSERR;
}


/**
* Generate the hash containing the information (= a nounce + merchant's IBAN) to
* redeem money from mint in a subsequent /deposit operation
* @param nounce the nounce
* @return the hash to be included in the contract's blob
*
*/

static struct GNUNET_HashCode
hash_wireformat (uint64_t nounce)
{
  struct GNUNET_HashContext *hc;
  struct GNUNET_HashCode hash;

  hc = GNUNET_CRYPTO_hash_context_start ();
  GNUNET_CRYPTO_hash_context_read (hc, wire->iban, strlen (wire->iban));
  GNUNET_CRYPTO_hash_context_read (hc, wire->name, strlen (wire->name));
  GNUNET_CRYPTO_hash_context_read (hc, wire->bic, strlen (wire->bic));
  nounce = GNUNET_htonll (nounce);
  GNUNET_CRYPTO_hash_context_read (hc, &nounce, sizeof (nounce));
  GNUNET_CRYPTO_hash_context_finish (hc, &hash);
  return hash;
}



/*
* Make a binary blob representing a contract, store it into the DB, sign it
* and return a pointer to it.
* @param a 0-terminated string representing the description of this
* @param c_id contract id provided by the frontend
* purchase (it should contain a human readable description of the good
* in question)
* @param product some product numerical id. Its indended use is to link the
* good, or service being sold to some entry in the DB managed by the frontend
* @price the cost of this good or service
* @return pointer to the allocated contract (which has a field, 'sig', holding
* its own signature), NULL upon errors
*/

struct Contract *
generate_and_store_contract (const char *a, uint64_t c_id, uint64_t product, struct TALER_Amount *price)
{
  
  struct Contract *contract;
  struct GNUNET_TIME_Absolute expiry;
  uint64_t nounce;
  uint64_t contract_id_nbo;

  expiry = GNUNET_TIME_absolute_add (GNUNET_TIME_absolute_get (),
                                     GNUNET_TIME_UNIT_DAYS);
  ROUND_TO_SECS (expiry, abs_value_us);
  nounce = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);
  EXITIF (GNUNET_SYSERR == MERCHANT_DB_contract_create (db_conn,
                                              &expiry,
                                              price,
 					      c_id,
                                              a,
                                              nounce,
                                              product));
  contract_id_nbo = GNUNET_htonll ((uint64_t) c_id);
  contract = GNUNET_malloc (sizeof (struct Contract) + strlen (a) + 1);
  contract->purpose.purpose = htonl (TALER_SIGNATURE_MERCHANT_CONTRACT);
  contract->purpose.size = htonl (sizeof (struct Contract)
                                  - offsetof (struct Contract, purpose)
                                  + strlen (a) + 1);
  GNUNET_STRINGS_data_to_string (&contract_id_nbo, sizeof (contract_id_nbo),
                                 contract->m, sizeof (contract->m));
  contract->t = GNUNET_TIME_absolute_hton (expiry);
  (void) strcpy (contract->a, a);
  contract->h_wire = hash_wireformat (nounce);
  TALER_amount_hton (&contract->amount,  price);
  GNUNET_CRYPTO_eddsa_sign (privkey, &contract->purpose, &contract->sig);
  return contract;

  /* legacy from old merchant */
  EXITIF_exit:
    if (NULL != contract)
    {
      GNUNET_free (contract);
    }
    return NULL;
}


/**
 * A client has requested the given url using the given method
 * (#MHD_HTTP_METHOD_GET, #MHD_HTTP_METHOD_PUT,
 * #MHD_HTTP_METHOD_DELETE, #MHD_HTTP_METHOD_POST, etc).  The callback
 * must call MHD callbacks to provide content to give back to the
 * client and return an HTTP status code (i.e. #MHD_HTTP_OK,
 * #MHD_HTTP_NOT_FOUND, etc.).
 *
 * @param cls argument given together with the function
 *        pointer when the handler was registered with MHD
 * @param url the requested url
 * @param method the HTTP method used (#MHD_HTTP_METHOD_GET,
 *        #MHD_HTTP_METHOD_PUT, etc.)
 * @param version the HTTP version string (i.e.
 *        #MHD_HTTP_VERSION_1_1)
 * @param upload_data the data being uploaded (excluding HEADERS,
 *        for a POST that fits into memory and that is encoded
 *        with a supported encoding, the POST data will NOT be
 *        given in upload_data and is instead available as
 *        part of #MHD_get_connection_values; very large POST
 *        data *will* be made available incrementally in
 *        @a upload_data)
 * @param upload_data_size set initially to the size of the
 *        @a upload_data provided; the method must update this
 *        value to the number of bytes NOT processed;
 * @param con_cls pointer that the callback can set to some
 *        address and that will be preserved by MHD for future
 *        calls for this request; since the access handler may
 *        be called many times (i.e., for a PUT/POST operation
 *        with plenty of upload data) this allows the application
 *        to easily associate some request-specific state.
 *        If necessary, this state can be cleaned up in the
 *        global #MHD_RequestCompletedCallback (which
 *        can be set with the #MHD_OPTION_NOTIFY_COMPLETED).
 *        Initially, `*con_cls` will be NULL.
 * @return #MHD_YES if the connection was handled successfully,
 *         #MHD_NO if the socket must be closed due to a serios
 *         error while handling the request
 */

static int
url_handler (void *cls,
             struct MHD_Connection *connection,
             const char *url,
             const char *method,
             const char *version,
             const char *upload_data,
             size_t *upload_data_size,
             void **connection_cls)
{

  printf ("%s\n", url);
  unsigned int status;
  unsigned int no_destroy;
  json_int_t prod_id;
  json_int_t contract_id;
  struct Contract *contract;
  struct MHD_Response *resp;
  struct TALER_Amount price;
  struct GNUNET_CRYPTO_EddsaPublicKey pub;
  json_t *json_price;
  json_t *root;
  json_t *contract_enc;
  json_t *sig_enc;
  json_t *eddsa_pub_enc;
  json_t *response;

  int res;
  const char *desc;

  #define URL_HELLO "/hello"
  #define URL_CONTRACT "/contract"
  no_destroy = 0;
  resp = NULL;
  contract = NULL;
  status = MHD_HTTP_INTERNAL_SERVER_ERROR;
  if (0 == strncasecmp (url, URL_HELLO, sizeof (URL_HELLO)))
    {
      if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
        status = generate_hello (&resp);
      else
        GNUNET_break (0);
    }

  // to be called by the frontend passing all the product's information
  // which are relevant for the contract's generation
  if (0 == strncasecmp (url, URL_CONTRACT, sizeof (URL_CONTRACT)))
  {
    if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
    {
      status = MHD_HTTP_METHOD_NOT_ALLOWED;

    }
    else
      res = TMH_PARSE_post_json (connection,
                               connection_cls,
                               upload_data,
                               upload_data_size,
                               &root);
  
    if (GNUNET_SYSERR == res)
      status = MHD_HTTP_METHOD_NOT_ALLOWED;

    if ((GNUNET_NO == res) || (NULL == root))
      return MHD_YES;
    
    /* The frontend should supply a JSON in the follwoing format:

    {
     
     "desc" : string human-readable describing this deal,
     "product" : uint64-like integer referring to the product in some
                 DB adminstered by the frontend,
     "cid" : uint64-like integer, this contract's id
     "price" : a 'struct TALER_Amount' in the Taler compliant JSON format }

     */

    #if 0
    /*res = json_typeof (root); <- seg fault*/
    json_int_t id;
    const char *desc_test;
    const char *cur_test;
    json_t *id_json;
    json_t *desc_json;
    json_t *cur_json;
    id_json = json_object_get (root, "product");
    desc_json = json_object_get (root, "desc");
    id = json_integer_value (id_json);
    desc_test = json_string_value (desc_json);
    json_price = json_object_get (root, "price");
    json_typeof (json_price);
    cur_json = json_object_get (json_price, "currency");
    cur_test = json_string_value (cur_json);
    printf ("id is %" JSON_INTEGER_FORMAT "\n", id);
    printf ("desc is %s\n", desc_test);
    TALER_json_to_amount (json_price, &price);
    printf ("cur_test is %s\n", price.currency);
    json_error_t err;
    if (res = json_unpack_ex (root, &err, JSON_VALIDATE_ONLY, "{s:s, s:I, s:o}", 
                      "desc",
		      //&desc,
		      "product", 
		      //&prod_id, 
		      "price"//, 
		      //json_price
		      ))
    #else
    if ((res = json_unpack (root, "{s:s, s:I, s:I, s:o}", 
                      "desc",
		      &desc,
		      "product", 
		      &prod_id,
		      "cid",
		      &contract_id,
		      "price", 
		      &json_price
		      )))
    #endif


      /* still not possible to return a taler-compliant error message
      since this JSON format is not among the taler officials ones */
      status = MHD_HTTP_BAD_REQUEST;
    else
    {

      if (GNUNET_OK != TALER_json_to_amount (json_price, &price))
        /* still not possible to return a taler-compliant error message
	since this JSON format is not among the taler officials ones */ 
	status = MHD_HTTP_BAD_REQUEST;
      else
      {
        /* Let's generate this contract! */
	if (NULL == (contract = generate_and_store_contract (desc, contract_id, prod_id, &price)))
          status = MHD_HTTP_INTERNAL_SERVER_ERROR;
	else
	{
          json_decref (root);
          json_decref (json_price);

          /* the contract is good and stored in DB, produce now JSON to return.
	  As of now, the format is {"contract" : base32contract,
	                            "sig" : contractSignature,
	                            "eddsa_pub" : keyToCheckSignatureAgainst
                                   }
	   
	  */
	
	  sig_enc = TALER_json_from_eddsa_sig (&contract->purpose, &contract->sig);
	  GNUNET_CRYPTO_eddsa_key_get_public (privkey, &pub);
	  eddsa_pub_enc = TALER_json_from_data ((void *) &pub, sizeof (pub));
	  /* cutting of the signature at the beginning */
	  contract_enc = TALER_json_from_data (&contract->purpose, sizeof (*contract)
	                                        - offsetof (struct Contract, purpose)
						+ strlen (desc) +1);
	  response = json_pack ("{s:o, s:o, s:o}", "contract", contract_enc, "sig", sig_enc,
	                         "eddsa_pub", eddsa_pub_enc);
	  TMH_RESPONSE_reply_json (connection, response, MHD_HTTP_OK);	 
	  return MHD_YES;

	  /* not needed (?) anymore
	  #define page_ok "\
          <!DOCTYPE html> <html><title>Ok</title><body><center> \
          <h3>Contract's generation succeeded</h3></center></body></html>"
	  status = generate_message (&resp, page_ok);
	  #undef page_ok
	  */
	  /* FRONTIER - CODE ABOVE STILL NOT TESTED */
	}
      }
    }
  }

  if (NULL != resp)
    {
      EXITIF (MHD_YES != MHD_queue_response (connection, status, resp));
      if (!no_destroy)
        MHD_destroy_response (resp);
    }
  else
    EXITIF (GNUNET_OK != failure_resp (connection, status));
  return MHD_YES;
  
 EXITIF_exit:
   result = GNUNET_SYSERR;
   GNUNET_SCHEDULER_shutdown ();
   return MHD_NO;
}

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

  if (NULL != mhd)
    {
      MHD_stop_daemon (mhd);
      mhd = NULL;
    }

  if (NULL != db_conn)
    {
      MERCHANT_DB_disconnect (db_conn);
      db_conn = NULL;
    }

  
}



/**
 * Function called with information about who is auditing
 * a particular mint and what key the mint is using.
 *
 * @param cls closure
 * @param keys information about the various keys used
 *        by the mint
 */
void
keys_mgmt_cb (void *cls, const struct TALER_MINT_Keys *keys)
{
  /* which kind of mint's keys a merchant should need? Sign
  keys? It has already the mint's (master?) public key from
	  the conf file */  
  return;

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

  char *keyfile;
  unsigned int nmints;
  unsigned int cnt;
  struct MERCHANT_MintInfo *mint_infos;
  void *keys_mgmt_cls;

  keys_mgmt_cls = NULL;
  mint_infos = NULL;
  keyfile = NULL;
  result = GNUNET_SYSERR;
  shutdown_task = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
                                                &do_shutdown, NULL);
  EXITIF (GNUNET_SYSERR == (nmints = TALER_MERCHANT_parse_mints (config,
                                                                 &mint_infos)));
  EXITIF (NULL == (wire = TALER_MERCHANT_parse_wireformat_sepa (config)));
  EXITIF (GNUNET_OK != GNUNET_CONFIGURATION_get_value_filename (config,
                                                                "merchant",
                                                                "KEYFILE",
                                                                &keyfile));
  EXITIF (NULL == (privkey = GNUNET_CRYPTO_eddsa_key_create_from_file (keyfile)));
  EXITIF (NULL == (db_conn = MERCHANT_DB_connect (config)));
  EXITIF (GNUNET_OK != MERCHANT_DB_initialize (db_conn, GNUNET_NO));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_number (config,
                                                 "merchant",
                                                 "port",
                                                 &port));
  EXITIF (GNUNET_SYSERR ==
          GNUNET_CONFIGURATION_get_value_string (config,
                                                 "merchant",
                                                 "hostname",
                                                 &hostname));

  EXITIF (NULL == (mctx = TALER_MINT_init ()));
  EXITIF (NULL == (mints_map = GNUNET_CONTAINER_multipeermap_create (nmints, GNUNET_YES)));
  
  for (cnt = 0; cnt < nmints; cnt++)
    {
      struct Mint *mint;
  
      mint = GNUNET_new (struct Mint);
      mint->pubkey = mint_infos[cnt].pubkey;
      /* port this to the new API */
      mint->conn = TALER_MINT_connect (mctx,
                                       mint_infos[cnt].hostname,
                                       &keys_mgmt_cb,
                                       keys_mgmt_cls); /*<- safe?segfault friendly?*/

      /* NOTE: the keys mgmt callback should roughly do what the following lines do */
      EXITIF (NULL == mint->conn);
      
      EXITIF (GNUNET_SYSERR == GNUNET_CONTAINER_multipeermap_put
      (mints_map,
       (struct GNUNET_PeerIdentity *) /* to retrieve now from cb's args -> */&mint->pubkey,
       mint,
       GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_FAST));
    }
  

  mhd = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY,
                          port,
                          NULL, NULL,
                          &url_handler, NULL,
                          MHD_OPTION_END);


  EXITIF (NULL == mhd);
  /* WARNING: a 'poll_mhd ()' call is here in the original merchant. Is that
  mandatory ? */
  GNUNET_CRYPTO_hash (wire, sizeof (*wire), &h_wire);
  result = GNUNET_OK;

  EXITIF_exit:
    if (GNUNET_OK != result)
      GNUNET_SCHEDULER_shutdown ();
  GNUNET_free_non_null (keyfile);
    if (GNUNET_OK != result)
      GNUNET_SCHEDULER_shutdown ();

}

/**
 * The main function of the serve tool
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
                          "taler-merchant-serve",
                          "Serve merchant's HTTP interface",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;


 
}
