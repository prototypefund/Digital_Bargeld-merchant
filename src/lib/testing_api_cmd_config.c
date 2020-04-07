/*
  This file is part of TALER
  Copyright (C) 2020 Taler Systems SA

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
 * @file lib/testing_api_cmd_config.c
 * @brief command to test config request
 * @author Christian Grothoff
 */

#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State for a "config" CMD.
 */
struct ConfigState
{
  /**
   * Operation handle for a GET /public/config request.
   */
  struct TALER_MERCHANT_ConfigGetHandle *vgh;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_code;

  /**
   * Interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

};


/**
 * Free the state of a "config" CMD, and
 * possibly cancel a pending "config" operation.
 *
 * @param cls closure with the `struct ConfigState`
 * @param cmd command currently being freed.
 */
static void
config_cleanup (void *cls,
                const struct TALER_TESTING_Command *cmd)
{
  struct ConfigState *cs = cls;

  if (NULL != cs->vgh)
  {
    TALER_LOG_WARNING ("config operation did not complete\n");
    TALER_MERCHANT_config_get_cancel (cs->vgh);
  }
  GNUNET_free (cs);
}


/**
 * Process "GET /public/config" (lookup) response.
 *
 * @param cls closure
 * @param hr HTTP response we got
 * @param ci basic information about the merchant
 * @param compat protocol compatibility information
 */
static void
config_cb (void *cls,
           const struct TALER_MERCHANT_HttpResponse *hr,
           const struct TALER_MERCHANT_ConfigInformation *ci,
           enum TALER_MERCHANT_VersionCompatibility compat)
{
  struct ConfigState *cs = cls;

  (void) ci;
  cs->vgh = NULL;
  if (cs->http_code != hr->http_status)
    TALER_TESTING_FAIL (cs->is);
  if (TALER_MERCHANT_VC_MATCH != compat)
    TALER_TESTING_FAIL (cs->is);
  TALER_TESTING_interpreter_next (cs->is);
}


/**
 * Run the "config" CMD.
 *
 * @param cls closure.
 * @param cmd command being currently run.
 * @param is interpreter state.
 */
static void
config_run (void *cls,
            const struct TALER_TESTING_Command *cmd,
            struct TALER_TESTING_Interpreter *is)
{
  struct ConfigState *cs = cls;

  cs->is = is;
  cs->vgh = TALER_MERCHANT_config_get (is->ctx,
                                       cs->merchant_url,
                                       &config_cb,
                                       cs);
  GNUNET_assert (NULL != cs->vgh);
}


/**
 * Define a "config" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        "config" request.
 * @param http_code expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_config (const char *label,
                          const char *merchant_url,
                          unsigned int http_code)
{
  struct ConfigState *cs;

  cs = GNUNET_new (struct ConfigState);
  cs->merchant_url = merchant_url;
  cs->http_code = http_code;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = cs,
      .label = label,
      .run = &config_run,
      .cleanup = &config_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_config.c */
