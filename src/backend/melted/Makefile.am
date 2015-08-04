# This Makefile.am is in the public domain
AM_CPPFLAGS = -I$(top_srcdir)/src/include

if USE_COVERAGE
  AM_CFLAGS = --coverage -O0
  XLIB = -lgcov
endif

bin_PROGRAMS = \
  taler-mint-httpd \
  taler-merchant-httpd

taler_mint_httpd_SOURCES = \
  taler-mint-httpd.c taler-mint-httpd.h \
  taler-mint-httpd_keystate.c taler-mint-httpd_keystate.h \
  taler-mint-httpd_db.c taler-mint-httpd_db.h \
  taler-mint-httpd_parsing.c taler-mint-httpd_parsing.h \
  taler-mint-httpd_responses.c taler-mint-httpd_responses.h \
  taler-mint-httpd_mhd.c taler-mint-httpd_mhd.h \
  taler-mint-httpd_admin.c taler-mint-httpd_admin.h \
  taler-mint-httpd_deposit.c taler-mint-httpd_deposit.h \
  taler-mint-httpd_withdraw.c taler-mint-httpd_withdraw.h \
  taler-mint-httpd_refresh.c taler-mint-httpd_refresh.h
taler_mint_httpd_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/util/libtalerutil.la \
  $(top_builddir)/src/mintdb/libtalermintdb.la \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil \
  -lpthread
taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c \
  merchant.c merchant.h \
  merchant_db.c merchant_db.h \
  taler-mint-httpd_keystate.c taler-mint-httpd_keystate.h \
  taler-mint-httpd_db.c taler-mint-httpd_db.h \
  taler-mint-httpd_parsing.c taler-mint-httpd_parsing.h \
  taler-mint-httpd_responses.c taler-mint-httpd_responses.h \
  taler-mint-httpd_mhd.c taler-mint-httpd_mhd.h \
  taler-mint-httpd_admin.c taler-mint-httpd_admin.h \
  taler-mint-httpd_deposit.c taler-mint-httpd_deposit.h \
  taler-mint-httpd_withdraw.c taler-mint-httpd_withdraw.h \
  taler-mint-httpd_refresh.c taler-mint-httpd_refresh.h

taler_merchant_httpd_LDADD = \
  $(LIBGCRYPT_LIBS) \
  $(top_builddir)/src/util/libtalerutil.la \
  $(top_builddir)/src/mintdb/libtalermintdb.la \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil \
  -ltalermint \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread

if HAVE_DEVELOPER
taler_mint_httpd_SOURCES += \
  taler-mint-httpd_test.c taler-mint-httpd_test.h
endif
