# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  taler-merchant-dbinit

taler_merchant_dbinit_SOURCES = \
  taler-merchant-dbinit.c


taler_merchant_dbinit_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -lgnunetutil \
  -ltalerutil \
  -ltalerpq
