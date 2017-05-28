all: manual.pdf manual.html

manual.pdf: arch.pdf manual.texi
	texi2pdf manual.texi
manual.html: arch.jpg manual.texi
	makeinfo --html --no-split manual.texi
arch.pdf: arch.dot
	dot -Tpdf arch.dot > arch.pdf
arch.jpg: arch.dot
	dot -Tjpg arch.dot > arch.jpg

info_TEXINFOS = manual.texi
manual_TEXINFOS = version.texi
