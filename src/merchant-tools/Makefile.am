# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  taler-merchant-dbinit \
  taler-merchant-benchmark

taler_merchant_dbinit_SOURCES = \
  taler-merchant-dbinit.c

taler_merchant_dbinit_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -lgnunetutil \
  -ltalerutil \
  -ltalerpq

taler_merchant_benchmark_SOURCES = \
  taler-merchant-benchmark.c

taler_merchant_benchmark_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(top_srcdir)/src/lib/libtalermerchanttesting.la \
  $(LIBGCRYPT_LIBS) \
  -ltalertesting \
  -ltalerfakebank \
  -ltalerbank \
  -ltalerwire \
  -ltalerbanktesting \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson
