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
#include <cstring>
#include <numeric>
#include <map>
#include <set>

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
const double SEG_GAP_EM = 0.55; // horizontal gap separating cells, in em (word gaps are ~0.35)

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
        // the style of the merged rule: the one covering most of its length
        double len_kind[2] = {0, 0};
        for (size_t k = i; k < j; ++k)
            len_kind[rs[k].kind ? 1 : 0] += rs[k].hi() - rs[k].lo();
        int kind = len_kind[1] > len_kind[0] ? 1 : 0;
        auto flush = [&]() {
            Rule r;
            r.horizontal = horizontal;
            r.kind = kind;
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
    int lead = 0; // first letter: +1 upper case, -1 lower case, 0 no letter
    bool numeric = false; // a number (or a dash standing for nothing)
    int row = 0; // output
};

// letter case of a code point: +1 upper, -1 lower, 0 not a (cased) letter
int letter_case(unsigned u)
{
    if (u >= 'A' && u <= 'Z') return 1;
    if (u >= 'a' && u <= 'z') return -1;
    if (u >= 0xC0 && u <= 0xDE && u != 0xD7) return 1;
    if (u >= 0xDF && u <= 0xFF && u != 0xF7) return -1;
    if ((u >= 0x100 && u <= 0x137) || (u >= 0x14A && u <= 0x177)) return (u % 2 == 0) ? 1 : -1;
    if ((u >= 0x139 && u <= 0x148) || (u >= 0x179 && u <= 0x17E)) return (u % 2 == 1) ? 1 : -1;
    if (u >= 0x391 && u <= 0x3A9) return 1;
    if (u >= 0x3B1 && u <= 0x3C9) return -1;
    if (u >= 0x400 && u <= 0x42F) return 1;
    if (u >= 0x430 && u <= 0x45F) return -1;
    return 0;
}

// a number ("1.234", "(56)", "12%", "-"): numbers never wrap inside a cell
bool is_numeric_text(const string & t)
{
    if (t.empty())
        return false;
    if (t == "-" || t == "\xe2\x80\x93" || t == "\xe2\x80\x94")
        return true;
    bool digit = false;
    for (unsigned char c : t)
    {
        if (c >= '0' && c <= '9')
            digit = true;
        else if (!strchr(".,()%-+/ ", c))
            return false;
    }
    return digit;
}

// letter case of the first non blank char of a sequence
int lead_case(const PageInput & in, const vector<int> & chars)
{
    for (int i : chars)
    {
        const string & t = in.chars[i].text;
        if (t.empty() || in.chars[i].is_space())
            continue;
        unsigned char c0 = t[0];
        unsigned u = c0;
        if (c0 >= 0xC0 && t.size() >= 2)
        {
            if (c0 < 0xE0)
                u = ((c0 & 0x1F) << 6) | (t[1] & 0x3F);
            else if (t.size() >= 3)
                u = ((c0 & 0x0F) << 12) | ((t[1] & 0x3F) << 6) | (t[2] & 0x3F);
        }
        return letter_case(u);
    }
    return 0;
}

