all: manual.pdf manual.html

manual.pdf: arch.pdf manual.texi
	texi2pdf manual.texi
manual.html: arch.jpg manual.texi
	makeinfo --html --no-split manual.texi
arch.pdf: arch.dot
	dot -Tpdf arch.dot > arch.pdf
arch.jpg: arch.dot
	dot -Tjpg arch.dot > arch.jpg

AM_MAKEINFOHTMLFLAGS = --no-split --css-ref=docstyle.css --css-ref=brown-paper.css

info_TEXINFOS = manual.texi
manual_TEXINFOS = version.texi

EXTRA_DIST = \
  arch.dot \
  lgpl.texi \
  agpl.texi \
  fdl-1.3.texi \
  docstyle.css \
  brown-paper.css
