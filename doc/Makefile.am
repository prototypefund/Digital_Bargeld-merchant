all: manual.pdf

manual.pdf: arch.pdf
	texi2pdf manual.texi
arch.pdf: arch.dot
	dot -Tpdf arch.dot > arch.pdf

info_TEXINFOS = manual.texi
manual_TEXINFOS = version.texi
