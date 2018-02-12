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
 * @file lib/testing_api_trait_hash.c
 * @brief offer any trait that is passed over as a hash code.
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>

#define TALER_TESTING_TRAIT_TIP_ID "tip-id"
#define TALER_TESTING_TRAIT_H_CONTRACT_TERMS "h-contract-terms"

/**
 * Obtain tip id from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which signature to pick if @a cmd has multiple
 *        on offer
 * @param tip_id[out] set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_tip_id
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   struct GNUNET_HashCode **tip_id)
{
  return cmd->traits (cmd->cls,
                      (void **) tip_id,
                      TALER_TESTING_TRAIT_TIP_ID,
                      index);
}


/**
 * Offer tip id.
 *
 * @param index which tip id to offer if there are
 *        multiple on offer
 * @param planchet_secrets set to the offered secrets.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_tip_id
  (unsigned int index,
   const struct GNUNET_HashCode *tip_id)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_TIP_ID,
    .ptr = (const void *) tip_id
  };
  return ret;
}


/**
 * Obtain contract terms hash from a @a cmd.
 *
 * @param cmd command to extract trait from
 * @param index which hash code to pick if @a cmd has multiple
 *        on offer
 * @param h_contract_terms[out] set to the wanted data.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_h_contract_terms
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const struct GNUNET_HashCode **h_contract_terms)
{
  return cmd->traits (cmd->cls,
                      (void **) h_contract_terms,
                      TALER_TESTING_TRAIT_H_CONTRACT_TERMS,
                      index);
}


/**
 * Offer contract terms hash code.
 *
 * @param index which hash code to offer if there are
 *        multiple on offer
 * @param h_contract_terms set to the offered hash code.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_h_contract_terms
  (unsigned int index,
   const struct GNUNET_HashCode *h_contract_terms)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_H_CONTRACT_TERMS,
    .ptr = (const void *) h_contract_terms
  };
  return ret;
}
