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
 * @file lib/testing_api_trait_planchet.c
 * @brief offer planchet secrets as trait.
 * @author Marcello Stanisci
 */
#include "platform.h"
#include <taler/taler_signatures.h>
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>

#define TALER_TESTING_TRAIT_PLANCHET_SECRETS "planchet-secrets"

/**
 * Obtain planchet secrets from a @a cmd.
 *
 * @param cmd command to extract trait from.
 * @param index index of the trait.
 * @param planchet_secrets[out] set to the wanted secrets.
 * @return #GNUNET_OK on success
 */
int
TALER_TESTING_get_trait_planchet_secrets
  (const struct TALER_TESTING_Command *cmd,
   unsigned int index,
   struct TALER_PlanchetSecretsP **planchet_secrets)
{
  return cmd->traits (cmd->cls,
                      (void **) planchet_secrets,
                      TALER_TESTING_TRAIT_PLANCHET_SECRETS,
                      index);
}

/**
 * Offer planchet secrets.
 *
 * @param index of the trait.
 * @param planchet_secrets set to the offered secrets.
 * @return the trait
 */
struct TALER_TESTING_Trait
TALER_TESTING_make_trait_planchet_secrets
  (unsigned int index,
   const struct TALER_PlanchetSecretsP *planchet_secrets)
{
  struct TALER_TESTING_Trait ret = {
    .index = index,
    .trait_name = TALER_TESTING_TRAIT_PLANCHET_SECRETS,
    .ptr = (const void *) planchet_secrets
  };
  return ret;
}

/* end of testing_api_trait_planchet.c */
