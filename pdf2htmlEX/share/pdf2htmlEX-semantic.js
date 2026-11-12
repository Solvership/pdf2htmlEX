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
      // the line ends with a hyphen added at the line break: the word goes on
      if (e === text.length && div.hasAttribute('data-hy') && pieces.length && pieces[pieces.length - 1].line === div)
        pieces[pieces.length - 1].hy = true;
    }
    return pieces;
  }

  // typographic ligatures (U+FB00..U+FB06) as letters
  var LIGATURES = { '\uFB00': 'ff', '\uFB01': 'fi', '\uFB02': 'fl', '\uFB03': 'ffi', '\uFB04': 'ffl', '\uFB05': 'st', '\uFB06': 'st' };
  function fixLigatures(s) {
    return s.replace(/[\uFB00-\uFB06]/g, function (c) { return LIGATURES[c]; });
  }

  // join the pieces of a text block; a word hyphenated at a line break is joined
  function joinParts(parts, sep) {
    var out = '';
    for (var i = 0; i < parts.length; ++i) {
      var t = parts[i].text;
      if (out && i > 0 && parts[i - 1].hy && /-\s*$/.test(out))
        out = out.replace(/-\s*$/, '') + t.replace(/^\s+/, '');
      else
        out += (out ? sep : '') + t;
    }
    return out;
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
    if (!hasSemantic) {
      // plain text: only the ligatures are replaced
      var plain = sel.toString();
      if (!/[\uFB00-\uFB06]/.test(plain) || !ev.clipboardData) return;
      var box = document.createElement('div');
      box.appendChild(sel.getRangeAt(0).cloneContents());
      ev.clipboardData.setData('text/plain', fixLigatures(plain));
      ev.clipboardData.setData('text/html', fixLigatures(box.innerHTML));
      ev.preventDefault();
      return;
    }

    var outText = [], outHtml = [];
    var cur = null; // current block: {kind:'table', tbl, cells} | {kind:'text', para, parts}
    function flush() {
      if (!cur) return;
      if (cur.kind === 'table') {
        var b = tableBlock(cur.tbl, cur.cells);
        outText.push(b.text);
        outHtml.push(b.html);
      } else {
        var t = clean(joinParts(cur.parts, cur.para ? ' ' : '\n'));
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
        cur.parts.push(p);
      }
    }
    flush();

    if (ev.clipboardData) {
      ev.clipboardData.setData('text/plain', fixLigatures(outText.join('\n')));
      ev.clipboardData.setData('text/html', fixLigatures(outHtml.join('\n')));
      ev.preventDefault();
    }
  }

  document.addEventListener('copy', onCopy);

  /*
   * Table overlay: shows the detected tables and cells over the page (the
   * semantic layer itself is invisible). Toggled with the "t" key, or shown
   * from the start with "#tables" in the URL. Nothing is drawn otherwise.
   */
  var COLORS = ['#d62728', '#1f77b4', '#2ca02c', '#9467bd', '#ff7f0e', '#17becf'];
  var overlayOn = false;
  var overlayPages = [];   // pages with cell boxes (page._semCells)
  var tip = null, hovered = null;

  // range of the text offsets [s, e) of a line div
  function textRange(div, s, e) {
    var r = document.createRange();
    var walker = document.createTreeWalker(div, NodeFilter.SHOW_TEXT, null, false);
    var pos = 0, node, started = false;
    while ((node = walker.nextNode())) {
      var len = node.nodeValue.length;
      if (!started && s <= pos + len) { r.setStart(node, s - pos); started = true; }
      if (started && e <= pos + len) { r.setEnd(node, e - pos); return r; }
      pos += len;
    }
    return started ? r : null;
  }

  function removeOverlay() {
    var old = document.querySelectorAll('.pdf2htmlEX-sem-overlay');
    for (var i = 0; i < old.length; ++i) old[i].parentNode.removeChild(old[i]);
    for (var j = 0; j < overlayPages.length; ++j) overlayPages[j]._semCells = null;
    overlayPages = [];
    hovered = null;
    if (tip) tip.style.display = 'none';
  }

  function box(page, cls, x0, y0, x1, y1, color, alpha) {
    var d = document.createElement('div');
    d.className = 'pdf2htmlEX-sem-overlay ' + cls;
    d.style.cssText = 'position:absolute;pointer-events:none;z-index:10;box-sizing:border-box;' +
      'left:' + x0 + 'px;top:' + y0 + 'px;width:' + (x1 - x0) + 'px;height:' + (y1 - y0) + 'px;' +
      'border:1px solid ' + color + ';background:' + color.replace(/^#(..)(..)(..)$/, function (m, r, g, b) {
        return 'rgba(' + parseInt(r, 16) + ',' + parseInt(g, 16) + ',' + parseInt(b, 16) + ',' + alpha + ')';
      });
    page.appendChild(d);
    return d;
  }

  function drawOverlay() {
    removeOverlay();
    if (!overlayOn) return;
    var root = document.getElementById('page-container') || document.body;
    var lines = root.querySelectorAll('.t[data-sem], .t[data-tbl]');
    var cells = {}, tables = {}, order = [];
    function add(map, key, page, rc, extra) {
      var pr = page.getBoundingClientRect();
      var b = map[key];
      if (!b) { b = map[key] = { page: page, x0: 1e9, y0: 1e9, x1: -1e9, y1: -1e9 }; for (var k in extra) b[k] = extra[k]; }
      b.x0 = Math.min(b.x0, rc.left - pr.left); b.y0 = Math.min(b.y0, rc.top - pr.top);
      b.x1 = Math.max(b.x1, rc.right - pr.left); b.y1 = Math.max(b.y1, rc.bottom - pr.top);
    }
    for (var i = 0; i < lines.length; ++i) {
      var div = lines[i];
      var page = div.closest('.pf');
      if (!page) continue;
      var parts = [];
      var sem = div.getAttribute('data-sem');
      if (sem) {
        var rs = parseSem(sem);
        for (var k = 0; k < rs.length; ++k) if (rs[k].tbl) parts.push({ e: rs[k], range: textRange(div, rs[k].start, rs[k].end) });
      } else {
        var key = lineKey(div);
        var r = document.createRange(); r.selectNodeContents(div);
        parts.push({ e: key, range: r });
      }
      for (var p = 0; p < parts.length; ++p) {
        if (!parts[p].range) continue;
        var e = parts[p].e;
        var rects = parts[p].range.getClientRects();
        var ckey = e.tbl + '|' + e.r + '|' + e.c;
        var piece = parts[p].range.toString().replace(/\s+/g, ' ').trim();
        for (var q = 0; q < rects.length; ++q) {
          if (rects[q].width <= 0 || rects[q].height <= 0) continue;
          if (!tables[e.tbl]) order.push(e.tbl);
          add(cells, e.tbl + '|' + e.r + '|' + e.c, page, rects[q], { tbl: e.tbl, r: e.r, c: e.c, texts: [] });
          add(tables, e.tbl, page, rects[q], { rows: 0, cols: 0 });
          tables[e.tbl].rows = Math.max(tables[e.tbl].rows, e.r + 1);
          tables[e.tbl].cols = Math.max(tables[e.tbl].cols, e.c + 1);
        }
        if (piece && cells[ckey]) cells[ckey].texts.push(piece);
      }
    }
    for (var t = 0; t < order.length; ++t) {
      var tb = tables[order[t]];
      tb.color = COLORS[t % COLORS.length];
      var frame = box(tb.page, 'pdf2htmlEX-sem-table', tb.x0 - 4, tb.y0 - 4, tb.x1 + 4, tb.y1 + 4, tb.color, 0);
      frame.style.borderWidth = '2px';
      var label = document.createElement('span');
      label.textContent = order[t] + ' · ' + tb.rows + '×' + tb.cols;
      label.style.cssText = 'position:absolute;right:0;bottom:100%;font:11px/1.3 sans-serif;color:#fff;padding:0 4px;background:' + tb.color;
      frame.appendChild(label);
    }
    for (var key2 in cells) {
      var c = cells[key2];
      var cell = box(c.page, 'pdf2htmlEX-sem-cell', c.x0 - 1, c.y0 - 1, c.x1 + 1, c.y1 + 1, tables[c.tbl].color, c.r % 2 ? 0.18 : 0.08);
      cell._sem = { tbl: c.tbl, r: c.r, c: c.c, text: c.texts.join(' '), color: tables[c.tbl].color };
      if (!c.page._semCells) { c.page._semCells = []; overlayPages.push(c.page); }
      c.page._semCells.push(cell);
    }
  }

  /*
   * Hover: the boxes let the mouse through (pointer-events:none, so text can
   * still be selected), the cell under the pointer is found by its rectangle.
   */
  function showTip(cell, x, y) {
    if (!tip) {
      tip = document.createElement('div');
      tip.className = 'pdf2htmlEX-sem-tip';
      tip.style.cssText = 'position:fixed;z-index:2147483647;pointer-events:none;max-width:420px;' +
        'font:12px/1.4 sans-serif;color:#fff;background:rgba(20,20,20,.92);padding:4px 7px;border-radius:3px;' +
        'white-space:pre-wrap;box-shadow:0 1px 4px rgba(0,0,0,.4)';
      document.body.appendChild(tip);
    }
    var m = cell._sem;
    // shown from 1, data-r / data-c and the json count from 0
    tip.textContent = m.tbl + '  red ' + (m.r + 1) + ', stupac ' + (m.c + 1) + (m.text ? '\n' + m.text : '');
    tip.style.borderLeft = '4px solid ' + m.color;
    tip.style.display = 'block';
    var w = tip.offsetWidth, h = tip.offsetHeight;
    tip.style.left = Math.max(0, Math.min(x + 14, window.innerWidth - w - 4)) + 'px';
    tip.style.top = (y + 18 + h > window.innerHeight ? y - h - 8 : y + 18) + 'px';
  }

  function onHover(ev) {
    if (!overlayOn) return;
    var x = ev.clientX, y = ev.clientY, found = null;
    var el = document.elementFromPoint(x, y);
    var page = el && el.closest && el.closest('.pf');
    var list = (page && page._semCells) || [];
    for (var i = list.length - 1; i >= 0 && !found; --i) {
      var b = list[i].getBoundingClientRect();
      if (x >= b.left && x <= b.right && y >= b.top && y <= b.bottom) found = list[i];
    }
    if (found !== hovered) {
      if (hovered) hovered.style.outline = '';
      if (found) found.style.outline = '2px solid ' + found._sem.color;
      hovered = found;
    }
    if (found) showTip(found, x, y);
    else if (tip) tip.style.display = 'none';
  }
  document.addEventListener('mousemove', onHover, { passive: true });
  document.addEventListener('scroll', function () { if (tip) tip.style.display = 'none'; }, { passive: true, capture: true });

  var pending = 0;
  function redraw() {
    if (!overlayOn || pending) return;
    pending = setTimeout(function () { pending = 0; drawOverlay(); }, 150);
  }

  function toggleOverlay(on) {
    overlayOn = (on === undefined) ? !overlayOn : on;
    drawOverlay();
  }

  document.addEventListener('keydown', function (ev) {
    var tg = ev.target && ev.target.tagName;
    if (ev.ctrlKey || ev.metaKey || ev.altKey || tg === 'INPUT' || tg === 'TEXTAREA' || tg === 'SELECT') return;
    if (ev.key === 't' || ev.key === 'T') toggleOverlay();
  });
  // the viewer rescales the pages on resize and zoom
  window.addEventListener('resize', redraw);
  window.addEventListener('wheel', function (ev) { if (ev.ctrlKey) redraw(); }, { passive: true });
  window.addEventListener('load', function () {
    if (/(^#|[#&])tables\b/.test(location.hash)) toggleOverlay(true);
  });

  // exported for tests
  window.pdf2htmlEXSemantic = { collectPieces: collectPieces, onCopy: onCopy, toggleOverlay: toggleOverlay };
})();
