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

Box reading_box(const Char & c)
{
    const Box & b = c.box;
    switch (c.dir)
    {
        case 1: return Box(-b.y1, b.x0, -b.y0, b.x1);  // bottom to top, next line to the right
        case 2: return Box(-b.x1, -b.y1, -b.x0, -b.y0); // upside down
        case 3: return Box(b.y0, -b.x1, b.y1, -b.x0);  // top to bottom, next line to the left
        default: return b;
    }
}

vector<VLine> build_visual_lines(const vector<Char> & chars, const vector<int> & subset)
{
    vector<int> order;
    order.reserve(subset.size());
    for (int i : subset)
        if (chars[i].box.valid() && !chars[i].text.empty())
            order.push_back(i);

    // rotated text is laid out in its own reading frame
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return reading_box(chars[a]).cy() < reading_box(chars[b]).cy();
    });

    vector<VLine> lines;
    vector<Box> rboxes; // line boxes in the reading frame
    for (int i : order)
    {
        const Box cb = reading_box(chars[i]);
        double ch = max(cb.h(), 0.01);
        // the char joins one of the most recent lines if it overlaps vertically enough
        int found = -1;
        for (int k = (int)lines.size() - 1; k >= 0 && k >= (int)lines.size() - 3; --k)
        {
            if (lines[k].dir != chars[i].dir)
                continue;
            const Box & lb = rboxes[k];
            double ov = vertical_overlap(cb, lb);
            if (ov >= 0.5 * min(ch, max(lb.h(), 0.01)))
            {
                found = k;
                break;
            }
            // a sub- or superscript: a smaller glyph right next to the line, shifted
            if (chars[i].em > 0 && lines[k].em > 0 && chars[i].em <= 0.8 * lines[k].em && ov > 0.15 * ch)
            {
                bool next_to = false;
                for (int j : lines[k].chars)
                {
                    Box o = reading_box(chars[j]);
                    if (cb.x0 - o.x1 < 0.3 * lines[k].em && o.x0 - cb.x1 < 0.3 * lines[k].em)
                    {
                        next_to = true;
                        break;
                    }
                }
                if (next_to)
                {
                    found = k;
                    break;
                }
            }
        }
        if (found < 0)
        {
            lines.emplace_back();
            lines.back().box = chars[i].box;
            lines.back().dir = chars[i].dir;
            rboxes.push_back(cb);
            found = (int)lines.size() - 1;
        }
        lines[found].chars.push_back(i);
        lines[found].box.expand(chars[i].box);
        rboxes[found].expand(cb);
        lines[found].em = max(lines[found].em, chars[i].em); // provisional, recomputed below
    }

    // order chars inside each line: keep the drawing order of chars coming from
    // the same HTML line (it is the logical order), order runs along the baseline
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
                if (last.line == c.line && c.line >= 0 && reading_box(c).x0 >= reading_box(last).x0 - 0.5 * max(last.em, 1.0))
                {
                    runs.back().push_back(i);
                    appended = true;
                }
            }
            if (!appended)
                runs.push_back({i});
        }
        std::stable_sort(runs.begin(), runs.end(), [&](const vector<int> & a, const vector<int> & b) {
            return reading_box(chars[a.front()]).x0 < reading_box(chars[b.front()]).x0;
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

    // top to bottom; lines of rotated text (all in the same direction) in their reading order
    bool same_rotated = !lines.empty() && lines[0].dir != 0;
    for (const auto & l : lines)
        if (l.dir != lines[0].dir)
            same_rotated = false;
    vector<size_t> idx(lines.size());
    for (size_t k = 0; k < idx.size(); ++k)
        idx[k] = k;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        if (same_rotated)
            return rboxes[a].cy() < rboxes[b].cy();
        return lines[a].box.cy() < lines[b].box.cy();
    });
    vector<VLine> out;
    out.reserve(lines.size());
    for (size_t k : idx)
        out.push_back(std::move(lines[k]));
    return out;
}

vector<Segment> split_segments(const vector<Char> & chars, const VLine & line, double gap_em)
{
    vector<Segment> segs;
    double seg_end = 0; // end of the last segment along the baseline
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
        const Box rb = reading_box(c);
        bool new_seg = segs.empty();
        if (!new_seg)
        {
            double em = max(max(c.em, line.em), 1.0);
            if (rb.x0 - seg_end > gap_em * em)
                new_seg = true;
        }
        if (new_seg)
        {
            segs.emplace_back();
            segs.back().box = c.box;
            seg_end = rb.x1;
        }
        segs.back().chars.push_back(i);
        segs.back().box.expand(c.box);
        seg_end = max(seg_end, rb.x1);
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
            bool gap = (reading_box(c).x0 - reading_box(*prev).x1) > 0.28 * em;
            if (prev->line == c.line && c.line >= 0 && c.index == prev->index + 1)
                space = c.space_before;
            else
                space = c.space_before || gap;
            // a space glyph between them in the same HTML line that is not part of the sequence
            if (!space && prev->line == c.line && c.line >= 0 && c.index > prev->index + 1 && c.index - prev->index < 64)
                for (int k = prev->index + 1; k < c.index; ++k)
                    if (k >= 0 && k < (int)chars.size() && chars[k].line == c.line && chars[k].is_space())
                    {
                        space = true;
                        break;
                    }
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
