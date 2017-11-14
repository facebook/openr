SOURCES=$(wildcard *.md)
PDF=$(SOURCES:%.md=%.pdf)
HTML=$(SOURCES:%.md=%.html)
INSTALL=install -c -m 644
PYTHON=python
PANDOCARGS=-s
PANDOC=/usr/bin/pandoc

export LANGUAGE=C
export LC_ALL=C

all: html

pdf: $(PDF)

html: $(HTML)

# This needs pandoc 1.9 or later to work
%.pdf: %.md
	$(PANDOC) -f markdown_github -o $*.pdf $*.md

%.html: %.md
	$(PANDOC) $(PANDOCARGS) -f markdown_github -t html --toc -o $*.html $*.md

clean:
	$(RM) $(PDF) $(HTML)
