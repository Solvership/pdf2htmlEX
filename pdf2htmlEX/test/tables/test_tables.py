#!/usr/bin/env python3
"""
Tests of the semantic table layer (run inside the dev container):

    scripts/dev.sh exec python3 pdf2htmlEX/test/tables/test_tables.py

For every PDF of this directory with an expected/<name>.json file the PDF is
converted with --semantic-tables 1 and the detected tables are compared with
the expectation (rows of cell texts, spans). The copy & paste handler is
exercised in headless Chromium.

Set P2H_TEST_GEN=1 to (re)generate the expectations from the current output
(review them before committing!).
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
BUILD = os.environ.get('P2H_BUILD_DIR', os.path.join(ROOT, 'pdf2htmlEX', 'build'))
DATA = os.path.join(BUILD, 'install', 'usr', 'local', 'share', 'pdf2htmlEX')
EXE = os.path.join(BUILD, 'pdf2htmlEX')
EXPECTED = os.path.join(HERE, 'expected')
GEN = bool(os.environ.get('P2H_TEST_GEN'))

# per document extra options
OPTIONS = {
    'borderless': ['--table-detect', 'all'],
    'twocolumn': ['--table-detect', 'all'],
}


def convert(pdf, outdir, extra=()):
    env = dict(os.environ, SOURCE_DATE_EPOCH='1700000000')
    subprocess.run([EXE, '--data-dir', DATA, '--poppler-data-dir', os.path.join(DATA, 'poppler'), '--quiet', '1',
                    '--semantic-tables', '1', '--semantic-output', 'attrs,inline,json,html',
                    *extra, '--dest-dir', outdir, pdf], check=True, env=env)
    name = os.path.splitext(os.path.basename(pdf))[0]
    with open(os.path.join(outdir, name + '.semantic.json')) as f:
        return name, json.load(f)


def summarize(sem):
    """Reduce the semantic json to what the tests check."""
    tables = []
    for p in sem['pages']:
        for t in p['tables']:
            rows = {}
            for c in t['cells']:
                cell = c['text']
                if c.get('rs', 1) > 1 or c.get('cs', 1) > 1:
                    cell = {'text': c['text'], 'rs': c.get('rs', 1), 'cs': c.get('cs', 1)}
                rows.setdefault(c['r'], {})[c['c']] = cell
            entry = {
                'page': p['page'],
                'source': t['source'],
                'rows': t['rows'],
                'cols': t['cols'],
                'cells': [[rows[r][c] for c in sorted(rows[r])] for r in sorted(rows)],
            }
            if 'continued_from' in t:
                entry['continued_from'] = t['continued_from']
            if t.get('repeated_header'):
                entry['repeated_header'] = True
            tables.append(entry)
    paragraphs = [q['text'] for p in sem['pages'] for q in p['paragraphs']]
    return {'tables': tables, 'paragraphs': paragraphs}


class TableTests(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix='p2h-tables-')
        cls.results = {}
        for f in sorted(os.listdir(HERE)):
            if f.endswith('.pdf'):
                name = f[:-4]
                _, sem = convert(os.path.join(HERE, f), cls.out, OPTIONS.get(name, []))
                cls.results[name] = summarize(sem)
        if GEN:
            os.makedirs(EXPECTED, exist_ok=True)
            for name, res in cls.results.items():
                with open(os.path.join(EXPECTED, name + '.json'), 'w') as f:
                    json.dump(res, f, ensure_ascii=False, indent=1)
                    f.write('\n')

    def test_expected(self):
        for name, res in sorted(self.results.items()):
            path = os.path.join(EXPECTED, name + '.json')
            if not os.path.exists(path):
                continue
            with self.subTest(document=name):
                with open(path) as f:
                    self.assertEqual(json.load(f), res)

    def test_multiline_cells_stay_one_row(self):
        t = self.results['ruled_multiline']['tables'][0]
        self.assertEqual(t['rows'], 5)
        self.assertEqual(t['cells'][1][1], 'Isporuka električne energije za razdoblje od siječnja do ožujka')

    def test_tagged_tables_identical_with_and_without_borders(self):
        a = self.results['tagged_bordered']['tables']
        b = self.results['tagged_borderless']['tables']
        self.assertEqual(a, b)
        self.assertEqual(a[0]['source'], 'tagged')

    def test_multipage_chain(self):
        tables = self.results['multipage']['tables']
        self.assertEqual(len(tables), 2)
        self.assertEqual(tables[1].get('continued_from'), 'p1t0')
        self.assertTrue(tables[1].get('repeated_header'))
        data_rows = tables[0]['rows'] - 1 + tables[1]['rows'] - 1
        self.assertEqual(data_rows, 45)

    def test_no_false_table_in_prose(self):
        # with all detectors on, the paragraphs of the borderless document are not tables
        b = self.results['borderless']
        self.assertEqual(len(b['tables']), 1)
        self.assertTrue(any(p.startswith('Ovo je običan odlomak') for p in b['paragraphs']))

    def test_two_column_prose(self):
        res = self.results['twocolumn']
        self.assertEqual(res['tables'], [])
        # read column by column, paragraphs never mix the two columns
        self.assertEqual(len(res['paragraphs']), 9)
        self.assertTrue(res['paragraphs'][5].startswith('jenu sustava'))

    def test_unruled_rows_in_frame(self):
        t = self.results['framed_unruled']['tables'][0]
        self.assertEqual(t['rows'], 5)
        self.assertEqual(t['cells'][2], ['K2', 'Transformator s dugim opisom koji se prelama u drugi redak', '1'])

    def test_financial_statement(self):
        # rules only under the numbers: labels and note numbers left of them are columns,
        # the wrapped label is one row, the sentence separates two tables
        tables = self.results['financial']['tables']
        self.assertEqual(len(tables), 2)
        t = tables[0]
        self.assertEqual(t['cells'][0], ['(u tisućama eura)', 'Bilješka', '2025.', '2024.'])
        self.assertEqual(t['cells'][1], ['Nabavna vrijednost'])  # cells without text are left out
        self.assertIn(['Potraživanja od kupaca i ostala potraživanja', '20', '650', '288'], t['cells'])
        self.assertEqual(t['cells'][-1], ['11.460', '1.346'])
        self.assertEqual(tables[1]['cells'][0], ['2025.', '2024.'])
        self.assertEqual(tables[1]['rows'], 5)

    def test_notes_headings_over_two_columns(self):
        # a year and the unit centered over "Brend | Goodwill" span both columns
        t = self.results['notes']['tables'][0]
        self.assertEqual(t['cells'][0], ['Brend', 'Goodwill', 'Brend', 'Goodwill'])
        self.assertEqual(t['cells'][1], [{'text': '2025.', 'rs': 1, 'cs': 2}, {'text': '2024.', 'rs': 1, 'cs': 2}])
        self.assertEqual(t['cells'][2][0], {'text': '(u tisućama eura)', 'rs': 1, 'cs': 2})

    def test_notes_wrapped_label_last_line_upper_case(self):
        t = self.results['notes']['tables'][1]
        # a wrapped heading ending with a full stop stays a row of its own ...
        self.assertEqual(t['cells'][1], ['Razdoblje od datuma stjecanja do 31. prosinca 2024.'])
        # ... a wrapped label is one row with the numbers on its last line
        self.assertEqual(t['cells'][3], ['Neto knjigovodstvena vrijednost Na 31. prosinca 2025.', '7.073', '2.730', '9.803'])
        self.assertEqual(t['rows'], 6)

    def test_notes_stacked_tables_stay_separate(self):
        tables = self.results['notes']['tables'][2:]
        self.assertEqual([t['cells'][0][0] for t in tables],
                         ['Prihodi od prodaje', 'Troškovi prodane robe', 'Krediti i zajmovi'])
        for t in tables:
            self.assertEqual(t['rows'], 5)
            self.assertEqual(t['cells'][1], [{'text': '(u tisućama eura)', 'rs': 1, 'cs': 2}])

    def test_statement_rows_below_last_rule(self):
        # "attributable to" / "earnings per share" rows without rules belong to the
        # statement, the sentence after it does not
        t = self.results['statement']['tables'][0]
        self.assertEqual(t['rows'], 12)
        self.assertEqual(t['cells'][6], ['Dobit za raspodjelu:'])
        self.assertEqual(t['cells'][-1], ['Razrijeđena', '0,57', '0,47'])
        self.assertTrue(self.results['statement']['paragraphs'][0].startswith('Računovodstvene politike'))

    def test_statement_short_total_rules(self):
        # values in the last columns only, a long label, short total rules under them
        t = self.results['statement']['tables'][1]
        self.assertEqual(t['rows'], 7)
        self.assertEqual(t['cells'][5], [{'text': 'Središnji administrativni i ostali troškovi', 'rs': 1, 'cs': 3},
                                         '(6.211)', '(6.261)'])
        self.assertEqual(t['cells'][6], ['Dobit prije poreza', '13.936', '10.516'])

    def test_chart_is_not_a_table(self):
        self.assertEqual(self.results['chart']['tables'], [])

    def test_overlay_hover_shows_cell(self):
        """Overlay on, the mouse over a cell box shows its table, row, column and text."""
        html = os.path.join(self.out, 'ruled_multiline.html')
        with open(html, encoding='utf-8') as f:
            doc = f.read()
        script = r"""
<script>
window.addEventListener('load', function() {
  window.pdf2htmlEXSemantic.toggleOverlay(true);
  var res = {cells: document.querySelectorAll('.pdf2htmlEX-sem-cell').length, tip: null};
  var cells = document.querySelectorAll('.pdf2htmlEX-sem-cell');
  for (var i = 0; i < cells.length; ++i) {
    if (cells[i]._sem.r !== 1 || cells[i]._sem.c !== 1) continue;
    var b = cells[i].getBoundingClientRect();
    cells[i].scrollIntoView({block: 'center'});
    b = cells[i].getBoundingClientRect();
    document.dispatchEvent(new MouseEvent('mousemove', {clientX: b.left + 3, clientY: b.top + b.height / 2, bubbles: true}));
    var tip = document.querySelector('.pdf2htmlEX-sem-tip');
    res.tip = tip && tip.style.display !== 'none' ? tip.textContent : null;
    res.outline = cells[i].style.outline;
    break;
  }
  var pre = document.createElement('pre'); pre.id = 'hover-result';
  pre.textContent = JSON.stringify(res);
  document.body.appendChild(pre);
});
</script>"""
        test_html = os.path.join(self.out, 'hover_test.html')
        with open(test_html, 'w', encoding='utf-8') as f:
            f.write(doc.replace('</body>', script + '</body>'))
        dom = subprocess.run(['chromium', '--headless=new', '--no-sandbox', '--disable-gpu', '--virtual-time-budget=3000',
                              '--window-size=1200,900', '--dump-dom', 'file://' + test_html],
                             check=True, capture_output=True, text=True).stdout
        m = re.search(r'<pre id="hover-result">(.*?)</pre>', dom, re.S)
        self.assertIsNotNone(m, 'overlay script did not run')
        import html as h
        res = json.loads(h.unescape(m.group(1)))
        self.assertGreater(res['cells'], 0)
        self.assertEqual(res['tip'], 'p1t0  red 2, stupac 2\n'
                                     'Isporuka električne energije za razdoblje od siječnja do ožujka')
        self.assertIn('2px', res['outline'])  # the hovered cell is highlighted

    def test_copy_paste_table(self):
        """Select the ruled table in Chromium, fire 'copy', check TSV / HTML."""
        html = os.path.join(self.out, 'ruled_multiline.html')
        with open(html, encoding='utf-8') as f:
            doc = f.read()
        script = r"""
