# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

plugindir = $(libdir)/taler

if HAVE_POSTGRESQL
if HAVE_TALERPQ
plugin_LTLIBRARIES = \
  libtaler_plugin_merchantdb_postgres.la
endif
endif

lib_LTLIBRARIES = \
  libtalermerchantdb.la

libtalermerchantdb_la_SOURCES = \
  merchantdb_plugin.c

libtalermerchantdb_la_LIBADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -lgnunetutil

libtalermerchantdb_la_LDFLAGS = \
  $(POSTGRESQL_LDFLAGS) \
  -version-info 0:0:0 \
  -no-undefined

libtaler_plugin_merchantdb_postgres_la_SOURCES = \
  plugin_merchantdb_postgres.c
libtaler_plugin_merchantdb_postgres_la_LIBADD = \
  $(LTLIBINTL)
libtaler_plugin_merchantdb_postgres_la_LDFLAGS = \
  $(TALER_PLUGIN_LDFLAGS) \
  -ltalerpq \
  -ltalerutil \
  -lpq \
  -lgnunetutil $(XLIB)
