/*
 * StructureMapper.cc
 */

#include <vector>
#include <set>
#include <functional>

#include <PDFDoc.h>
#include <Catalog.h>
#include <StructTreeRoot.h>
#include <StructElement.h>

#include "StructureMapper.h"

namespace pdf2htmlEX {
namespace semantic {

using std::vector;
using std::string;
using std::pair;

struct StructureMapper::Ctx
{
    std::map<long long, int> page_of_ref; // (num << 20 | gen) -> page number
    int depth = 0;

    int page(const Ref & r) const
    {
        auto it = page_of_ref.find(((long long)r.num << 20) | (r.gen & 0xfffff));
        return it == page_of_ref.end() ? -1 : it->second;
    }
};

bool StructureMapper::load(PDFDoc * doc)
{
    Catalog * catalog = doc->getCatalog();
    if (!catalog)
        return false;
    StructTreeRoot * root = catalog->getStructTreeRoot();
    if (!root || root->getNumChildren() == 0)
        return false;

    Ctx ctx;
    for (int i = 1; i <= doc->getNumPages(); ++i)
    {
        Ref * r = catalog->getPageRef(i);
        if (r)
            ctx.page_of_ref[((long long)r->num << 20) | (r->gen & 0xfffff)] = i;
    }

    for (unsigned i = 0; i < root->getNumChildren(); ++i)
        walk(root->getChild(i), ctx);

    return !cells.empty() || !blocks.empty();
}

void StructureMapper::fill(int page, PageInput & in) const
{
    auto it = cells.find(page);
    if (it != cells.end())
        in.mcid_cells = it->second;
    auto it2 = blocks.find(page);
    if (it2 != blocks.end())
        in.mcid_blocks = it2->second;

    /*
     * Hyphens added at line breaks: InDesign tags each of them as an inline
     * Span of its own holding just the hyphen, after a letter. A hyphen that
     * belongs to the text (a compound word, a dash for zero) is not.
     */
    auto it3 = span_mcids.find(page);
    if (it3 != span_mcids.end())
    {
        std::map<int, vector<int>> by_mcid;
        for (const auto & c : in.chars)
            if (c.mcid >= 0 && it3->second.count(c.mcid))
                by_mcid[c.mcid].push_back(c.index);
        auto is_hyphen = [](const string & t) {
            return t == "-" || t == "\xc2\xad" || t == "\xe2\x80\x90";
        };
        auto is_letter = [](const string & t) {
            unsigned char c = t.empty() ? 0 : t[0];
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC3;
        };
        for (const auto & kv : by_mcid)
        {
            int hy = -1;
            bool only = true;
            for (int ci : kv.second)
            {
                const auto & c = in.chars[ci];
                if (c.text.empty() || c.is_space())
                    continue;
                if (hy < 0 && is_hyphen(c.text))
                    hy = ci;
                else
                    only = false;
            }
            if (!only || hy <= 0)
                continue;
            // the glyph drawn before it: a letter on the same line
            const auto & prev = in.chars[hy - 1];
            if (prev.line == in.chars[hy].line && is_letter(prev.text))
                in.chars[hy].auto_hyphen = true;
        }
    }
}

static string block_tag(StructElement::Type t)
{
    switch (t)
    {
        case StructElement::P: return "p";
        case StructElement::H: return "h2";
        case StructElement::H1: return "h1";
        case StructElement::H2: return "h2";
        case StructElement::H3: return "h3";
        case StructElement::H4: return "h4";
        case StructElement::H5: return "h5";
        case StructElement::H6: return "h6";
        case StructElement::Caption: return "caption";
        case StructElement::LI: return "li";
        case StructElement::TOCI: return "p";
        default: return "";
    }
}

void StructureMapper::walk(const StructElement * e, Ctx & ctx)
{
    if (!e || !e->isOk() || e->isContent())
        return;
    if (++ctx.depth > 200)
    {
        --ctx.depth;
        return;
    }
    if (e->getType() == StructElement::Table)
    {
        process_table(e, ctx);
    }
    else
    {
        string tag = block_tag(e->getType());
        if (!tag.empty())
            process_block(e, tag, ctx);
        else
            for (unsigned i = 0; i < e->getNumChildren(); ++i)
                walk(e->getChild(i), ctx);
    }
    --ctx.depth;
}

void StructureMapper::collect_mcids(const StructElement * e, Ctx & ctx, vector<pair<int, int>> & out)
{
    if (!e)
        return;
    if (e->isContent())
    {
        if (e->getType() == StructElement::MCID && !e->hasStmRef())
        {
            Ref r;
            if (e->getPageRef(r))
            {
                int p = ctx.page(r);
                if (p > 0)
                    out.emplace_back(p, e->getMCID());
            }
        }
        return;
    }
    for (unsigned i = 0; i < e->getNumChildren(); ++i)
        collect_mcids(e->getChild(i), ctx, out);
}

void StructureMapper::collect_spans(const StructElement * e, Ctx & ctx)
{
    if (!e || e->isContent())
        return;
    if (e->getType() == StructElement::Span)
    {
        vector<pair<int, int>> sm;
        collect_mcids(e, ctx, sm);
        for (auto & pm : sm)
            span_mcids[pm.first].insert(pm.second);
        return;
    }
    for (unsigned i = 0; i < e->getNumChildren(); ++i)
        collect_spans(e->getChild(i), ctx);
}

void StructureMapper::process_block(const StructElement * e, const string & tag, Ctx & ctx)
{
    int seq = seq_counter++;
    // tables nested in a block are still tables
    vector<pair<int, int>> mcids;
    std::function<void(const StructElement *)> rec = [&](const StructElement * x) {
        if (!x)
            return;
        if (!x->isContent() && x->getType() == StructElement::Table)
        {
            process_table(x, ctx);
            return;
        }
        if (!x->isContent() && x->getType() == StructElement::Span)
            collect_spans(x, ctx);
        if (x->isContent())
        {
            collect_mcids(x, ctx, mcids);
            return;
        }
        for (unsigned i = 0; i < x->getNumChildren(); ++i)
            rec(x->getChild(i));
    };
    for (unsigned i = 0; i < e->getNumChildren(); ++i)
        rec(e->getChild(i));

    string key = "B" + std::to_string(block_counter++);
    for (auto & pm : mcids)
        blocks[pm.first][pm.second] = PageInput::BlockTag{key, tag, seq};
}

static int int_attr(const StructElement * e, Attribute::Type type, int def)
{
    const Attribute * a = e->findAttribute(type);
    if (a && a->getValue() && a->getValue()->isInt())
    {
        int v = a->getValue()->getInt();
        if (v >= 1 && v < 10000)
            return v;
    }
    return def;
}

void StructureMapper::process_table(const StructElement * table, Ctx & ctx)
{
    string key = "S" + std::to_string(table_counter++);
    int seq = seq_counter++;

    // collect rows (TR may be wrapped in THead / TBody / TFoot)
    vector<pair<const StructElement *, bool>> rows; // row, in THead
    std::function<void(const StructElement *, bool)> find_rows = [&](const StructElement * e, bool head) {
        for (unsigned i = 0; i < e->getNumChildren(); ++i)
        {
            const StructElement * c = e->getChild(i);
            if (!c || c->isContent())
                continue;
            switch (c->getType())
            {
                case StructElement::TR:
                    rows.emplace_back(c, head);
                    break;
                case StructElement::THead:
                    find_rows(c, true);
                    break;
                case StructElement::TBody:
                case StructElement::TFoot:
                    find_rows(c, false);
                    break;
                case StructElement::Caption:
                    process_block(c, "caption", ctx);
                    break;
                default:
                    break;
            }
        }
    };
    find_rows(table, false);

    std::set<pair<int, int>> occupied;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const StructElement * tr = rows[r].first;
        int col = 0;
        for (unsigned i = 0; i < tr->getNumChildren(); ++i)
        {
            const StructElement * cell = tr->getChild(i);
            if (!cell || cell->isContent())
                continue;
            if (cell->getType() != StructElement::TD && cell->getType() != StructElement::TH)
                continue;
            while (occupied.count({(int)r, col}))
                ++col;
            int rs = int_attr(cell, Attribute::RowSpan, 1);
            int cs = int_attr(cell, Attribute::ColSpan, 1);
            for (int dr = 0; dr < rs; ++dr)
                for (int dc = 0; dc < cs; ++dc)
                    occupied.insert({(int)r + dr, col + dc});

            vector<pair<int, int>> mcids;
            collect_mcids(cell, ctx, mcids);
            collect_spans(cell, ctx);
            bool header = (cell->getType() == StructElement::TH) || rows[r].second;
            for (auto & pm : mcids)
                cells[pm.first][pm.second] = PageInput::CellTag{key, (int)r, col, rs, cs, header, seq};
            col += cs;
        }
    }
}

} // namespace semantic
} // namespace pdf2htmlEX
