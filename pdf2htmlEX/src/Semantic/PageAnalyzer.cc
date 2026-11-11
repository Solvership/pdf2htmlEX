/*
 * PageAnalyzer.cc
 *
 * Runs the semantic analysis of one page: tables (tagged, ruled, hrules,
 * stream) first, then paragraphs from the remaining text.
 */

#include <algorithm>
#include <cmath>
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
                key2p[it->second.key] = out.size();
                out.push_back(p);
                pi = key2p.find(it->second.key);
            }
            out[pi->second].chars.push_back(ch.index);
            used[ch.index] = true;
        }
        for (auto & p : out)
        {
            auto vls = build_visual_lines(in.chars, p.chars);
            for (const auto & vl : vls)
            {
                string s = chars_text(in.chars, vl.chars);
                if (!s.empty())
                    p.lines.push_back(s);
                p.box.expand(vl.box);
            }
            p.text = join_lines(p.lines, opt.dehyphenate);
        }
    }

    vector<int> ids;
    for (const auto & ch : in.chars)
        if (!taken[ch.index] && !used[ch.index] && ch.box.valid() && !ch.text.empty())
            ids.push_back(ch.index);
    auto lines = build_visual_lines(in.chars, ids);

    vector<Segment> segs;
    for (size_t li = 0; li < lines.size(); ++li)
        for (auto & s : split_segments(in.chars, lines[li], 1.5))
        {
            if (s.chars.empty())
                continue;
            s.vline = (int)li;
            segs.push_back(s);
        }
    std::stable_sort(segs.begin(), segs.end(), [](const Segment & a, const Segment & b) {
        return a.box.y0 < b.box.y0 - 0.5 || (abs(a.box.y0 - b.box.y0) <= 0.5 && a.box.x0 < b.box.x0);
    });

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
        if (!para.text.empty())
            out.push_back(para);
    }
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
        finalize_table(in, t, opt);

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

    std::stable_sort(tables.begin(), tables.end(), [](const Table & a, const Table & b) {
        return a.box.y0 < b.box.y0 - 1 || (abs(a.box.y0 - b.box.y0) <= 1 && a.box.x0 < b.box.x0);
    });
    for (size_t k = 0; k < tables.size(); ++k)
    {
        tables[k].id = "p" + std::to_string(in.page) + "t" + std::to_string(k);
        tables[k].page = in.page;
        tables[k].group = tables[k].id;
    }

    if (opt.paragraphs)
        build_paragraphs(in, taken, opt, res.paragraphs);
    std::stable_sort(res.paragraphs.begin(), res.paragraphs.end(), [](const Paragraph & a, const Paragraph & b) {
        return a.box.y0 < b.box.y0 - 1 || (abs(a.box.y0 - b.box.y0) <= 1 && a.box.x0 < b.box.x0);
    });
    for (size_t k = 0; k < res.paragraphs.size(); ++k)
    {
        res.paragraphs[k].id = "p" + std::to_string(in.page) + "p" + std::to_string(k);
        res.paragraphs[k].page = in.page;
    }

    // attributes for the text layer
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
