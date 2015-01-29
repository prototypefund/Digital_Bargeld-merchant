#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([taler], [0.0.0], [taler-bug@gnunet.org])
AC_CONFIG_SRCDIR([src/merchant/merchant.c])
AC_CONFIG_HEADERS([taler_config.h])
# support for non-recursive builds
AM_INIT_AUTOMAKE([subdir-objects])

# pretty build rules
AM_SILENT_RULES([yes])

AC_CONFIG_MACRO_DIR([m4])

LT_INIT

# Checks for programs.
AC_PROG_CC

CFLAGS="-Wall $CFLAGS"

# Checks for header files.
AC_CHECK_HEADERS([stdint.h stdlib.h string.h unistd.h])

# Check for GNUnet's libgnunetutil.
libgnunetutil=0
AC_MSG_CHECKING([for libgnunetutil])
AC_ARG_WITH(gnunet,
            [AS_HELP_STRING([--with-gnunet=PFX], [base of GNUnet installation])],
            [AC_MSG_RESULT([given as $with_gnunet])],
            [AC_MSG_RESULT(not given)
             with_gnunet=yes])
AS_CASE([$with_gnunet],
        [yes], [],
        [no], [AC_MSG_ERROR([--with-gnunet is required])],
        [LDFLAGS="-L$with_gnunet/lib $LDFLAGS"
         CPPFLAGS="-I$with_gnunet/include $CPPFLAGS"])
AC_CHECK_HEADERS([gnunet/platform.h gnunet/gnunet_util_lib.h],
 [AC_CHECK_LIB([gnunetutil], [GNUNET_SCHEDULER_run], libgnunetutil=1)],
  [], [#ifdef HAVE_GNUNET_PLATFORM_H
        #include <gnunet/platform.h>
       #endif])
AS_IF([test $libgnunetutil != 1],
  [AC_MSG_ERROR([[
***
*** You need libgnunetutil to build this program.
*** This library is part of GNUnet, available at
***   https://gnunet.org
*** ]])])


# check for libpq (postgresql)
AX_LIB_POSTGRESQL([9.3])
AS_IF([test ! "$found_postgresql" = "yes"],
  [AC_MSG_ERROR([[
***
*** You need postgresql / libpq to build this program.
*** ]])])


# Check for Taler's libtalerpq
libtalerpq=0
AC_MSG_CHECKING([for libtalerpq])
AC_ARG_WITH(mint,
            [AS_HELP_STRING([--with-mint=PFX], [base of Taler MINT installation])],
            [AC_MSG_RESULT([given as $with_mint])],
            [AC_MSG_RESULT(not given)
             with_mint=yes])
AS_CASE([$with_mint],
        [yes], [],
        [no], [AC_MSG_ERROR([--with-mint is required])],
        [LDFLAGS="-L$with_mint/lib $LDFLAGS"
         CPPFLAGS="-I$with_mint/include $CPPFLAGS $POSTGRESQL_CPPFLAGS"])
AC_CHECK_HEADERS([taler/db_pq.h],
 [AC_CHECK_LIB([talerpq], [TALER_DB_exec_prepared], libtalerpq=1)],
   [], [#ifdef HAVE_GNUNET_PLATFORM_H
        #include <gnunet/platform.h>
       #endif])
AS_IF([test $libtalerpq != 1],
  [AC_MSG_ERROR([[
***
*** You need libtalerpq to build this program.
*** This library is part of the Taler MINT, available at
***   https://taler.net
*** ]])])


# check for libmicrohttpd
microhttpd=0
AC_MSG_CHECKING([for microhttpd])
AC_ARG_WITH([microhttpd],
            [AS_HELP_STRING([--with-microhttpd=PFX], [base of microhttpd installation])],
            [AC_MSG_RESULT([given as $with_microhttpd])],
            [AC_MSG_RESULT([not given])
             with_microhttpd=yes])
AS_CASE([$with_microhttpd],
        [yes], [],
        [no], [AC_MSG_ERROR([--with-microhttpd is required])],
        [LDFLAGS="-L$with_microhttpd/lib $LDFLAGS"
         CPPFLAGS="-I$with_microhttpd/include $CPPFLAGS"])
AC_CHECK_LIB(microhttpd,MHD_start_daemon,
  [AC_CHECK_HEADER([microhttpd.h],[microhttpd=1])])
AS_IF([test $microhttpd = 0],
  [AC_MSG_ERROR([[
***
*** You need libmicrohttpd to build this program.
*** ]])])


# check for libjansson (Jansson JSON library)
jansson=0
AC_MSG_CHECKING([for jansson])
AC_ARG_WITH([jansson],
            [AS_HELP_STRING([--with-jansson=PFX], [base of jansson installation])],
            [AC_MSG_RESULT([given as $with_jansson])],
            [AC_MSG_RESULT([not given])
             with_jansson=yes])
AS_CASE([$with_jansson],
        [yes], [],
        [no], [AC_MSG_ERROR([--with-jansson is required])],
        [LDFLAGS="-L$with_jansson/lib $LDFLAGS"
         CPPFLAGS="-I$with_jansson/include $CPPFLAGS"])
AC_CHECK_LIB(jansson,json_pack,
  [AC_CHECK_HEADER([jansson.h],[jansson=1])])
AS_IF([test $jansson = 0],
  [AC_MSG_ERROR([[
***
*** You need libjansson to build this program.
*** ]])])

# check for libgnurl
LIBGNURL_CHECK_CONFIG([], [7.34.0], [gnurl=1], [gnurl=0])
if test "$gnurl" = 1
then
	AM_CONDITIONAL(HAVE_LIBGNURL, true)
	AC_DEFINE([HAVE_LIBGNURL],[1],[Have libgnurl])
else
	AM_CONDITIONAL(HAVE_LIBGNURL, false)
fi
AS_IF([test $gnurl = 0],
  [AC_MSG_ERROR([[
***
*** You need libgnurl to build this program.
*** ]])])

# Require minimum libgcrypt version
need_libgcrypt_version=1.6.1
AC_DEFINE_UNQUOTED([NEED_LIBGCRYPT_VERSION], ["$need_libgcrypt_version"],
                                             [minimum version of libgcrypt required])
AM_PATH_LIBGCRYPT([$need_libgcrypt_version])

# logging
extra_logging=0
AC_ARG_ENABLE([logging],
   AS_HELP_STRING([--enable-logging@<:@=value@:>@],[Enable logging calls. Possible values: yes,no,verbose ('yes' is the default)]),
   [AS_IF([test "x$enableval" = "xyes"], [],
          [test "x$enableval" = "xno"], [AC_DEFINE([GNUNET_CULL_LOGGING],[],[Define to cull all logging calls])],
          [test "x$enableval" = "xverbose"], [extra_logging=1]
          [test "x$enableval" = "xveryverbose"], [extra_logging=2])
   ], [])
AC_DEFINE_UNQUOTED([GNUNET_EXTRA_LOGGING],[$extra_logging],[1 if extra logging is enabled, 2 for very verbose extra logging, 0 otherwise])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_INTMAX_T
AC_TYPE_UINTMAX_T

# Checks for library functions.
AC_CHECK_FUNCS([strdup])

AC_CONFIG_FILES([Makefile
                 doc/Makefile
                 src/Makefile
                 src/include/Makefile
                 src/merchant/Makefile])
AC_OUTPUT
