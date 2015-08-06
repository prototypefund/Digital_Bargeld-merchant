/*
  This file is part of TALER
  Copyright (C) 2014, 2015 Christian Grothoff (and other contributing authors)

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
 * @file include/taler_util.h
 * @brief Interface for common utility functions
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */
#ifndef TALER_UTIL_H
#define TALER_UTIL_H

#include <gnunet/gnunet_util_lib.h>
#include "taler_amount_lib.h"
#include "taler_crypto_lib.h"
#include "taler_json_lib.h"



/* Define logging functions */
#define TALER_LOG_DEBUG(...)                                  \
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, __VA_ARGS__)

#define TALER_LOG_WARNING(...)                                \
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, __VA_ARGS__)

#define TALER_LOG_ERROR(...)                                  \
  GNUNET_log (GNUNET_ERROR_TYPE_ERROR, __VA_ARGS__)


/**
 * Tests a given as assertion and if failed prints it as a warning with the
 * given reason
 *
 * @param EXP the expression to test as assertion
 * @param reason string to print as warning
 */
#define TALER_assert_as(EXP, reason)                           \
  do {                                                          \
    if (EXP) break;                                             \
    TALER_LOG_ERROR("%s at %s:%d\n", reason, __FILE__, __LINE__);       \
    abort();                                                    \
  } while(0)


/**
 * Log an error message at log-level 'level' that indicates
 * a failure of the command 'cmd' with the message given
 * by gcry_strerror(rc).
 */
#define TALER_LOG_GCRY_ERROR(cmd, rc) do { TALER_LOG_ERROR("`%s' failed at %s:%d with error: %s\n", cmd, __FILE__, __LINE__, gcry_strerror(rc)); } while(0)


#define TALER_gcry_ok(cmd) \
  do {int rc; rc = cmd; if (!rc) break; TALER_LOG_ERROR("A Gcrypt call failed at %s:%d with error: %s\n", __FILE__, __LINE__, gcry_strerror(rc)); abort(); } while (0)


/**
 * Initialize Gcrypt library.
 */
void
TALER_gcrypt_init (void);


/**
 * Round a time value so that it is suitable for transmission
 * via JSON encodings.
 *
 * @param at time to round
 * @return #GNUNET_OK if time was already rounded, #GNUNET_NO if
 *         it was just now rounded
 */
int
TALER_round_abs_time (struct GNUNET_TIME_Absolute *at);


/**
 * Round a time value so that it is suitable for transmission
 * via JSON encodings.
 *
 * @param rt time to round
 * @return #GNUNET_OK if time was already rounded, #GNUNET_NO if
 *         it was just now rounded
 */
int
TALER_round_rel_time (struct GNUNET_TIME_Relative *rt);


/**
 * Load configuration by parsing all configuration
 * files in the given directory.
 *
 * @param base_dir directory with the configuration files
 * @return NULL on error, otherwise configuration
 */
struct GNUNET_CONFIGURATION_Handle *
TALER_config_load (const char *base_dir);


/**
 * Obtain denomination amount from configuration file.
 *
 * @param section section of the configuration to access
 * @param option option of the configuration to access
 * @param[out] denom set to the amount found in configuration
 * @return #GNUNET_OK on success, #GNUNET_SYSERR on error
 */
int
TALER_config_get_denom (struct GNUNET_CONFIGURATION_Handle *cfg,
                        const char *section,
                        const char *option,
                        struct TALER_Amount *denom);


/**
 * Get the path to a specific Taler installation directory or, with
 * #GNUNET_OS_IPK_SELF_PREFIX, the current running apps installation
 * directory.
 *
 * @param dirkind what kind of directory is desired?
 * @return a pointer to the dir path (to be freed by the caller)
 */
char *
TALER_OS_installation_get_path (enum GNUNET_OS_InstallationPathKind dirkind);


/**
 * Print out details on command line options (implements --help).
 *
 * @param ctx command line processing context
 * @param scls additional closure (points to about text)
 * @param option name of the option
 * @param value not used (NULL)
 * @return #GNUNET_NO (do not continue, not an error)
 */
int
TALER_GETOPT_format_help_ (struct GNUNET_GETOPT_CommandLineProcessorContext *ctx,
			   void *scls,
			   const char *option,
			   const char *value);

/**
 * Macro defining the option to print the command line
 * help text (-h option).
 *
 * @param about string with brief description of the application
 */
#define TALER_GETOPT_OPTION_HELP(about) \
  { 'h', "help", (const char *) NULL, gettext_noop("print this help"), 0, &TALER_GETOPT_format_help_, (void *) about }

#endif
