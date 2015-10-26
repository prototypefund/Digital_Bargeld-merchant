# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend-lib/

bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c \
  merchant.c merchant.h \
  ../backend-lib/merchant_db.c ../backend-lib/merchant_db.h \
  taler-mint-httpd_parsing.c taler-mint-httpd_parsing.h \
  taler-mint-httpd_responses.c taler-mint-httpd_responses.h \
  taler-mint-httpd.h \
  taler-mint-httpd_mhd.c taler-mint-httpd_mhd.h \
  taler-merchant-httpd_contract.c \
  taler-merchant-httpd_contract.h

taler_merchant_httpd_LDADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -lmicrohttpd \
  -ljansson \
  -lcurl \
  -lgnunetutil \
  $(top_srcdir)/src/backend-lib/libtalermerchant.la \
  -ltalermint \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread
