AM_CPPFLAGS = -I$(top_srcdir)/src/include $(POSTGRESQL_CPPFLAGS)

MERCHANT_DB = merchant_db.c merchant_db.h
bin_PROGRAMS = \
  taler-merchant-httpd

taler_merchant_httpd_SOURCES = \
  taler-merchant-httpd.c 

taler_merchant_httpd_LDADD = \
  -lmicrohttpd \
  -lgnunetutil
