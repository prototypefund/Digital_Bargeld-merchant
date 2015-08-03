#!/bin/bash
# copy all the backend relevant files into the directory given
# as argument. The intended use is to move back and forth those files
# (since they are git'ed on the merchant's repository) as long as not
# configure flag like --enable-merchant will be available from the mint

# STILL NOT TESTED

cp -t $2 \
$1/Makefile.am \
$1/merchant.c \
$1/merchant_db.c \
$1/merchant_db.h \
$1/merchant.h \
$1/taler-merchant-httpd.c
