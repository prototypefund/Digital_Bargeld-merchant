/*
  This file is part of TALER
  Copyright (C) 2015, 2016 GNUnet e.V. and INRIA

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
 * @file merchantdb/test_merchantdb.c
 * @brief Testcases for backenddb
 * @author Marcello Stanisci
 * @author Sree Harsha Totakura
 */

/**
 * Connection handle to the our database
 */
struct TALER_MERCHANTDB_Plugin *db;

/* FIXME define 'drop' label */
#define FAILIF(cond)                              \
  do {                                          \
    if (!(cond)){ break;}                      \
    GNUNET_break (0);                           \
    goto drop;                                  \
  } while (0)


/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param cfg configuration
 */
static void
run (void *cls,
     char *const *args,
     const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *cfg)
{

  EXITIF (NULL == (db = TALER_MERCHANTDB_plugin_load (cfg)));
  EXITIF (GNUNET_SYSERR == db->initialize())
  // crea sample data

  // call plugin's functions


  TALER_MERCHANTDB_plugin_unload (db);
  
  // define FAILIF
  // define EXITIF
}

int
main (int argc,
      char *const argv[])
{
   static const struct GNUNET_GETOPT_CommandLineOption options[] = {
    GNUNET_GETOPT_OPTION_END
  };
   char *argv2[] = {
     "test-merchant-db-<plugin_name>", /* will be replaced later */
     "-c", "test-merchant-db-<plugin_name>.conf", /* will be replaced later */
     NULL,
   };
   const char *plugin_name;
   char *config_filename;
   char *testname;

   result = -1;
   if (NULL == (plugin_name = strrchr (argv[0], (int) '-')))
   {
     GNUNET_break (0);
     return -1;
   }
   plugin_name++;
   (void) GNUNET_asprintf (&testname,
                           "test-merchant-db-%s", plugin_name);
   (void) GNUNET_asprintf (&config_filename,
                           "%s.conf", testname);
   argv2[0] = argv[0];
   argv2[2] = config_filename;
  if (GNUNET_OK !=
      GNUNET_PROGRAM_run ((sizeof (argv2)/sizeof (char *)) - 1, argv2,
                          testname,
                          "Test cases for merchant database helper functions.",
                          options, &run, NULL))
  {
    GNUNET_free (config_filename);
    GNUNET_free (testname);
    return 3;
  }
  GNUNET_free (config_filename);
  GNUNET_free (testname);
  return result;
}
