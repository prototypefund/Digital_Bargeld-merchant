#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.
#
# This configure file is in the public domain

AC_PREREQ([2.69])
AC_INIT([taler-merchant], [0.2.0], [taler-bug@gnunet.org])
AC_CONFIG_SRCDIR([src/backend/taler-merchant-httpd.c])
AC_CONFIG_HEADERS([taler_merchant_config.h])
# support for non-recursive builds
AM_INIT_AUTOMAKE([subdir-objects 1.9 tar-pax])

# pretty build rules
AM_SILENT_RULES([yes])

AC_CONFIG_MACRO_DIR([m4])

LT_INIT

# Checks for programs.
AC_PROG_CC
AC_PROG_CC_C99

CFLAGS="-Wall $CFLAGS"

# Checks for header files.
AC_CHECK_HEADERS([stdint.h stdlib.h string.h unistd.h])

# Check for compiling documentation
with_doc = yes
AC_MSG_CHECKING([for compiling documentation])
AC_ARG_WITH(doc,
            [AS_HELP_STRING([--with-doc], [Compile documentation])],
            [AC_MSG_RESULT([given])],
            [AC_MSG_RESULT(not given)
	    with_doc = no])

AS_CASE([$with_doc],
        [yes], AC_CHECK_PROG([texicomp], [texi2pdf], [yes]))
AS_IF([test "x$with_doc" = "xyes" -a "x$texicomp" != "xyes"], AC_MSG_ERROR([A texinfo compiler is needed. Install texi2pdf.]))
AM_CONDITIONAL([WITH_DOC], [test "x$with_doc" = "xyes"])

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


# test for postgres
AX_LIB_POSTGRESQL([9.3])
if test "$found_postgresql" = "yes"; then
  postgres=true
fi
AM_CONDITIONAL(HAVE_POSTGRESQL, test x$postgres = xtrue)

# Check for Taler's libtalerpq
libtalerpq=0
AC_MSG_CHECKING([for libtalerpq])
AC_ARG_WITH(exchange,
            [AS_HELP_STRING([--with-exchange=PFX], [base of Taler EXCHANGE installation])],
            [AC_MSG_RESULT([given as $with_exchange])],
            [AC_MSG_RESULT(not given)
             with_exchange=yes])
AS_CASE([$with_exchange],
        [yes], [],
        [no], [AC_MSG_ERROR([--with-exchange is required])],
        [LDFLAGS="-L$with_exchange/lib $LDFLAGS"
         CPPFLAGS="-I$with_exchange/include $CPPFLAGS $POSTGRESQL_CPPFLAGS"])

CPPFLAGS="$CPPFLAGS $POSTGRESQL_CPPFLAGS"
LDFLAGS="$LDFLAGS -L/usr/local/lib"

