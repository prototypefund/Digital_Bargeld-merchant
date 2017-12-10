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
  merchant_api_common.c merchant_api_common.h \
  merchant_api_proposal.c \
  merchant_api_pay.c \
  merchant_api_tip_authorize.c \
  merchant_api_tip_enable.c \
  merchant_api_tip_pickup.c \
  merchant_api_track_transaction.c \
  merchant_api_track_transfer.c \
  merchant_api_history.c \
  merchant_api_refund.c

libtalermerchant_la_LIBADD = \
  -ltalerexchange \
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

if HAVE_TALERFAKEBANK
check_PROGRAMS = \
  test_merchant_api
endif

TESTS = \
  $(check_PROGRAMS)

test_merchant_api_SOURCES = \
  test_merchant_api.c
test_merchant_api_LDADD = \
  $(top_srcdir)/src/backenddb/libtalermerchantdb.la \
  libtalermerchant.la \
  $(LIBGCRYPT_LIBS) \
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
  test_merchant_api_home/.local/share/taler/exchange/offline-keys/master.priv \
  test_merchant_api_home/.config/taler/merchant/wire/test.json \
  test_merchant_api_home/.config/taler/test.json \
  test_merchant.priv
