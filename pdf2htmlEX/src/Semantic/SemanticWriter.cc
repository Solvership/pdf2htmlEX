/*
 * SemanticWriter.cc
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <cmath>

#include "SemanticWriter.h"
#include "util/path.h"

namespace pdf2htmlEX {
namespace semantic {

using std::string;
using std::vector;
using std::ostream;
using std::endl;

SemanticWriter::SemanticWriter(const Param & param)
    : param(param)
    , opt(Options::from_param(param))
{ }

bool SemanticWriter::want(const char * what) const
{
    return ("," + param.semantic_output + ",").find(string(",") + what + ",") != string::npos;
}

static string num(double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

static string bbox_json(const Box & b)
{
    return "[" + num(b.x0) + "," + num(b.y0) + "," + num(b.x1) + "," + num(b.y1) + "]";
}

static string cell_html(const Cell & c, bool join_br)
{
    if (!join_br)
        return html_escape(c.text);
    string out;
    for (size_t i = 0; i < c.lines.size(); ++i)
    {
        if (i)
            out += "<br>";
        out += html_escape(c.lines[i]);
    }
    return out;
}

/*
 * Rows of a table chain as a grid of HTML rows.
 */
void SemanticWriter::write_table_html(ostream & out, const vector<const Table *> & parts, bool with_ids) const
{
    const Table & first = *parts.front();
    out << "<table data-tbl=\"" << first.id << "\" data-source=\"" << first.source << "\"";
    if (with_ids && !first.continued_from.empty())
        out << " data-continued-from=\"" << first.continued_from << "\"";
    if (!with_ids && parts.size() > 1)
    {
        out << " data-parts=\"";
        for (size_t i = 0; i < parts.size(); ++i)
            out << (i ? " " : "") << parts[i]->id;
        out << "\"";
    }
    out << ">";

    int cols = 0;
    for (auto * t : parts)
        cols = std::max(cols, t->cols);

    for (size_t pi = 0; pi < parts.size(); ++pi)
    {
        const Table & t = *parts[pi];
        int skip_rows = (pi > 0 && t.repeated_header && !with_ids) ? 1 : 0;
        std::set<std::pair<int, int>> covered;
        size_t ci = 0;
        for (int r = 0; r < t.rows; ++r)
        {
            // cells sorted by (row, col)
            while (ci < t.cells.size() && t.cells[ci].row < r)
                ++ci;
            if (r < skip_rows)
                continue;
            out << "<tr>";
            int col = 0;
            size_t k = ci;
            while (col < cols)
            {
                if (k < t.cells.size() && t.cells[k].row == r && t.cells[k].col == col)
                {
                    const Cell & c = t.cells[k];
                    const char * tag = c.header ? "th" : "td";
                    out << "<" << tag;
                    if (with_ids)
                        out << " data-r=\"" << c.row << "\" data-c=\"" << c.col << "\"";
                    if (c.rowspan > 1)
                        out << " rowspan=\"" << std::min(c.rowspan, t.rows - r) << "\"";
                    if (c.colspan > 1)
                        out << " colspan=\"" << c.colspan << "\"";
                    out << ">" << cell_html(c, opt.cell_join_br) << "</" << tag << ">";
                    for (int dr = 0; dr < c.rowspan; ++dr)
                        for (int dc = 0; dc < c.colspan; ++dc)
                            covered.insert({r + dr, col + dc});
                    col += c.colspan;
                    ++k;
                    continue;
                }
                if (k < t.cells.size() && t.cells[k].row == r && t.cells[k].col < col)
                {
                    // overlapping cell (should not happen), skip it
                    ++k;
                    continue;
                }
                if (!covered.count({r, col}))
                    out << "<td></td>";
                ++col;
            }
            out << "</tr>";
        }
    }
    out << "</table>";
}

void SemanticWriter::write_inline(ostream & out, const PageResult & page) const
{
    struct Block { double y, x; int kind; size_t idx; };
    vector<Block> blocks;
    for (size_t i = 0; i < page.tables.size(); ++i)
        blocks.push_back({page.tables[i].box.y0, page.tables[i].box.x0, 0, i});
    for (size_t i = 0; i < page.paragraphs.size(); ++i)
        blocks.push_back({page.paragraphs[i].box.y0, page.paragraphs[i].box.x0, 1, i});
    std::stable_sort(blocks.begin(), blocks.end(), [](const Block & a, const Block & b) {
        return a.y < b.y - 1 || (std::abs(a.y - b.y) <= 1 && a.x < b.x);
    });

    out << "<div class=\"pdf2htmlEX-semantic\" data-page-no=\"" << std::dec << page.page << "\" hidden>";
    bool in_list = false;
    for (const auto & b : blocks)
    {
        if (b.kind == 0)
        {
            if (in_list) { out << "</ul>"; in_list = false; }
            write_table_html(out, {&page.tables[b.idx]}, true);
        }
        else
        {
            const Paragraph & p = page.paragraphs[b.idx];
            string tag = p.tag;
            if (tag == "li")
            {
                if (!in_list) { out << "<ul>"; in_list = true; }
            }
            else if (in_list)
            {
                out << "</ul>";
                in_list = false;
            }
            string cls;
            if (tag == "caption")
            {
                tag = "p";
                cls = " class=\"caption\"";
            }
            out << "<" << tag << cls << " data-para=\"" << p.id << "\">" << html_escape(p.text) << "</" << tag << ">";
        }
    }
    if (in_list)
        out << "</ul>";
    out << "</div>";
    out << std::hex;
}

void SemanticWriter::add_page(const PageResult & page)
{
    pages.push_back(page);
    pages.back().chars.clear();
    pages.back().chars.shrink_to_fit();
    pages.back().char_group.clear();
    pages.back().char_group.shrink_to_fit();
}

void SemanticWriter::link_continuations()
{
    for (size_t p = 0; p + 1 < pages.size(); ++p)
    {
        PageResult & a = pages[p];
        PageResult & b = pages[p + 1];
        if (a.tables.empty() || b.tables.empty() || b.page != a.page + 1)
            continue;
        // last table of a, first table of b
        Table * last = &a.tables.front();
        for (auto & t : a.tables)
            if (t.box.y1 > last->box.y1)
                last = &t;
        Table * first = &b.tables.front();
        for (auto & t : b.tables)
            if (t.box.y0 < first->box.y0)
                first = &t;

        bool cont = false;
        if (!last->struct_key.empty() || !first->struct_key.empty())
        {
            cont = last->struct_key == first->struct_key;
        }
        else if (last->cols == first->cols && last->source == first->source)
        {
            bool near_bottom = last->box.y1 > 0.6 * a.height;
            bool near_top = first->box.y0 < 0.4 * b.height;
            bool same_cols = true;
            if (last->col_bounds.size() == first->col_bounds.size() && !last->col_bounds.empty())
            {
                for (size_t i = 0; i < last->col_bounds.size(); ++i)
                    if (std::abs(last->col_bounds[i] - first->col_bounds[i]) > 3)
                        same_cols = false;
            }
            else if (std::abs(last->box.x0 - first->box.x0) > 3 || std::abs(last->box.x1 - first->box.x1) > 3)
            {
                same_cols = false;
            }
            cont = near_bottom && near_top && same_cols;
        }
        if (!cont)
            continue;

        first->continued_from = last->id;
        first->group = last->group;

        // find the first table of the chain to compare headers
        const Table * origin = nullptr;
        for (auto & pg : pages)
            for (auto & t : pg.tables)
                if (t.id == last->group)
                    origin = &t;
        if (origin)
        {
            vector<string> h0, h1;
            for (const auto & c : origin->cells)
                if (c.row == 0)
                    h0.push_back(c.text);
            for (const auto & c : first->cells)
                if (c.row == 0)
                    h1.push_back(c.text);
            if (!h0.empty() && h0 == h1)
                first->repeated_header = true;
        }
    }
}

