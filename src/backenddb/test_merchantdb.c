/*
  This file is part of TALER
  (C) 2014-2017, 2020 Taler Systems SA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 3, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along with
  TALER; see the file COPYING.  If not, see <http://www.gnu.org/licenses/>
*/
/**
 * @file merchant/test_merchantdb.c
 * @brief testcase for merchant's postgres db plugin
 * @author Marcello Stanisci
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_util.h>
#include <taler/taler_json_lib.h>
#include "taler_merchantdb_lib.h"


/**
 * Global return value for the test.  Initially -1, set to 0 upon
 * completion.  Other values indicate some kind of error.
 */
static int result;

/**
 * Handle to the plugin we are testing.
 */
static struct TALER_MERCHANTDB_Plugin *plugin;


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure with config
 */
static void
run (void *cls)
{
  struct GNUNET_CONFIGURATION_Handle *cfg = cls;
  /* Data for 'store_payment()' */

  if (NULL == (plugin = TALER_MERCHANTDB_plugin_load (cfg)))
  {
    result = 77;
    return;
  }
  if (GNUNET_OK != plugin->drop_tables (plugin->cls))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Dropping tables failed\n");
    result = 77;
    return;
  }
  TALER_MERCHANTDB_plugin_unload (plugin);
  if (NULL == (plugin = TALER_MERCHANTDB_plugin_load (cfg)))
  {
    result = 77;
    return;
  }

  result = 0;

  GNUNET_break (GNUNET_OK ==
                plugin->drop_tables (plugin->cls));
  TALER_MERCHANTDB_plugin_unload (plugin);
  plugin = NULL;
}


int
main (int argc,
      char *const argv[])
{
  const char *plugin_name;
  char *config_filename;
  char *testname;
  struct GNUNET_CONFIGURATION_Handle *cfg;

  result = -1;
  if (NULL == (plugin_name = strrchr (argv[0],
                                      (int) '-')))
  {
    GNUNET_break (0);
    return -1;
  }
  GNUNET_log_setup (argv[0], "DEBUG", NULL);
  plugin_name++;
  (void) GNUNET_asprintf (&testname,
                          "test-merchantdb-%s",
                          plugin_name);
  (void) GNUNET_asprintf (&config_filename,
                          "%s.conf",
                          testname);
  cfg = GNUNET_CONFIGURATION_create ();
  if (GNUNET_OK !=
      GNUNET_CONFIGURATION_parse (cfg,
                                  config_filename))
  {
    GNUNET_break (0);
    GNUNET_free (config_filename);
    GNUNET_free (testname);
    return 2;
  }
  GNUNET_SCHEDULER_run (&run,
                        cfg);
  GNUNET_CONFIGURATION_destroy (cfg);
  GNUNET_free (config_filename);
  GNUNET_free (testname);
  return result;
}


/* end of test_merchantdb.c */
