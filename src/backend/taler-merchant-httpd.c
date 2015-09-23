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
#include <curl/curl.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_mint_service.h>
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "merchant_db.h"
#include "merchant.h"
#include "taler_merchant_lib.h"

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
 * File holding the merchant's private key
 */
char *keyfile;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
 * Connection handle to the our database
 */
PGconn *db_conn;

/**
 * merchant's conf handle
 */
struct GNUNET_CONFIGURATION_Handle *cfg;

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
 * Mints' URL,port,key triples
 */
struct MERCHANT_MintInfo *mint_infos;

/**
 * The number of accepted mints
 */
unsigned int nmints;


#if FUTURE_USE
/**
* Return the given message to the other end of connection
* @msg (0-terminated) message to show
* @param connection a MHD connection
* @param resp where to store the response for the calling function
* @return HTTP status code reflecting the operation outcome
*
*/
 
static unsigned int
generate_message (struct MHD_Response **resp, const char *msg) 
{
 
  unsigned int ret;

  *resp = MHD_create_response_from_buffer (strlen (msg), (void *) msg,
                                           MHD_RESPMEM_PERSISTENT);
  ret = 200;
  return ret;


}
#endif

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

#ifdef PANIC_MGMT
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
#endif

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
<html><title>Method NOT allowed</title><body><center>   \
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
 * Take the global wire details and return a JSON containing them,
 * compliantly with the Taler's API.
 * @param edate when the beneficiary wants this transfer to take place
 * @return JSON representation of the wire details, NULL upon errors
 */

static json_t *
get_wire_json (struct GNUNET_TIME_Absolute edate)
{
  
  json_t *root;
  json_t *j_edate;
  json_t *j_nounce;
  uint64_t nounce;

  nounce = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);
  j_nounce = json_integer (nounce);

  j_edate = TALER_json_from_abs (edate);
  if (NULL == (root = json_pack ("{s:s, s:s, s:s, s:s, s:o}",
                                 "type", "SEPA",
		                 "IBAN", wire->iban,
		                 "name", wire->name,
		                 "BIC", wire->bic,
		                 "edate", j_edate,
		                 "r", json_integer_value (j_nounce))))
    return NULL;

  return root;
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
  struct GNUNET_CRYPTO_EddsaSignature c_sig;
  struct GNUNET_CRYPTO_EddsaPublicKey pub;
  #ifdef OBSOLETE
  struct ContractNBO contract;
  #else
  struct Contract contract;
  #endif
  struct MHD_Response *resp;
  json_t *root;
  json_t *j_contract_complete;
  json_t *j_sig_enc;
  json_t *eddsa_pub_enc;
  json_t *response;
  json_t *j_mints;
  json_t *j_mint;
  json_t *j_wire;
  int cnt; /* loop counter */
  char *str; /* to debug JSONs */
  char *deposit_body;
  json_t *root_tmp;
  json_t *j_refund_deadline;
  json_t *j_amount_tmp;
  json_t *j_depperm;
  json_t *j_details_tmp;
  json_t *j_max_fee_tmp;
  json_int_t j_int_trans_id_tmp;
  json_t *j_trans_id_tmp;
  int64_t trans_id_tmp;
  char *ub_sig;
  char *coin_pub;
  char *denom_key;
  char *contract_sig;
  char *h_contract;
  struct GNUNET_HashCode h_json_wire;
  struct TALER_Amount amount_tmp;
  json_t *j_h_json_wire;
  struct curl_slist *slist;

  struct GNUNET_TIME_Absolute now;

  CURL *curl;
  CURLcode curl_res;

  int res = GNUNET_SYSERR;

  #define URL_HELLO "/hello"
  #define URL_CONTRACT "/contract"
  #define URL_DEPPERM "/pay"
  no_destroy = 0;
  resp = NULL;
  status = MHD_HTTP_INTERNAL_SERVER_ERROR;

  if (0 == strncasecmp (url, URL_HELLO, sizeof (URL_HELLO)))
  {
    if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
      status = generate_hello (&resp);
    else
    {
      status = MHD_HTTP_METHOD_NOT_ALLOWED;
    }
  }

  if (0 == strncasecmp (url, URL_DEPPERM, sizeof (URL_DEPPERM)))
  {
    if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
    {
      status = MHD_HTTP_METHOD_NOT_ALLOWED;
      goto end;

    }
    else
      res = TMH_PARSE_post_json (connection,
                                 connection_cls,
                                 upload_data,
                                 upload_data_size,
                                 &root);
    if (GNUNET_SYSERR == res)
    {
      status = MHD_HTTP_BAD_REQUEST;
      goto end;
    }

    /* the POST's body has to be fetched furthermore */
    if ((GNUNET_NO == res) || (NULL == root))
      return MHD_YES;
    
    /* The frontend should supply a JSON in the format described in
    http://link-to-specs : .. In practice, it just forwards what it
    received from the wallet.
    
    Roughly, the backend must add to this JSON:
     
     1. The merchant's public key
     2. A timestamp (?)
     3. wire (see mint's specs)
     4. h_wire
     5. refund deadline (?)
     
     (?) : may the frontend will handle dates ?

    */

    #ifdef DEBUG
    /* NOTE: there is no need to thoroughly unpack this JSON, since
    the backend must just *add* fields to it */
    if (-1 == (json_unpack (root,
                            "{s:s, s:I, s:s, s:s, s:s}",
                            "ub_sig", &ub_sig,
			    "coin_pub", &coin_pub,
	                    "denom_pub", &denom_key,
			    "H_contract", &h_contract,
	 		    "sig", &contract_sig)))
    {
      printf ("no unpack\n");
      status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      goto end;
    }

    printf ("Got this deposit permission:\nub_sig: %s\ncoin_pub: %s\ndenom_key: %s\nsig: %s\n",
    ub_sig, coin_pub, denom_key, contract_sig);

    return MHD_NO;
    #endif

    /* TODO: Check if there is a row in 'contracts' table corresponding to this
    contract ('s hash). This query has to return the trans_id and the amount for
    this contract - faked values for now. See bug #XXXX */

    /* FIXME fake trans_id */
    trans_id_tmp = (int64_t) GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);

    if (trans_id_tmp < 0)
       j_trans_id_tmp  = json_integer ((-1) * trans_id_tmp);
    else
       j_trans_id_tmp = json_integer (trans_id_tmp);

    /* FIXME fake amount to redeem */
    TALER_amount_get_zero ("EUR", &amount_tmp);
    amount_tmp.value = 5;
    j_amount_tmp = TALER_json_from_amount (&amount_tmp);

    /* Encoding merchant's key */
    GNUNET_CRYPTO_eddsa_key_get_public (privkey, &pub);
    eddsa_pub_enc = TALER_json_from_data ((void *) &pub, sizeof (pub));

    /* Timestamping 'now' */
    now = GNUNET_TIME_absolute_get ();
    
    /* getting the SEPA-aware JSON */
    if (NULL == (j_wire = get_wire_json (GNUNET_TIME_absolute_add (now, GNUNET_TIME_UNIT_WEEKS))))
      goto end;

    /* hash it*/
    if (GNUNET_SYSERR == TALER_hash_json (j_wire, &h_json_wire))
      goto end;

    j_h_json_wire = TALER_json_from_data (&h_json_wire, sizeof (struct GNUNET_HashCode));
    /* refund deadline */
    j_refund_deadline = TALER_json_from_abs (GNUNET_TIME_absolute_add (now, GNUNET_TIME_UNIT_WEEKS));

    /* pack it!*/
    eddsa_pub_enc = TALER_json_from_data ((void *) &pub, sizeof (pub));

    if (NULL == (j_depperm = json_pack ("{s:o, s:o, s:o, s:o, s:o, s:I, s:o}",
                                        "merchant_pub", eddsa_pub_enc,
					"timestamp", TALER_json_from_abs (now),
					"wire", j_wire,
					"H_wire", j_h_json_wire,
					"refund_deadline", j_refund_deadline,
					"transaction_id", json_integer_value (j_trans_id_tmp),
					"f", j_amount_tmp)))
    {
      printf ("BAD depperm packaging\n");
      goto end;
    }


    /* melt to what received from the wallet */
    if (-1 == json_object_update (j_depperm, root))
    {
      printf ("depperm response not built\n");
      goto end; 
    }

    #define DEBUGG
    #ifdef DEBUG
    str = json_dumps (j_depperm, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    printf ("Depperm is: \n%s", str);
    return MHD_NO;
    #endif

    /* POST to mint's "/deposit" */
    curl = curl_easy_init (); 

    if (curl)
    {
    
      slist = curl_slist_append (slist, "Content-type: application/json");
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER, slist);

      /* FIXME the mint's URL is be retrieved from the partial deposit permission
      (received by the wallet) */
      curl_easy_setopt (curl, CURLOPT_URL, "http://demo.taler.net/deposit");
    
      /* NOTE: hopefully, this string won't need any URL-encoding, since as for the
      Jansson specs, any space and-or newline are not in place using JSON_COMPACT
      flag */
      deposit_body = json_dumps (j_depperm, JSON_COMPACT | JSON_PRESERVE_ORDER);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, deposit_body);

      curl_res = curl_easy_perform (curl);

      curl_slist_free_all(slist);
      if(curl_res != CURLE_OK)
      {
        printf ("deposit rejected by mint\n");
        goto end; 
      }
      else
        printf ("deposit ok\n");

     curl_easy_cleanup(curl);
    
    
    }

  }

  /* 
  * To be called by the frontend passing the contract with some "holes"
  * which will be completed, stored in DB, signed, and returned
  *
  */

  if (0 == strncasecmp (url, URL_CONTRACT, sizeof (URL_CONTRACT)))
  {
    if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
    {
      status = MHD_HTTP_METHOD_NOT_ALLOWED;
      goto end;

    }
    else
      res = TMH_PARSE_post_json (connection,
                                 connection_cls,
                                 upload_data,
                                 upload_data_size,
                                 &root);
    if (GNUNET_SYSERR == res)
    {
      status = MHD_HTTP_BAD_REQUEST;
      goto end;
    }

    /* the POST's body has to be fetched furthermore */
    if ((GNUNET_NO == res) || (NULL == root))
      return MHD_YES;
    
    /* The frontend should supply a JSON in the format described in
    http://link-to-specs : */

    /* TODO Specifying the accepted mints, in the contract */
    j_mints = json_array ();
    for (cnt = 0; cnt < nmints; cnt++)
    {
      j_mint = json_pack ("{s:s}",
                          mint_infos[cnt].hostname,
                          GNUNET_CRYPTO_eddsa_public_key_to_string (&mint_infos[cnt].pubkey));
      json_array_append_new (j_mints, j_mint);

      #define DEBUGG
      #ifdef DEBUG
      printf ("mint(s): url %s, key %s\n", mint_infos[cnt].hostname,
              GNUNET_CRYPTO_eddsa_public_key_to_string (&mint_infos[cnt].pubkey));
      str = json_dumps (j_mint, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
      printf ("%s\n", str);
      #endif
        
    }

    if (-1 == (json_object_update (root, j_mints)))
    {
      printf ("no mints specified in contract\n");  
      goto end;
    

    }
    if (NULL == (j_contract_complete = MERCHANT_handle_contract (root,
                                                                 db_conn,
								 wire,
								 &contract)))
    {
      status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      goto end;
    }

    GNUNET_CRYPTO_eddsa_sign (privkey, &contract.purpose, &c_sig);

    /**
    *
    * As of now, the format is 
    *
    * {"contract" : {the contract in "plain" JSON},
    *  "sig" : base32 encoding of the signed 'struct ContractNBO',
    *  "eddsa_pub" : base32 encoding of merchant's public key}
    *
    */
	
    j_sig_enc = TALER_json_from_eddsa_sig (&contract.purpose, &c_sig);
    GNUNET_CRYPTO_eddsa_key_get_public (privkey, &pub);
    eddsa_pub_enc = TALER_json_from_data ((void *) &pub, sizeof (pub));
    response = json_pack ("{s:o, s:o, s:o}",
                          "contract", j_contract_complete,
                          "sig", j_sig_enc,
	                  "eddsa_pub", eddsa_pub_enc);
    TMH_RESPONSE_reply_json (connection, response, MHD_HTTP_OK);	 
    return MHD_YES;

  }

  end:

  if (NULL != resp)
  {
    EXITIF (MHD_YES != MHD_queue_response (connection, status, resp));
    return MHD_YES;
    if (!no_destroy)
      MHD_destroy_response (resp);
  }
  else
  {
  
    EXITIF (GNUNET_OK != failure_resp (connection, status));
    return MHD_YES;
  
  }
  
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
  if (keyfile != NULL)
    GNUNET_free (privkey);

  
}

/**
 * Function called with information about who is auditing
 * a particular mint and what key the mint is using.
 *
 * @param cls closure
 * @param keys information about the various keys used
 *        by the mint
 */
static void
keys_mgmt_cb (void *cls, const struct TALER_MINT_Keys *keys)
{
  /* which kind of mint's keys a merchant should need? Sign
  keys? It has already the mint's master key from the conf file */  
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
void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{

  unsigned int cnt;
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
  EXITIF (GNUNET_OK != MERCHANT_DB_initialize (db_conn, GNUNET_YES));
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
