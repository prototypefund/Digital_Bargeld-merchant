# This Makefile is in the public domain

SUBDIRS = src copylib examples
ACLOCAL_AMFLAGS = -I m4
EXTRA_DIST = AUTHORS COPYING.GPL COPING.AGPL COPYING.LGPL

app:
	mkdir -p $(PACKAGE)-$(VERSION)-app
	tar cf - configure.ac AUTHORS README copylib/Makefile.am copylib/*.php examples/blog/*.php examples/blog/*.html examples/blog/Makefile.am examples/blog/web-common/taler-presence.* Makefile.am `find src/ -name "*.c" -o -name "*.h" -o -name Makefile.am` | (cd $(PACKAGE)-$(VERSION)-app ; tar xf -)
	tar cf $(PACKAGE)-$(VERSION)-app.tgz $(PACKAGE)-$(VERSION)-app
	rm -rf $(PACKAGE)-$(VERSION)-app
