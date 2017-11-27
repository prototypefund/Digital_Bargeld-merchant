# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend-lib/

pkgcfgdir = $(prefix)/share/taler/config.d/

pkgcfg_DATA = \
  merchant.conf

EXTRA_DIST = \
  $(pkgcfg_DATA)

bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c taler-merchant-httpd.h \
  taler-merchant-httpd_parsing.c taler-merchant-httpd_parsing.h \
  taler-merchant-httpd_responses.c taler-merchant-httpd_responses.h \
  taler-merchant-httpd_mhd.c taler-merchant-httpd_mhd.h \
  taler-merchant-httpd_auditors.c taler-merchant-httpd_auditors.h \
  taler-merchant-httpd_exchanges.c taler-merchant-httpd_exchanges.h \
  taler-merchant-httpd_proposal.c taler-merchant-httpd_proposal.h \
  taler-merchant-httpd_pay.c taler-merchant-httpd_pay.h \
  taler-merchant-httpd_history.c taler-merchant-httpd_history.h \
  taler-merchant-httpd_tip-authorize.c taler-merchant-httpd_tip-authorize.h \
  taler-merchant-httpd_tip-enable.c taler-merchant-httpd_tip-enable.h \
  taler-merchant-httpd_tip-pickup.c taler-merchant-httpd_tip-pickup.h \
  taler-merchant-httpd_tip-query.c taler-merchant-httpd_tip-query.h \
  taler-merchant-httpd_track-transaction.c taler-merchant-httpd_track-transaction.h \
  taler-merchant-httpd_track-transfer.c taler-merchant-httpd_track-transfer.h \
  taler-merchant-httpd_refund.c taler-merchant-httpd_refund.h


taler_merchant_httpd_LDADD = \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -ltalerpq \
  -lmicrohttpd \
  -ljansson \
  -lgnunetcurl \
  -lgnunetjson \
  -lgnunetutil
