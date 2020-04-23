# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include -I$(top_srcdir)/src/backend

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

lib_LTLIBRARIES = \
  libtalermerchanttesting.la

libtalermerchanttesting_la_LDFLAGS = \
  -version-info 2:0:0 \
  -no-undefined

libtalermerchanttesting_la_SOURCES = \
  testing_api_cmd_config.c \
  testing_api_cmd_get_instance.c \
  testing_api_cmd_get_instances.c \
  testing_api_cmd_delete_instance.c \
  testing_api_cmd_post_instances.c \
  testing_api_cmd_post_products.c \
  testing_api_cmd_patch_instance.c \
   \
  testing_api_cmd_check_payment.c \
  testing_api_cmd_history.c \
  testing_api_cmd_pay.c \
  testing_api_cmd_pay_abort.c \
  testing_api_cmd_pay_abort_refund.c \
  testing_api_cmd_poll_payment.c \
  testing_api_cmd_proposal.c \
  testing_api_cmd_proposal_lookup.c \
  testing_api_cmd_refund_increase.c \
  testing_api_cmd_refund_lookup.c \
  testing_api_cmd_rewind.c \
  testing_api_cmd_tip_authorize.c \
  testing_api_cmd_tip_pickup.c \
  testing_api_cmd_tip_query.c \
  testing_api_cmd_track_transaction.c \
  testing_api_cmd_track_transfer.c \
  testing_api_helpers.c \
  testing_api_trait_merchant_sig.c \
  testing_api_trait_string.c \
  testing_api_trait_hash.c \
  testing_api_trait_planchet.c \
  testing_api_trait_refund_entry.c

libtalermerchanttesting_la_LIBADD = \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetcurl \
  -lgnunetjson \
  -lgnunetutil \
  -ljansson \
  -ltalertesting \
  $(XLIB)


if HAVE_TALERFAKEBANK
check_PROGRAMS = \
  test_merchant_api

if HAVE_TWISTER
check_PROGRAMS += test_merchant_api_twisted
endif
endif

TESTS = \
  $(check_PROGRAMS)

test_merchant_api_twisted_SOURCES = \
  test_merchant_api_twisted.c
test_merchant_api_twisted_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(LIBGCRYPT_LIBS) \
  -ltalertesting \
  -ltalermerchanttesting \
  -ltalertwistertesting \
  -ltalerfakebank \
  -ltalerbank \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson \
  -ltalertwister

test_merchant_api_SOURCES = \
  test_merchant_api.c
test_merchant_api_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  $(top_srcdir)/src/lib/libtalermerchant.la \
  $(LIBGCRYPT_LIBS) \
  -ltalertesting \
  -ltalermerchanttesting \
  -ltalerfakebank \
  -ltalerbank \
  -ltalerexchange \
  -ltalerjson \
  -ltalerutil \
  -lgnunetjson \
  -lgnunetcurl \
  -lgnunetutil \
  -ljansson

EXTRA_DIST = \
  test_merchant_api.conf \
  test_merchant_api_twisted.conf \
  test_merchant_api_proxy_merchant.conf \
  test_merchant_api_proxy_exchange.conf \
  test_merchant_api_home/.local/share/taler/exchange/offline-keys/master.priv \
  test_merchant_api_home/.config/taler/exchange/account-2.json \
  test_merchant.priv
