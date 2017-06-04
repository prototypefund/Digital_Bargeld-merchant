# This Makefile is in the public domain

if DOC_ONLY
  SUBDIRS = . doc
else
  SUBDIRS = . src doc
endif

@DX_RULES@

ACLOCAL_AMFLAGS = -I m4
EXTRA_DIST = \
 AUTHORS \
 COPYING.GPL \
 COPYING.AGPL \
 COPYING.LGPL \
 contrib/gnunet.tag \
 contrib/microhttpd.tag \
 Doxyfile

app:
	mkdir -p $(PACKAGE)-frontend-$(VERSION)-app
	tar cf - AUTHORS README copylib/Makefile.am copylib/*.php examples/blog/*.php examples/blog/*.html examples/blog/Makefile.am examples/blog/web-common/taler-presence.* | (cd $(PACKAGE)-frontend-$(VERSION)-app ; tar xf -)
	tar cf $(PACKAGE)-frontend-$(VERSION)-app.tgz $(PACKAGE)-frontend-$(VERSION)-app
	rm -rf $(PACKAGE)-frontend-$(VERSION)-app
	mkdir -p $(PACKAGE)-backend-$(VERSION)-app
	tar cf - configure.ac AUTHORS README Makefile.am `find src/ -name "*.c" -o -name "*.h" -o -name Makefile.am` | (cd $(PACKAGE)-backend-$(VERSION)-app ; tar xf -)
	tar cf $(PACKAGE)-backend-$(VERSION)-app.tgz $(PACKAGE)-backend-$(VERSION)-app
	rm -rf $(PACKAGE)-backend-$(VERSION)-app
