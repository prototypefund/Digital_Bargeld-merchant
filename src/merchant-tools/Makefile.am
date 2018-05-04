# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  taler-merchant-dbinit \
  taler-merchant-generate-payments-new

taler_merchant_dbinit_SOURCES = \
  taler-merchant-dbinit.c

taler_merchant_dbinit_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -lgnunetutil \
  -ltalerutil \
  -ltalerpq

taler_merchant_generate_payments_new_SOURCES = \
  taler-merchant-generate-payments_new.c

taler_merchant_generate_payments_new_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(LIBGCRYPT_LIBS) \
  -ltalertesting \
  -ltalermerchanttesting \
  -ltalerfakebank \
  -ltalerbank \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson
