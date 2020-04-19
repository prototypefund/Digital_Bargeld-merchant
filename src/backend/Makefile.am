# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend-lib/

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

pkgcfgdir = $(prefix)/share/taler/config.d/

pkgcfg_DATA = \
  merchant.conf

EXTRA_DIST = \
  $(pkgcfg_DATA)

bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c taler-merchant-httpd.h \
  taler-merchant-httpd_auditors.c taler-merchant-httpd_auditors.h \
  taler-merchant-httpd_config.c taler-merchant-httpd_config.h \
  taler-merchant-httpd_exchanges.c taler-merchant-httpd_exchanges.h \
  taler-merchant-httpd_mhd.c taler-merchant-httpd_mhd.h \
  taler-merchant-httpd_private-delete-instances-ID.c \
    taler-merchant-httpd_private-delete-instances-ID.h \
  taler-merchant-httpd_private-get-instances.c \
    taler-merchant-httpd_private-get-instances.h \
  taler-merchant-httpd_private-get-instances-ID.c \
    taler-merchant-httpd_private-get-instances-ID.h \
  taler-merchant-httpd_private-get-products.c \
    taler-merchant-httpd_private-get-products.h \
  taler-merchant-httpd_private-get-products-ID.c \
    taler-merchant-httpd_private-get-products-ID.h \
  taler-merchant-httpd_private-patch-instances-ID.c \
    taler-merchant-httpd_private-patch-instances-ID.h \
  taler-merchant-httpd_private-patch-products-ID.c \
    taler-merchant-httpd_private-patch-products-ID.h \
  taler-merchant-httpd_private-post-instances.c \
    taler-merchant-httpd_private-post-instances.h \
  taler-merchant-httpd_private-post-products.c \
    taler-merchant-httpd_private-post-products.h

DEAD = \
  taler-merchant-httpd_check-payment.c taler-merchant-httpd_check-payment.h \
  taler-merchant-httpd_history.c taler-merchant-httpd_history.h \
  taler-merchant-httpd_order.c taler-merchant-httpd_order.h \
  taler-merchant-httpd_pay.c taler-merchant-httpd_pay.h \
  taler-merchant-httpd_poll-payment.c taler-merchant-httpd_poll-payment.h \
  taler-merchant-httpd_proposal.c taler-merchant-httpd_proposal.h \
  taler-merchant-httpd_refund.c taler-merchant-httpd_refund.h \
  taler-merchant-httpd_refund_increase.c taler-merchant-httpd_refund_increase.h \
  taler-merchant-httpd_refund_lookup.c taler-merchant-httpd_refund_lookup.h \
  taler-merchant-httpd_tip-authorize.c taler-merchant-httpd_tip-authorize.h \
  taler-merchant-httpd_tip-pickup.c taler-merchant-httpd_tip-pickup.h \
  taler-merchant-httpd_tip-pickup_get.c \
  taler-merchant-httpd_tip-query.c taler-merchant-httpd_tip-query.h \
  taler-merchant-httpd_tip-reserve-helper.c taler-merchant-httpd_tip-reserve-helper.h \
  taler-merchant-httpd_track-transaction.c taler-merchant-httpd_track-transaction.h \
  taler-merchant-httpd_track-transfer.c taler-merchant-httpd_track-transfer.h
taler_merchant_httpd_LDADD = \
  $(top_builddir)/src/backenddb/libtalermerchantdb.la \
  -ltalerexchange \
  -ltalermhd \
  -ltalerbank \
  -ltalerjson \
  -ltalerutil \
  -ltalerpq \
  -lmicrohttpd \
  -ljansson \
  -lgnunetcurl \
  -lgnunetjson \
  -lgnunetutil \
  $(XLIB)
