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

struct Mint_Response
{
  char *ptr;
  size_t size;
    
};


/**
* Generate the 'hello world' response
* @param connection a MHD connection
* @param resp where to store the response for the calling function.
* Note that in its original implementation this parameter was preceeded
* by a '_'. Still not clear why.
* @return HTTP status code reflecting the operation outcome
*
*/
 
static unsigned int
generate_hello (struct MHD_Response **resp)
{
 
  const char *hello = "Hello customer\n";
  unsigned int ret;

  *resp = MHD_create_response_from_buffer (strlen (hello), (void *) hello,
                                           MHD_RESPMEM_PERSISTENT);
  ret = 200;
  return ret;
}

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

/**
* Callback to pass to curl used to store a HTTP response
* in a custom memory location.
* See http://curl.haxx.se/libcurl/c/getinmemory.html for a
* detailed example
*
* @param contents the data gotten so far from the server
* @param size symbolic (arbitrarily chosen by libcurl) unit
* of bytes
* @param nmemb factor to multiply by @a size to get the real
* size of @a contents
* @param userdata a pointer to a memory location which remains
* the same across all the calls to this callback (i.e. it has
* to be grown at each invocation of this callback)
* @return number of written bytes
* See http://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
* for official documentation
*
*/
size_t
get_response_in_memory (char *contents,
                        size_t size,
			size_t nmemb,
			void *userdata)
{
  struct Mint_Response *mr;
  size_t realsize;

  realsize = size * nmemb;
  mr = userdata;
  mr->ptr = realloc (mr->ptr, mr->size + realsize + 1);

  if (mr->ptr == NULL) {
    printf ("Out of memory, could not get in memory mint's"
            "response");
    return 0;
  }
  memcpy(&(mr->ptr[mr->size]), contents, realsize);
  mr->size += realsize;
  mr->ptr[mr->size] = 0;

  return realsize;

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

  /*printf ("%s\n", url);*/
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
  json_t *j_sig_enc;
  json_t *j_h_contract;
  json_t *j_tmp;
  json_t *eddsa_pub_enc;
  json_t *response;
  json_t *j_mints;
  json_t *j_mint;
  json_t *j_wire;
  int cnt; /* loop counter */
  char *deposit_body;
  json_t *j_contract_add;
  struct GNUNET_TIME_Absolute now;
  struct GNUNET_TIME_Absolute expiry;
  struct GNUNET_TIME_Absolute edate;
  struct GNUNET_TIME_Absolute refund;
  struct GNUNET_HashCode h_json_wire;
  json_t *j_h_json_wire;
  struct curl_slist *slist;
  char *contract_str;
  struct GNUNET_HashCode h_contract_str;
  uint64_t nounce;

  CURL *curl;
  CURLcode curl_res;

  uint32_t res = GNUNET_SYSERR;

  #define URL_HELLO "/hello"
  #define URL_CONTRACT "/contract"
  #define URL_PAY "/pay"
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

  if (0 == strncasecmp (url, URL_PAY, sizeof (URL_PAY)))
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

    /* the POST's body has to be further fetched */
    if ((GNUNET_NO == res) || (NULL == root))
      return MHD_YES;
    
    /* The merchant will only add its 'wire' object to the JSON
    it got from the wallet */

    /* Get this dep. perm.'s H_contract */
	    
    if (NULL == (j_h_contract = json_object_get (root, "H_contract")))
    {
      printf ("H_contract field missing\n");
      status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      goto end;
    }
    TALER_json_to_data (j_h_contract, &h_contract_str, sizeof (struct GNUNET_HashCode));

    nounce = 0;
    edate.abs_value_us = 0;

    if (GNUNET_SYSERR ==
          MERCHANT_DB_get_contract_values (db_conn,
	                                   &h_contract_str,
					   &nounce,
					   &edate))
    {
      printf ("not existing contract\n");
      status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      goto end;
    }

    /* Reproducing the wire object */
    if (NULL == (j_wire = MERCHANT_get_wire_json (wire,
                                                  nounce,
				                  edate)))

    {
       printf ("wire object not reproduced\n");
       status = MHD_HTTP_INTERNAL_SERVER_ERROR;
       goto end;
    }

    if (-1 == json_object_set (root, "wire", j_wire))
    {
       printf ("depperm not augmented\n");
       status = MHD_HTTP_INTERNAL_SERVER_ERROR;
       goto end;
    }

    /* POST to mint's "/deposit" */
    curl = curl_easy_init (); 

    struct Mint_Response mr;
    mr.ptr = malloc(1);
    mr.size = 0;



    if (curl)
    {
    
      slist = curl_slist_append (slist, "Content-type: application/json");
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER, slist);

      /* FIXME the mint's URL is be retrieved from the partial deposit permission
      (received by the wallet) */
      curl_easy_setopt (curl, CURLOPT_URL, "http://demo.taler.net/deposit");
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, get_response_in_memory); 
      curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *) &mr); 
 
      /* NOTE: hopefully, this string won't need any URL-encoding, since as for the
      Jansson specs, any space and-or newline are not in place using JSON_COMPACT
      flag */
      deposit_body = json_dumps (root, JSON_COMPACT | JSON_PRESERVE_ORDER);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, deposit_body);

      curl_res = curl_easy_perform (curl);

      curl_slist_free_all(slist);
      if(curl_res != CURLE_OK)
      {
        printf ("deposit rejected by mint\n");
        goto end; 
      }
      else
        printf ("\ndeposit request issued\n");

      curl_easy_cleanup(curl);
    
      /* Bounce back to the frontend what the mint said */
      status = generate_message (&resp, mr.ptr);
      GNUNET_free (mr.ptr);
 
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
    
    j_mints = json_array ();
    for (cnt = 0; cnt < nmints; cnt++)
    {
      j_mint = json_pack ("{s:s}",
                          mint_infos[cnt].hostname,
                          GNUNET_CRYPTO_eddsa_public_key_to_string (&mint_infos[cnt].pubkey));
      json_array_append_new (j_mints, j_mint);

    }

    /* timestamp */
    now = GNUNET_TIME_absolute_get ();
    /* expiry */
    expiry = GNUNET_TIME_absolute_add (now, GNUNET_TIME_UNIT_WEEKS);
    /* edate, note: this value must be generated now (and not when the
    wallet sends back a deposit permission because the hashed 'wire' object,
    which carries this values in it, has to be included in the signed bundle
    by the wallet) */
    edate = GNUNET_TIME_absolute_add (now, GNUNET_TIME_UNIT_WEEKS);
    refund = GNUNET_TIME_absolute_add (now, GNUNET_TIME_UNIT_WEEKS);

    TALER_round_abs_time (&now);
    TALER_round_abs_time (&expiry);
    TALER_round_abs_time (&edate);
    TALER_round_abs_time (&refund);

    /* getting the SEPA-aware JSON */
    /* nounce for hashing the wire object */
    nounce = GNUNET_CRYPTO_random_u64 (GNUNET_CRYPTO_QUALITY_NONCE, UINT64_MAX);

    /* get wire object */
    
    if (NULL == (j_wire = MERCHANT_get_wire_json (wire,
                                                  nounce,                       
                                                  edate)))
    {
      status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      goto end;
    }

    /* hash wire objcet */
    if (GNUNET_SYSERR == TALER_hash_json (j_wire, &h_json_wire))
      goto end;

    j_h_json_wire = TALER_json_from_data ((void *) &h_json_wire, sizeof (struct GNUNET_HashCode));
    /* JSONify public key */
    eddsa_pub_enc = TALER_json_from_data ((void *) &pub, sizeof (pub));

    if (NULL == (j_contract_add = json_pack ("{s:o, s:s, s:o, s:o, s:o}",
                                             "merchant_pub", eddsa_pub_enc,
                                             "H_wire", json_string_value (j_h_json_wire),
                                             "timestamp", TALER_json_from_abs (now),
                                             "refund", TALER_json_from_abs (refund),
					     "mints", j_mints)))
    {
      printf ("BAD contract enhancement\n");
      goto end;
    }

    /* melt to what received from the wallet */
    if (-1 == json_object_update (root, j_contract_add))
    {
      printf ("depperm response not built\n");
      goto end; 
    }

    res = MERCHANT_handle_contract (root,
                                       db_conn,
                                       &contract,
                                       now,
                                       expiry,
				       edate,
				       refund,
				       &contract_str,
				       nounce);
    if (GNUNET_SYSERR == res)
    {
      status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      goto end;
    }
    if (GNUNET_NO == res)
    {
      status = 	MHD_HTTP_METHOD_NOT_ACCEPTABLE;
      goto end;
    }

    GNUNET_CRYPTO_eddsa_sign (privkey, &contract.purpose, &c_sig);
    GNUNET_CRYPTO_hash (contract_str, strlen (contract_str) + 1, &h_contract_str);

    j_sig_enc = TALER_json_from_eddsa_sig (&contract.purpose, &c_sig);
    GNUNET_CRYPTO_eddsa_key_get_public (privkey, &pub);
    eddsa_pub_enc = TALER_json_from_data ((void *) &pub, sizeof (pub));

    response = json_pack ("{s:o, s:o, s:o, s:o}",
                          "contract", root,
                          "sig", j_sig_enc,
	                  "eddsa_pub", eddsa_pub_enc,
			  "h_contract",
			  TALER_json_from_data ((void *) &h_contract_str, sizeof (struct GNUNET_HashCode)));

    GNUNET_free (contract_str);

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
