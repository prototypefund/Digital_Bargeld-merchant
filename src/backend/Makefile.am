AM_CPPFLAGS = -I$(top_srcdir)/src/include $(POSTGRESQL_CPPFLAGS)

MERCHANT_DB = merchant_db.c merchant_db.h
bin_PROGRAMS = \
  taler-merchant-httpd \
  taler-merchant-dbinit

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c 

taler_merchant_dbinit_SOURCES = \
  taler-merchant-dbinit.c \
  $(MERCHANT_DB)

taler_merchant_httpd_LDADD = \
  -lmicrohttpd \
  -lgnunetutil


taler_merchant_dbinit_LDADD = \
  -ltalerpq \
  -lgnunetutil \
  -lgnunetpostgres \
  -lpq
