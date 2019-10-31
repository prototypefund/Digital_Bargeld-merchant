/*
  This file is part of TALER
  Copyright (C) 2014-2018 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 3, or
  (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public
  License along with TALER; see the file COPYING.  If not, see
  <http://www.gnu.org/licenses/>
*/

/**
 * @file lib/testing_api_trait_refund_entry.c
 * @brief command to offer refund entry trait.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"

#define TALER_TESTING_TRAIT_REFUND_ENTRY "refund-entry"

/**
 * Obtain refund entry from a @a cmd.
 *
 * @param cmd command to extract the trait from.
 * @param index the trait index.
 * @param refund_entry[out] set to the wanted data.
 *
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_refund_entry
  (const struct TALER_TESTING_Command *cmd,
  unsigned int index,
  const struct TALER_MERCHANT_RefundEntry **refund_entry)
{
  return cmd->traits (cmd->cls,
                      (const void **) refund_entry,
                      TALER_TESTING_TRAIT_REFUND_ENTRY,
                      index);
}


/**
 * Offer refund entry.
 *
 * @param index index number of the trait to offer.
 * @param refund_entry set to the offered refund entry.
 *
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_refund_entry
  (unsigned int index,
  const struct TALER_MERCHANT_RefundEntry *refund_entry)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_REFUND_ENTRY,
    .ptr = (const void *) refund_entry
  };
  return ret;
}


/* end of testing_api_trait_refund_entry.c */
