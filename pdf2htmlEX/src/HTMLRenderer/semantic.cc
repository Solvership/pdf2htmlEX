/*
 * semantic.cc
 *
 * Glue between the renderer and the semantic layer (Semantic/*):
 * marked content tracking and the per page analysis.
 */

#include <cmath>

#include "HTMLRenderer.h"
#include "util/math.h"

namespace pdf2htmlEX {

using std::vector;

// true if the UTF-8 string is a single code point of a private use area
static bool is_private_use(const std::string & s)
{
    if (s.size() == 3 && (unsigned char)s[0] == 0xEE)
        return true; // U+E000 - U+EFFF
    if (s.size() == 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] < 0xA4)
        return true; // U+F000 - U+F8FF
    if (s.size() == 4 && (unsigned char)s[0] == 0xF3 && (unsigned char)s[1] >= 0xB0)
        return true; // planes 15/16
    return false;
}

void HTMLRenderer::beginMarkedContent(const char * name, Dict * properties)
{
    if (!param.semantic_tables)
        return;
    int mcid = -1;
    if (properties)
    {
        Object o = properties->lookup("MCID");
        if (o.isInt())
            mcid = o.getInt();
    }
    mcid_stack.push_back(mcid);
}

void HTMLRenderer::endMarkedContent(GfxState * state)
{
    if (!param.semantic_tables)
        return;
    if (!mcid_stack.empty())
        mcid_stack.pop_back();
}

void HTMLRenderer::analyze_page_semantics(void)
{
    semantic::PageInput in;
    in.page = pageNum;

    const double zoom = text_zoom_factor();
    const double page_h = html_text_page.get_height(); // device units
    in.width = html_text_page.get_width() / zoom;
    in.height = page_h / zoom;

    // tracer space -> visual page space (pt, origin top-left, y down)
    auto to_visual = [&](double x, double y, double & vx, double & vy) {
        tm_transform(default_ctm, x, y);
        vx = x / zoom;
        vy = (page_h - y) / zoom;
    };

    // chars
    const auto & bboxes = covered_text_detector.get_char_bboxes();
    const int n = (int)(bboxes.size() / 4);
    in.chars.resize(n);
    for (int i = 0; i < n; ++i)
    {
        semantic::Char & c = in.chars[i];
        c.index = i;
        const double * b = &bboxes[i * 4];
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0)
            continue;
        double xa, ya, xb, yb, xc, yc, xd, yd;
        to_visual(b[0], b[1], xa, ya);
        to_visual(b[2], b[3], xb, yb);
        to_visual(b[0], b[3], xc, yc);
        to_visual(b[2], b[1], xd, yd);
        c.box = semantic::Box(std::min(std::min(xa, xb), std::min(xc, xd)), std::min(std::min(ya, yb), std::min(yc, yd)),
                              std::max(std::max(xa, xb), std::max(xc, xd)), std::max(std::max(ya, yb), std::max(yc, yd)));
        if (i < (int)char_mcids.size())
            c.mcid = char_mcids[i];
        if (i < (int)char_ems.size())
            c.em = char_ems[i];
    }

    // text of the chars
    {
        const auto & lines = html_text_page.get_lines();
        vector<LineChar> lc;
        for (size_t li = 0; li < lines.size(); ++li)
        {
            lc.clear();
            lines[li]->collect_chars(lc);
            for (const auto & x : lc)
            {
                if (x.char_index < 0 || x.char_index >= n)
                    continue;
                semantic::Char & c = in.chars[x.char_index];
                c.line = (int)li;
                c.text = x.text;
                // prefer Poppler's unicode mapping when the HTML text had to use a private use code point
                if (x.char_index < (int)char_unicode.size() && !char_unicode[x.char_index].empty() && is_private_use(x.text))
                    c.text = char_unicode[x.char_index];
                c.space_before = x.space_before;
            }
        }
    }

    // rules
    for (const auto & r : page_rules)
    {
        double x0, y0, x1, y1;
        to_visual(r.x0, r.y0, x0, y0);
        to_visual(r.x1, r.y1, x1, y1);
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        double w = r.width / zoom;
        // line width in visual space (assume uniform scaling)
        {
            double sx = std::hypot(default_ctm[0], default_ctm[1]);
            w = r.width * sx / zoom;
        }
        if (r.kind == DrawingTracer::RULE_STROKE)
        {
            double dx = x1 - x0, dy = y1 - y0;
            if (dx < 0.01 && dy < 0.01)
                continue;
            if (w > 8)
            {
                // a very thick line is a filled band: use its edges
                if (dy <= 0.5)
                {
                    in.rules.push_back({x0, y0 - w / 2, x1, y0 - w / 2, true});
                    in.rules.push_back({x0, y0 + w / 2, x1, y0 + w / 2, true});
                }
                else if (dx <= 0.5)
                {
                    in.rules.push_back({x0 - w / 2, y0, x0 - w / 2, y1, false});
                    in.rules.push_back({x0 + w / 2, y0, x0 + w / 2, y1, false});
                }
                continue;
            }
            if (dy <= 0.5)
                in.rules.push_back({x0, (y0 + y1) / 2, x1, (y0 + y1) / 2, true});
            else if (dx <= 0.5)
                in.rules.push_back({(x0 + x1) / 2, y0, (x0 + x1) / 2, y1, false});
        }
        else
        {
            double w2 = x1 - x0, h2 = y1 - y0;
            if (w2 < 0.01 && h2 < 0.01)
                continue;
            if (w2 * h2 > 0.9 * in.width * in.height)
                continue; // page background
            if (h2 <= 3 && w2 > h2)
                in.rules.push_back({x0, (y0 + y1) / 2, x1, (y0 + y1) / 2, true});
            else if (w2 <= 3 && h2 > w2)
                in.rules.push_back({(x0 + x1) / 2, y0, (x0 + x1) / 2, y1, false});
            else
            {
                // filled box (e.g. cell background): its edges
                in.rules.push_back({x0, y0, x1, y0, true});
                in.rules.push_back({x0, y1, x1, y1, true});
                in.rules.push_back({x0, y0, x0, y1, false});
                in.rules.push_back({x1, y0, x1, y1, false});
            }
        }
    }

    if (structure_mapper)
        structure_mapper->fill(pageNum, in);

    cur_semantic = semantic::analyze_page(in, semantic::Options::from_param(param));
    cur_semantic_tags.char_group = &cur_semantic.char_group;
    cur_semantic_tags.group_attrs = &cur_semantic.group_attrs;
    cur_semantic_tags.group_keys = &cur_semantic.group_keys;
}

} // namespace pdf2htmlEX
