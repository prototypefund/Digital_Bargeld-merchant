/*
  This file is part of TALER
  Copyright (C) 2020 Taler Systems SA

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
 * @file lib/merchant_api_common.c
 * @brief Implementation of common logic for libtalermerchant
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <jansson.h>
#include <microhttpd.h> /* just for HTTP status codes */
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include "taler_merchant_service.h"
#include <taler/taler_json_lib.h>


/**
 * Take a @a response from the merchant API that (presumably) contains
 * error details and setup the corresponding @a hr structure. Internally
 * used to convert merchant's responses in to @a hr.
 *
 * @param response, if NULL we will report #TALER_EC_INVALID_RESPONSE in `ec
 * @param http_status http status to use
 * @param[out] hr response object to initialize, fields will
 *        only be valid as long as @a response is valid as well
 */
void
TALER_MERCHANT_parse_error_details_ (const json_t *response,
                                     unsigned int http_status,
                                     struct TALER_MERCHANT_HttpResponse *hr)
{
  const json_t *jc;

  memset (hr, 0, sizeof (*hr));
  hr->reply = response;
  hr->http_status = http_status;
  if (NULL == response)
  {
    hr->ec = TALER_EC_INVALID_RESPONSE;
    return;
  }
  hr->ec = TALER_JSON_get_error_code (response);
  hr->hint = TALER_JSON_get_error_hint (response);

  /* handle 'exchange_http_status' */
  jc = json_object_get (response,
                        "exchange_http_status");
  /* The caller already knows that the JSON represents an error,
     so we are dealing with a missing error code here.  */
  if (NULL == jc)
    return; /* no need to bother with exchange_code/hint if we had no status */
  if (! json_is_integer (jc))
  {
    GNUNET_break_op (0);
    return;
  }
  hr->exchange_http_status = (unsigned int) json_integer_value (jc);

  /* handle 'exchange_reply' */
  jc = json_object_get (response,
                        "exchange_reply");
  if (! json_is_object (jc))
  {
    GNUNET_break_op (0);
  }
  else
  {
    hr->exchange_reply = jc;
  }

  /* handle 'exchange_code' */
  jc = json_object_get (response,
                        "exchange_code");
  /* The caller already knows that the JSON represents an error,
     so we are dealing with a missing error code here.  */
  if (NULL == jc)
    return; /* no need to bother with exchange-hint if we had no code */
  if (! json_is_integer (jc))
  {
    GNUNET_break_op (0);
    hr->exchange_code = TALER_EC_INVALID;
  }
  else
  {
    hr->exchange_code = (enum TALER_ErrorCode) json_integer_value (jc);
  }

  /* handle 'exchange-hint' */
  jc = json_object_get (response,
                        "exchange-hint");
  /* The caller already knows that the JSON represents an error,
     so we are dealing with a missing error code here.  */
  if (NULL == jc)
    return;
  if (! json_is_string (jc))
  {
    GNUNET_break_op (0);
  }
  else
  {
    hr->exchange_hint = json_string_value (jc);
  }
}
