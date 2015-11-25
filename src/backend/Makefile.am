# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend-lib/

bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c taler-merchant-httpd.h \
  taler-merchant-httpd_parsing.c taler-merchant-httpd_parsing.h \
  taler-merchant-httpd_responses.c taler-merchant-httpd_responses.h \
  taler-merchant-httpd_mhd.c taler-merchant-httpd_mhd.h \
  taler-merchant-httpd_auditors.c taler-merchant-httpd_auditors.h \
  taler-merchant-httpd_mints.c taler-merchant-httpd_mints.h \
  taler-merchant-httpd_contract.c taler-merchant-httpd_contract.h \
  taler-merchant-httpd_pay.c taler-merchant-httpd_pay.h

taler_merchant_httpd_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  -ltalermint \
  -ltalerutil \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil
