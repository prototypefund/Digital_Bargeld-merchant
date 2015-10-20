# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

lib_LTLIBRARIES = \
  libtalermerchant.la 

include_HEADERS = \
  taler_merchant_lib.h \
  taler_merchant_contract_lib.h

libtalermerchant_la_SOURCES = \
  taler-merchant-httpd_contract.c \
  taler-merchant-httpd_deposit.c \
  taler_merchant_contract_lib.h \
  merchant_db.c merchant_db.h \
  merchant.h

libtalermerchant_la_LIBADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -lgnunetutil \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread
