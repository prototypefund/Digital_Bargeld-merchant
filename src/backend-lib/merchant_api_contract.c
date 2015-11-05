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
 * @file merchant/merchant_db.c
 * @brief DB work related to contract management
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <jansson.h>
#include <taler/taler_signatures.h>
#include <gnunet/gnunet_util_lib.h>
#include "merchant.h"
#include "merchant_db.h"
#include "taler_merchant_contract_lib.h"

/**
 * Take the global wire details and return a JSON containing them,
 * compliantly with the Taler's API.
 * @param wire the merchant's wire details
 * @param salt the nounce for hashing the wire details with
 * @param edate when the beneficiary wants this transfer to take place
 * @return JSON representation of the wire details, NULL upon errors
 */

json_t *
MERCHANT_get_wire_json (const struct MERCHANT_WIREFORMAT_Sepa *wire,
                        uint64_t salt)

{
  
  json_t *root;
  json_t *j_salt;

  j_salt = json_integer (salt);

  if (NULL == (root = json_pack ("{s:s, s:s, s:s, s:s, s:I}",
                                 "type", "SEPA",
		                 "IBAN", wire->iban,
		                 "name", wire->name,
		                 "bic", wire->bic,
		                 "r", json_integer_value (j_salt))))
    return NULL;

  return root;
}
