# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

lib_LTLIBRARIES = \
  libtalermerchant.la

libtalermerchant_la_LDFLAGS = \
  -version-info 2:0:0 \
  -no-undefined

libtalermerchant_la_SOURCES = \
  merchant_api_common.c \
  merchant_api_delete_instance.c \
  merchant_api_delete_product.c \
  merchant_api_get_config.c \
  merchant_api_get_instance.c \
  merchant_api_get_instances.c \
  merchant_api_get_product.c \
  merchant_api_get_products.c \
  merchant_api_lock_product.c \
  merchant_api_patch_instance.c \
  merchant_api_patch_product.c \
  merchant_api_post_instances.c \
  merchant_api_post_products.c \
  merchant_api_check_payment.c \
  merchant_api_history.c \
  merchant_api_proposal.c \
  merchant_api_proposal_lookup.c \
  merchant_api_pay.c \
  merchant_api_poll_payment.c \
  merchant_api_refund.c \
  merchant_api_refund_increase.c \
  merchant_api_tip_authorize.c \
  merchant_api_tip_pickup.c \
  merchant_api_tip_pickup2.c \
  merchant_api_tip_query.c \
  merchant_api_track_transaction.c \
  merchant_api_track_transfer.c
libtalermerchant_la_LIBADD = \
  -ltalerexchange \
  -ltalercurl \
  -ltalerjson \
  -ltalerutil \
  -lgnunetcurl \
  -lgnunetjson \
  -lgnunetutil \
  -ljansson \
  $(XLIB)

if HAVE_LIBCURL
libtalermerchant_la_LIBADD += -lcurl
else
if HAVE_LIBGNURL
libtalermerchant_la_LIBADD += -lgnurl
endif
endif
