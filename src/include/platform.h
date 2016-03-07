/*
  This file is part of TALER
  Copyright (C) 2014, 2015 GNUnet e.V. and INRIA

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
 * @file include/platform.h
 * @brief This file contains the includes and definitions which are used by the
 *        rest of the modules
 * @author Sree Harsha Totakura <sreeharsha@totakura.in>
 */

#ifndef PLATFORM_H_
#define PLATFORM_H_

/* Include our configuration header */
#ifndef HAVE_USED_CONFIG_H
# define HAVE_USED_CONFIG_H
# ifdef HAVE_CONFIG_H
#  include "taler_merchant_config.h"
# endif
#endif


#if (GNUNET_EXTRA_LOGGING >= 1)
#define VERBOSE(cmd) cmd
#else
#define VERBOSE(cmd) do { break; }while(0)
#endif

/* Include the features available for GNU source */
#define _GNU_SOURCE

/* Include GNUnet's platform file */
#include <gnunet/platform.h>

/* Do not use shortcuts for gcrypt mpi */
#define GCRYPT_NO_MPI_MACROS 1

/* Do not use deprecated functions from gcrypt */
#define GCRYPT_NO_DEPRECATED 1

#endif  /* PLATFORM_H_ */

/* end of platform.h */
