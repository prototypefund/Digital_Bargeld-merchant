all: manual.pdf manual.html

manual.pdf: arch.pdf manual.texi
manual.html: arch.png manual.texi

arch.pdf: arch.dot
	dot -Tpdf arch.dot > arch.pdf
arch.png: arch.dot
	dot -Tpng arch.dot > arch.png

AM_MAKEINFOHTMLFLAGS = --no-split --css-ref=docstyle.css --css-ref=brown-paper.css

man_MANS = \
  taler-merchant-generate-payments.1 \
  taler-merchant-httpd.1

info_TEXINFOS = manual.texi

manual_TEXINFOS = version.texi

extra_TEXINFOS = \
  fdl-1.3.texi \
  agpl.texi \
  syntax.texi

EXTRA_DIST = \
  arch.dot \
  $(extra_TEXINFOS) \
  docstyle.css \
  brown-paper.css \
  $(man_MANS)

DISTCLEANFILES = \
  manual.cps \
  manual.dvi
