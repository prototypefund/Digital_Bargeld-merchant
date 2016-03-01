# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend-lib/

bin_PROGRAMS = \
  test-contract

#merchant_contract_test_CFLAGS = \
#  -Wl,--verbose

test_contract_SOURCES = \
  test_contract.c \
  ../backend/merchant.c \
  ../backend-lib/merchant_db.c ../backend-lib/merchant_db.h

test_contract_LDADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -ljansson \
  -lgnunetutil \
  -ltalerexchange \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  $(top_srcdir)/src/backend-lib/libtalermerchant.la \
  -lpthread
