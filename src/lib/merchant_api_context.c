/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant-lib/merchant_api_context.c
 * @brief Implementation of the context part of the merchant's HTTP API
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include "taler_merchant_service.h"
#include "merchant_api_context.h"


/**
 * Log error related to CURL operations.
 *
 * @param type log level
 * @param function which function failed to run
 * @param code what was the curl error code
 */
#define CURL_STRERROR(type, function, code)      \
 GNUNET_log (type,                               \
             "Curl function `%s' has failed at `%s:%d' with error: %s\n", \
             function, __FILE__, __LINE__, curl_easy_strerror (code));

/**
 * Print JSON parsing related error information
 */
#define JSON_WARN(error)                                                \
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,                              \
                "JSON parsing failed at %s:%u: %s (%s)\n",              \
                __FILE__, __LINE__, error.text, error.source)


/**
 * Failsafe flag. Raised if our constructor fails to initialize
 * the Curl library.
 */
static int TALER_MERCHANT_curl_fail;


/**
 * Jobs are CURL requests running within a `struct TALER_MERCHANT_Context`.
 */
struct MAC_Job
{

  /**
   * We keep jobs in a DLL.
   */
  struct MAC_Job *next;

  /**
   * We keep jobs in a DLL.
   */
  struct MAC_Job *prev;

  /**
   * Easy handle of the job.
   */
  CURL *easy_handle;

  /**
   * Context this job runs in.
   */
  struct TALER_MERCHANT_Context *ctx;

  /**
   * Function to call upon completion.
   */
  MAC_JobCompletionCallback jcc;

  /**
   * Closure for @e jcc.
   */
  void *jcc_cls;

};


/**
 * Context
 */
struct TALER_MERCHANT_Context
{
  /**
   * Curl multi handle
   */
  CURLM *multi;

  /**
   * Curl share handle
   */
  CURLSH *share;

  /**
   * We keep jobs in a DLL.
   */
  struct MAC_Job *jobs_head;

  /**
   * We keep jobs in a DLL.
   */
  struct MAC_Job *jobs_tail;

  /**
   * HTTP header "application/json", created once and used
   * for all requests that need it.
   */
  struct curl_slist *json_header;

};


/**
 * Initialise this library.  This function should be called before using any of
 * the following functions.
 *
 * @return library context
 */
struct TALER_MERCHANT_Context *
TALER_MERCHANT_init ()
{
  struct TALER_MERCHANT_Context *ctx;
  CURLM *multi;
  CURLSH *share;

  if (TALER_MERCHANT_curl_fail)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Curl was not initialised properly\n");
    return NULL;
  }
  if (NULL == (multi = curl_multi_init ()))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to create a Curl multi handle\n");
    return NULL;
  }
  if (NULL == (share = curl_share_init ()))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Failed to create a Curl share handle\n");
    return NULL;
  }
  ctx = GNUNET_new (struct TALER_MERCHANT_Context);
  ctx->multi = multi;
  ctx->share = share;
  GNUNET_assert (NULL != (ctx->json_header =
                          curl_slist_append (NULL,
                                             "Content-Type: application/json")));
  return ctx;
}


/**
 * Schedule a CURL request to be executed and call the given @a jcc
 * upon its completion.  Note that the context will make use of the
 * CURLOPT_PRIVATE facility of the CURL @a eh.  Applications can
 * instead use #MAC_easy_to_closure to extract the @a jcc_cls argument
 * from a valid @a eh afterwards.
 *
 * This function modifies the CURL handle to add the
 * "Content-Type: application/json" header if @a add_json is set.
 *
 * @param ctx context to execute the job in
 * @param eh curl easy handle for the request, will
 *           be executed AND cleaned up
 * @param add_json add "application/json" content type header
 * @param jcc callback to invoke upon completion
 * @param jcc_cls closure for @a jcc
 */
struct MAC_Job *
MAC_job_add (struct TALER_MERCHANT_Context *ctx,
             CURL *eh,
             int add_json,
             MAC_JobCompletionCallback jcc,
             void *jcc_cls)
{
  struct MAC_Job *job;

  if (GNUNET_YES == add_json)
    GNUNET_assert (CURLE_OK ==
                   curl_easy_setopt (eh,
                                     CURLOPT_HTTPHEADER,
                                     ctx->json_header));

  job = GNUNET_new (struct MAC_Job);
  job->easy_handle = eh;
  job->ctx = ctx;
  job->jcc = jcc;
  job->jcc_cls = jcc_cls;
  GNUNET_CONTAINER_DLL_insert (ctx->jobs_head,
                               ctx->jobs_tail,
                               job);
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_PRIVATE,
                                   job));
  GNUNET_assert (CURLE_OK ==
                 curl_easy_setopt (eh,
                                   CURLOPT_SHARE,
                                   ctx->share));
  GNUNET_assert (CURLM_OK ==
                 curl_multi_add_handle (ctx->multi,
                                        eh));
  return job;
}


