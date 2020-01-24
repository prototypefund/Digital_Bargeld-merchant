# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

plugindir = $(libdir)/taler

sqldir = $(prefix)/share/taler/sql/merchant/

sql_DATA = \
  0000.sql \
  0001.sql \
  drop0000.sql

if HAVE_POSTGRESQL
if HAVE_GNUNETPQ
plugin_LTLIBRARIES = \
  libtaler_plugin_merchantdb_postgres.la
endif
endif

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

lib_LTLIBRARIES = \
  libtalermerchantdb.la

libtalermerchantdb_la_SOURCES = \
  merchantdb_plugin.c

libtalermerchantdb_la_LIBADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -ltalerpq \
  -lgnunetutil

libtalermerchantdb_la_LDFLAGS = \
  $(POSTGRESQL_LDFLAGS) \
  -version-info 2:0:0 \
  -no-undefined

libtaler_plugin_merchantdb_postgres_la_SOURCES = \
  plugin_merchantdb_postgres.c
libtaler_plugin_merchantdb_postgres_la_LIBADD = \
  $(LTLIBINTL)
libtaler_plugin_merchantdb_postgres_la_LDFLAGS = \
  $(TALER_PLUGIN_LDFLAGS) \
  -lgnunetpq \
  -ltalerpq \
  -ltalerutil \
  -ltalerjson \
  -lpq \
  -lgnunetutil $(XLIB)

if HAVE_POSTGRESQL
if HAVE_GNUNETPQ
check_PROGRAMS = \
  test-merchantdb-postgres
endif
endif

test_merchantdb_postgres_SOURCES = \
  test_merchantdb.c

test_merchantdb_postgres_LDFLAGS = \
  -lgnunetutil \
  -ltalerutil \
  -ltalerjson \
  -ljansson

test_merchantdb_postgres_LDADD = \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la

TESTS = \
  test-merchantdb-postgres

EXTRA_DIST = \
  test-merchantdb-postgres.conf \
  $(sql_DATA)
