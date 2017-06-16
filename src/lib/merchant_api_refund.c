/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016, 2017 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, see
  <http://www.gnu.org/licenses/>
*/
/**
 * @file lib/merchant_api_proposal.c
 * @brief Implementation of the /refund POST and GET
 * @author Christian Grothoff
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_json_lib.h>
#include <taler/taler_signatures.h>

struct TALER_MERCHANT_RefundIncreaseOperation
{
  /**
   * Complete URL where the backend offers /refund
   */
  char *url;

  /**
   * The CURL context to connect to the backend
   */
  struct GNUNET_CURL_Context *ctx;

  /**
   * The callback to pass the backend response to
   */
  TALER_MERCHANT_RefundIncreaseCallback cb;

  /**
   * Clasure to pass to the callback
   */
  void *cb_cls;
};


/**
 * Increase the refund associated to a order
 *
 * @param ctx the CURL context used to connect to the backend
 * @param backend_uri backend's base URL, including final "/"
 * @param order_id id of the order whose refund is to be increased
 */
struct TALER_MERCHANT_RefundIncreaseOperation *
TALER_MERCHANT_refund_increase (struct GNUNET_CURL_Context *ctx,
                                const char *backend_uri,
                                const char *order_id,
                                const struct TALER_Amount *refund,
                                const char *reason,
                                TALER_MERCHANT_RefundIncreaseCallback cb,
                                void *cb_cls)
{
  struct TALER_MERCHANT_RefundIncreaseOperation *rio;
  json_t *req;
  CURL *eh;

  rio = GNUNET_new (struct TALER_MERCHANT_RefundIncreaseOperation);
  rio->ctx = ctx;
  rio->cb = cb;
  rio->cb_cls = cb_cls;
  GNUNET_asprintf (&rio->url,
                   "%s/%s",
                   backend_uri,
                   "/refund");
  /**
   * FIXME: pack the data to POST.
   */

  return NULL;
}
