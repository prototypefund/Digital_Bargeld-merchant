# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

lib_LTLIBRARIES = \
  libtalermerchantdb.la

libtalermerchantdb_la_SOURCES = \
  merchant_db.c

libtalermerchantdb_la_LIBADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -lgnunetutil \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread
