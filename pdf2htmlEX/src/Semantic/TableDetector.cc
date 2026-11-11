/*
 * TableDetector.cc
 *
 * Geometric table detection:
 *  - ruled   : tables with a full grid of ruling lines ("lattice")
 *  - hrules  : tables delimited by horizontal rules only (booktabs style)
 *  - stream  : tables without rules, from text alignment (opt-in)
 *
 * The key property for the user facing result: a table row whose cells wrap
 * onto several text lines stays ONE row.
 */

#include <algorithm>
#include <cmath>
#include <numeric>
#include <map>

#include "Semantic.h"

namespace pdf2htmlEX {
namespace semantic {

using std::vector;
using std::string;
using std::min;
using std::max;
using std::abs;

namespace {

const double SNAP = 1.0;       // rules closer than this (pt) are on the same line
const double TOL = 2.5;        // intersection / coverage tolerance (pt)
const double MIN_CELL = 3.0;   // grid lines closer than this are merged (double rules)
const double SEG_GAP_EM = 0.75; // horizontal gap separating cells, in em

struct UnionFind
{
    vector<int> p;
    explicit UnionFind(int n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[b] = a; }
};

vector<Rule> merge_rules(const vector<Rule> & rules, bool horizontal)
{
    vector<Rule> rs;
    for (const auto & r : rules)
        if (r.horizontal == horizontal)
            rs.push_back(r);
    std::sort(rs.begin(), rs.end(), [](const Rule & a, const Rule & b) {
        return a.pos() < b.pos() || (a.pos() == b.pos() && a.lo() < b.lo());
    });

    vector<Rule> out;
    size_t i = 0;
    while (i < rs.size())
    {
        // cluster by position
        size_t j = i;
        double sum = 0;
        while (j < rs.size() && rs[j].pos() - rs[i].pos() <= SNAP)
        {
            sum += rs[j].pos();
            ++j;
        }
        double pos = sum / (j - i);
        vector<std::pair<double, double>> iv;
        for (size_t k = i; k < j; ++k)
            iv.emplace_back(rs[k].lo(), rs[k].hi());
        std::sort(iv.begin(), iv.end());
        double lo = iv[0].first, hi = iv[0].second;
        auto flush = [&]() {
            Rule r;
            r.horizontal = horizontal;
            if (horizontal) { r.x0 = lo; r.x1 = hi; r.y0 = r.y1 = pos; }
            else { r.y0 = lo; r.y1 = hi; r.x0 = r.x1 = pos; }
            out.push_back(r);
        };
        for (size_t k = 1; k < iv.size(); ++k)
        {
            if (iv[k].first <= hi + TOL)
                hi = max(hi, iv[k].second);
            else
            {
                flush();
                lo = iv[k].first;
                hi = iv[k].second;
            }
        }
        flush();
        i = j;
    }
    return out;
}

bool rules_intersect(const Rule & h, const Rule & v)
{
    return v.x0 >= h.x0 - TOL && v.x0 <= h.x1 + TOL
        && h.y0 >= v.y0 - TOL && h.y0 <= v.y1 + TOL;
}

// cluster sorted positions, merging values closer than `dist`
vector<double> cluster_positions(vector<double> v, double dist)
{
    std::sort(v.begin(), v.end());
    vector<double> out;
    size_t i = 0;
    while (i < v.size())
    {
        size_t j = i;
        double sum = 0;
        while (j < v.size() && v[j] - v[i] <= dist)
        {
            sum += v[j];
            ++j;
        }
        out.push_back(sum / (j - i));
        i = j;
    }
    return out;
}

int locate(const vector<double> & bounds, double v)
{
    // index i such that bounds[i] <= v < bounds[i+1], or -1
    if (bounds.size() < 2 || v < bounds.front() || v > bounds.back())
        return -1;
    auto it = std::upper_bound(bounds.begin(), bounds.end(), v);
    int i = (int)(it - bounds.begin()) - 1;
    if (i >= (int)bounds.size() - 1)
        i = (int)bounds.size() - 2;
    return i;
}

bool usable_char(const Char & c)
{
    return c.box.valid() && !c.text.empty() && !c.is_space();
}

double median(vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/*
 * A piece of text belonging to one logical cell candidate.
 */
struct LSeg
{
    Box box;
    vector<int> chars;
    int col = 0;
    int colspan = 1;
    double em = 10;
    int row = 0; // output
};

/*
 * Split a band (a group of segments that belong to one ruled row, or to the
 * area between two horizontal rules) into logical rows.
 *
 * Row starts are the text lines of the anchor column (the leftmost column
 * with text). A line of the anchor column that is not accompanied by text in
 * any other column is a wrapped continuation, not a new row.
 *
 * Returns the number of rows; sets LSeg::row.
 */
int split_band_rows(vector<LSeg> & segs)
{
    if (segs.empty())
        return 0;
    int anchor = segs[0].col;
    for (const auto & s : segs)
        anchor = min(anchor, s.col);

    vector<const LSeg *> anchor_segs;
    for (const auto & s : segs)
        if (s.col == anchor)
            anchor_segs.push_back(&s);
    std::sort(anchor_segs.begin(), anchor_segs.end(), [](const LSeg * a, const LSeg * b) {
        return a->box.y0 < b->box.y0;
    });

    vector<double> starts;
    for (size_t k = 0; k < anchor_segs.size(); ++k)
    {
        const LSeg * a = anchor_segs[k];
        if (!starts.empty() && a->box.y0 - starts.back() < 0.3 * a->em)
            continue; // same line
        if (k > 0)
        {
            bool accompanied = false;
            for (const auto & s : segs)
            {
                if (s.col == anchor)
                    continue;
                if (abs(s.box.y0 - a->box.y0) <= 0.5 * a->em || abs(s.box.y1 - a->box.y1) <= 0.5 * a->em)
                {
                    accompanied = true;
                    break;
                }
            }
            if (!accompanied)
                continue;
        }
        starts.push_back(a->box.y0);
    }

    if (starts.size() <= 1)
    {
        for (auto & s : segs)
            s.row = 0;
        return 1;
    }

    for (auto & s : segs)
    {
        int r = 0;
        for (size_t k = 1; k < starts.size(); ++k)
            if (s.box.y0 + 0.3 * s.em >= starts[k])
                r = (int)k;
        s.row = r;
    }
    return (int)starts.size();
}

/*
 * Convert the segments of a split band into cells. Segments with the same
 * (row, col) are merged.
 */
void band_to_cells(const vector<LSeg> & segs, int row_offset, const vector<double> & col_bounds,
                   double band_y0, double band_y1, int nrows, vector<Cell> & cells)
{
    // row boundaries: from the segments
    vector<double> row_top(nrows, band_y1), row_bottom(nrows, band_y0);
    for (const auto & s : segs)
    {
        row_top[s.row] = min(row_top[s.row], s.box.y0);
        row_bottom[s.row] = max(row_bottom[s.row], s.box.y1);
    }
    vector<double> ybounds(nrows + 1);
    ybounds[0] = band_y0;
    ybounds[nrows] = band_y1;
    for (int r = 1; r < nrows; ++r)
        ybounds[r] = (row_bottom[r - 1] + row_top[r]) / 2;

    std::map<std::pair<int, int>, size_t> idx;
    for (const auto & s : segs)
    {
        auto key = std::make_pair(s.row, s.col);
        auto it = idx.find(key);
        if (it == idx.end())
        {
            Cell c;
            c.row = row_offset + s.row;
            c.col = s.col;
            c.colspan = s.colspan;
            c.box = Box(col_bounds[s.col], ybounds[s.row], col_bounds[s.col + s.colspan], ybounds[s.row + 1]);
            idx[key] = cells.size();
            cells.push_back(c);
            it = idx.find(key);
        }
        Cell & c = cells[it->second];
        if (s.colspan > c.colspan)
        {
            c.colspan = s.colspan;
            c.box.x1 = col_bounds[s.col + s.colspan];
        }
        c.chars.insert(c.chars.end(), s.chars.begin(), s.chars.end());
    }
}

/*
 * Find columns of a text region from whitespace gaps.
 * lines: visual lines of the region. Returns column boundaries (size ncols+1)
 * or empty on failure. Also rejects prose.
 */
vector<double> find_columns(const PageInput & in, const vector<VLine> & lines, const Box & region,
                            vector<vector<Segment>> & line_segs)
{
    line_segs.clear();
    int multi = 0;
    for (const auto & l : lines)
    {
        line_segs.push_back(split_segments(in.chars, l, SEG_GAP_EM));
        if (line_segs.back().size() >= 2)
            ++multi;
    }
    if (multi < 2)
        return {};

    // coverage of the segments of multi-segment lines
    vector<std::pair<double, double>> iv;
    double em_sum = 0;
    int em_cnt = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (line_segs[i].size() < 2)
            continue;
        for (const auto & s : line_segs[i])
            iv.emplace_back(s.box.x0, s.box.x1);
        em_sum += lines[i].em;
        ++em_cnt;
    }
    double em = em_cnt ? max(em_sum / em_cnt, 1.0) : 10;
    std::sort(iv.begin(), iv.end());
    vector<std::pair<double, double>> cover;
    for (const auto & p : iv)
    {
        if (!cover.empty() && p.first <= cover.back().second + 0.4 * em)
            cover.back().second = max(cover.back().second, p.second);
        else
            cover.push_back(p);
    }
    if (cover.size() < 2)
        return {};

    // prose rejection
    double width = cover.back().second - cover.front().first;
    int long_single = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (line_segs[i].size() == 1 && line_segs[i][0].box.w() > 0.6 * width)
            ++long_single;
    if (long_single > 2)
        return {};

    // words per segment by column: a column of sentences is prose
    for (size_t c = 0; c < cover.size(); ++c)
    {
        int segs = 0, wordy = 0;
        for (size_t i = 0; i < lines.size(); ++i)
            for (const auto & s : line_segs[i])
            {
                if (s.box.cx() < cover[c].first || s.box.cx() > cover[c].second)
                    continue;
                ++segs;
                string t = chars_text(in.chars, s.chars);
                int words = 1 + (int)std::count(t.begin(), t.end(), ' ');
                if (words >= 7)
                    ++wordy;
            }
        if (segs >= 3 && wordy >= 0.7 * segs)
            return {};
    }
    if (cover.size() == 2)
    {
        // two wide text columns: a two column page layout, not a table
        double w0 = cover[0].second - cover[0].first, w1 = cover[1].second - cover[1].first;
        if (w0 > 0.4 * width && w1 > 0.4 * width)
            return {};
    }

    vector<double> bounds;
    bounds.push_back(min(region.x0, cover.front().first));
    for (size_t c = 1; c < cover.size(); ++c)
        bounds.push_back((cover[c - 1].second + cover[c].first) / 2);
    bounds.push_back(max(region.x1, cover.back().second));
    return bounds;
}

// turn the segments of a text region into LSegs assigned to columns
vector<LSeg> make_lsegs(const PageInput & in, const vector<vector<Segment>> & line_segs, const vector<double> & bounds)
{
    vector<LSeg> out;
    int ncols = (int)bounds.size() - 1;
    for (const auto & segs : line_segs)
        for (const auto & s : segs)
        {
            if (s.chars.empty())
                continue;
            LSeg ls;
            ls.box = s.box;
            ls.chars = s.chars;
            double em_sum = 0;
            for (int i : s.chars)
                em_sum += in.chars[i].em;
            ls.em = max(em_sum / s.chars.size(), 1.0);
            int c0 = -1, c1 = -1;
            for (int c = 0; c < ncols; ++c)
            {
                double ov = min(s.box.x1, bounds[c + 1]) - max(s.box.x0, bounds[c]);
                if (ov > 0.3 * ls.em || (s.box.w() < 0.6 * ls.em && ov > 0))
                {
                    if (c0 < 0)
                        c0 = c;
                    c1 = c;
                }
            }
            if (c0 < 0)
            {
                c0 = c1 = max(0, min(ncols - 1, locate(bounds, s.box.cx())));
            }
            ls.col = c0;
            ls.colspan = c1 - c0 + 1;
            out.push_back(ls);
        }
    return out;
}

} // namespace

/*
 * Ruled (lattice) tables
 */
vector<Table> detect_ruled_tables(const PageInput & in, vector<bool> & taken, vector<Rule> & used_rules_out)
{
    vector<Table> tables;
    vector<Rule> H, V;
    for (const auto & r : merge_rules(in.rules, true))
        if (r.x1 - r.x0 >= 4)
            H.push_back(r);
    for (const auto & r : merge_rules(in.rules, false))
        if (r.y1 - r.y0 >= 4)
            V.push_back(r);

    int nh = H.size(), nv = V.size();
    if (nh < 2 || nv < 2 || (double)nh * nv > 4e6)
        return tables;

    UnionFind uf(nh + nv);
    for (int i = 0; i < nh; ++i)
        for (int j = 0; j < nv; ++j)
            if (rules_intersect(H[i], V[j]))
                uf.unite(i, nh + j);

    std::map<int, std::pair<vector<int>, vector<int>>> comps;
    for (int i = 0; i < nh; ++i)
        comps[uf.find(i)].first.push_back(i);
    for (int j = 0; j < nv; ++j)
        comps[uf.find(nh + j)].second.push_back(j);

    for (auto & kv : comps)
    {
        const auto & hs = kv.second.first;
        const auto & vs = kv.second.second;
        if (hs.size() < 2 || vs.size() < 2)
            continue;

        vector<double> xr, yr;
        double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
        for (int i : hs)
        {
            yr.push_back(H[i].y0);
            minx = min(minx, H[i].x0);
            maxx = max(maxx, H[i].x1);
        }
        for (int j : vs)
        {
            xr.push_back(V[j].x0);
            miny = min(miny, V[j].y0);
            maxy = max(maxy, V[j].y1);
        }
        xr.push_back(minx);
        xr.push_back(maxx);
        yr.push_back(miny);
        yr.push_back(maxy);
        vector<double> xs = cluster_positions(xr, MIN_CELL);
        vector<double> ys = cluster_positions(yr, MIN_CELL);
        int nc = (int)xs.size() - 1, nr = (int)ys.size() - 1;
        if (nc < 2 || nr < 1)
            continue;

        // page frame / background guard
        if ((xs.back() - xs.front()) * (ys.back() - ys.front()) > 0.95 * in.width * in.height && nc * nr <= 4)
            continue;

        auto has_v = [&](double x, double ymid) {
            for (int j : vs)
                if (abs(V[j].x0 - x) <= TOL && ymid >= V[j].y0 - TOL && ymid <= V[j].y1 + TOL)
                    return true;
            return false;
        };
        auto has_h = [&](double y, double xmid) {
            for (int i : hs)
                if (abs(H[i].y0 - y) <= TOL && xmid >= H[i].x0 - TOL && xmid <= H[i].x1 + TOL)
                    return true;
            return false;
        };

        UnionFind cells_uf(nr * nc);
        for (int r = 0; r < nr; ++r)
        {
            double ymid = (ys[r] + ys[r + 1]) / 2;
            for (int c = 1; c < nc; ++c)
                if (!has_v(xs[c], ymid))
                    cells_uf.unite(r * nc + c - 1, r * nc + c);
        }
        for (int c = 0; c < nc; ++c)
        {
            double xmid = (xs[c] + xs[c + 1]) / 2;
            for (int r = 1; r < nr; ++r)
                if (!has_h(ys[r], xmid))
                    cells_uf.unite((r - 1) * nc + c, r * nc + c);
        }

        // groups -> rectangular cells
        std::map<int, vector<int>> groups;
        for (int k = 0; k < nr * nc; ++k)
            groups[cells_uf.find(k)].push_back(k);
        vector<int> base_to_cell(nr * nc, -1);
        vector<Cell> cells;
        auto add_cell = [&](int r0, int r1, int c0, int c1, const vector<int> & members) {
            Cell cell;
            cell.row = r0;
            cell.col = c0;
            cell.rowspan = r1 - r0 + 1;
            cell.colspan = c1 - c0 + 1;
            cell.box = Box(xs[c0], ys[r0], xs[c1 + 1], ys[r1 + 1]);
            for (int k : members)
                base_to_cell[k] = cells.size();
            cells.push_back(cell);
        };
        for (auto & g : groups)
        {
            int r0 = nr, r1 = -1, c0 = nc, c1 = -1;
            for (int k : g.second)
            {
                r0 = min(r0, k / nc); r1 = max(r1, k / nc);
                c0 = min(c0, k % nc); c1 = max(c1, k % nc);
            }
            if ((int)g.second.size() == (r1 - r0 + 1) * (c1 - c0 + 1))
                add_cell(r0, r1, c0, c1, g.second);
            else
                for (int k : g.second)
                    add_cell(k / nc, k / nc, k % nc, k % nc, {k});
        }

        // assign chars
        Box tbox(xs.front(), ys.front(), xs.back(), ys.back());
        vector<int> assigned;
        for (const auto & ch : in.chars)
        {
            // spaces are kept too: they help the text reconstruction
            if (!ch.box.valid() || ch.text.empty() || taken[ch.index])
                continue;
            int c = locate(xs, ch.box.cx());
            int r = locate(ys, ch.box.cy());
            if (c < 0 || r < 0)
                continue;
            cells[base_to_cell[r * nc + c]].chars.push_back(ch.index);
            assigned.push_back(ch.index);
        }

        int non_empty = 0;
        for (const auto & c : cells)
            for (int ci : c.chars)
                if (!in.chars[ci].is_space())
                {
                    ++non_empty;
                    break;
                }
        if (non_empty < 2 || cells.size() < 2)
            continue;

        /*
         * Unruled rows inside a ruled frame (e.g. only the header is separated):
         * split big grid rows into logical rows. Only for tables with very few
         * ruled rows, so that properly ruled rows with wrapped text are never split.
         */
        vector<int> row_map(nr + 1, 0); // grid row -> first logical row
        vector<int> row_count(nr, 1);
        vector<Cell> final_cells;
        if (nr <= 3)
        {
            vector<vector<Cell>> split_rows_cells(nr);
            for (int r = 0; r < nr; ++r)
            {
                bool splittable = true;
                vector<const Cell *> rc;
                for (const auto & c : cells)
                {
                    if (c.row <= r && c.row + c.rowspan > r)
                    {
                        if (c.rowspan != 1)
                            splittable = false;
                        rc.push_back(&c);
                    }
                }
                if (!splittable)
                    continue;
                vector<LSeg> segs;
                for (const Cell * c : rc)
                {
                    auto vls = build_visual_lines(in.chars, c->chars);
                    for (const auto & vl : vls)
                    {
                        LSeg s;
                        s.box = vl.box;
                        s.chars = vl.chars;
                        s.col = c->col;
                        s.colspan = c->colspan;
                        s.em = max(vl.em, 1.0);
                        segs.push_back(s);
                    }
                }
                int n = split_band_rows(segs);
                if (n <= 1)
                    continue;
                row_count[r] = n;
                band_to_cells(segs, 0, xs, ys[r], ys[r + 1], n, split_rows_cells[r]);
            }
            for (int r = 0; r < nr; ++r)
                row_map[r + 1] = row_map[r] + row_count[r];
            for (const auto & c : cells)
            {
                int r = c.row;
                if (c.rowspan == 1 && row_count[r] > 1)
                    continue; // replaced by split cells
                Cell nc2 = c;
                nc2.row = row_map[r];
                nc2.rowspan = row_map[r + c.rowspan] - row_map[r];
                final_cells.push_back(nc2);
            }
            for (int r = 0; r < nr; ++r)
                if (row_count[r] > 1)
                    for (auto c : split_rows_cells[r])
                    {
                        c.row += row_map[r];
                        final_cells.push_back(c);
                    }
        }
        else
        {
            for (int r = 0; r < nr; ++r)
                row_map[r + 1] = row_map[r] + 1;
            final_cells = cells;
        }

        Table t;
        t.source = "ruled";
        t.rows = row_map[nr];
        t.cols = nc;
        t.box = tbox;
        t.cells = std::move(final_cells);
        t.col_bounds = xs;
        tables.push_back(std::move(t));

        for (int i : assigned)
            taken[i] = true;
        for (int i : hs)
            used_rules_out.push_back(H[i]);
        for (int j : vs)
            used_rules_out.push_back(V[j]);
    }
    return tables;
}

/*
 * Tables delimited by horizontal rules only.
 */
vector<Table> detect_hrule_tables(const PageInput & in, const vector<Rule> & free_rules, vector<bool> & taken)
{
    vector<Table> tables;
    vector<Rule> H;
    for (const auto & r : merge_rules(free_rules, true))
        if (r.x1 - r.x0 >= 30)
            H.push_back(r);
    if (H.size() < 2)
        return tables;

    // group rules with the same horizontal extent
    vector<bool> grouped(H.size(), false);
    vector<vector<int>> groups;
    for (size_t i = 0; i < H.size(); ++i)
    {
        if (grouped[i])
            continue;
        vector<int> g{(int)i};
        grouped[i] = true;
        for (size_t j = i + 1; j < H.size(); ++j)
            if (!grouped[j] && abs(H[j].x0 - H[i].x0) <= 4 && abs(H[j].x1 - H[i].x1) <= 4)
            {
                g.push_back(j);
                grouped[j] = true;
            }
        if (g.size() >= 2)
        {
            std::sort(g.begin(), g.end(), [&](int a, int b) { return H[a].y0 < H[b].y0; });
            groups.push_back(g);
        }
    }

    auto chars_in = [&](const Box & b) {
        vector<int> v;
        for (const auto & ch : in.chars)
            if (usable_char(ch) && !taken[ch.index] && b.contains(ch.box.cx(), ch.box.cy()))
                v.push_back(ch.index);
        return v;
    };

    for (const auto & g : groups)
    {
        size_t i = 0;
        while (i + 1 < g.size())
        {
            // extend the region band by band while the bands look tabular
            size_t j = i + 1;
            size_t best_j = 0;
            vector<double> best_bounds;
            vector<vector<Segment>> best_segs;
            vector<VLine> best_lines;
            while (j < g.size())
            {
                Box region(H[g[i]].x0 - 2, H[g[i]].y0, H[g[i]].x1 + 2, H[g[j]].y0);
                auto ids = chars_in(region);
                if (ids.empty())
                {
                    ++j;
                    continue;
                }
                auto lines = build_visual_lines(in.chars, ids);
                vector<vector<Segment>> segs;
                auto bounds = find_columns(in, lines, region, segs);
                if (bounds.empty())
                {
                    // a single band (e.g. only the header) may be too small to find columns
                    if (best_j == 0 && lines.size() <= 2)
                    {
                        ++j;
                        continue;
                    }
                    break;
                }
                best_j = j;
                best_bounds = bounds;
                best_segs = segs;
                best_lines = lines;
                ++j;
            }
            if (best_j == 0)
            {
                ++i;
                continue;
            }

            // rows: each band between consecutive rules is split by the anchor rule
            vector<double> rule_ys;
            for (size_t k = i; k <= best_j; ++k)
                rule_ys.push_back(H[g[k]].y0);
            auto lsegs = make_lsegs(in, best_segs, best_bounds);

            Table t;
            t.source = "hrules";
            t.cols = (int)best_bounds.size() - 1;
            t.box = Box(best_bounds.front(), rule_ys.front(), best_bounds.back(), rule_ys.back());
            t.col_bounds = best_bounds;
            int row = 0;
            for (size_t b = 0; b + 1 < rule_ys.size(); ++b)
            {
                vector<LSeg> band;
                for (const auto & s : lsegs)
                    if (s.box.cy() >= rule_ys[b] && s.box.cy() < rule_ys[b + 1])
                        band.push_back(s);
                if (band.empty())
                    continue;
                int n = split_band_rows(band);
                size_t first = t.cells.size();
                band_to_cells(band, row, best_bounds, rule_ys[b], rule_ys[b + 1], n, t.cells);
                if (b == 0 && rule_ys.size() > 2 && n == 1)
                    for (size_t k = first; k < t.cells.size(); ++k)
                        t.cells[k].header = true;
                row += n;
            }
            t.rows = row;
            if (t.rows >= 2 || (t.rows == 1 && t.cols >= 2 && t.cells.size() >= 2))
            {
                for (const auto & c : t.cells)
                    for (int ci : c.chars)
                        taken[ci] = true;
                tables.push_back(std::move(t));
            }
            i = best_j;
        }
    }
    return tables;
}

/*
 * Tables without any rules (opt-in, heuristic)
 */
vector<Table> detect_stream_tables(const PageInput & in, vector<bool> & taken)
{
    vector<Table> tables;
    vector<int> ids;
    for (const auto & ch : in.chars)
        if (usable_char(ch) && !taken[ch.index])
            ids.push_back(ch.index);
    auto lines = build_visual_lines(in.chars, ids);
    if (lines.size() < 3)
        return tables;

    // blocks of lines without big vertical gaps
    vector<vector<int>> blocks;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        double em = max(lines[i].em, 1.0);
        if (blocks.empty() || lines[i].box.y0 - lines[blocks.back().back()].box.y1 > 2.0 * em)
            blocks.push_back({});
        blocks.back().push_back(i);
    }

