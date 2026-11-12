#!/bin/bash
#
# Convert any PDF with the semantic layer and check its tables:
#
#   scripts/try_pdf.sh "/path/to/Some report.pdf" [NAME] [pdf2htmlEX options...]
#
# The PDF is copied to .realpdf/NAME/NAME.pdf (git-excluded; the container only
# sees the repository, and names with spaces are avoided), converted with
# --preset semantic into .realpdf/NAME/out/, and checked with
# scripts/check_tables.py. NAME defaults to the file name without spaces.
#
# A copy of the previous result is kept as .realpdf/NAME/previous.semantic.json
# and compared with the new one (useful after changing the detector).

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$1"
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
    sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
shift
NAME="${1:-$(basename "$SRC" .pdf | tr -c 'A-Za-z0-9._\n-' '_' | cut -c1-40)}"
[ $# -gt 0 ] && shift

DIR="$REPO/.realpdf/$NAME"
mkdir -p "$DIR/out"
grep -qxE '/?\.realpdf/?' "$REPO/.git/info/exclude" 2>/dev/null || echo '.realpdf/' >> "$REPO/.git/info/exclude"
cp "$SRC" "$DIR/$NAME.pdf"

JSON="$DIR/out/$NAME.semantic.json"
[ -f "$JSON" ] && cp "$JSON" "$DIR/previous.semantic.json"

echo "== converting $NAME.pdf"
"$REPO/scripts/dev.sh" run --quiet 1 --preset semantic "$@" --dest-dir "/work/.realpdf/$NAME/out" "/work/.realpdf/$NAME/$NAME.pdf"

echo "== checking the tables"
"$REPO/scripts/dev.sh" exec python3 scripts/check_tables.py check "/work/.realpdf/$NAME/out/$NAME.semantic.json" "/work/.realpdf/$NAME/$NAME.pdf"

if [ -f "$DIR/previous.semantic.json" ]; then
    echo "== compared with the previous run"
    "$REPO/scripts/dev.sh" exec python3 scripts/check_tables.py diff "/work/.realpdf/$NAME/previous.semantic.json" "/work/.realpdf/$NAME/out/$NAME.semantic.json"
fi

echo
echo "Open $DIR/out/$NAME.html in a browser and press T to see the tables,"
echo "or $DIR/out/$NAME.tables.html for the tables only."
