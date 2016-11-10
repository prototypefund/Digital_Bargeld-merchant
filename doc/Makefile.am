all: manual.pdf

manual.pdf: arch.pdf examples.zip
	texi2pdf manual.texi
examples.zip:
	zip examples.zip examples/*
arch.pdf: arch.dot
	dot -Tpdf arch.dot > arch.pdf

info_TEXINFOS = manual.texi
manual_TEXINFOS = version.texi
