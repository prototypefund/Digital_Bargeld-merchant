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
 * @file lib/testing_api_cmd_delete_instance.c
 * @brief command to test DELETE /instance/$ID
 * @author Christian Grothoff
 */
#include "platform.h"
#include <taler/taler_exchange_service.h>
#include <taler/taler_testing_lib.h>
#include "taler_merchant_service.h"
#include "taler_merchant_testing_lib.h"


/**
 * State of a "DELETE /instances/$ID" CMD.
 */
struct DeleteInstanceState
{

  /**
   * Handle for a "DELETE instance" request.
   */
  struct TALER_MERCHANT_InstanceDeleteHandle *igh;

  /**
   * The interpreter state.
   */
  struct TALER_TESTING_Interpreter *is;

  /**
   * Base URL of the merchant serving the request.
   */
  const char *merchant_url;

  /**
   * ID of the instance to run DELETE for.
   */
  const char *instance_id;

  /**
   * Expected HTTP response code.
   */
  unsigned int http_status;

};


/**
 * Callback for a /delete/instances/$ID operation.
 *
 * @param cls closure for this function
 */
static void
delete_instance_cb (void *cls,
                    const struct TALER_MERCHANT_HttpResponse *hr)
{
  struct DeleteInstanceState *dis = cls;

  dis->igh = NULL;
  if (dis->http_status != hr->http_status)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Unexpected response code %u (%d) to command %s\n",
                hr->http_status,
                (int) hr->ec,
                TALER_TESTING_interpreter_get_current_label (dis->is));
    TALER_TESTING_interpreter_fail (dis->is);
    return;
  }
  switch (hr->http_status)
  {
  case MHD_HTTP_OK:
    break;
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Unhandled HTTP status.\n");
  }
  TALER_TESTING_interpreter_next (dis->is);
}


/**
 * Run the "DELETE instance" CMD.
 *
 *
 * @param cls closure.
 * @param cmd command being run now.
 * @param is interpreter state.
 */
static void
delete_instance_run (void *cls,
                     const struct TALER_TESTING_Command *cmd,
                     struct TALER_TESTING_Interpreter *is)
{
  struct DeleteInstanceState *dis = cls;

  dis->is = is;
  dis->igh = TALER_MERCHANT_instance_delete (is->ctx,
                                             dis->merchant_url,
                                             dis->instance_id,
                                             &delete_instance_cb,
                                             dis);
  GNUNET_assert (NULL != dis->igh);
}


/**
 * Free the state of a "DELETE instance" CMD, and possibly
 * cancel a pending operation thereof.
 *
 * @param cls closure.
 * @param cmd command being run.
 */
static void
delete_instance_cleanup (void *cls,
                         const struct TALER_TESTING_Command *cmd)
{
  struct DeleteInstanceState *dis = cls;

  if (NULL != dis->igh)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "DELETE /instances/$ID operation did not complete\n");
    TALER_MERCHANT_instance_delete_cancel (dis->igh);
  }
  GNUNET_free (dis);
}


/**
 * Define a "DELETE instance" CMD.
 *
 * @param label command label.
 * @param merchant_url base URL of the merchant serving the
 *        DELETE /instances/$ID request.
 * @param instance_id the ID of the instance to query
 * @param http_status expected HTTP response code.
 * @return the command.
 */
struct TALER_TESTING_Command
TALER_TESTING_cmd_merchant_delete_instance (const char *label,
                                            const char *merchant_url,
                                            const char *instance_id,
                                            unsigned int http_status)
{
  struct DeleteInstanceState *dis;

  dis = GNUNET_new (struct DeleteInstanceState);
  dis->merchant_url = merchant_url;
  dis->instance_id = instance_id;
  dis->http_status = http_status;
  {
    struct TALER_TESTING_Command cmd = {
      .cls = dis,
      .label = label,
      .run = &delete_instance_run,
      .cleanup = &delete_instance_cleanup
    };

    return cmd;
  }
}


/* end of testing_api_cmd_delete_instance.c */