/**
 * Obtain the `jcc_cls` argument from an `eh` that was
 * given to #MAC_job_add().
 *
 * @param eh easy handle that was used
 * @return the `jcc_cls` that was given to #MAC_job_add().
 */
void *
MAC_easy_to_closure (CURL *eh)
{
  struct MAC_Job *job;

  GNUNET_assert (CURLE_OK ==
                 curl_easy_getinfo (eh,
                                    CURLINFO_PRIVATE,
                                    (char **) &job));
  return job->jcc_cls;
}


/**
 * Cancel a job.  Must only be called before the job completion
 * callback is called for the respective job.
 *
 * @param job job to cancel
 */
void
MAC_job_cancel (struct MAC_Job *job)
{
  struct TALER_MERCHANT_Context *ctx = job->ctx;

  GNUNET_CONTAINER_DLL_remove (ctx->jobs_head,
                               ctx->jobs_tail,
                               job);
  GNUNET_assert (CURLM_OK ==
                 curl_multi_remove_handle (ctx->multi,
                                           job->easy_handle));
  curl_easy_cleanup (job->easy_handle);
  GNUNET_free (job);
}


/**
 * Run the main event loop for the Taler interaction.
 *
 * @param ctx the library context
 */
void
TALER_MERCHANT_perform (struct TALER_MERCHANT_Context *ctx)
{
  CURLMsg *cmsg;
  struct MAC_Job *job;
  int n_running;
  int n_completed;

  (void) curl_multi_perform (ctx->multi,
                             &n_running);
  while (NULL != (cmsg = curl_multi_info_read (ctx->multi,
                                               &n_completed)))
  {
    /* Only documented return value is CURLMSG_DONE */
    GNUNET_break (CURLMSG_DONE == cmsg->msg);
    GNUNET_assert (CURLE_OK ==
                   curl_easy_getinfo (cmsg->easy_handle,
                                      CURLINFO_PRIVATE,
                                      (char *) &job));
    GNUNET_assert (job->ctx == ctx);
    job->jcc (job->jcc_cls,
              cmsg->easy_handle);
    MAC_job_cancel (job);
  }
}


/**
 * Obtain the information for a select() call to wait until
 * #TALER_MERCHANT_perform() is ready again.  Note that calling
 * any other TALER_MERCHANT-API may also imply that the library
 * is again ready for #TALER_MERCHANT_perform().
 *
 * Basically, a client should use this API to prepare for select(),
 * then block on select(), then call #TALER_MERCHANT_perform() and then
 * start again until the work with the context is done.
 *
 * This function will NOT zero out the sets and assumes that @a max_fd
 * and @a timeout are already set to minimal applicable values.  It is
 * safe to give this API FD-sets and @a max_fd and @a timeout that are
 * already initialized to some other descriptors that need to go into
 * the select() call.
 *
 * @param ctx context to get the event loop information for
 * @param read_fd_set will be set for any pending read operations
 * @param write_fd_set will be set for any pending write operations
 * @param except_fd_set is here because curl_multi_fdset() has this argument
 * @param max_fd set to the highest FD included in any set;
 *        if the existing sets have no FDs in it, the initial
 *        value should be "-1". (Note that `max_fd + 1` will need
 *        to be passed to select().)
 * @param timeout set to the timeout in milliseconds (!); -1 means
 *        no timeout (NULL, blocking forever is OK), 0 means to
 *        proceed immediately with #TALER_MERCHANT_perform().
 */
void
TALER_MERCHANT_get_select_info (struct TALER_MERCHANT_Context *ctx,
				fd_set *read_fd_set,
				fd_set *write_fd_set,
				fd_set *except_fd_set,
				int *max_fd,
				long *timeout)
{
  long to;
  int m;

  m = -1;
  GNUNET_assert (CURLM_OK ==
                 curl_multi_fdset (ctx->multi,
                                   read_fd_set,
                                   write_fd_set,
                                   except_fd_set,
                                   &m));
  *max_fd = GNUNET_MAX (m, *max_fd);
  to = *timeout;
  GNUNET_assert (CURLM_OK ==
                 curl_multi_timeout (ctx->multi,
                                     &to));
  /* Only if what we got back from curl is smaller than what we
     already had (-1 == infinity!), then update timeout */
  if ( (to < *timeout) &&
       (-1 != to) )
    *timeout = to;
  if ( (-1 == (*timeout)) &&
       (NULL != ctx->jobs_head) )
    *timeout = 1000 * 60 * 5; /* curl is not always good about giving timeouts */
}


/**
 * Cleanup library initialisation resources.  This function should be called
 * after using this library to cleanup the resources occupied during library's
 * initialisation.
 *
 * @param ctx the library context
 */
