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
#include <gnunet/gnunet_util_lib.h>
#include <taler/taler_json_lib.h>


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
 * Shutdown task identifier
 */
static struct GNUNET_SCHEDULER_Task *shutdown_task;

/**
 * Should we do a dry run where temporary tables are used for storing the data.
 */
static int dry;

/**
 * Global return code
 */
static int result;

/** Beginning of JSON parse logic 
* Located here only for testing purposes since the service they provide is already
* implemented in the mint's code; it just needs to be exported as a library. To be announced as a issue.
*/

/**
 * Initial size for POST
 * request buffer.
 */
#define REQUEST_BUFFER_INITIAL 1024

/**
 * Maximum POST request size
 */
#define REQUEST_BUFFER_MAX (1024*1024)


/**
 * Buffer for POST requests.
 */
struct Buffer
{
  /**
   * Allocated memory
   */
  char *data;

  /**
   * Number of valid bytes in buffer.
   */
  size_t fill;

  /**
   * Number of allocated bytes in buffer.
   */
  size_t alloc;
};


/**
 * Initialize a buffer.
 *
 * @param buf the buffer to initialize
 * @param data the initial data
 * @param data_size size of the initial data
 * @param alloc_size size of the buffer
 * @param max_size maximum size that the buffer can grow to
 * @return a GNUnet result code
 */
static int
buffer_init (struct Buffer *buf, const void *data, size_t data_size, size_t alloc_size, size_t max_size)
{
  if (data_size > max_size || alloc_size > max_size)
    return GNUNET_SYSERR;
  if (data_size > alloc_size)
    alloc_size = data_size;
  buf->data = GNUNET_malloc (alloc_size);
  memcpy (buf->data, data, data_size);
  return GNUNET_OK;
}


/**
 * Free the data in a buffer.  Does *not* free
 * the buffer object itself.
 *
 * @param buf buffer to de-initialize
 */
static void
buffer_deinit (struct Buffer *buf)
{
  GNUNET_free (buf->data);
  buf->data = NULL;
}


/**
 * Append data to a buffer, growing the buffer if necessary.
 *
 * @param buf the buffer to append to
 * @param data the data to append
 * @param size the size of @a data
 * @param max_size maximum size that the buffer can grow to
 * @return GNUNET_OK on success,
 *         GNUNET_NO if the buffer can't accomodate for the new data
 *         GNUNET_SYSERR on fatal error (out of memory?)
 */
static int
buffer_append (struct Buffer *buf, const void *data, size_t data_size, size_t max_size)
{
  if (buf->fill + data_size > max_size)
    return GNUNET_NO;
  if (data_size + buf->fill > buf->alloc)
  {
    char *new_buf;
    size_t new_size = buf->alloc;
    while (new_size < buf->fill + data_size)
      new_size += 2;
    if (new_size > max_size)
      return GNUNET_NO;
    new_buf = GNUNET_malloc (new_size);
    memcpy (new_buf, buf->data, buf->fill);
    buf->data = new_buf;
    buf->alloc = new_size;
  }
  memcpy (buf->data + buf->fill, data, data_size);
  buf->fill += data_size;
  return GNUNET_OK;
}



/**
 * Process a POST request containing a JSON object.
 *
 * @param connection the MHD connection
 * @param con_cs the closure (contains a 'struct Buffer *')
 * @param upload_data the POST data
 * @param upload_data_size the POST data size
 * @param json the JSON object for a completed request
 *
 * @returns
 *    GNUNET_YES if json object was parsed
 *    GNUNET_NO is request incomplete or invalid
 *    GNUNET_SYSERR on internal error
 */
static int
process_post_json (struct MHD_Connection *connection,
                   void **con_cls,
                   const char *upload_data,
                   size_t *upload_data_size,
                   json_t **json)
{
  struct Buffer *r = *con_cls;

  if (NULL == *con_cls)
  {
    /* We are seeing a fresh POST request. */

    r = GNUNET_new (struct Buffer);
    if (GNUNET_OK != buffer_init (r, upload_data, *upload_data_size,
                 REQUEST_BUFFER_INITIAL, REQUEST_BUFFER_MAX))
    {
      *con_cls = NULL;
      buffer_deinit (r);
      GNUNET_free (r);
      return GNUNET_SYSERR;
    }
    *upload_data_size = 0;
    *con_cls = r;
    return GNUNET_NO;
  }
  if (0 != *upload_data_size)
  {
    /* We are seeing an old request with more data available. */

    if (GNUNET_OK != buffer_append (r, upload_data, *upload_data_size,
                                    REQUEST_BUFFER_MAX))
    {
      /* Request too long or we're out of memory. */

      *con_cls = NULL;
      buffer_deinit (r);
      GNUNET_free (r);
      return GNUNET_SYSERR;
    }
    *upload_data_size = 0;
    return GNUNET_NO;
  }

  /* We have seen the whole request. */

  *json = json_loadb (r->data, r->fill, 0, NULL);
  buffer_deinit (r);
  GNUNET_free (r);
  if (NULL == *json)
  {
    struct MHD_Response *resp;
    int ret;

    GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Can't parse JSON request body\n");
    resp = MHD_create_response_from_buffer (strlen ("parse error"),
                                            "parse error",
                                            MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response (connection,
                              MHD_HTTP_BAD_REQUEST,
                              resp);
    MHD_destroy_response (resp);
    return ret;
  }
  *con_cls = NULL;

  return GNUNET_YES;
}


/* ************** END of JSON POST processing logic ************ */



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

  unsigned int status;
  unsigned int no_destroy;
  struct MHD_Response *resp;


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

        GNUNET_break (0);


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
