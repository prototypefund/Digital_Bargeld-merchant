# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  merchant-create-payments

merchant_create_payments_SOURCES = \
  create_payments.c

merchant_create_payments_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  -ltalermerchant \
  $(LIBGCRYPT_LIBS) \
  -ltalerfakebank \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson
