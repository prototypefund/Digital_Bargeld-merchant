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
#include "taler-mint-httpd_parsing.h"
#include "taler-mint-httpd_mhd.h"
#include "taler-mint-httpd_admin.h"
#include "taler-mint-httpd_deposit.h"
#include "taler-mint-httpd_withdraw.h"
#include "taler-mint-httpd_refresh.h"
#include "taler-mint-httpd_keystate.h"
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

// task 1. Just implement a hello world server launched a` la GNUNET

/**
 * The port we are running on
 */
unsigned short port;

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
 * Should we do a dry run where temporary tables are used for storing the data.
 */
static int dry;

/**
 * Global return code
 */
static int result;

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
 
  const char *hello = "Hello customer";
  unsigned int ret;

  *resp = MHD_create_response_from_buffer (strlen (hello), (void *) hello,
                                           MHD_RESPMEM_PERSISTENT);
  ret = 200;
  return ret;


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
  printf ("called failure mgmt\n");
  static char page_404[]="\
<!DOCTYPE html>                                         \
<html><title>Resource not found</title><body><center>   \
<h3>The resource you are looking for is not found.</h3> \
</center></body></html>";
  static char page_500[]="\
<!DOCTYPE html> <html><title>Internal Server Error</title><body><center> \
<h3>The server experienced an internal error and hence cannot serve your \
request</h3></center></body></html>";
  struct MHD_Response *resp;
  char *page;
  size_t size;
#define PAGE(number) \
  do {page=page_ ## number; size=sizeof(page_ ## number)-1;} while(0)

  GNUNET_assert (400 <= status);
  resp = NULL;
  switch (status)
  {
  case 404:
    PAGE(404);
    break;
  default:
    status = 500;
  case 500:
    PAGE(500);
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

  unsigned int status;
  unsigned int no_destroy;
  struct MHD_Response *resp;
  struct TALER_Amount price;
  json_t json_price;
  json_t *root;
  int res;
  char *desc;

  #define URL_HELLO "/hello"
  #define URL_CONTRACT "/contract"
  no_destroy = 0;
  resp = NULL;
  status = 500;
  if (0 == strncasecmp (url, URL_HELLO, sizeof (URL_HELLO)))
    {
      if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
        status = generate_hello (&resp); //TBD
      else
        GNUNET_break (0);
    }

  // to be called by the frontend passing all the product's information
  // which are relevant for the contract's generation
  if (0 == strncasecmp (url, URL_CONTRACT, sizeof (URL_CONTRACT)))
    {
      if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
        status = generate_message (&resp, "Sorry, only POST is allowed");
      else

        /*
          1. parse the json
	  2. generate the contract
	  3. pack the contract's json
	  4. return it
	*/

    res = TMH_PARSE_post_json (connection,
                               connection_cls,
                               upload_data,
                               upload_data_size,
                               &root);
  
    if (GNUNET_SYSERR == res)
      return MHD_NO;
    if ( (GNUNET_NO == res) || (NULL == root) )
      return MHD_YES;
  
    /* not really needed for getting just a string. Though it'd be very handy
    to enhace the mint's JSON-parsing capabilities with the merchant's needs.
  
    struct TMH_PARSE_FieldSpecification spec[] = {
      TMH_PARSE_member_variable ("desc", (void **) &desc, &desc_len),
      TMH_PARSE_member_amount ("price", &price),
      TMH_PARSE_MEMBER_END
    };
    res = TMH_PARSE_json_data (connection,
                               root,
                               spec); */
    /*
  
     The expected JSON :
  
      {
      "desc"  : "some description",
      "price" : a JSON compliant TALER_Amount objet
      }
  
    */
  
    if (!json_unpack (root, "{s:s, s:o}", "desc", &desc, "price", &json_price))
      status = generate_message (&resp, "unable to parse /contract JSON");
    else
    {
      if (GNUNET_OK != TALER_json_to_amount (&json_price, &price))
        status = generate_message (&resp, "unable to parse `price' field in /contract JSON");
      else
        {
         /* Let's generate this contract! */
	 /* First, initialize the DB, since it'll be stored there */
       
       
       
       
       
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
    //GNUNET_SCHEDULER_shutdown (); to a later stage, maybe
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

  port = 9966;


  EXITIF (NULL == (wire = TALER_MERCHANT_parse_wireformat_sepa (config)));
  EXITIF (GNUNET_OK != MERCHANT_DB_initialize (db_conn, dry));

  shutdown_task = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
                                                &do_shutdown, NULL);

  mhd = MHD_start_daemon (MHD_USE_SELECT_INTERNALLY,
                          port,
                          NULL, NULL,
                          &url_handler, NULL,
                          MHD_OPTION_END);


  EXITIF (NULL == mhd);
  result = GNUNET_OK;

  EXITIF_exit:
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