void SemanticWriter::write_json(const string & path) const
{
    std::ofstream out(path, std::ofstream::binary);
    if (!out)
        throw string("Cannot open ") + path + " for writing";
    out << "{\n\"version\": 1,\n\"source\": \"" << json_escape(param.input_filename) << "\",\n\"pages\": [";
    for (size_t pi = 0; pi < pages.size(); ++pi)
    {
        const PageResult & p = pages[pi];
        out << (pi ? ",\n" : "\n") << "{\"page\": " << p.page << ", \"width\": " << num(p.width) << ", \"height\": " << num(p.height)
            << ",\n \"tables\": [";
        for (size_t ti = 0; ti < p.tables.size(); ++ti)
        {
            const Table & t = p.tables[ti];
            out << (ti ? ",\n" : "\n") << "  {\"id\": \"" << t.id << "\", \"source\": \"" << t.source << "\", \"rows\": " << t.rows
                << ", \"cols\": " << t.cols << ", \"bbox\": " << bbox_json(t.box)
                << ", \"group\": \"" << t.group << "\"";
            if (!t.continued_from.empty())
                out << ", \"continued_from\": \"" << t.continued_from << "\"";
            if (t.repeated_header)
                out << ", \"repeated_header\": true";
            out << ",\n   \"cells\": [";
            for (size_t ci = 0; ci < t.cells.size(); ++ci)
            {
                const Cell & c = t.cells[ci];
                out << (ci ? ",\n" : "\n") << "    {\"r\": " << c.row << ", \"c\": " << c.col;
                if (c.rowspan > 1)
                    out << ", \"rs\": " << c.rowspan;
                if (c.colspan > 1)
                    out << ", \"cs\": " << c.colspan;
                if (c.header)
                    out << ", \"header\": true";
                out << ", \"bbox\": " << bbox_json(c.box) << ", \"text\": \"" << json_escape(c.text) << "\", \"lines\": [";
                for (size_t li = 0; li < c.lines.size(); ++li)
                    out << (li ? ", " : "") << "\"" << json_escape(c.lines[li]) << "\"";
                out << "]}";
            }
            out << "]}";
        }
        out << "],\n \"paragraphs\": [";
        for (size_t qi = 0; qi < p.paragraphs.size(); ++qi)
        {
            const Paragraph & q = p.paragraphs[qi];
            out << (qi ? ",\n" : "\n") << "  {\"id\": \"" << q.id << "\", \"tag\": \"" << q.tag << "\", \"bbox\": " << bbox_json(q.box)
                << ", \"text\": \"" << json_escape(q.text) << "\"}";
        }
        out << "]}";
    }
    out << "\n]\n}\n";
}

void SemanticWriter::write_tables_html(const string & path) const
{
    std::ofstream out(path, std::ofstream::binary);
    if (!out)
        throw string("Cannot open ") + path + " for writing";
    out << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" << html_escape(get_filename(param.input_filename))
        << " - tables</title>\n<style>body{font-family:sans-serif;margin:2em}table{border-collapse:collapse;margin:0.5em 0 2em}"
           "td,th{border:1px solid #888;padding:3px 6px;vertical-align:top;text-align:left}th{background:#eee}"
           "h2{font-size:1em;color:#555}</style></head><body>\n";
    // chains of continued tables
    for (const auto & p : pages)
        for (const auto & t : p.tables)
        {
            if (!t.continued_from.empty())
                continue; // rendered with its chain
            vector<const Table *> parts{&t};
            for (const auto & p2 : pages)
                for (const auto & t2 : p2.tables)
                    if (&t2 != &t && t2.group == t.group)
                        parts.push_back(&t2);
            out << "<h2>" << t.id << " (page " << t.page;
            if (parts.size() > 1)
                out << "-" << parts.back()->page;
            out << ", " << t.source << ")</h2>\n";
            write_table_html(out, parts, false);
            out << "\n";
        }
    out << "</body></html>\n";
}

void SemanticWriter::finish()
{
    link_continuations();

    string base = param.output_filename;
    string suffix = get_suffix(base);
    if (suffix == ".html" || suffix == ".htm")
        base = base.substr(0, base.size() - suffix.size());

    if (want("json"))
        write_json(param.dest_dir + "/" + base + ".semantic.json");
    if (want("html"))
        write_tables_html(param.dest_dir + "/" + base + ".tables.html");
}

} // namespace semantic
} // namespace pdf2htmlEX
