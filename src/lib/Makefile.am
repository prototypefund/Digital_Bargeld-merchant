# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

lib_LTLIBRARIES = \
  libtalermerchant.la

libtalermerchant_la_LDFLAGS = \
  -version-info 0:0:0 \
  -no-undefined

libtalermerchant_la_SOURCES = \
  merchant_api_contract.c \
  merchant_api_pay.c

libtalermerchant_la_LIBADD = \
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

check_PROGRAMS = \
  test_merchant_api

TESTS = \
  $(check_PROGRAMS)

test_merchant_api_SOURCES = \
  test_merchant_api.c
test_merchant_api_LDADD = \
  libtalermerchant.la \
  $(LIBGCRYPT_LIBS) \
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
