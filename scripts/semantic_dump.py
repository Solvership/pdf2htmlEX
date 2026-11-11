#!/usr/bin/env python3
"""Print the tables / paragraphs of a <name>.semantic.json file."""
import json, sys
for path in sys.argv[1:]:
    d = json.load(open(path))
    print("==", path)
    for p in d["pages"]:
        for t in p["tables"]:
            extra = (" continued_from=" + t["continued_from"]) if "continued_from" in t else ""
            extra += " repeated_header" if t.get("repeated_header") else ""
            print(f"  [{t['id']}] {t['source']} {t['rows']}x{t['cols']}{extra}")
            for c in t["cells"]:
                span = f" rs={c['rs']}" if "rs" in c else ""
                span += f" cs={c['cs']}" if "cs" in c else ""
                h = " H" if c.get("header") else ""
                print(f"     r{c['r']} c{c['c']}{span}{h}: {c['text']!r}")
        for q in p["paragraphs"]:
            print(f"  ({q['id']} {q['tag']}) {q['text'][:110]!r}")