void
TALER_MERCHANT_fini (struct TALER_MERCHANT_Context *ctx)
{
  /* all jobs must have been cancelled at this time, assert this */
  GNUNET_assert (NULL == ctx->jobs_head);
  curl_share_cleanup (ctx->share);
  curl_multi_cleanup (ctx->multi);
  curl_slist_free_all (ctx->json_header);
  GNUNET_free (ctx);
}


/**
 * Callback used when downloading the reply to an HTTP request.
 * Just appends all of the data to the `buf` in the
 * `struct MAC_DownloadBuffer` for further processing. The size of
 * the download is limited to #GNUNET_MAX_MALLOC_CHECKED, if
 * the download exceeds this size, we abort with an error.
 *
 * @param bufptr data downloaded via HTTP
 * @param size size of an item in @a bufptr
 * @param nitems number of items in @a bufptr
 * @param cls the `struct KeysRequest`
 * @return number of bytes processed from @a bufptr
 */
size_t
MAC_download_cb (char *bufptr,
                 size_t size,
                 size_t nitems,
                 void *cls)
{
  struct MAC_DownloadBuffer *db = cls;
  size_t msize;
  void *buf;

  if (0 == size * nitems)
  {
    /* Nothing (left) to do */
    return 0;
  }
  msize = size * nitems;
  if ( (msize + db->buf_size) >= GNUNET_MAX_MALLOC_CHECKED)
  {
    db->eno = ENOMEM;
    return 0; /* signals an error to curl */
  }
  db->buf = GNUNET_realloc (db->buf,
                            db->buf_size + msize);
  buf = db->buf + db->buf_size;
  memcpy (buf, bufptr, msize);
  db->buf_size += msize;
  return msize;
}


/**
 * Obtain information about the final result about the
 * HTTP download. If the download was successful, parses
 * the JSON in the @a db and returns it. Also returns
 * the HTTP @a response_code.  If the download failed,
 * the return value is NULL.  The response code is set
 * in any case, on download errors to zero.
 *
 * Calling this function also cleans up @a db.
 *
 * @param db download buffer
 * @param eh CURL handle (to get the response code)
 * @param[out] response_code set to the HTTP response code
 *             (or zero if we aborted the download, i.e.
 *              because the response was too big, or if
 *              the JSON we received was malformed).
 * @return NULL if downloading a JSON reply failed
 */
json_t *
MAC_download_get_result (struct MAC_DownloadBuffer *db,
                         CURL *eh,
                         long *response_code)
{
  json_t *json;
  json_error_t error;
  char *ct;

  if ( (CURLE_OK !=
        curl_easy_getinfo (eh,
                           CURLINFO_CONTENT_TYPE,
                           &ct)) ||
       (NULL == ct) ||
       (0 != strcasecmp (ct,
                         "application/json")) )
  {
    /* No content type or explicitly not JSON, refuse to parse
       (but keep response code) */
    if (CURLE_OK !=
        curl_easy_getinfo (eh,
                           CURLINFO_RESPONSE_CODE,
                           response_code))
    {
      /* unexpected error... */
      GNUNET_break (0);
      *response_code = 0;
    }
    return NULL;
  }

  json = NULL;
  if (0 == db->eno)
  {
    json = json_loadb (db->buf,
                       db->buf_size,
                       JSON_REJECT_DUPLICATES | JSON_DISABLE_EOF_CHECK,
                       &error);
    if (NULL == json)
    {
      JSON_WARN (error);
      *response_code = 0;
    }
  }
  GNUNET_free_non_null (db->buf);
  db->buf = NULL;
  db->buf_size = 0;
  if (NULL != json)
  {
    if (CURLE_OK !=
        curl_easy_getinfo (eh,
                           CURLINFO_RESPONSE_CODE,
                           response_code))
    {
      /* unexpected error... */
      GNUNET_break (0);
      *response_code = 0;
    }
  }
  return json;
}


/**
 * Initial global setup logic, specifically runs the Curl setup.
 */
__attribute__ ((constructor))
void
TALER_MERCHANT_constructor__ (void)
{
  CURLcode ret;

  if (CURLE_OK != (ret = curl_global_init (CURL_GLOBAL_DEFAULT)))
  {
    CURL_STRERROR (GNUNET_ERROR_TYPE_ERROR,
                   "curl_global_init",
                   ret);
    TALER_MERCHANT_curl_fail = 1;
  }
}


/**
 * Cleans up after us, specifically runs the Curl cleanup.
 */
__attribute__ ((destructor))
void
TALER_MERCHANT_destructor__ (void)
{
  if (TALER_MERCHANT_curl_fail)
    return;
  curl_global_cleanup ();
}

/* end of merchant_api_context.c */
