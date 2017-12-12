# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  taler-merchant-dbinit \
  taler-merchant-generate-payments \
  taler-merchant-tip-enable

taler_merchant_dbinit_SOURCES = \
  taler-merchant-dbinit.c

taler_merchant_dbinit_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -lgnunetutil \
  -ltalerutil \
  -ltalerpq

taler_merchant_tip_enable_SOURCES = \
  taler-merchant-tip-enable.c

taler_merchant_tip_enable_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/lib/libtalermerchant.la \
  -lgnunetcurl \
  -lgnunetutil \
  -ltalerutil

taler_merchant_generate_payments_SOURCES = \
  taler-merchant-generate-payments.c

taler_merchant_generate_payments_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(LIBGCRYPT_LIBS) \
  -ltalerbank \
  -ltalerfakebank \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson
