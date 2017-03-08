/*
  This file is part of TALER
  Copyright (C) 2014, 2015, 2016 GNUnet e.V. and INRIA

  TALER is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License as published by the Free Software
  Foundation; either version 2.1, or (at your option) any later version.

  TALER is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License along with
  TALER; see the file COPYING.LGPL.  If not, see <http://www.gnu.org/licenses/>
*/

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_fakebank_lib.h>
#include <taler/taler_json_lib.h>
#include <taler/taler_util.h>
#include <taler/taler_signatures.h>
#include "taler_merchant_service.h"
#include "taler_merchantdb_lib.h"
#include <gnunet/gnunet_util_lib.h>
#include <gnunet/gnunet_curl_lib.h>
#include <microhttpd.h>


/**
 * Configuration handle.
 */
struct GNUNET_CONFIGURATION_Handle *cfg;


int
main ()
{
  struct GNUNET_OS_Process *proc;
  struct GNUNET_OS_Process *exchanged;
  struct GNUNET_OS_Process *merchantd;
  unsigned int cnt;
  struct GNUNET_SIGNAL_Context *shc_chld;


  /* 1 Launch exchange */

  unsetenv ("XDG_DATA_HOME");
  unsetenv ("XDG_CONFIG_HOME");
  GNUNET_log_setup ("merchant-create-payments",
                    "DEBUG",
                    NULL);
  cfg = GNUNET_CONFIGURATION_create ();
  GNUNET_assert (GNUNET_OK ==
                 GNUNET_CONFIGURATION_load (cfg,
                                            "merchant_generate_payments.conf"));


  proc = GNUNET_OS_start_process (GNUNET_NO,
                                  GNUNET_OS_INHERIT_STD_ALL,
                                  NULL, NULL, NULL,
                                  "taler-exchange-keyup",
                                  "taler-exchange-keyup",
                                  "-c", "merchant_generate_payments.conf",
                                  NULL);
  if (NULL == proc)
  {
    fprintf (stderr,
             "Failed to run taler-exchange-keyup. Check your PATH.\n");
    return 77;
  }


  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);

  proc = GNUNET_OS_start_process (GNUNET_NO,
                                  GNUNET_OS_INHERIT_STD_ALL,
                                  NULL, NULL, NULL,
                                  "taler-exchange-dbinit",
                                  "taler-exchange-dbinit",
                                  "-c", "merchant_generate_payments.conf",
                                  "-r",
                                  NULL);
  if (NULL == proc)
  {
    fprintf (stderr,
             "Failed to run taler-exchange-dbinit. Check your PATH.\n");
    return 77;
  }
  GNUNET_OS_process_wait (proc);
  GNUNET_OS_process_destroy (proc);

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "About to launch the exchange.\n");

  exchanged = GNUNET_OS_start_process (GNUNET_NO,
                                       GNUNET_OS_INHERIT_STD_ALL,
                                       NULL, NULL, NULL,
                                       "taler-exchange-httpd",
                                       "taler-exchange-httpd",
                                       "-c", "merchant_generate_payments.conf",
                                       NULL);
  if (NULL == exchanged)
  {
    fprintf (stderr,
             "Failed to run taler-exchange-httpd. Check your PATH.\n");
    return 77;
  }

  /*Remove this!*/
  GNUNET_OS_process_wait (exchanged);
  GNUNET_OS_process_destroy (exchanged);

}
