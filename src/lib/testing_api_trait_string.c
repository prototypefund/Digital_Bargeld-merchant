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
 * @file lib/testing_api_trait_string.c
 * @brief offer traits that come as strings.
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>

#define TALER_TESTING_TRAIT_PROPOSAL_REFERENCE "proposal-reference"
#define TALER_TESTING_TRAIT_COIN_REFERENCE "coin-reference"

/**
 * Obtain a reference to a proposal command.  Any command that
 * works with proposals, might need to offer their reference to
 * it.  Notably, the "pay" command, offers its proposal reference
 * to the "pay abort" command as the latter needs to reconstruct
 * the same data needed by the former in order to use the "pay
 * abort" API.
 *
 * @param cmd command to extract trait from
 * @param index which reference to pick if @a cmd has multiple
 *        on offer
 * @param proposal_reference[out] set to the wanted reference.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_proposal_reference
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const char **proposal_reference)
{
  return cmd->traits (cmd->cls,
                      (void **) proposal_reference,
                      TALER_TESTING_TRAIT_PROPOSAL_REFERENCE,
                      index);
}


/**
 * Offer a proposal reference.
 *
 * @param index which reference to offer if there are
 *        multiple on offer
 * @param proposal_reference set to the offered reference.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_proposal_reference
  (unsigned int index,
   const char *proposal_reference)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_PROPOSAL_REFERENCE,
    .ptr = (const void *) proposal_reference
  };
  return ret;
}


/**
 * Obtain a reference to any command that can provide coins as
 * traits.
 *
 * @param cmd command to extract trait from
 * @param index which reference to pick if @a cmd has multiple
 *        on offer
 * @param coin_reference[out] set to the wanted reference. NOTE:
 *        a _single_ reference can contain _multiple_ instances,
 *        using semi-colon as separator.  For example, a _single_
 *        reference can be this: "coin-ref-1", or even this:
 *        "coin-ref-1;coin-ref-2".  The "pay" command contains
 *        functions that can parse such format.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_coin_reference
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   const char **coin_reference)
{
  return cmd->traits (cmd->cls,
                      (void **) coin_reference,
                      TALER_TESTING_TRAIT_COIN_REFERENCE,
                      index);
}


/**
 * Offer a coin reference.
 *
 * @param index which reference to offer if there are
 *        multiple on offer
 * @param coin_reference set to the offered reference.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_coin_reference
  (unsigned int index,
   const char *coin_reference)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_COIN_REFERENCE,
    .ptr = (const void *) coin_reference
  };
  return ret;
}


/* end of testing_api_trait_string.c */
