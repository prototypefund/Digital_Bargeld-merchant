# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  merchant-contract-test

#merchant_contract_test_CFLAGS = \
#  -Wl,--verbose

merchant_contract_test_SOURCES = \
  merchant-contract-test.c \
  merchant.c \
  merchant_db.c merchant_db.h

merchant_contract_test_LDADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -ltalermerchant \
  -ljansson \
  -lgnunetutil \
  -ltalermint \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -ltalermerchant \
  -lpthread
