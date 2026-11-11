#!/bin/bash
# Converts every PDF given with the baseline (.baseline worktree, master) and the
# current build, using the same extra options, and compares the HTML byte by byte.
# usage (inside the container): scripts/compare_outputs.sh OUTDIR "extra opts" file.pdf...
# NEW_OPTS (environment) is passed to the current build only.
set -e
# FontForge writes timestamps into the fonts: make the output reproducible
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1700000000}
OUT=$1; shift
OPTS=$1; shift
NEW=/work/pdf2htmlEX/build
OLD=/work/.baseline/pdf2htmlEX/build
D=usr/local/share/pdf2htmlEX
mkdir -p $OUT/old $OUT/new
rc=0
for f in "$@"; do
    b=$(basename "$f" .pdf)
    $OLD/pdf2htmlEX --data-dir $OLD/install/$D --poppler-data-dir $OLD/install/$D/poppler --quiet 1 $OPTS --dest-dir $OUT/old "$f" || echo "old FAILED: $f"
    $NEW/pdf2htmlEX --data-dir $NEW/install/$D --poppler-data-dir $NEW/install/$D/poppler --quiet 1 $OPTS $NEW_OPTS --dest-dir $OUT/new "$f" || echo "new FAILED: $f"
    if cmp -s $OUT/old/$b.html $OUT/new/$b.html; then echo "IDENTICAL  $b"; else echo "DIFFERENT  $b"; rc=1; fi
done
exit $rc
