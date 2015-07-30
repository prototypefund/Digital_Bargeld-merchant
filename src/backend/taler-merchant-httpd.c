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

#include <microhttpd.h>
#include <gnunet/gnunet_util_lib.h>

// task 1. Just implement a hello world server launched a` la GNUNET

/**
 * The port we are running on
 */
static long long unsigned port;

/**
 * The MHD Daemon
 */
static struct MHD_Daemon *mhd;

/**
* Generate the 'hello world' response
* @param connection a MHD connection
* @param resp where to store the response for the calling function
* @return HTTP status code reflecting the operation outcome
*
*/
 
static unsigned int
generate_hello (struct MHD_Connection *connection,
                     struct MHD_Response **resp) // this parameter was preceded by a '_' in its original file. Why?
{
 
  const char *hello = "Hello customer";
  unsigned int ret;

  *resp = MHD_create_response_from_buffer (strlen (hello), (void *) hello,
                                           MHD_RESPMEM_PERSISTENT);
  ret = 200;
  return ret;


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
  struct MHD_Response *resp;

  #define URL_HELLO "/hello"
  #define STR_404_NOTFOUND "The requested resource is not found"

  if (0 == strncasecmp (url, URL_HELLO, sizeof (URL_HELLO)))
    {
      /* parse for /contract */
      if (0 == strcmp (MHD_HTTP_METHOD_GET, method))
        status = generate_hello (connection, &resp); //TBD
      else
        GNUNET_break (0);         /* FIXME: implement for post */
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
 * The main function of the serve tool
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  
  mhd = MHD_start_daemon (MHD_USE_DEBUG, //| MHD_USE_TCP_FASTOPEN,
                          (unsigned short) port,
                          NULL, NULL,
                          &url_handler, NULL,
                          //MHD_OPTION_TCP_FASTOPEN_QUEUE_SIZE,
                          //(unsigned int) 16,
                          MHD_OPTION_END);

  getchar (); 
  MHD_stop_daemon (daemon);
  return 0;





}
