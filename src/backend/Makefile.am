# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend-lib/

bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c \
  merchant.c merchant.h \
  merchant_db.c merchant_db.h \
  taler-mint-httpd_parsing.c taler-mint-httpd_parsing.h \
  taler-mint-httpd_responses.c taler-mint-httpd_responses.h \
  ../backend-lib/taler-merchant-httpd_contract.h

taler_merchant_httpd_LDADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil \
  $(top_srcdir)/src/backend-lib/libtalermerchant.la \
  -ltalermint \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread
