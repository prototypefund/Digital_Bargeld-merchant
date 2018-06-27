# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  taler-merchant-dbinit \
  taler-merchant-generate-payments \
  taler-merchant-generate-payments-alt

taler_merchant_dbinit_SOURCES = \
  taler-merchant-dbinit.c

taler_merchant_dbinit_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -lgnunetutil \
  -ltalerutil \
  -ltalerpq

taler_merchant_generate_payments_alt_SOURCES = \
  taler-merchant-generate-payments-alt.c

taler_merchant_generate_payments_alt_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(top_srcdir)/src/lib/libtalermerchanttesting.la \
  $(LIBGCRYPT_LIBS) \
  -ltalertesting \
  -ltalerfakebank \
  -ltalerbank \
  -ltalerbanktesting \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson

taler_merchant_generate_payments_SOURCES = \
  taler-merchant-generate-payments.c

taler_merchant_generate_payments_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(top_srcdir)/src/lib/libtalermerchanttesting.la \
  $(LIBGCRYPT_LIBS) \
  -ltalertesting \
  -ltalerfakebank \
  -ltalerbank \
  -ltalerbanktesting \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson
