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
  taler-merchant-httpd_exchanges.c taler-merchant-httpd_exchanges.h \
  taler-merchant-httpd_contract.c taler-merchant-httpd_contract.h \
  taler-merchant-httpd_pay.c taler-merchant-httpd_pay.h \
  taler-merchant-httpd_util.c taler-merchant-httpd_util.h

taler_merchant_httpd_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  -ltalerexchange \
  -ltalerutil \
  -ltalerpq \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil
