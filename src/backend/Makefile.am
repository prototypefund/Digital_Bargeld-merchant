# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c \
  merchant.c merchant.h \
  merchant_db.c merchant_db.h \
  taler-mint-httpd_parsing.c taler-mint-httpd_parsing.h \
  taler-mint-httpd_responses.c taler-mint-httpd_responses.h 

taler_merchant_httpd_LDADD = \
  $(LIBGCRYPT_LIBS) \
  -ltalerutil \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil \
  -ltalermint \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread
