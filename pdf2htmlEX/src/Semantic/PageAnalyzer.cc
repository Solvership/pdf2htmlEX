/*
 * PageAnalyzer.cc
 *
 * Runs the semantic analysis of one page: tables (tagged, ruled, hrules,
 * stream) first, then paragraphs from the remaining text.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "Semantic.h"

namespace pdf2htmlEX {
namespace semantic {

using std::vector;
using std::string;
using std::min;
using std::max;
using std::abs;

Options Options::from_param(const Param & param)
{
    Options o;
    const string & d = param.table_detect;
    auto has = [&](const char * w) {
        return d == "all" || ("," + d + ",").find(string(",") + w + ",") != string::npos;
    };
    o.detect_tagged = has("tagged");
    o.detect_ruled = has("ruled");
    o.detect_hrules = has("hrules");
    o.detect_stream = has("stream");
    o.paragraphs = param.semantic_paragraphs != 0;
    o.dehyphenate = param.dehyphenate != 0;
    o.cell_join_br = param.cell_line_join == "br";
    return o;
}

namespace {

void build_tagged_tables(const PageInput & in, vector<bool> & taken, vector<Table> & tables)
{
    std::map<string, size_t> key2t;
    std::map<std::pair<size_t, std::pair<int, int>>, size_t> cell_idx;
    for (const auto & ch : in.chars)
    {
        if (ch.mcid < 0 || ch.text.empty() || taken[ch.index])
            continue;
        auto it = in.mcid_cells.find(ch.mcid);
        if (it == in.mcid_cells.end())
            continue;
        const auto & tag = it->second;
        auto ti = key2t.find(tag.table_key);
        if (ti == key2t.end())
        {
            Table t;
            t.source = "tagged";
            t.struct_key = tag.table_key;
            t.struct_seq = tag.seq;
            key2t[tag.table_key] = tables.size();
            tables.push_back(t);
            ti = key2t.find(tag.table_key);
        }
        Table & t = tables[ti->second];
        auto ck = std::make_pair(ti->second, std::make_pair(tag.row, tag.col));
        auto ci = cell_idx.find(ck);
        if (ci == cell_idx.end())
        {
            Cell c;
            c.row = tag.row;
            c.col = tag.col;
            c.rowspan = tag.rowspan;
            c.colspan = tag.colspan;
            c.header = tag.header;
            cell_idx[ck] = t.cells.size();
            t.cells.push_back(c);
            ci = cell_idx.find(ck);
        }
        t.cells[ci->second].chars.push_back(ch.index);
        taken[ch.index] = true;
    }

    for (size_t k = 0; k < tables.size(); ++k)
    {
        Table & t = tables[k];
        if (t.source != "tagged")
            continue;
        // rows / cols relative to this page: keep the row numbers of the
        // structure, but start at 0 for the first row present on the page
        int rmin = 1 << 30;
        for (const auto & c : t.cells)
            rmin = min(rmin, c.row);
        for (auto & c : t.cells)
            c.row -= rmin;
        for (const auto & c : t.cells)
        {
            t.rows = max(t.rows, c.row + c.rowspan);
            t.cols = max(t.cols, c.col + c.colspan);
        }
    }
}

/*
 * A gutter is a vertical white channel shared by several consecutive lines,
 * e.g. between the columns of a two column layout.
 */
struct Gutter { double x0, x1; int line0, line1; };

vector<Gutter> find_gutters(const vector<Char> & chars, const vector<VLine> & lines)
{
    const int MIN_LINES = 4;
    vector<Gutter> out;
    struct Chain { double x0, x1; int line0, last, real; }; // real: lines with the gap
    vector<Chain> chains;
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const VLine & l = lines[li];
        double em = max(l.em, 1.0);
        // gaps of the line
        vector<std::pair<double, double>> gaps;
        const Char * prev = nullptr;
        for (int i : l.chars)
        {
            const Char & c = chars[i];
            if (c.is_space() || !c.box.valid())
                continue;
            if (prev && c.box.x0 - prev->box.x1 >= 0.8 * em)
                gaps.emplace_back(prev->box.x1, c.box.x0);
            if (!prev || c.box.x1 > prev->box.x1)
                prev = &c;
        }
        vector<Chain> next;
        for (const auto & g : gaps)
        {
            bool extended = false;
            for (const auto & ch : chains)
            {
                double a = max(ch.x0, g.first), b = min(ch.x1, g.second);
                if (b - a >= 0.3 * em && ch.last + 1 == (int)li)
                {
                    next.push_back({a, b, ch.line0, (int)li, ch.real + 1});
                    extended = true;
                }
            }
            if (!extended)
                next.push_back({g.first, g.second, (int)li, (int)li, 1});
        }
        /*
         * A line of one column only (the columns' baselines need not match, so
         * a visual line may hold just one of them) has no ink in the gutter:
         * it does not end the gutter.
         */
        for (const auto & ch : chains)
        {
            if (ch.last + 1 != (int)li)
                continue;
            bool continued = false;
            for (const auto & n : next)
                if (n.line0 == ch.line0 && n.x0 >= ch.x0 - 0.01 && n.x1 <= ch.x1 + 0.01)
                    continued = true;
            if (continued)
                continue;
            bool ink = false;
            for (int i : l.chars)
            {
                const Char & c = chars[i];
                if (!c.is_space() && c.box.valid() && c.box.x1 > ch.x0 + 0.5 && c.box.x0 < ch.x1 - 0.5)
                {
                    ink = true;
                    break;
                }
            }
            if (!ink)
                next.push_back({ch.x0, ch.x1, ch.line0, (int)li, ch.real});
        }
        // chains that ended
        for (const auto & ch : chains)
        {
            bool continued = false;
            for (const auto & n : next)
                if (n.line0 == ch.line0 && n.x0 >= ch.x0 - 0.01 && n.x1 <= ch.x1 + 0.01)
                    continued = true;
            if (!continued && ch.last - ch.line0 + 1 >= MIN_LINES && ch.real >= 2 && ch.real >= 0.3 * (ch.last - ch.line0 + 1))
                out.push_back({ch.x0, ch.x1, ch.line0, ch.last});
        }
        chains = next;
    }
    for (const auto & ch : chains)
        if (ch.last - ch.line0 + 1 >= MIN_LINES && ch.real >= 2 && ch.real >= 0.3 * (ch.last - ch.line0 + 1))
            out.push_back({ch.x0, ch.x1, ch.line0, ch.last});
    return out;
}

vector<Segment> split_at_gutters(const vector<Char> & chars, const Segment & seg, const vector<Gutter> & gutters, int line)
{
    vector<double> cuts;
    for (const auto & g : gutters)
        if (line >= g.line0 && line <= g.line1 && g.x0 > seg.box.x0 && g.x1 < seg.box.x1)
            cuts.push_back((g.x0 + g.x1) / 2);
    if (cuts.empty())
        return {seg};
    std::sort(cuts.begin(), cuts.end());
    vector<Segment> out(cuts.size() + 1);
    for (int i : seg.chars)
    {
        const Char & c = chars[i];
        size_t k = std::upper_bound(cuts.begin(), cuts.end(), c.box.cx()) - cuts.begin();
        if (out[k].chars.empty() && c.is_space())
            continue;
        out[k].chars.push_back(i);
        if (!c.is_space())
        {
            if (out[k].box.valid())
                out[k].box.expand(c.box);
            else
                out[k].box = c.box;
        }
    }
    vector<Segment> res;
    for (auto & s : out)
        if (!s.chars.empty() && s.box.valid())
            res.push_back(s);
    return res;
}

struct Para
{
    Box box;
    vector<Segment> segs;
    double body_x0 = 0;
    double em = 10;
};

void build_paragraphs(const PageInput & in, const vector<bool> & taken, const Options & opt, vector<Paragraph> & out)
{
    vector<bool> used(in.chars.size(), false);

    // tagged blocks first
    if (opt.detect_tagged && !in.mcid_blocks.empty())
    {
        std::map<string, size_t> key2p;
        for (const auto & ch : in.chars)
        {
            if (ch.mcid < 0 || ch.text.empty() || taken[ch.index] || !ch.box.valid())
                continue;
            auto it = in.mcid_blocks.find(ch.mcid);
            if (it == in.mcid_blocks.end())
                continue;
            auto pi = key2p.find(it->second.key);
            if (pi == key2p.end())
            {
                Paragraph p;
                p.tag = it->second.tag;
                p.struct_seq = it->second.seq;
                key2p[it->second.key] = out.size();
                out.push_back(p);
                pi = key2p.find(it->second.key);
            }
            out[pi->second].chars.push_back(ch.index);
            used[ch.index] = true;
        }
        for (auto & p : out)
        {
            /*
             * A block may flow from the bottom of one column to the top of the
             * next one: its text is cut where the content order jumps back up,
             * and the pieces are read in that order (lines of different columns
             * at the same height must not become one line).
             */
            vector<int> sorted = p.chars;
            std::sort(sorted.begin(), sorted.end());
            vector<vector<int>> pieces(1);
            double bottom = -1e30;
            for (int ci : sorted)
            {
                const Char & c = in.chars[ci];
                double em = max(c.em, 1.0);
                if (!pieces.back().empty() && !c.is_space() && c.box.y1 < bottom - 1.5 * em)
                {
                    pieces.emplace_back();
                    bottom = -1e30;
                }
                pieces.back().push_back(ci);
                if (!c.is_space())
                    bottom = max(bottom, c.box.y1);
            }
            for (const auto & piece : pieces)
                for (const auto & vl : build_visual_lines(in.chars, piece))
                {
                    string s = chars_text(in.chars, vl.chars);
                    if (!s.empty())
                        p.lines.push_back(s);
                    p.box.expand(vl.box);
                }
            p.text = join_lines(p.lines, opt.dehyphenate);
            for (auto & l : p.lines)
                l = visible_line(l);
        }
    }

    vector<int> ids;
    for (const auto & ch : in.chars)
        if (!taken[ch.index] && !used[ch.index] && ch.box.valid() && !ch.text.empty())
            ids.push_back(ch.index);
    auto lines = build_visual_lines(in.chars, ids);

    // column gutters (multi column layouts): split lines there as well
    auto gutters = find_gutters(in.chars, lines);

    vector<Segment> segs;
    for (size_t li = 0; li < lines.size(); ++li)
    {
        size_t first = segs.size();
        for (auto & s0 : split_segments(in.chars, lines[li], 1.5))
            for (auto & s : split_at_gutters(in.chars, s0, gutters, (int)li))
            {
                if (s.chars.empty())
                    continue;
                s.vline = (int)li;
                segs.push_back(s);
            }
        /*
         * A number or a label in front of a heading or a list item ("1.1.",
         * "a)", a bullet), set apart by a tab, belongs to the text after it -
         * unless a column gutter separates them.
         */
        for (size_t k = first; k + 1 < segs.size(); )
        {
            Segment & a = segs[k];
            const Segment & b = segs[k + 1];
            string t = chars_text(in.chars, a.chars);
            double em = max(lines[li].em, 1.0);
            bool label = !t.empty() && t.size() <= 8 && t.find(' ') == string::npos
                    && (std::any_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })
                        || t == "\xe2\x80\xa2" || t == "-" || (t.size() == 2 && t[1] == ')'));
            bool gutter = false;
            for (const auto & g : gutters)
                if ((int)li >= g.line0 && (int)li <= g.line1 && g.x0 >= a.box.x1 - 0.5 && g.x1 <= b.box.x0 + 0.5)
                    gutter = true;
            if (label && !gutter && b.box.x0 > a.box.x1 && b.box.x0 - a.box.x1 < 4 * em)
            {
                a.chars.insert(a.chars.end(), b.chars.begin(), b.chars.end());
                a.box.expand(b.box);
                segs.erase(segs.begin() + k + 1);
            }
            else
                ++k;
        }
    }
    std::stable_sort(segs.begin(), segs.end(), [](const Segment & a, const Segment & b) {
        return a.box.y0 < b.box.y0 - 0.5 || (abs(a.box.y0 - b.box.y0) <= 0.5 && a.box.x0 < b.box.x0);
    });
    if (getenv("P2H_PARA_DEBUG"))
    {
        for (const auto & g : gutters)
            fprintf(stderr, "para: gutter %.1f-%.1f lines %d-%d (y %.1f-%.1f)\n", g.x0, g.x1, g.line0, g.line1,
                    lines[g.line0].box.y0, lines[g.line1].box.y1);
        for (const auto & sg : segs)
            fprintf(stderr, "para: seg line %d %.1f,%.1f-%.1f,%.1f '%s'\n", sg.vline, sg.box.x0, sg.box.y0, sg.box.x1, sg.box.y1,
                    chars_text(in.chars, sg.chars).substr(0, 50).c_str());
    }

    auto seg_em = [&](const Segment & s) {
        double sum = 0;
        int n = 0;
        for (int i : s.chars)
            if (!in.chars[i].is_space())
            {
                sum += in.chars[i].em;
                ++n;
            }
        return n ? max(sum / n, 1.0) : 10.0;
    };

    vector<Para> paras;
    for (const auto & s : segs)
    {
        double em = seg_em(s);
        int best = -1;
        for (int k = (int)paras.size() - 1; k >= 0; --k)
        {
            Para & p = paras[k];
            const Segment & last = p.segs.back();
            double vgap = s.box.y0 - last.box.y1;
            double e = max(em, p.em);
            if (vgap > 0.9 * e || vgap < -0.3 * e)
                continue;
            if (s.vline == last.vline)
                continue;
            if (max(em, p.em) / min(em, p.em) > 1.3)
                continue;
            double hov = min(s.box.x1, last.box.x1) - max(s.box.x0, last.box.x0);
            if (hov < 0.5 * min(s.box.w(), last.box.w()) && abs(s.box.x0 - p.box.x0) > e)
                continue;
            if (p.segs.size() >= 2)
            {
                // indented first line of a new paragraph
                if (s.box.x0 > p.body_x0 + 0.8 * e && s.box.x0 < p.body_x0 + 6 * e)
                    continue;
                // the previous line was short: it ended the paragraph
                if (last.box.x1 < p.box.x1 - 4 * e)
                    continue;
            }
            best = k;
            break;
        }
        if (best < 0)
        {
            Para p;
            p.box = s.box;
            p.segs.push_back(s);
            p.body_x0 = s.box.x0;
            p.em = em;
            paras.push_back(p);
        }
        else
        {
            Para & p = paras[best];
            if (p.segs.size() == 1)
                p.body_x0 = s.box.x0;
            else
                p.body_x0 = min(p.body_x0, s.box.x0);
            p.segs.push_back(s);
            p.box.expand(s.box);
        }
    }

    for (const auto & p : paras)
    {
        Paragraph para;
        para.box = p.box;
        for (const auto & s : p.segs)
        {
            para.chars.insert(para.chars.end(), s.chars.begin(), s.chars.end());
            string t = chars_text(in.chars, s.chars);
            if (!t.empty())
                para.lines.push_back(t);
        }
        para.text = join_lines(para.lines, opt.dehyphenate);
        for (auto & l : para.lines)
            l = visible_line(l);
        if (!para.text.empty())
            out.push_back(para);
    }
}

/*
 * Reading order of blocks by recursive XY-cut: columns first (so that a two
 * column layout is read column by column), then horizontal bands.
 */
void xy_cut(const vector<Box> & boxes, vector<int> idx, vector<int> & order)
{
    if (idx.size() <= 1)
    {
        for (int i : idx)
            order.push_back(i);
        return;
    }
    for (int dir = 0; dir < 2; ++dir)
    {
        // dir 0: vertical cuts (split along x), dir 1: horizontal cuts (along y)
        vector<int> sorted = idx;
        std::sort(sorted.begin(), sorted.end(), [&](int a, int b) {
            return dir == 0 ? boxes[a].x0 < boxes[b].x0 : boxes[a].y0 < boxes[b].y0;
        });
        vector<vector<int>> groups;
        double reach = -1e30;
        for (int i : sorted)
        {
            double lo = dir == 0 ? boxes[i].x0 : boxes[i].y0;
            double hi = dir == 0 ? boxes[i].x1 : boxes[i].y1;
            if (groups.empty() || lo > reach + 0.5)
                groups.push_back({});
            groups.back().push_back(i);
            reach = max(reach, hi);
        }
        /*
         * Bands of a multi column layout: where the paragraphs of the columns
         * end at the same height, the page falls apart into bands holding a
         * paragraph of each column, read left, right, left, right... Consecutive
         * bands sharing a column gap are one block, cut into columns first.
         */
        if (dir == 1 && groups.size() > 1)
        {
            auto gaps_of = [&](const vector<int> & g) {
                vector<std::pair<double, double>> iv, gaps;
                for (int i : g)
                    iv.emplace_back(boxes[i].x0, boxes[i].x1);
                std::sort(iv.begin(), iv.end());
                double r = iv[0].second;
                for (size_t k = 1; k < iv.size(); ++k)
                {
                    if (iv[k].first > r + 0.5)
                        gaps.emplace_back(r, iv[k].first);
                    r = max(r, iv[k].second);
                }
                return gaps;
            };
            vector<vector<int>> merged;
            vector<std::pair<double, double>> cur;
            for (auto & g : groups)
            {
                auto gg = gaps_of(g);
                if (!merged.empty() && !gg.empty() && !cur.empty())
                {
                    vector<std::pair<double, double>> common;
                    for (const auto & a : cur)
                        for (const auto & b : gg)
                            if (min(a.second, b.second) - max(a.first, b.first) >= 2)
                                common.emplace_back(max(a.first, b.first), min(a.second, b.second));
                    if (!common.empty())
                    {
                        merged.back().insert(merged.back().end(), g.begin(), g.end());
                        cur = common;
                        continue;
                    }
                }
                merged.push_back(g);
                cur = gg;
            }
            groups = merged;
        }
        if (groups.size() > 1)
        {
            for (auto & g : groups)
                xy_cut(boxes, g, order);
            return;
        }
    }
    // no cut possible: top to bottom, left to right
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return boxes[a].y0 < boxes[b].y0 - 1 || (abs(boxes[a].y0 - boxes[b].y0) <= 1 && boxes[a].x0 < boxes[b].x0);
    });
    for (int i : idx)
        order.push_back(i);
}

} // namespace

PageResult analyze_page(const PageInput & in, const Options & opt)
{
    PageResult res;
    res.page = in.page;
    res.width = in.width;
    res.height = in.height;

    vector<bool> taken(in.chars.size(), false);
    vector<Table> tables;

    if (opt.detect_tagged && !in.mcid_cells.empty())
        build_tagged_tables(in, taken, tables);
    for (auto & t : tables)
    {
        // layout programs add empty rows and columns for spacing
        drop_empty_rows_and_columns(in, t);
        finalize_table(in, t, opt);
    }

    vector<Rule> used;
    if (opt.detect_ruled)
        for (auto & t : detect_ruled_tables(in, taken, used))
            tables.push_back(std::move(t));

    if (opt.detect_hrules)
    {
        // rules not belonging to any table found so far
        vector<Rule> free_rules;
        for (const auto & r : in.rules)
        {
            if (!r.horizontal)
                continue;
            bool inside = false;
            for (const auto & t : tables)
            {
                if (!t.box.valid())
                    continue;
                if (r.y0 >= t.box.y0 - 5 && r.y0 <= t.box.y1 + 5 && r.x0 >= t.box.x0 - 5 && r.x1 <= t.box.x1 + 5)
                {
                    inside = true;
                    break;
                }
            }
            if (!inside)
                free_rules.push_back(r);
        }
        for (auto & t : detect_hrule_tables(in, free_rules, taken))
            tables.push_back(std::move(t));
    }

    if (opt.detect_stream)
        for (auto & t : detect_stream_tables(in, taken))
            tables.push_back(std::move(t));

    for (auto & t : tables)
        if (t.source != "tagged")
            finalize_table(in, t, opt);

    if (opt.paragraphs)
        build_paragraphs(in, taken, opt, res.paragraphs);

    // reading order of all blocks
    {
        vector<Box> boxes;
        for (const auto & t : tables)
            boxes.push_back(t.box);
        for (const auto & p : res.paragraphs)
            boxes.push_back(p.box);
        vector<int> idx(boxes.size());
        for (size_t i = 0; i < idx.size(); ++i)
            idx[i] = (int)i;
        vector<int> order;
        xy_cut(boxes, idx, order);
        /*
         * Tagged PDF: the structure tree gives the reading order of its blocks
         * and tables; untagged ones (page headers and footers, artifacts) keep
         * their place.
         */
        {
            auto seq_of = [&](int i) {
                return i < (int)tables.size() ? tables[i].struct_seq : res.paragraphs[i - tables.size()].struct_seq;
            };
            vector<int> slots, items;
            for (size_t k = 0; k < order.size(); ++k)
                if (seq_of(order[k]) >= 0)
                {
                    slots.push_back((int)k);
                    items.push_back(order[k]);
                }
            std::stable_sort(items.begin(), items.end(), [&](int a, int b) { return seq_of(a) < seq_of(b); });
            for (size_t j = 0; j < slots.size(); ++j)
                order[slots[j]] = items[j];
        }
        for (size_t k = 0; k < order.size(); ++k)
        {
            int i = order[k];
            if (i < (int)tables.size())
                tables[i].order = (int)k;
            else
                res.paragraphs[i - tables.size()].order = (int)k;
        }
        std::stable_sort(tables.begin(), tables.end(), [](const Table & a, const Table & b) { return a.order < b.order; });
        std::stable_sort(res.paragraphs.begin(), res.paragraphs.end(),
                         [](const Paragraph & a, const Paragraph & b) { return a.order < b.order; });
    }
    for (size_t k = 0; k < tables.size(); ++k)
    {
        tables[k].id = "p" + std::to_string(in.page) + "t" + std::to_string(k);
        tables[k].page = in.page;
        tables[k].group = tables[k].id;
    }
    for (size_t k = 0; k < res.paragraphs.size(); ++k)
    {
        res.paragraphs[k].id = "p" + std::to_string(in.page) + "p" + std::to_string(k);
        res.paragraphs[k].page = in.page;
    }

    // attributes for the text layer
    res.auto_hyphen.assign(in.chars.size(), 0);
    for (const auto & c : in.chars)
        if (c.auto_hyphen && c.index >= 0 && c.index < (int)in.chars.size())
            res.auto_hyphen[c.index] = 1;
    res.char_group.assign(in.chars.size(), -1);
    for (const auto & t : tables)
        for (const auto & c : t.cells)
        {
            std::ostringstream ss;
            ss << " data-tbl=\"" << t.id << "\" data-r=\"" << c.row << "\" data-c=\"" << c.col << "\"";
            if (c.rowspan > 1)
                ss << " data-rs=\"" << c.rowspan << "\"";
            if (c.colspan > 1)
                ss << " data-cs=\"" << c.colspan << "\"";
            int g = res.group_attrs.size();
            res.group_attrs.push_back(ss.str());
            res.group_keys.push_back("t:" + t.id + ":" + std::to_string(c.row) + ":" + std::to_string(c.col));
            for (int ci : c.chars)
                res.char_group[ci] = g;
        }
    for (const auto & p : res.paragraphs)
    {
        string attrs = " data-para=\"" + p.id + "\"";
        if (p.tag != "p")
            attrs += " data-tag=\"" + p.tag + "\"";
        int g = res.group_attrs.size();
        res.group_attrs.push_back(attrs);
        res.group_keys.push_back("p:" + p.id);
        for (int ci : p.chars)
            res.char_group[ci] = g;
    }

    res.tables = std::move(tables);
    return res;
}

} // namespace semantic
} // namespace pdf2htmlEX