    for (auto & b : blocks)
    {
        // trim single segment lines at both ends
        vector<int> segcount;
        for (int li : b)
            segcount.push_back(split_segments(in.chars, lines[li], SEG_GAP_EM).size());
        int s = 0, e = (int)b.size() - 1;
        while (s <= e && segcount[s] < 2) ++s;
        while (e >= s && segcount[e] < 2) --e;
        if (e - s + 1 < 3)
            continue;
        int multi = 0;
        for (int k = s; k <= e; ++k)
            if (segcount[k] >= 2)
                ++multi;
        if (multi < 3)
            continue;

        vector<VLine> blines(lines.begin() + b[s], lines.begin() + b[e] + 1);
        Box region;
        for (const auto & l : blines)
            region.expand(l.box);
        vector<vector<Segment>> segs;
        auto bounds = find_columns(in, blines, region, segs);
        if (bounds.empty())
            continue;
        auto lsegs = make_lsegs(in, segs, bounds);
        int n = split_band_rows(lsegs);
        if (n < 2)
            continue;
        Table t;
        t.source = "stream";
        t.cols = (int)bounds.size() - 1;
        t.box = Box(bounds.front(), region.y0, bounds.back(), region.y1);
        t.col_bounds = bounds;
        band_to_cells(lsegs, 0, bounds, region.y0, region.y1, n, t.cells);
        t.rows = n;
        for (const auto & c : t.cells)
            for (int ci : c.chars)
                taken[ci] = true;
        tables.push_back(std::move(t));
    }
    return tables;
}

void finalize_table(const PageInput & in, Table & t, const Options & opt)
{
    Box tb = t.box;
    for (auto & c : t.cells)
    {
        auto vls = build_visual_lines(in.chars, c.chars);
        c.lines.clear();
        Box cb;
        for (const auto & vl : vls)
        {
            string s = chars_text(in.chars, vl.chars);
            if (!s.empty())
                c.lines.push_back(s);
            cb.expand(vl.box);
        }
        c.text = join_lines(c.lines, opt.dehyphenate);
        if (!c.box.valid())
            c.box = cb;
        tb.expand(c.box);
    }
    t.box = tb;
    std::sort(t.cells.begin(), t.cells.end(), [](const Cell & a, const Cell & b) {
        return a.row < b.row || (a.row == b.row && a.col < b.col);
    });
}

} // namespace semantic
} // namespace pdf2htmlEX
