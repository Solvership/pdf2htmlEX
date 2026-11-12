#!/usr/bin/env python3
"""
Check the tables found by the semantic layer against the text of the PDF.

    check_tables.py check DOC.semantic.json DOC.pdf [--pages 3,5-7]
    check_tables.py show  DOC.semantic.json [--pages 3,5-7]
    check_tables.py diff  OLD.semantic.json NEW.semantic.json [--show]

check   compares every table with the words of the PDF (pdftotext -bbox):
          MISSING  numbers inside the table's area that are in no cell
          SPLIT    numbers of one printed line that ended up in different rows
          TEXT?    cells with long running text in a table found from rules
                   (often a false table: a page of text, a chart)
          OUTSIDE  lines with several numbers far apart that are in no table
                   (a missed table - or a table without any rules)
        Only table lines with a remark are printed, unless --all is given.
show    prints the tables row by row: [column, text], spans as cs=/rs=.
diff    lists the pages whose tables differ between two runs (regressions);
        --show prints the differences.

pdftotext (poppler-utils) must be installed; inside the dev container:
    scripts/dev.sh exec python3 scripts/check_tables.py check ...
"""
import argparse
import collections
import difflib
import html
import json
import re
import shutil
import subprocess
import sys

NUM = re.compile(r'^\(?[-+]?[\d.,]*\d[\d.,]*\)?%?$')


def is_number(t):
    # "12." alone is usually a numbered heading, not a value
    return bool(NUM.match(t)) and not re.match(r'^\d{1,2}\.$', t)


def parse_pages(spec):
    if not spec:
        return None
    pages = set()
    for part in spec.split(','):
        if '-' in part:
            a, b = part.split('-')
            pages.update(range(int(a), int(b) + 1))
        elif part:
            pages.add(int(part))
    return pages


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def pdf_words(pdf):
    """Words per page: (x0, y0, x1, y1, text), in points, origin top left."""
    if not shutil.which('pdftotext'):
        sys.exit('pdftotext not found: install poppler-utils, or run inside the dev container '
                 '(scripts/dev.sh exec python3 scripts/check_tables.py ...)')
    out = subprocess.run(['pdftotext', '-bbox', pdf, '-'], check=True, capture_output=True).stdout.decode('utf-8', 'replace')
    pages = []
    for pg in re.finditer(r'<page [^>]*>(.*?)</page>', out, re.S):
        words = []
        for m in re.finditer(r'<word xMin="([\d.]+)" yMin="([\d.]+)" xMax="([\d.]+)" yMax="([\d.]+)">(.*?)</word>', pg.group(1)):
            words.append(tuple(float(m.group(i)) for i in range(1, 5)) + (html.unescape(m.group(5)),))
        pages.append(words)
    return pages


def lines_of(words):
    """Group words into printed lines (by their vertical center)."""
    lines = []
    for w in sorted(words, key=lambda w: ((w[1] + w[3]) / 2, w[0])):
        cy = (w[1] + w[3]) / 2
        h = max(w[3] - w[1], 1)
        if lines and abs(lines[-1][0] - cy) <= 0.4 * h:
            lines[-1][1].append(w)
        else:
            lines.append([cy, [w]])
    return [sorted(l[1]) for l in lines]


def inside(w, box, tol=2):
    cy = (w[1] + w[3]) / 2
    return w[0] >= box[0] - tol and w[2] <= box[2] + tol and box[1] - 1 <= cy <= box[3] + 1


def table_rows(t):
    rows = collections.defaultdict(list)
    for c in t['cells']:
        rows[c['r']].append(c)
    return [sorted(rows[r], key=lambda c: c['c']) for r in sorted(rows)]


def check_table(t, words):
    remarks = []
    tok_rows = collections.defaultdict(set)
    cell_words = set()
    for c in t['cells']:
        for tok in c['text'].split():
            cell_words.add(tok)
            if is_number(tok):
                tok_rows[tok].add(c['r'])
    box = t['bbox']
    lines = lines_of([w for w in words if inside(w, box)])
    missing, split = [], []
    for l in lines:
        if len(l) >= 8:
            continue  # a sentence crossing the table's area, not part of it
        nums = [w[4] for w in l if is_number(w[4])]
        lost = [n for n in nums if n not in tok_rows]
        # a line partly in the table, or a whole row of numbers; not a numbered
        # heading between two tables ("Note 12 - ...") inside the table's area
        if lost and (any(w[4] in cell_words for w in l) or len(nums) >= 2):
            missing += lost
        rows = [tok_rows[n] for n in nums if n in tok_rows]
        if len(rows) >= 2 and not set.intersection(*rows):
            split.append(' '.join(nums))
    if missing:
        remarks.append('MISSING ' + ' '.join(missing[:8]) + (' ...' if len(missing) > 8 else ''))
    if split:
        remarks.append('SPLIT ' + ' | '.join(split[:3]))
    if t['source'] == 'hrules':
        long = [c['text'] for c in t['cells'] if len(c['text'].split()) >= 14]
        if len(long) >= 2:
            remarks.append('TEXT? ' + long[0][:60] + '...')
    return remarks


