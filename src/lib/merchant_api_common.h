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
 * @file lib/merchant_api_common.h
 * @brief Shared functions
 * @author Christian Grothoff
 */
#ifndef MERCHANT_API_COMMON_H
#define MERCHANT_API_COMMON_H

/**
 * Obtain the URL to use for an API request.
 *
 * @param base_url base URL of the exchange (i.e. "http://exchange/")
 * @param path Taler API path (i.e. "/reserve/withdraw")
 * @return the full URI to use with cURL
 */
char *
MAH_path_to_url_ (const char *base_url,
		  const char *path);

#endif
