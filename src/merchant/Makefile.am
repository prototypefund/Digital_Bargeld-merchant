AM_CPPFLAGS = -I$(top_srcdir)/src/include $(POSTGRESQL_CPPFLAGS)

MERCHANT_DB = merchant_db.c merchant_db.h
bin_PROGRAMS = \
  taler-merchant-dbinit \
  taler-merchant-serve

taler_merchant_dbinit_SOURCES = \
  taler_merchant_dbinit.c \
  $(MERCHANT_DB)
taler_merchant_dbinit_LDADD = \
  -lpq \
  -lgnunetutil \
  -ltalerutil \
  -ltalerpq \
  -lgnunetpostgres
taler_merchant_dbinit_LDFLAGS = \
  $(POSTGRESQL_LDFLAGS)

check_PROGRAMS = \
  test-merchant \
  test-merchant-db

test_merchant_SOURCES = \
  merchant.c \
  test_merchant.c
test_merchant_LDADD = \
  -ltalerutil \
  -lgnunetutil

test_merchant_db_SOURCES = \
  $(MERCHANT_DB) \
  test_merchant_db.c
test_merchant_db_LDADD = \
  -ltalerutil \
  -lgnunetutil \
  -lgnunetpostgres \
  -lpq

taler_merchant_serve_SOURCES = \
  taler_merchant_serve.c \
  merchant.c merchant.h \
  $(MERCHANT_DB)
taler_merchant_serve_LDADD = \
  -lpq \
  -lgnunetutil \
  -lgnunetpostgres \
  -lmicrohttpd \
  -ltalermint \
  -ltalerutil \
  -ltalerpq \
  -ljansson
taler_merchant_serve_LDFLAGS = \
  $(POSTGRESQL_LDFLAGS)
