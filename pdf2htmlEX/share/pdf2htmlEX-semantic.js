/*
 * pdf2htmlEX-semantic.js
 *
 * Copy & paste support for the semantic layer (--semantic-tables 1):
 * when the selection touches table cells, the clipboard receives a real
 * table (text/html) and tab separated values (text/plain), so pasting into
 * a spreadsheet or word processor keeps one table row per row - also for
 * cells whose text wraps onto several lines in the PDF. Lines of the same
 * paragraph are joined with spaces instead of line breaks.
 *
 * Only included in the output when the semantic layer is enabled.
 */
(function () {
  'use strict';

  function textOffset(root, node, offset) {
    // offset of the boundary point (node, offset) in root.textContent
    var r = document.createRange();
    r.setStart(root, 0);
    r.setEnd(node, offset);
    return r.toString().length;
  }

  function parseSem(attr) {
    // "t:p1t0:4:2@0-4 p:p1p3@5-21"
    var out = [];
    if (!attr) return out;
    var parts = attr.split(' ');
    for (var i = 0; i < parts.length; ++i) {
      var at = parts[i].lastIndexOf('@');
      if (at < 0) continue;
      var key = parts[i].substring(0, at).split(':');
      var rng = parts[i].substring(at + 1).split('-');
      var e = { start: +rng[0], end: +rng[1] };
      if (key[0] === 't') { e.tbl = key[1]; e.r = +key[2]; e.c = +key[3]; }
      else if (key[0] === 'p') { e.para = key[1]; }
      out.push(e);
    }
    return out;
  }

  function lineKey(div) {
    return {
      tbl: div.getAttribute('data-tbl'),
      r: +div.getAttribute('data-r'),
      c: +div.getAttribute('data-c'),
      para: div.getAttribute('data-para')
    };
  }

  // pieces of the selection: {tbl,r,c,text} | {para,text} | {text}
  function collectPieces(range) {
    var pieces = [];
    var root = document.getElementById('page-container') || document.body;
    var lines = root.querySelectorAll('.t');
    for (var i = 0; i < lines.length; ++i) {
      var div = lines[i];
      if (!range.intersectsNode(div)) continue;
      var text = div.textContent;
      var s = div.contains(range.startContainer) ? textOffset(div, range.startContainer, range.startOffset) : 0;
      var e = div.contains(range.endContainer) ? textOffset(div, range.endContainer, range.endOffset) : text.length;
      if (e <= s) continue;
      var sem = div.getAttribute('data-sem');
      if (sem) {
        var rs = parseSem(sem);
        var pos = s;
        for (var k = 0; k < rs.length; ++k) {
          var a = Math.max(rs[k].start, s), b = Math.min(rs[k].end, e);
          if (b <= a) continue;
          if (a > pos && text.substring(pos, a).trim()) pieces.push({ text: text.substring(pos, a), line: div });
          var p = { text: text.substring(a, b), line: div };
          if (rs[k].tbl) { p.tbl = rs[k].tbl; p.r = rs[k].r; p.c = rs[k].c; }
          else if (rs[k].para) { p.para = rs[k].para; }
          pieces.push(p);
          pos = b;
        }
        if (pos < e && text.substring(pos, e).trim()) pieces.push({ text: text.substring(pos, e), line: div });
      } else {
        var key = lineKey(div);
        var piece = { text: text.substring(s, e), line: div };
        if (key.tbl) { piece.tbl = key.tbl; piece.r = key.r; piece.c = key.c; }
        else if (key.para) { piece.para = key.para; }
        pieces.push(piece);
      }
    }
    return pieces;
  }

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  function clean(s) {
    return s.replace(/[\t\r\n]+/g, ' ').replace(/ {2,}/g, ' ').trim();
  }

  function tableBlock(tbl, cells) {
    // cells: map "r,c" -> selected text pieces
    var rmin = 1e9, rmax = -1, cmin = 1e9, cmax = -1, k;
    for (k in cells) {
      var rc = k.split(',');
      rmin = Math.min(rmin, +rc[0]); rmax = Math.max(rmax, +rc[0]);
      cmin = Math.min(cmin, +rc[1]); cmax = Math.max(cmax, +rc[1]);
    }
    var semTable = document.querySelector('.pdf2htmlEX-semantic table[data-tbl="' + tbl + '"]');
    var grid = {};
    if (semTable) {
      var tds = semTable.querySelectorAll('[data-r]');
      for (var i = 0; i < tds.length; ++i) {
        var td = tds[i];
        grid[td.getAttribute('data-r') + ',' + td.getAttribute('data-c')] = {
          text: td.textContent,
          rs: +(td.getAttribute('rowspan') || 1),
          cs: +(td.getAttribute('colspan') || 1)
        };
      }
    }
    var tsv = [], html = ['<table>'];
    var covered = {};
    for (var r = rmin; r <= rmax; ++r) {
      var row = [];
      html.push('<tr>');
      for (var c = cmin; c <= cmax; ++c) {
        var key = r + ',' + c;
        var cell = grid[key];
        var txt = cell ? cell.text : (cells[key] ? cells[key].join(' ') : '');
        txt = clean(txt);
        row.push(covered[key] ? '' : txt);
        if (covered[key]) continue;
        var rs = cell ? Math.min(cell.rs, rmax - r + 1) : 1;
        var cs = cell ? Math.min(cell.cs, cmax - c + 1) : 1;
        for (var dr = 0; dr < rs; ++dr)
          for (var dc = 0; dc < cs; ++dc)
            if (dr || dc) covered[(r + dr) + ',' + (c + dc)] = true;
        html.push('<td' + (rs > 1 ? ' rowspan="' + rs + '"' : '') + (cs > 1 ? ' colspan="' + cs + '"' : '') + '>' + escapeHtml(txt) + '</td>');
      }
      html.push('</tr>');
      tsv.push(row.join('\t'));
    }
    html.push('</table>');
    return { text: tsv.join('\n'), html: html.join('') };
  }

  function onCopy(ev) {
    var sel = window.getSelection();
    if (!sel || sel.rangeCount === 0 || sel.isCollapsed) return;
    var pieces = collectPieces(sel.getRangeAt(0));
    var hasSemantic = false;
    for (var i = 0; i < pieces.length; ++i)
      if (pieces[i].tbl || pieces[i].para) { hasSemantic = true; break; }
    if (!hasSemantic) return;

    var outText = [], outHtml = [];
    var cur = null; // current block: {kind:'table', tbl, cells} | {kind:'text', para, parts}
    function flush() {
      if (!cur) return;
      if (cur.kind === 'table') {
        var b = tableBlock(cur.tbl, cur.cells);
        outText.push(b.text);
        outHtml.push(b.html);
      } else {
        var t = clean(cur.parts.join(cur.para ? ' ' : '\n'));
        if (t) {
          outText.push(t);
          outHtml.push('<p>' + escapeHtml(t) + '</p>');
        }
      }
      cur = null;
    }
    for (var j = 0; j < pieces.length; ++j) {
      var p = pieces[j];
      if (p.tbl) {
        if (!cur || cur.kind !== 'table' || cur.tbl !== p.tbl) { flush(); cur = { kind: 'table', tbl: p.tbl, cells: {} }; }
        var key = p.r + ',' + p.c;
        (cur.cells[key] = cur.cells[key] || []).push(p.text);
      } else {
        if (!p.text.trim()) continue;
        if (!cur || cur.kind !== 'text' || cur.para !== p.para || !p.para) { flush(); cur = { kind: 'text', para: p.para, parts: [] }; }
        cur.parts.push(p.text);
      }
    }
    flush();

    if (ev.clipboardData) {
      ev.clipboardData.setData('text/plain', outText.join('\n'));
      ev.clipboardData.setData('text/html', outHtml.join('\n'));
      ev.preventDefault();
    }
  }

  document.addEventListener('copy', onCopy);

  // exported for tests
  window.pdf2htmlEXSemantic = { collectPieces: collectPieces, onCopy: onCopy };
})();
