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
#include <taler/taler_util.h>
#include <taler/taler_mint_service.h>
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_responses.h"
#include "merchant_db.h"
#include "merchant.h"
#include "taler_merchant_lib.h"
#include "taler-mint-httpd_mhd.h"
#include "taler-merchant-httpd_contract.h"

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
 * Mint context
 */
static struct TALER_MINT_Context *mctx;

/**
 * Collection of all trusted mints informations
 */
struct MERCHANT_MintInfo *mint_infos;

/**
 * Shutdown task identifier
 */
static struct GNUNET_SCHEDULER_Task *shutdown_task;

/**
 * Hashmap to store the mint context information
 */
static struct GNUNET_CONTAINER_MultiPeerMap *mints_map;

/**
 * Our wireformat
 */
static struct MERCHANT_WIREFORMAT_Sepa *wire;

/**
 * The number of accepted mints
 */
unsigned int nmints;

/**
 * Should we do a dry run where temporary tables are used for storing the data.
 */
static int dry;

/**
 * Global return code
 */
static int result;

/**
 * Connection handle to the our database
 */
PGconn *db_conn;

/**
 * Hashmap (with 'big entries') to make a mint's base URL
 * to point to some mint-describing structure
 */
static struct GNUNET_CONTAINER_MultiHashMap *mints_hashmap;

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
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

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
             void **con_cls)
{
  static struct TMH_RequestHandler handlers[] =
    {
      /* Landing page, tell humans to go away. */
      { "/", MHD_HTTP_METHOD_GET, "text/plain",
        "Hello, I'm a merchant's Taler backend. This HTTP server is not for humans.\n", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK },

      /* Further test page */
      { "/hello", MHD_HTTP_METHOD_GET, "text/plain",
        "Hello, Customer.\n", 0,
        &TMH_MHD_handler_static_response, MHD_HTTP_OK },

      { "/contract", MHD_HTTP_METHOD_GET, "application/json",
        NULL, 0,
        &MH_handler_contract, MHD_HTTP_OK },

      { "/contract", NULL, "text/plain",
        "Only POST is allowed", 0,
        &TMH_MHD_handler_send_json_pack_error, MHD_HTTP_METHOD_NOT_ALLOWED },

      {NULL, NULL, NULL, NULL, 0, 0 }
    };

  static struct TMH_RequestHandler h404 =
    {
      "", NULL, "text/html",
      "<html><title>404: not found</title></html>", 0,
      &TMH_MHD_handler_static_response, MHD_HTTP_NOT_FOUND
    };

  /* Compiler complains about non returning a value in a non-void
    declared function: the FIX is to return what the handler for
    a particular URL returns */

  struct TMH_RequestHandler *rh;
  unsigned int i;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Handling request for URL '%s'\n",
              url);

  for (i=0;NULL != handlers[i].url;i++)
  {
    rh = &handlers[i];
    if ( (0 == strcasecmp (url,
                           rh->url)) &&
         ( (NULL == rh->method) ||
           (0 == strcasecmp (method,
                             rh->method)) ) )
      return rh->handler (rh,
                          connection,
                          con_cls,
                          upload_data,
                          upload_data_size);
  }
  return TMH_MHD_handler_static_response (&h404,
                                          connection,
                                          con_cls,
                                          upload_data,
                                          upload_data_size);

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
  
  /* HOT UPDATE: the merchants needs the denomination keys!
    Because it wants to (firstly) verify the deposit confirmation
    sent by the mint, and the signed blob depends (among the
    other things) on the coin's deposit fee. That information
    is never communicated by the wallet to the merchant.
    Again, the merchant needs it because it wants to verify that
    the wallet didn't exceede the limit imposed by the merchant
    on the total deposit fee for a purchase */


  return;

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
 * Debugging function which prints all non-null fields within
 * a mint descriptor. FIXME: Convert 'printf' in 'GNUNET_log'
 * @param mint mint whose values are getting dumped
 */
void
dump_mint (struct MERCHANT_MintInfo *mint)
{
  char *dump;
  
  #define GET_MINT_VALUE_STRING(fieldname) \
  do { if (NULL != mint->fieldname) \
  { \
    dump = GNUNET_realloc (dump, strlen (dump) \
                                 + strlen (mint->fieldname) \
                                 + strlen (#fieldname ": ") \
				 + 2); \
    sprintf (dump + strlen (dump), #fieldname ": %s\n", mint->fieldname); \
  } \
  } while (0);

  #define GET_MINT_VALUE_UINT16(fieldname) \
  do { if (0 != mint->fieldname && mint->fieldname < 65536) \
  { \
    dump = GNUNET_realloc (dump, strlen (dump) \
                                 + 5 \
                                 + strlen (#fieldname ": ") \
				 + 2); \
    sprintf (dump + strlen (dump), #fieldname ": %d\n", mint->fieldname); \
  } \
  } while (0);

  dump = GNUNET_malloc (1);

  // TODO public key fetch

  GET_MINT_VALUE_STRING(city);
  GET_MINT_VALUE_STRING(province);
  GET_MINT_VALUE_UINT16(zip_code);
  GET_MINT_VALUE_UINT16(port);
  GET_MINT_VALUE_STRING(street);
  GET_MINT_VALUE_STRING(country);
  GET_MINT_VALUE_UINT16(street_no);
  printf ("country = %s\n", mint->country); 
  printf ("Dumping mint:\n%s", dump);
  GNUNET_free (dump);

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
  /* Still not used */
  EXITIF (NULL == (mints_map = GNUNET_CONTAINER_multipeermap_create (nmints, GNUNET_YES)));
  /* Used when the wallet points out which mint it want to deal with.
    That indication is made through the mint's base URL, which will be
    the hash-key for this table */
  EXITIF (NULL == (mints_hashmap = GNUNET_CONTAINER_multihashmap_create (nmints, GNUNET_NO)));
  
  for (cnt = 0; cnt < nmints; cnt++)
  {
    dump_mint (&mint_infos[cnt]);

    struct Mint *mint;
    struct GNUNET_HashCode mint_key;

    mint = GNUNET_new (struct Mint);
    mint->pubkey = mint_infos[cnt].pubkey;

    /* port this to the new API */

    /* ToTest
    mint->conn = TALER_MINT_connect (mctx,
                                     mint_infos[cnt].hostname,
                                     &keys_mgmt_cb,
                                     keys_mgmt_cls);
    EXITIF (NULL == mint->conn);
    */
    
    EXITIF (GNUNET_SYSERR ==
            GNUNET_CONTAINER_multipeermap_put (mints_map,
                                               (struct GNUNET_PeerIdentity *) &mint->pubkey,
                                               mint,
                                               GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_FAST));

     GNUNET_CRYPTO_hash (mint_infos[cnt].hostname,
                         strlen (mint_infos[cnt].hostname),
                         &mint_key); 

     GNUNET_CONTAINER_multihashmap_put (mints_hashmap,
                                        &mint_key,
		                        &mint_infos[cnt],
				        GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_ONLY); 
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
