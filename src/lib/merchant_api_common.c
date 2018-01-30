/*
  This file is part of TALER
  Copyright (C) 2014-2017 GNUnet e.V. and INRIA

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
 * @brief Shared functionality
 * @author Christian Grothoff
 */
#include "platform.h"
#include <curl/curl.h>
#include <gnunet/gnunet_util_lib.h>


/**
 * Obtain the URL to use for an API request.
 *
 * @param base_url base URL of the exchange (i.e. "http://exchange/")
 * @param path Taler API path (i.e. "/reserve/withdraw")
 * @return the full URL to use with cURL
 */
char *
MAH_path_to_url_ (const char *base_url,
		  const char *path)
{
  char *url;

  if ( ('/' == path[0]) &&
       (0 < strlen (base_url)) &&
       ('/' == base_url[strlen (base_url) - 1]) )
    path++; /* avoid generating URL with "//" from concat */
  GNUNET_asprintf (&url,
                   "%s%s",
                   base_url,
                   path);
  return url;
}


/**
 * Concatenate two strings and grow the first buffer (of size n)
 * if necessary.
 */
#define STR_CAT_GROW(s, p, n) do { \
    for (; strlen (s) + strlen (p) >= n; (n) = (n) * 2); \
    (s) = GNUNET_realloc ((s), (n)); \
    GNUNET_assert (NULL != (s)); \
    strncat (s, p, n); \
  } while (0)


/**
 * Make an absolute URL with query parameters.
 *
 * @param base_url absolute base URL to use
 * @param path path of the url
 * @param ... NULL-terminated key-value pairs (char *) for query parameters
 * @returns the URL, must be freed with #GNUNET_free
 */
char *
MAH_make_url (const char *base_url,
              const char *path,
              ...)
{
  static CURL *curl = NULL;
  if (NULL == curl)
  {
    curl = curl_easy_init();
    GNUNET_assert (NULL != curl);
  }

  size_t n = 256;
  char *res = GNUNET_malloc (n);

  GNUNET_assert (NULL != res);

  STR_CAT_GROW (res, base_url, n);

  if ( ('/' == path[0]) &&
       (0 < strlen (base_url)) &&
       ('/' == base_url[strlen (base_url) - 1]) )
  {
   /* avoid generating URL with "//" from concat */
    path++;
  }
  else if ( ('/' != path[0]) && 
            ('/' != base_url[strlen (base_url) - 1]))
  {
    /* put '/' between path and base URL if necessary */
    STR_CAT_GROW (res, "/", n);
  }

  STR_CAT_GROW (res, path, n);

  va_list args;
  va_start (args, path);

  unsigned int iparam = 0;

  while (1) {
    char *key = va_arg (args, char *);
    if (NULL == key)
      break;
    char *value = va_arg (args, char *);
    if (NULL == value)
      continue;
    if (0 == iparam)
      STR_CAT_GROW (res, "?", n);
    else
      STR_CAT_GROW (res, "&", n);
    iparam++;
    char *urlencoded_value = curl_easy_escape (curl, value, strlen (value));
    STR_CAT_GROW (res, key, n);
    STR_CAT_GROW (res, "=", n);
    STR_CAT_GROW (res, urlencoded_value, n);
    curl_free (urlencoded_value);
  }

  va_end (args);

  return res;
}

/* end of merchant_api_common.c */