/*
 * Split a band (a group of segments that belong to one ruled row, or to the
 * area between two horizontal rules) into logical rows.
 *
 * Row starts are the text lines of the anchor column (the leftmost column
 * with text). A line of the anchor column that is not accompanied by text in
 * any other column is a wrapped continuation, not a new row - unless it starts
 * with an upper case letter: then it is a row of its own (a section heading
 * like "Current assets") or the first line of a label whose values are on its
 * last line; a following accompanied line that starts in lower case completes
 * it.
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
    bool open_label = false; // the last row so far has a label only
    for (size_t k = 0; k < anchor_segs.size(); ++k)
    {
        const LSeg * a = anchor_segs[k];
        if (!starts.empty() && a->box.y0 - starts.back() < 0.3 * a->em)
            continue; // same line
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
        if (k > 0)
        {
            if (accompanied)
            {
                if (open_label && a->lead < 0)
                {
                    open_label = false; // completes the label above
                    continue;
                }
            }
            else if (a->lead <= 0)
                continue; // wrapped continuation
        }
        open_label = !accompanied;
        starts.push_back(a->box.y0);
    }

    /*
     * Numbers do not wrap: a number below another one in the same column, not
     * on a line of the anchor column, starts a row (e.g. a row without label).
     */
    if (!starts.empty())
    {
        vector<double> extra;
        for (const auto & s : segs)
        {
            if (!s.numeric || s.col == anchor)
                continue;
            int r = 0;
            for (size_t k = 1; k < starts.size(); ++k)
                if (s.box.y0 + 0.3 * s.em >= starts[k])
                    r = (int)k;
            for (const auto & o : segs)
            {
                if (&o == &s || !o.numeric || o.col != s.col || o.box.y1 > s.box.y0 - 0.2 * s.em)
                    continue;
                int ro = 0;
                for (size_t k = 1; k < starts.size(); ++k)
                    if (o.box.y0 + 0.3 * o.em >= starts[k])
                        ro = (int)k;
                if (ro == r && s.box.y0 > starts[r] + 0.3 * s.em)
                {
                    extra.push_back(s.box.y0);
                    break;
                }
            }
        }
        for (double y : extra)
        {
            bool dup = false;
            for (double st : starts)
                if (abs(st - y) < 0.3 * segs[0].em)
                    dup = true;
            if (!dup)
                starts.push_back(y);
        }
        std::sort(starts.begin(), starts.end());
    }

    /*
     * Text above the first row of a band with several rows (e.g. a unit line
     * "(in thousands)" under the column headings) is a row of its own. With a
     * single row, only above numbers (a multi-line text cell may be
     * vertically centered on its label).
     */
    bool first_row_numeric = false;
    for (const auto & s : segs)
        if (s.col != anchor && s.numeric && !starts.empty() && abs(s.box.y0 - starts[0]) <= 0.5 * s.em)
            first_row_numeric = true;
    if (starts.size() >= 2 || (starts.size() == 1 && first_row_numeric))
    {
        double above = 1e30;
        for (const auto & s : segs)
            if (s.box.y1 <= starts[0] - 0.2 * s.em)
                above = min(above, s.box.y0);
        if (above < starts[0])
            starts.insert(starts.begin(), above);
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
                            vector<vector<Segment>> & line_segs, const vector<double> & bands = {})
{
    line_segs.clear();
    for (const auto & l : lines)
        line_segs.push_back(split_segments(in.chars, l, SEG_GAP_EM));

    /*
     * Units whose segments are side by side: text lines, or - between
     * horizontal rules - whole bands (each band is one table row, its cells
     * may be vertically centered on different text lines).
     */
    vector<vector<const Segment *>> units;
    vector<double> unit_em;
    if (bands.size() >= 2)
    {
        units.resize(bands.size() - 1);
        unit_em.assign(bands.size() - 1, 0);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            int b = locate(bands, lines[i].box.cy());
            if (b < 0)
                continue;
            for (const auto & sg : line_segs[i])
                units[b].push_back(&sg);
            unit_em[b] = max(unit_em[b], lines[i].em);
        }
    }
    else
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            units.emplace_back();
            for (const auto & sg : line_segs[i])
                units.back().push_back(&sg);
            unit_em.push_back(lines[i].em);
        }
    }

    // a unit is multi-column if its segments do not overlap horizontally
    auto multi_col = [](const vector<const Segment *> & u) {
        if (u.size() < 2)
            return false;
        vector<std::pair<double, double>> v;
        for (auto * sg : u)
            v.emplace_back(sg->box.x0, sg->box.x1);
        std::sort(v.begin(), v.end());
        for (size_t k = 1; k < v.size(); ++k)
            if (v[k].first > v[k - 1].second)
                return true;
        return false;
    };

    int multi = 0;
    for (const auto & u : units)
        if (multi_col(u))
            ++multi;
    if (multi < 2)
        return {};

    // coverage of the segments of multi-column units
    vector<std::pair<double, double>> iv;
    double em_sum = 0;
    int em_cnt = 0;
    for (size_t i = 0; i < units.size(); ++i)
    {
        if (!multi_col(units[i]))
            continue;
        for (auto * sg : units[i])
            iv.emplace_back(sg->box.x0, sg->box.x1);
        em_sum += unit_em[i];
        ++em_cnt;
    }
    /*
     * Between rules, a band also holds lines spanning several columns (a unit
     * line "(in thousands)" over two number columns) that would merge the
     * columns: when there are enough lines with several cells side by side,
     * the columns come from those lines only.
     */
    if (bands.size() >= 2)
    {
        vector<std::pair<double, double>> iv_lines;
        int multi_lines = 0;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            vector<const Segment *> u;
            for (const auto & sg : line_segs[i])
                u.push_back(&sg);
            if (!multi_col(u))
                continue;
            ++multi_lines;
            for (auto * sg : u)
                iv_lines.emplace_back(sg->box.x0, sg->box.x1);
        }
        if (multi_lines >= 2)
            iv = iv_lines;
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
            ls.lead = lead_case(in, s.chars);
            ls.numeric = is_numeric_text(chars_text(in.chars, s.chars));
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

/*
 * Columns in which no cell starts (their text went to a spanning neighbour)
 * are removed: they come from text running across a column boundary.
 */
