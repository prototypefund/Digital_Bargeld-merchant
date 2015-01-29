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
 * @file merchant/test_merchant.c
 * @brief File to test merchant-internal helper functions.
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#include "platform.h"
#include <gnunet/gnunet_util_lib.h>
#include "merchant.h"

/**
 * Array of parsed mints
 */
struct MERCHANT_MintInfo *mints;

/**
 * Number of mints in the above array
 */
int n_mints;

/**
 * Test result
 */
static int result;

static void
do_shutdown (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  unsigned int cnt;

  for (cnt=0; cnt < n_mints; cnt++)
    GNUNET_free (mints[cnt].hostname);
  GNUNET_free_non_null (mints);
  mints = 0;
}


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param config configuration
 */
static void
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *config)
{

  mints = NULL;
  n_mints = GNUNET_SYSERR;
  n_mints = TALER_MERCHANT_parse_mints (config, &mints);
  GNUNET_assert (GNUNET_SYSERR != n_mints);
  GNUNET_assert (NULL != mints);
  result = GNUNET_OK;
  GNUNET_SCHEDULER_add_now (&do_shutdown, NULL);
}

int
main (int argc, char *const argv[])
{
  char *argv2[] = {
    "test-merchant",
    "-c", "test_merchant.conf",
    NULL
  };
   static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };

  result = GNUNET_SYSERR;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run ((sizeof (argv2) / sizeof (char *)) - 1,
                          argv2, "test-merchant",
                          "File to test merchant-internal helper functions.",
                          options, &run, NULL))
    return 3;
  return (GNUNET_OK == result) ? 0 : 1;
}
