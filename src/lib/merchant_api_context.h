/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V.

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant-lib/merchant_api_context.h
 * @brief Internal interface to the context part of the merchant's HTTP API
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <gnunet/gnunet_util_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_signatures.h>


/**
 * Entry in the context's job queue.
 */
struct MAC_Job;

/**
 * Function to call upon completion of a job.
 *
 * @param cls closure
 * @param eh original easy handle (for inspection)
 */
typedef void
(*MAC_JobCompletionCallback)(void *cls,
                             CURL *eh);


/**
 * Schedule a CURL request to be executed and call the given @a jcc
 * upon its completion. Note that the context will make use of the
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
             void *jcc_cls);


/**
 * Obtain the `jcc_cls` argument from an `eh` that was
 * given to #MAC_job_add().
 *
 * @param eh easy handle that was used
 * @return the `jcc_cls` that was given to #MAC_job_add().
 */
void *
MAC_easy_to_closure (CURL *eh);


/**
 * Cancel a job.  Must only be called before the job completion
 * callback is called for the respective job.
 *
 * @param job job to cancel
 */
void
MAC_job_cancel (struct MAC_Job *job);


/**
 * @brief Buffer data structure we use to buffer the HTTP download
 * before giving it to the JSON parser.
 */
struct MAC_DownloadBuffer
{

  /**
   * Download buffer
   */
  void *buf;

  /**
   * The size of the download buffer
   */
  size_t buf_size;

  /**
   * Error code (based on libc errno) if we failed to download
   * (i.e. response too large).
   */
  int eno;

};


/**
 * Callback used when downloading the reply to an HTTP request.
 * Just appends all of the data to the `buf` in the
 * `struct MAC_DownloadBuffer` for further processing. The size of
 * the download is limited to #GNUNET_MAX_MALLOC_CHECKED, if
 * the download exceeds this size, we abort with an error.
 *
 * Should be used by the various routines as the
 * CURLOPT_WRITEFUNCTION.  A `struct MAC_DownloadBuffer` needs to be
 * passed to the CURLOPT_WRITEDATA.
 *
 * Afterwards, `eno` needs to be checked to ensure that the download
 * completed correctly.
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
                 void *cls);


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
                         long *response_code);


/* end of merchant_api_context.h */