AC_CHECK_HEADERS([gnunet/gnunet_pq_lib.h],
 [AC_CHECK_LIB([gnunetpq], [GNUNET_PQ_exec_prepared], libgnunetpq=1)],
   [], [#ifdef HAVE_GNUNET_PLATFORM_H
        #include <gnunet/platform.h>
       #endif])
AM_CONDITIONAL(HAVE_GNUNETPQ, test x$libgnunetpq = x1)



# Check for Taler's libtalerfakebank
libtalerfakebank=0
AC_MSG_CHECKING([for libtalerfakebank])
AC_ARG_WITH(exchange,
            [AS_HELP_STRING([--with-exchange=PFX], [base of Taler EXCHANGE installation])],
            [AC_MSG_RESULT([given as $with_exchange])],
            [AC_MSG_RESULT(not given)
             with_exchange=yes])
AS_CASE([$with_exchange],
        [yes], [],
        [no], [AC_MSG_ERROR([--with-exchange is required])],
        [LDFLAGS="-L$with_exchange/lib $LDFLAGS"
         CPPFLAGS="-I$with_exchange/include $CPPFLAGS $POSTGRESQL_CPPFLAGS"])

CPPFLAGS="$CPPFLAGS $POSTGRESQL_CPPFLAGS"
LDFLAGS="$LDFLAGS -L/usr/local/lib"

AC_CHECK_HEADERS([taler/taler_fakebank_lib.h],
 [AC_CHECK_LIB([talerfakebank], [TALER_FAKEBANK_start], libtalerfakebank=1)],
   [], [#ifdef HAVE_GNUNET_PLATFORM_H
        #include <gnunet/platform.h>
       #endif])
AM_CONDITIONAL(HAVE_TALERFAKEBANK, test x$libtalerfakebank = x1)



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
# libgnurl
LIBGNURL_CHECK_CONFIG(,7.34.0,gnurl=1,gnurl=0)
if test "$gnurl" = 1
then
	AM_CONDITIONAL(HAVE_LIBGNURL, true)
	AC_DEFINE([HAVE_LIBGNURL],[1],[Have libgnurl])
else
	AM_CONDITIONAL(HAVE_LIBGNURL, false)
fi

# libcurl-gnutls
LIBCURL_CHECK_CONFIG(,7.34.0,[curl=true],[curl=false])
if test "x$curl" = xtrue
then
 AC_CHECK_HEADERS([curl/curl.h],
   AC_CHECK_DECLS(CURLINFO_TLS_SESSION,[curl=true],[curl=false],[[#include <curl/curl.h>]]),
   [curl=false])
 # need libcurl-gnutls.so, everything else is not acceptable
 AC_CHECK_LIB([curl-gnutls],[curl_easy_getinfo],,[curl=false])
 # cURL must support CURLINFO_TLS_SESSION, version >= 7.34

fi
if test x$curl = xfalse
then
	AM_CONDITIONAL(HAVE_LIBCURL, false)
if test "$gnurl" = 0
then
	AC_MSG_WARN([GNUnet requires libcurl-gnutls  >= 7.34])
fi
else
	AM_CONDITIONAL(HAVE_LIBCURL, true)
	AC_DEFINE([HAVE_LIBCURL],[1],[Have CURL])
fi


# Check for curl/curl.h and gnurl/curl.h so we can use #ifdef
# HAVE_CURL_CURL_H later (the above LIBCURL_CHECK_CONFIG accepted
# *either* header set).
AC_CHECK_HEADERS([curl/curl.h],,
  curl=false
  AC_CHECK_HEADERS([gnurl/curl.h],,
  gnurl=false))


# libgnurl
if test "x$gnurl" = "x0"
then
  if test "x$curl" = "x0"
  then
    AC_MSG_NOTICE([NOTICE: libgnurl not found.  http client support will not be compiled.])
    AC_MSG_WARN([ERROR: libgnurl not found.  hostlist daemon will not be compiled, and you probably WANT the hostlist daemon])
  else
    AC_MSG_NOTICE([WARNING: libgnurl not found, trying to use libcurl-gnutls instead.])
  fi
fi

# gcov compilation
AC_MSG_CHECKING(whether to compile with support for code coverage analysis)
AC_ARG_ENABLE([coverage],
              AS_HELP_STRING([--enable-coverage],
                             [compile the library with code coverage support]),
              [use_gcov=${enableval}],
              [use_gcov=no])
AC_MSG_RESULT($use_gcov)
AM_CONDITIONAL([USE_COVERAGE], [test "x$use_gcov" = "xyes"])

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


#
# Check for tsc
#
AC_CHECK_PROG([tsc],[tsc],[yes],[no])
AM_CONDITIONAL([HAVE_TSC], [test "x$tsc" = xyes])


AC_CONFIG_FILES([Makefile
doc/Makefile
src/Makefile
src/merchant-tools/Makefile
src/include/Makefile
src/backenddb/Makefile
src/backend/Makefile
src/samples/Makefile
src/lib/Makefile
src/mitm/taler-merchant-mitm
src/mitm/merchant-mitm.wsgi
src/mitm/Makefile])
AC_OUTPUT