<script>
window.addEventListener('load', function() {
  var lines = document.querySelectorAll('.t[data-tbl="p1t0"], .t[data-sem*="t:p1t0:"]');
  var range = document.createRange();
  range.setStartBefore(lines[0]);
  range.setEndAfter(lines[lines.length - 1]);
  var sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(range);
  var dt = new DataTransfer();
  var ev = new ClipboardEvent('copy', {clipboardData: dt, bubbles: true, cancelable: true});
  document.dispatchEvent(ev);
  var pre = document.createElement('pre'); pre.id = 'copy-result';
  pre.textContent = JSON.stringify({text: dt.getData('text/plain'), html: dt.getData('text/html')});
  document.body.appendChild(pre);
});
</script>"""
        test_html = os.path.join(self.out, 'copy_test.html')
        with open(test_html, 'w', encoding='utf-8') as f:
            f.write(doc.replace('</body>', script + '</body>'))
        dom = subprocess.run(['chromium', '--headless=new', '--no-sandbox', '--disable-gpu', '--virtual-time-budget=3000',
                              '--dump-dom', 'file://' + test_html],
                             check=True, capture_output=True, text=True).stdout
        m = re.search(r'<pre id="copy-result">(.*?)</pre>', dom, re.S)
        self.assertIsNotNone(m, 'copy handler did not run')
        import html as h
        res = json.loads(h.unescape(m.group(1)))
        rows = res['text'].split('\n')
        self.assertEqual(rows[0].split('\t'), ['Šifra', 'Opis stavke', 'Iznos'])
        self.assertEqual(rows[1].split('\t'),
                         ['A-001', 'Isporuka električne energije za razdoblje od siječnja do ožujka', '1.250,00'])
        self.assertEqual(len(rows), 5)
        self.assertIn('colspan="2"', res['html'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
