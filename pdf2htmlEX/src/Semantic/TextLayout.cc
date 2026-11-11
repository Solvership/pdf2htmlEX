/*
 * TextLayout.cc
 *
 * Grouping glyphs into visual lines / segments and extracting their text.
 */

#include <algorithm>
#include <cmath>

#include "Semantic.h"

namespace pdf2htmlEX {
namespace semantic {

using std::vector;
using std::string;
using std::min;
using std::max;

static double vertical_overlap(const Box & a, const Box & b)
{
    return min(a.y1, b.y1) - max(a.y0, b.y0);
}

vector<VLine> build_visual_lines(const vector<Char> & chars, const vector<int> & subset)
{
    vector<int> order;
    order.reserve(subset.size());
    for (int i : subset)
        if (chars[i].box.valid() && !chars[i].text.empty())
            order.push_back(i);

    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return chars[a].box.cy() < chars[b].box.cy();
    });

    vector<VLine> lines;
    for (int i : order)
    {
        const Box & cb = chars[i].box;
        double ch = max(cb.h(), 0.01);
        // the char joins one of the most recent lines if it overlaps vertically enough
        int found = -1;
        for (int k = (int)lines.size() - 1; k >= 0 && k >= (int)lines.size() - 3; --k)
        {
            const Box & lb = lines[k].box;
            double ov = vertical_overlap(cb, lb);
            if (ov >= 0.5 * min(ch, max(lb.h(), 0.01)))
            {
                found = k;
                break;
            }
        }
        if (found < 0)
        {
            lines.emplace_back();
            lines.back().box = cb;
            found = (int)lines.size() - 1;
        }
        lines[found].chars.push_back(i);
        lines[found].box.expand(cb);
    }

    // order chars inside each line: keep the drawing order of chars coming from
    // the same HTML line (it is the logical order), order runs by x
    for (auto & l : lines)
    {
        // build runs
        vector<vector<int>> runs;
        vector<int> sorted = l.chars;
        std::sort(sorted.begin(), sorted.end()); // by index == drawing order
        for (int i : sorted)
        {
            bool appended = false;
            if (!runs.empty())
            {
                const Char & last = chars[runs.back().back()];
                const Char & c = chars[i];
                // continue the run if same HTML line and not jumping backwards
                if (last.line == c.line && c.line >= 0 && c.box.x0 >= last.box.x0 - 0.5 * max(last.em, 1.0))
                {
                    runs.back().push_back(i);
                    appended = true;
                }
            }
            if (!appended)
                runs.push_back({i});
        }
        std::stable_sort(runs.begin(), runs.end(), [&](const vector<int> & a, const vector<int> & b) {
            return chars[a.front()].box.x0 < chars[b.front()].box.x0;
        });
        l.chars.clear();
        double em_sum = 0;
        for (auto & r : runs)
            for (int i : r)
            {
                l.chars.push_back(i);
                em_sum += chars[i].em;
            }
        l.em = l.chars.empty() ? 0 : em_sum / l.chars.size();
    }

    std::stable_sort(lines.begin(), lines.end(), [](const VLine & a, const VLine & b) {
        return a.box.cy() < b.box.cy();
    });
    return lines;
}

vector<Segment> split_segments(const vector<Char> & chars, const VLine & line, double gap_em)
{
    vector<Segment> segs;
    for (int i : line.chars)
    {
        const Char & c = chars[i];
        if (c.is_space())
        {
            // spaces never start a segment, and do not extend its box
            if (!segs.empty())
                segs.back().chars.push_back(i);
            continue;
        }
        bool new_seg = segs.empty();
        if (!new_seg)
        {
            const Segment & s = segs.back();
            double em = max(max(c.em, line.em), 1.0);
            if (c.box.x0 - s.box.x1 > gap_em * em)
                new_seg = true;
        }
        if (new_seg)
        {
            segs.emplace_back();
            segs.back().box = c.box;
        }
        segs.back().chars.push_back(i);
        segs.back().box.expand(c.box);
    }
    // drop trailing spaces
    for (auto & s : segs)
        while (!s.chars.empty() && chars[s.chars.back()].is_space())
            s.chars.pop_back();
    return segs;
}

string chars_text(const vector<Char> & chars, const vector<int> & seq)
{
    string out;
    const Char * prev = nullptr;
    for (int i : seq)
    {
        const Char & c = chars[i];
        if (c.text.empty())
            continue;
        if (prev && !c.is_space() && !prev->is_space())
        {
            bool space = false;
            double em = max(max(c.em, prev->em), 1.0);
            bool gap = (c.box.x0 - prev->box.x1) > 0.28 * em;
            if (prev->line == c.line && c.line >= 0 && c.index == prev->index + 1)
                space = c.space_before;
            else
                space = c.space_before || gap;
            if (space)
                out += ' ';
        }
        if (c.is_space())
        {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
        }
        else
        {
            out += c.text;
        }
        prev = &c;
    }
    // trim
    size_t b = out.find_first_not_of(' ');
    if (b == string::npos)
        return "";
    size_t e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1);
}

static bool is_lower_start(const string & s)
{
    if (s.empty())
        return false;
    unsigned char c = s[0];
    if (c >= 'a' && c <= 'z')
        return true;
    // common lower case latin letters with diacritics (č ć ž š đ ...) start with 0xC3-0xC5
    if (c >= 0xC3 && c <= 0xC5 && s.size() >= 2)
    {
        // crude: in these blocks odd code points are usually lower case
        unsigned cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        if (cp >= 0xDF && cp <= 0xFF)
            return true;
        if (cp >= 0x100 && cp <= 0x17F)
            return (cp % 2) == 1;
    }
    return false;
}

string join_lines(const vector<string> & lines, bool dehyphenate)
{
    string out;
    for (const auto & l : lines)
    {
        if (l.empty())
            continue;
        if (out.empty())
        {
            out = l;
            continue;
        }
        // a line ending with a hyphen after a letter: the word continues on the next line
        bool hyphen = out.size() >= 2 && out.back() == '-' && out[out.size() - 2] != ' ' && is_lower_start(l);
        if (hyphen)
        {
            if (dehyphenate)
                out.pop_back();
            out += l;
        }
        else
        {
            out += ' ';
            out += l;
        }
    }
    return out;
}

string json_escape(const string & s)
{
    string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out += (char)c;
        }
    }
    return out;
}

string html_escape(const string & s)
{
    string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

} // namespace semantic
} // namespace pdf2htmlEX
