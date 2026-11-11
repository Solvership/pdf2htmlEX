#!/bin/bash
# Builds the table test corpus PDFs (run inside the dev container:
#   scripts/dev.sh exec pdf2htmlEX/test/tables/src/build.sh)
set -e
cd "$(dirname "$0")"
OUT=..
for f in *.tex; do
    pdflatex -interaction=nonstopmode -halt-on-error "$f" >/dev/null
    pdflatex -interaction=nonstopmode -halt-on-error "$f" >/dev/null
    mv "${f%.tex}.pdf" "$OUT/"
done
rm -f *.aux *.log
TAGGED='pdf:writer_pdf_Export:{"UseTaggedPDF":{"type":"boolean","value":true}}'
for f in tagged_*.html; do
    soffice -env:UserInstallation=file:///tmp/lo-profile --headless --infilter="HTML (StarWriter)" --convert-to "$TAGGED" --outdir "$OUT" "$f" >/dev/null
done
ls -l "$OUT"/*.pdf
