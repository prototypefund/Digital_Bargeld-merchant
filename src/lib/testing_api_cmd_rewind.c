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
 * @file lib/testing_api_cmd_rewind.c
 * @brief command to rewind the instruction pointer.
 * @author Marcello Stanisci
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "rewind" CMD.
 */
struct RewindIpState
{
  /**
   * Instruction pointer to set into the interpreter.
   */
  unsigned int new_ip;

  /**
   * How many times this set should take place.
   * However, this value lives at the calling process,
   * and this CMD is only in charge of checking and
   * decremeting it.
   */
  unsigned int *counter;
};


/**
 * Only defined to respect the API.
 */
static void
rewind_ip_cleanup (void *cls,
                   const struct TALER_TESTING_Command *cmd)
{}


/**
 * Run the "rewind" CMD.
 *
 * @param cls closure.
 * @param cmd command being executed now.
 * @param is the interpreter state.
 */
static void
rewind_ip_run (void *cls,
               const struct TALER_TESTING_Command *cmd,
               struct TALER_TESTING_Interpreter *is)
{
  struct RewindIpState *ris = cls;

  if (1 < *ris->counter)
  {
    is->ip = ris->new_ip;
    *ris->counter -= 1; 
  }

  TALER_TESTING_interpreter_next (is);
}

/**
 * Make the instruction pointer point to @a new_ip
 * only if @a counter is greater than zero.
 *
 * @param label command label
 * @param new_ip new instruction pointer's value.  Note that,
 * when the next instruction will be called, the interpreter
 * will increment the ip _anyway_ so this value must be
 * set to the index of the instruction we want to execute next
 * MINUS one.
 * @param counter counts how many times the rewinding has
 * to happen.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_rewind_ip
  (const char *label,
   int new_ip,
   unsigned int *counter)
{
  struct RewindIpState *ris;

  ris = GNUNET_new (struct RewindIpState);
  ris->new_ip = new_ip;
  ris->counter = counter;

  struct TALER_TESTING_Command cmd = {
    .cls = ris,
    .label = label,
    .run = &rewind_ip_run,
    .cleanup = &rewind_ip_cleanup
  };

  return cmd;
}