def outside_lines(page, words):
    boxes = [t['bbox'] for t in page['tables']]
    found = []
    # not the page header and footer (page numbers)
    h = page.get('height') or 842
    body = [w for w in words if 0.07 * h < (w[1] + w[3]) / 2 < 0.93 * h]
    for l in lines_of([w for w in body if not any(inside(w, b) for b in boxes)]):
        nums = [w for w in l if is_number(w[4])]
        gaps = [l[i + 1][0] - l[i][2] for i in range(len(l) - 1)]
        if len(nums) >= 2 and gaps and max(gaps) > 25 and len(l) <= 12:
            found.append(' '.join(w[4] for w in l))
    return found


def cmd_check(args):
    sem = load(args.json)
    pages = parse_pages(args.pages)
    words = pdf_words(args.pdf)
    n_tables = n_remarks = n_outside = 0
    sources = collections.Counter()
    for p in sem['pages']:
        if pages and p['page'] not in pages:
            continue
        ws = words[p['page'] - 1] if p['page'] - 1 < len(words) else []
        for t in p['tables']:
            n_tables += 1
            sources[t['source']] += 1
            remarks = check_table(t, ws)
            n_remarks += bool(remarks)
            if remarks or args.all:
                print(f"p{p['page']:<4} {t['id']:<8} {t['source']:<7} {t['rows']}x{t['cols']:<4} {'  '.join(remarks) or 'ok'}")
        out = outside_lines(p, ws)
        if out:
            n_outside += 1
            more = f' (+{len(out) - 3} more)' if len(out) > 3 else ''
            print(f"p{p['page']:<4} OUTSIDE  " + ' // '.join(o[:60] for o in out[:3]) + more)
    print(f"\n{n_tables} tables ({', '.join(f'{k} {v}' for k, v in sorted(sources.items()))}); "
          f"{n_remarks} with remarks; {n_outside} pages with numbers outside tables")


def show_lines(sem, pages):
    out = []
    for p in sem['pages']:
        if pages and p['page'] not in pages:
            continue
        for t in p['tables']:
            extra = f" continued_from={t['continued_from']}" if 'continued_from' in t else ''
            out.append(f"p{p['page']} {t['id']} {t['source']} {t['rows']}x{t['cols']}{extra}")
            for r, cells in enumerate(table_rows(t)):
                parts = []
                for c in cells:
                    span = ''.join(f" {k}={c[k]}" for k in ('cs', 'rs') if c.get(k, 1) > 1)
                    parts.append(f"[{c['c']}{span}] {c['text']}")
                out.append(f"   {cells[0]['r']:>3}  " + '  |  '.join(parts))
    return out


def cmd_show(args):
    print('\n'.join(show_lines(load(args.json), parse_pages(args.pages))))


def cmd_diff(args):
    a, b = load(args.old), load(args.new)
    pa = {p['page']: p for p in a['pages']}
    pb = {p['page']: p for p in b['pages']}
    na = sum(len(p['tables']) for p in a['pages'])
    nb = sum(len(p['tables']) for p in b['pages'])
    print(f'tables {na} -> {nb}')
    changed = 0
    for pg in sorted(set(pa) | set(pb)):
        la = show_lines(a, {pg}) if pg in pa else []
        lb = show_lines(b, {pg}) if pg in pb else []
        # the table boxes may move a little without any change of content
        if la == lb:
            continue
        changed += 1
        print(f"page {pg}: {len(pa.get(pg, {}).get('tables', []))} -> {len(pb.get(pg, {}).get('tables', []))} tables")
        if args.show:
            for d in difflib.unified_diff(la, lb, lineterm='', n=1):
                if not d.startswith(('---', '+++')):
                    print('   ' + d)
    if not changed:
        print('no differences')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check', help='compare the tables with the text of the PDF')
    c.add_argument('json')
    c.add_argument('pdf')
    c.add_argument('--pages', help='e.g. 3,5-7')
    c.add_argument('--all', action='store_true', help='also print the tables without remarks')
    c.set_defaults(func=cmd_check)
    s = sub.add_parser('show', help='print the tables row by row')
    s.add_argument('json')
    s.add_argument('--pages', help='e.g. 3,5-7')
    s.set_defaults(func=cmd_show)
    d = sub.add_parser('diff', help='pages whose tables differ between two runs')
    d.add_argument('old')
    d.add_argument('new')
    d.add_argument('--show', action='store_true', help='print the differences')
    d.set_defaults(func=cmd_diff)
    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
