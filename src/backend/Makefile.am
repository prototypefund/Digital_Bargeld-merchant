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
  /home/marcello/Taler/trans_mint/src/util/libtalerutil.la \
  /home/marcello/Taler/trans_mint/src/mintdb/libtalermintdb.la \
  -lmicrohttpd \
  -ljansson \
  -lgnunetutil \
  -ltalermint \
  -ltalerpq \
  -lgnunetpostgres \
  -lpq \
  -lpthread

# NOTE: the lines
# /home/demo/mint/src/util/libtalerutil.la \
# /home/demo/mint/src/mintdb/libtalermintdb.la \
# don't seem to be replaceable with -ltalerutil and -ltalermintdb
# -ltalerutil \
# -ltalermintdb \
# so that this Makefile.am depends on where the mint source tree
# is in the user's computer.

