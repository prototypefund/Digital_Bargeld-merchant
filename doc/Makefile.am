all: manual.pdf manual.html

manual.pdf: arch.pdf manual.texi
manual.html: arch.png manual.texi

%.png: %.dot
	dot -Tpng $< > $@
%.pdf: %.dot
	dot -Tpdf $< > $@

clean-local:
	-rm -f arch.png arch.pdf arch-api.png arch-api.pdf

merchant-api-curl.pdf: merchant-api.content.texi arch-api.pdf
merchant-api-python.pdf: merchant-api.content.texi arch-api.pdf
merchant-api-curl.html: merchant-api.content.texi arch-api.png
merchant-api-python.html: merchant-api.content.texi arch-api.png

# NOTE: While GNU makeinfo 6.5 supports --css-ref=URL,
# makeinfo 4.8 (in NetBSD 8.0, macOS, and maybe other
# base) does only support --css-include=FILE.
# The only difference is a shorter html output and
# in 6.5 the ability to use refs instead of include.
# We prefer not to break builds in this case, so
# we use the include version which is backwards compatible
# and upwards compatible, while the ref variant is neither.

AM_MAKEINFOHTMLFLAGS = --no-split --css-include=docstyle.css --css-include=brown-paper.css

man_MANS = \
  taler-merchant-benchmark.1 \
  taler-merchant-httpd.1

info_TEXINFOS = \
  manual.texi \
  merchant-api-python.texi \
  merchant-api-curl.texi \
  merchant-api-php.texi

manual_TEXINFOS = \
  version-manual.texi \
  merchant-api.content.texi

merchant_api_python_TEXINFOS = \
  version-merchant-api-python.texi \
  merchant-api.content.texi

merchant_api_curl_TEXINFOS = \
  version-merchant-api-curl.texi \
  merchant-api.content.texi

merchant_api_php_TEXINFOS = \
  version-merchant-api-php.texi \
  merchant-api.content.texi

extra_TEXINFOS = \
  fdl-1.3.texi \
  agpl.texi \
  syntax.texi \
  merchant-api.content.texi

EXTRA_DIST = \
  arch.dot \
  $(extra_TEXINFOS) \
  docstyle.css \
  brown-paper.css \
  $(man_MANS)

DISTCLEANFILES = \
  manual.cps \
  manual.dvi
