/*
  This file is part of TALER
  Copyright (C) 2018 Taler Systems SA

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
 * @file lib/testing_api_trait_merchant_sig.c
 * @brief offer merchant signature over contract
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>

#define TALER_TESTING_TRAIT_MERCHANT_SIG "reserve-private-key"

/**
 * Obtain a merchant signature over a contract from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param merchant_sig[out] set to the wanted signature.
 *
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_merchant_sig
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   struct TALER_MerchantSignatureP **merchant_sig)
{
  return cmd->traits (cmd->cls,
                      (const void **) merchant_sig,
                      TALER_TESTING_TRAIT_MERCHANT_SIG,
                      index);
}

/**
 * Offer a merchant signature over a contract.
 *
 * @param index which signature to offer if there are multiple
 *        on offer
 * @param merchant_sig set to the offered signature.
 *
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_merchant_sig
  (unsigned int index,
   const struct TALER_MerchantSignatureP *merchant_sig)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_MERCHANT_SIG,
    .ptr = (const void *) merchant_sig
  };
  return ret;
}

/* end of testing_api_trait_merchant_sig.c */