void drop_empty_columns(const PageInput & in, Table & t)
{
    for (int c = t.cols - 1; c >= 0 && t.cols > 1; --c)
    {
        bool starts = false;
        for (const auto & cell : t.cells)
            if (cell.col == c)
            {
                for (int ci : cell.chars)
                    if (!in.chars[ci].is_space())
                    {
                        starts = true;
                        break;
                    }
                if (starts)
                    break;
            }
        if (starts)
            continue;
        vector<Cell> kept;
        for (auto cell : t.cells)
        {
            if (cell.col == c && cell.colspan == 1)
                continue; // an empty cell of the column
            if (cell.col == c)
            {
                cell.colspan -= 1; // started here (empty), now starts at the next column
            }
            else if (cell.col < c && cell.col + cell.colspan > c)
                cell.colspan -= 1;
            else if (cell.col > c)
                cell.col -= 1;
            kept.push_back(cell);
        }
        t.cells = std::move(kept);
        if (!t.col_bounds.empty() && c + 1 < (int)t.col_bounds.size())
            t.col_bounds.erase(t.col_bounds.begin() + c + 1 - (c + 1 == (int)t.col_bounds.size() - 1 ? 1 : 0));
        t.cols -= 1;
    }
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
        vector<int> hs = kv.second.first;
        vector<int> vs = kv.second.second;
        if (hs.size() < 2 || vs.size() < 2)
            continue;

        /*
         * A cell boundary runs from one grid line to another. Rules whose ends
         * are not on a crossing grid line (underlines, short marks inside a
         * cell) are not boundaries: drop them. The grid positions across are
         * taken from the crossing rules and the extent of the parallel ones.
         */
        auto grid_positions = [&](const vector<int> & across, const vector<Rule> & A, bool a_horizontal,
                                  const vector<int> & along, const vector<Rule> & B) {
            vector<double> pos;
            double lo = 1e30, hi = -1e30;
            for (int k : across)
                pos.push_back(a_horizontal ? A[k].y0 : A[k].x0);
            for (int k : along)
            {
                lo = min(lo, a_horizontal ? B[k].y0 : B[k].x0);
                hi = max(hi, a_horizontal ? B[k].y1 : B[k].x1);
            }
            pos.push_back(lo);
            pos.push_back(hi);
            return cluster_positions(pos, MIN_CELL);
        };
        auto on_grid = [&](double v, const vector<double> & grid) {
            for (double g : grid)
                if (abs(v - g) <= MIN_CELL)
                    return true;
            return false;
        };
        {
            vector<double> gx = grid_positions(vs, V, false, hs, H);
            vector<int> hs2;
            for (int i : hs)
                if (on_grid(H[i].x0, gx) && on_grid(H[i].x1, gx))
                    hs2.push_back(i);
            if (hs2.size() >= 2)
                hs = hs2;
            vector<double> gy = grid_positions(hs, H, true, vs, V);
            vector<int> vs2;
            for (int j : vs)
                if (on_grid(V[j].y0, gy) && on_grid(V[j].y1, gy))
                    vs2.push_back(j);
            if (vs2.size() >= 2)
                vs = vs2;
        }

        vector<double> xs = grid_positions(vs, V, false, hs, H);
        vector<double> ys = grid_positions(hs, H, true, vs, V);
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
                        s.lead = lead_case(in, vl.chars);
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

        /*
         * Charts: the bars (filled boxes) of a bar chart form a lattice too.
         * Their grid is mostly empty, with whole columns or rows without
         * text (the gaps between the bars).
         */
        {
            int nrows = row_map[nr];
            vector<bool> col_used(nc, false), row_used(nrows, false);
            int filled = 0;
            for (const auto & c : final_cells)
            {
                bool has_text = false;
                for (int ci : c.chars)
                    if (!in.chars[ci].is_space())
                    {
                        has_text = true;
                        break;
                    }
                if (!has_text)
                    continue;
                ++filled;
                for (int k = c.col; k < c.col + c.colspan && k < nc; ++k)
                    col_used[k] = true;
                for (int k = c.row; k < c.row + c.rowspan && k < nrows; ++k)
                    row_used[k] = true;
            }
            int empty_cols = (int)std::count(col_used.begin(), col_used.end(), false);
            int empty_rows = (int)std::count(row_used.begin(), row_used.end(), false);
            bool sparse = filled < 0.5 * final_cells.size();
            if (sparse && (empty_cols > 0 || empty_rows > 0.25 * nrows))
                continue;
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

    // same extent: within 4pt or 5% of the width
    auto ext_tol = [](const Rule & r) { return max(4.0, 0.05 * (r.x1 - r.x0)); };

    // group rules with the same horizontal extent and the same style (stroked
    // lines vs. edges of filled boxes): two tables of the same width drawn in
    // different styles must not be merged
    vector<bool> grouped(H.size(), false);
    vector<vector<int>> groups;
    for (size_t i = 0; i < H.size(); ++i)
    {
        if (grouped[i])
            continue;
        vector<int> g{(int)i};
        grouped[i] = true;
        for (size_t j = i + 1; j < H.size(); ++j)
            if (!grouped[j] && H[j].kind == H[i].kind && abs(H[j].x0 - H[i].x0) <= ext_tol(H[i]) && abs(H[j].x1 - H[i].x1) <= ext_tol(H[i]))
            {
                g.push_back(j);
                grouped[j] = true;
            }
        std::sort(g.begin(), g.end(), [&](int a, int b) { return H[a].y0 < H[b].y0; });
        groups.push_back(g);
    }
    // a shaded header directly followed by stroked rules (or vice versa) is one table
    for (bool merged = true; merged; )
    {
        merged = false;
        for (size_t a = 0; a < groups.size() && !merged; ++a)
            for (size_t b = 0; b < groups.size() && !merged; ++b)
            {
                if (a == b)
                    continue;
                const Rule & last = H[groups[a].back()];
                const Rule & first = H[groups[b].front()];
                if (abs(last.y0 - first.y0) <= 2 && abs(last.x0 - first.x0) <= ext_tol(last) && abs(last.x1 - first.x1) <= ext_tol(last))
                {
                    groups[a].insert(groups[a].end(), groups[b].begin() + 1, groups[b].end());
                    groups.erase(groups.begin() + b);
                    merged = true;
                }
            }
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const vector<int> & g) { return g.size() < 2; }), groups.end());

    /*
     * Chart grid lines: most rules have a tick label (a short text) on their
     * own height just before or after them ("400", "300", ...). The text of a
     * table is between its rules, not on them.
     */
    {
        vector<int> all;
        for (const auto & ch : in.chars)
            if (usable_char(ch) && !taken[ch.index])
                all.push_back(ch.index);
        auto lines = build_visual_lines(in.chars, all);
        vector<Segment> segs;
        for (const auto & l : lines)
            for (auto & sg : split_segments(in.chars, l, SEG_GAP_EM))
                segs.push_back(std::move(sg));
        auto has_tick = [&](const Rule & r) {
            for (const auto & sg : segs)
            {
                // a short number whose box touches the rule's height (tick labels sit on
                // the grid line or are centered on it)
                double em = max(sg.box.h(), 4.0);
                if (r.y0 < sg.box.y0 - 0.15 * em || r.y0 > sg.box.y1 + 0.3 * em || sg.box.w() > 6 * em)
                    continue;
                string t = chars_text(in.chars, sg.chars);
                if (t.size() > 10 || std::none_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    continue;
                if ((sg.box.x1 <= r.x0 + 1 && sg.box.x1 >= r.x0 - 3 * em)
                        || (sg.box.x0 >= r.x1 - 1 && sg.box.x0 <= r.x1 + 3 * em))
                    return true;
            }
            return false;
        };
        groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const vector<int> & g) {
            int ticks = 0;
            for (int k : g)
                if (has_tick(H[k]))
                    ++ticks;
            return ticks >= 3 && ticks >= 0.5 * g.size();
        }), groups.end());
    }

    auto chars_in = [&](const Box & b) {
        vector<int> v;
        for (const auto & ch : in.chars)
            if (usable_char(ch) && !taken[ch.index] && b.contains(ch.box.cx(), ch.box.cy()))
                v.push_back(ch.index);
        return v;
    };

    auto body_em = [](const vector<VLine> & lines) {
        vector<double> v;
        for (const auto & l : lines)
            v.push_back(l.em);
        return max(median(v), 4.0);
    };

    /*
     * A band (between two rules) that starts with a lot of empty space is not
     * part of the table above it: e.g. two tables of the same width, one
     * below the other, with a caption in between. Text left of the rules
     * (row labels, captions) counts too.
     */
    auto band_has_gap = [&](double y0, double y1, double x1, const vector<VLine> & table_lines) {
        vector<int> ids;
        for (const auto & ch : in.chars)
            if (usable_char(ch) && !taken[ch.index] && ch.box.cx() <= x1 && ch.box.cy() > y0 && ch.box.cy() < y1)
                ids.push_back(ch.index);
        double limit = 2.5 * body_em(table_lines);
        double prev = y0;
        for (const auto & l : build_visual_lines(in.chars, ids))
        {
            if (l.box.y0 - prev > limit)
                return true;
            prev = max(prev, l.box.y1);
        }
        return false;
    };

    /*
     * A band with a line of text running from the left of the rules across
     * most of their width is a paragraph between two tables.
     */
    auto band_has_prose = [&](double y0, double y1, const Box & region) {
        vector<int> ids;
        for (const auto & ch : in.chars)
            if (usable_char(ch) && !taken[ch.index] && ch.box.cx() <= region.x1 && ch.box.cy() > y0 && ch.box.cy() < y1)
                ids.push_back(ch.index);
        for (const auto & l : build_visual_lines(in.chars, ids))
        {
            if (!(l.box.x0 < region.x0 && min(l.box.x1, region.x1) - region.x0 > 0.5 * region.w()))
                continue;
            // running text (justified: wide word gaps) has no column gap
            double max_gap = 0, prev_x1 = -1e30;
            for (int ci : l.chars)
            {
                const Box & b = in.chars[ci].box;
                if (prev_x1 > -1e30)
                    max_gap = max(max_gap, b.x0 - prev_x1);
                prev_x1 = max(prev_x1, b.x1);
            }
            if (max_gap < 2.0 * max(l.em, 4.0))
                return true;
        }
        return false;
    };

    for (const auto & g : groups)
    {
        size_t i = 0;
        while (i + 1 < g.size())
        {
            // a first band with a paragraph in it (between two tables sharing their
            // rules' extent): the table starts at the next rule, its headings are
            // found above it
            {
                Box first(H[g[i]].x0 - 2, H[g[i]].y0, H[g[i]].x1 + 2, H[g[i + 1]].y0);
                if (band_has_prose(first.y0, first.y1, first))
                {
                    ++i;
                    continue;
                }
            }
            // extend the region band by band while the bands look tabular
            size_t j = i + 1;
            size_t best_j = 0;
            vector<double> best_bounds;
            vector<vector<Segment>> best_segs;
            vector<VLine> best_lines;
            while (j < g.size())
            {
                Box region(H[g[i]].x0 - 2, H[g[i]].y0, H[g[i]].x1 + 2, H[g[j]].y0);
                if (getenv("P2H_SEM_DEBUG"))
                    fprintf(stderr, "hrules: group rule %zu..%zu region %.1f,%.1f-%.1f,%.1f best_j %zu\n", i, j, region.x0, region.y0, region.x1, region.y1, best_j);
                if (best_j != 0 && (band_has_gap(H[g[j - 1]].y0, H[g[j]].y0, region.x1, best_lines)
                                    || band_has_prose(H[g[j - 1]].y0, H[g[j]].y0, region)))
                    break; // a different table further down
                auto ids = chars_in(region);
                if (ids.empty())
                {
                    ++j;
                    continue;
                }
                auto lines = build_visual_lines(in.chars, ids);
                vector<vector<Segment>> segs;
                vector<double> bands;
                for (size_t k = i; k <= j; ++k)
                    bands.push_back(H[g[k]].y0);
                auto bounds = find_columns(in, lines, region, segs, bands);
                if (bounds.empty())
                {
                    // the first band(s) (e.g. only the header) may be too small to find columns:
                    // keep extending until something is found, then stop at the first failure
                    if (best_j == 0)
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

            bool label_extended = false;
            /*
             * Row labels left of the rules: in financial statements the rules
             * often run under the number columns only. Text left of the table
             * whose lines are aligned with the table's lines becomes the first
             * column. Lines above the first or below the last aligned label
             * (captions, paragraphs) are not included.
             */
            {
                double x_left = H[g[i]].x0 - 2;
                double top = rule_ys.front(), bottom = rule_ys.back();
                vector<int> ids;
                for (const auto & ch : in.chars)
                    if (usable_char(ch) && !taken[ch.index] && ch.box.x1 <= x_left + 1
                            && ch.box.cy() > top && ch.box.cy() < bottom)
                        ids.push_back(ch.index);
                auto llines = build_visual_lines(in.chars, ids);
                vector<Box> tseg_boxes;
                for (const auto & ls : best_segs)
                    for (const auto & sg : ls)
                        tseg_boxes.push_back(sg.box);
                vector<int> aligned_idx;
                for (size_t k = 0; k < llines.size(); ++k)
                {
                    const auto & l = llines[k];
                    for (const auto & b : tseg_boxes)
                        if (abs(b.cy() - l.box.cy()) <= 0.35 * max(l.em, 1.0))
                        {
                            aligned_idx.push_back((int)k);
                            break;
                        }
                }
                int table_lines = 0;
                for (const auto & ls : best_segs)
                    if (!ls.empty())
                        ++table_lines;
                if (aligned_idx.size() >= 2 && aligned_idx.size() >= 0.4 * table_lines)
                {
                    double em = body_em(llines);
                    // all label lines inside the table (section headings, labels whose
                    // numbers are on another line), up to the last aligned one and its
                    // wrapped continuation
                    // going up from the first aligned label: section headings of the table,
                    // not a heading above it (bigger font, or a gap)
                    double tem = body_em(best_lines);
                    int first = aligned_idx.front();
                    while (first > 0 && llines[first].box.y0 - llines[first - 1].box.y1 <= 2.0 * tem
                            && llines[first - 1].em <= 1.08 * tem)
                        --first;
                    int last = aligned_idx.back();
                    double text_bottom = -1e30;
                    for (const auto & b : tseg_boxes)
                        text_bottom = max(text_bottom, b.y1);
                    while (last + 1 < (int)llines.size()
                            && (llines[last + 1].box.y0 < text_bottom || llines[last + 1].box.y0 - llines[last].box.y1 < 0.6 * em))
                        ++last;
                    double lx0 = 1e30;
                    for (int k = first; k <= last; ++k)
                    {
                        vector<Segment> parts(1);
                        Segment & sg = parts.back();
                        sg.box = llines[k].box;
                        sg.chars = llines[k].chars;
                        // a long label running into the first number column: take the rest of it
                        for (auto & ls : best_segs)
                            for (auto it = ls.begin(); it != ls.end(); ++it)
                                if (abs(it->box.cy() - sg.box.cy()) <= 0.3 * em && it->box.x0 >= sg.box.x1
                                        && it->box.x0 - sg.box.x1 <= SEG_GAP_EM * em)
                                {
                                    sg.chars.insert(sg.chars.end(), it->chars.begin(), it->chars.end());
                                    sg.box.expand(it->box);
                                    ls.erase(it);
                                    break;
                                }
                        best_segs.push_back(parts);
                        lx0 = min(lx0, llines[k].box.x0);
                    }
                    best_bounds.insert(best_bounds.begin(), lx0 - 1);
                    label_extended = true;
                }
            }

            /*
             * Rows above the first rule: the column headings, and - when the
             * rules only separate the totals - the first rows of the table.
             * Lines are taken going up while they fit the columns, without a
             * big gap, a line across the columns or a bigger font (caption).
             * The leading lines that are not data rows (a label with numbers)
             * form one header row.
             */
            int header_band = -1; // index of the band holding the header row
            std::set<int> heading_chars; // first chars of the header segments
            {
                double top = rule_ys.front();
                double tl = best_bounds.front(), tr = best_bounds.back();
                double em = body_em(best_lines);
                vector<int> ids;
                for (const auto & ch : in.chars)
                    if (usable_char(ch) && !taken[ch.index] && ch.box.cy() < top && ch.box.cy() > top - 40 * em
                            && ch.box.cx() > tl - 3 * em && ch.box.cx() < tr + 3 * em)
                        ids.push_back(ch.index);
                // data row: text in the first column and a number in another one
                bool label_col = best_bounds.size() >= 3;
                auto is_number = [&](const Segment & sg) {
                    string t = chars_text(in.chars, sg.chars);
                    bool digit = false;
                    for (char c : t)
                    {
                        if (c >= '0' && c <= '9')
                            digit = true;
                        else if (!strchr(".,()%-+/ ", c) && (unsigned char)c < 0x80)
                            return false;
                    }
                    return digit || t == "-" || t == "\xe2\x80\x93"; // dash: nothing
                };
                auto is_data_line = [&](const vector<Segment> & segs) {
                    if (!label_col)
                        return false;
                    bool label = false, number = false;
                    for (const auto & sg : segs)
                    {
                        if (sg.box.cx() < best_bounds[1])
                            label = true;
                        else if (is_number(sg))
                            number = true;
                    }
                    return label && number;
                };
                vector<VLine> up;
                if (label_extended)
                {
                    // label and numbers are matched by their baselines: a heading above the
                    // column headings must not become one line with them
                    vector<int> ids_l, ids_v;
                    for (int ci : ids)
                        (in.chars[ci].box.cx() < best_bounds[1] ? ids_l : ids_v).push_back(ci);
                    auto ll = build_visual_lines(in.chars, ids_l), lv = build_visual_lines(in.chars, ids_v);
                    vector<bool> used(lv.size(), false);
                    for (auto & l : ll)
                    {
                        for (size_t k = 0; k < lv.size(); ++k)
                            if (!used[k] && abs(lv[k].box.cy() - l.box.cy()) <= 0.3 * max(l.em, 1.0))
                            {
                                used[k] = true;
                                l.chars.insert(l.chars.end(), lv[k].chars.begin(), lv[k].chars.end());
                                l.box.expand(lv[k].box);
                                l.em = max(l.em, lv[k].em);
                                break;
                            }
                        up.push_back(l);
                    }
                    for (size_t k = 0; k < lv.size(); ++k)
                        if (!used[k])
                            up.push_back(lv[k]);
                    std::stable_sort(up.begin(), up.end(), [](const VLine & a, const VLine & b) { return a.box.cy() < b.box.cy(); });
                }
                else
                    up = build_visual_lines(in.chars, ids);
                vector<std::pair<VLine, vector<Segment>>> accepted; // bottom to top
                double cur = top;
                for (int k = (int)up.size() - 1; k >= 0; --k)
                {
                    const VLine & l = up[k];
                    if (getenv("P2H_SEM_DEBUG"))
                        fprintf(stderr, "up: line %.1f-%.1f x %.1f-%.1f em %.2f (table em %.2f, tl %.1f tr %.1f cur %.1f) '%s'\n", l.box.y0, l.box.y1, l.box.x0, l.box.x1, l.em, em, tl, tr, cur, chars_text(in.chars, l.chars).c_str());
                    if (cur - l.box.y1 > 2.0 * em || l.em > 1.08 * em)
                        break;
                    if (l.box.x0 < tl - 0.25 * em || l.box.x1 > tr + 0.5 * em)
                        break;
                    auto segs = split_segments(in.chars, l, SEG_GAP_EM);
                    // headings may be wider than their numbers: only text running across
                    // the table (a caption, a paragraph) does not fit
                    bool fits = !segs.empty();
                    for (const auto & sg : segs)
                        if (sg.box.w() > 0.6 * (tr - tl) && segs.size() == 1)
                        {
                            if (getenv("P2H_SEM_DEBUG"))
                                fprintf(stderr, "  seg %.1f-%.1f does not fit '%s'\n", sg.box.x0, sg.box.x1, chars_text(in.chars, sg.chars).c_str());
                            fits = false;
                        }
                    if (fits && segs.size() == 1 && !is_data_line(segs))
                    {
                        // a lone text above the table: part of a multi-line heading only if
                        // close to the line below, never a caption ending with ':'
                        string t = chars_text(in.chars, segs[0].chars);
                        if ((!t.empty() && t.back() == ':') || cur - l.box.y1 > 0.8 * em)
                            fits = false;
                    }
                    if (!fits)
                        break;
                    accepted.emplace_back(l, std::move(segs));
                    cur = l.box.y0;
                }
                std::reverse(accepted.begin(), accepted.end());

                size_t nh = 0;
                while (nh < accepted.size() && !is_data_line(accepted[nh].second))
                    ++nh;
                // a single header line needs at least two headings (old behaviour)
                if (nh == 1 && accepted.size() == 1 && accepted[0].second.size() < 2)
                    accepted.clear(), nh = 0;
                if (!accepted.empty())
                {
                    vector<double> new_ys;
                    if (nh > 0)
                        new_ys.push_back(accepted[0].first.box.y0 - 1);
                    if (nh < accepted.size())
                        new_ys.push_back(nh > 0 ? (accepted[nh - 1].first.box.y1 + accepted[nh].first.box.y0) / 2
                                                : accepted[0].first.box.y0 - 1);
                    if (nh > 0)
                        header_band = 0;
                    for (size_t k = 0; k < accepted.size(); ++k)
                    {
                        if (k < nh)
                            for (const auto & sg : accepted[k].second)
                                if (!sg.chars.empty())
                                    heading_chars.insert(sg.chars.front());
                        best_segs.push_back(accepted[k].second);
                    }
                    rule_ys.insert(rule_ys.begin(), new_ys.begin(), new_ys.end());
                }
            }

            /*
             * Columns inside the label area (e.g. the note numbers of a financial
             * statement): whitespace gaps shared by the label lines that have
             * several pieces; a column must hold short pieces in two lines at
             * least, not just the ends of a few long labels.
             */
            if (label_extended)
            {
                double x_left = best_bounds[1];
                double em = body_em(best_lines);
                vector<std::pair<double, double>> iv;
                vector<vector<Segment>> pieces; // per label segment
                vector<std::pair<size_t, size_t>> where;
                int multi = 0;
                for (size_t a = 0; a < best_segs.size(); ++a)
                    for (size_t b = 0; b < best_segs[a].size(); ++b)
                    {
                        const Segment & sg = best_segs[a][b];
                        if (sg.chars.empty() || sg.box.x0 >= x_left)
                            continue;
                        VLine v;
                        v.box = sg.box;
                        v.chars = sg.chars;
                        v.em = em;
                        pieces.push_back(split_segments(in.chars, v, SEG_GAP_EM));
                        where.emplace_back(a, b);
                    }
                // pieces of the same line together
                vector<size_t> order(pieces.size());
                for (size_t k = 0; k < order.size(); ++k)
                    order[k] = k;
                auto seg_cy = [&](size_t k) { return best_segs[where[k].first][where[k].second].box.cy(); };
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return seg_cy(a) < seg_cy(b); });
                vector<std::pair<int, vector<size_t>>> by_line;
                for (size_t k : order)
                {
                    if (by_line.empty() || seg_cy(k) - seg_cy(by_line.back().second.front()) > 0.3 * em)
                        by_line.emplace_back((int)by_line.size(), vector<size_t>());
                    by_line.back().second.push_back(k);
                }
                if (getenv("P2H_SEM_DEBUG"))
                    fprintf(stderr, "label columns: %zu pieces, %zu lines, x_left %.1f\n", pieces.size(), by_line.size(), x_left);
                for (const auto & kv : by_line)
                {
                    vector<std::pair<double, double>> line_iv;
                    for (size_t k : kv.second)
                        for (const auto & pc : pieces[k])
                            if (pc.box.x1 <= x_left + 1)
                                line_iv.emplace_back(pc.box.x0, pc.box.x1);
                    if (line_iv.size() >= 2)
                    {
                        ++multi;
                        iv.insert(iv.end(), line_iv.begin(), line_iv.end());
                    }
                }
                vector<double> inner;
                if (getenv("P2H_SEM_DEBUG"))
                    fprintf(stderr, "label columns: multi %d\n", multi);
                if (multi >= 3 || (multi >= 2 && multi >= 0.2 * by_line.size()))
                {
                    std::sort(iv.begin(), iv.end());
                    vector<std::pair<double, double>> cover;
                    for (const auto & pr : iv)
                    {
                        if (!cover.empty() && pr.first <= cover.back().second + 0.4 * em)
                            cover.back().second = max(cover.back().second, pr.second);
                        else
                            cover.push_back(pr);
                    }
                    for (size_t c = 1; c < cover.size(); ++c)
                    {
                        double lo = (cover[c - 1].second + cover[c].first) / 2;
                        double hi = c + 1 < cover.size() ? (cover[c].second + cover[c + 1].first) / 2 : x_left;
                        int n = 0;
                        for (const auto & kv : by_line)
                        {
                            bool hit = false;
                            for (size_t k : kv.second)
                                for (const auto & pc : pieces[k])
                                    if (pc.box.x0 >= lo && pc.box.x1 <= hi + 1 && pc.box.w() < 6 * em)
                                        hit = true;
                            if (hit)
                                ++n;
                        }
                        if (getenv("P2H_SEM_DEBUG"))
                            fprintf(stderr, "label columns: cover %.1f-%.1f boundary %.1f..%.1f n %d\n", cover[c].first, cover[c].second, lo, hi, n);
                        if (n >= 2 && lo < x_left - 1)
                            inner.push_back(lo);
                    }
                }
                if (!inner.empty())
                {
                    // replace the label segments by their pieces
                    for (int k = (int)pieces.size() - 1; k >= 0; --k)
                    {
                        auto & line = best_segs[where[k].first];
                        line.erase(line.begin() + where[k].second);
                        line.insert(line.begin() + where[k].second, pieces[k].begin(), pieces[k].end());
                    }
                    best_bounds.insert(best_bounds.begin() + 1, inner.begin(), inner.end());
                }
            }

            auto lsegs = make_lsegs(in, best_segs, best_bounds);

            /*
             * A heading belongs to the column whose values it is aligned with
             * (left, centered or right), even when it is wider than them.
             */
            if (!heading_chars.empty())
            {
                int ncols = (int)best_bounds.size() - 1;
                vector<double> cl(ncols, 1e30), cr(ncols, -1e30);
                for (const auto & ls : lsegs)
                    if (ls.colspan == 1 && !ls.chars.empty() && !heading_chars.count(ls.chars.front()))
                    {
                        cl[ls.col] = min(cl[ls.col], ls.box.x0);
                        cr[ls.col] = max(cr[ls.col], ls.box.x1);
                    }
                // per heading line: an order preserving assignment of its segments to
                // the value columns, minimizing the alignment distance
                auto dist = [&](const LSeg & ls, int c) {
                    if (cl[c] > cr[c])
                        return 1e6 + abs(ls.box.cx() - (best_bounds[c] + best_bounds[c + 1]) / 2);
                    return min(min(abs(ls.box.x0 - cl[c]), abs(ls.box.x1 - cr[c])),
                               abs(ls.box.cx() - (cl[c] + cr[c]) / 2));
                };
                vector<LSeg *> heads;
                for (auto & ls : lsegs)
                    if (!ls.chars.empty() && heading_chars.count(ls.chars.front()))
                        heads.push_back(&ls);
                std::sort(heads.begin(), heads.end(), [](const LSeg * a, const LSeg * b) {
                    return a->box.cy() < b->box.cy() || (a->box.cy() == b->box.cy() && a->box.x0 < b->box.x0);
                });
                int first_value_col = (best_bounds.size() >= 3 && label_extended) ? 1 : 0;
                for (size_t a0 = 0; a0 < heads.size(); )
                {
                    size_t a1 = a0;
                    while (a1 < heads.size() && abs(heads[a1]->box.cy() - heads[a0]->box.cy()) < 0.3 * heads[a0]->em)
                        ++a1;
                    vector<LSeg *> line(heads.begin() + a0, heads.begin() + a1);
                    std::sort(line.begin(), line.end(), [](const LSeg * a, const LSeg * b) { return a->box.x0 < b->box.x0; });
                    // the label column keeps its text
                    vector<LSeg *> vals;
                    for (LSeg * ls : line)
                    {
                        if (first_value_col == 1 && ls->box.cx() < best_bounds[1])
                        {
                            ls->col = 0;
                            ls->colspan = 1;
                        }
                        else
                            vals.push_back(ls);
                    }
                    int m = ncols - first_value_col, k = (int)vals.size();
                    if (k > 0 && k <= m)
                    {
                        const double INF = 1e30;
                        vector<vector<double>> f(k, vector<double>(m, INF));
                        vector<vector<int>> from(k, vector<int>(m, -1));
                        for (int j = 0; j < m; ++j)
                            f[0][j] = dist(*vals[0], first_value_col + j);
                        for (int i = 1; i < k; ++i)
                        {
                            double best_prev = INF;
                            int best_j = -1;
                            for (int j = i; j < m; ++j)
                            {
                                if (f[i - 1][j - 1] < best_prev)
                                {
                                    best_prev = f[i - 1][j - 1];
                                    best_j = j - 1;
                                }
                                if (best_prev < INF)
                                {
                                    f[i][j] = best_prev + dist(*vals[i], first_value_col + j);
                                    from[i][j] = best_j;
                                }
                            }
                        }
                        int j = (int)(std::min_element(f[k - 1].begin(), f[k - 1].end()) - f[k - 1].begin());
                        for (int i = k - 1; i >= 0 && j >= 0; --i)
                        {
                            vals[i]->col = first_value_col + j;
                            vals[i]->colspan = 1;
                            j = from[i][j];
                        }
                    }
                    a0 = a1;
                }
            }

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
                int n;
                if ((int)b == header_band)
                {
                    // the column headings: one row of (multi-line) cells
                    for (auto & sg : band)
                        sg.row = 0;
                    n = 1;
                }
                else
                    n = split_band_rows(band);
                size_t first = t.cells.size();
                band_to_cells(band, row, best_bounds, rule_ys[b], rule_ys[b + 1], n, t.cells);
                if ((int)b == header_band || (b == 0 && header_band < 0 && rule_ys.size() > 2 && n == 1))
                    for (size_t k = first; k < t.cells.size(); ++k)
                        t.cells[k].header = true;
                row += n;
            }
            t.rows = row;
            drop_empty_columns(in, t);
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
