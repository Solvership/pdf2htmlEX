/*
 * Semantic.h
 *
 * Semantic layer on top of the (pixel exact) visual output of pdf2htmlEX:
 * detection of tables (from the PDF structure tree, from ruling lines or from
 * text alignment) and paragraphs.
 *
 * The visual output is never changed by this layer: it only adds data-*
 * attributes to the text elements and optionally emits a hidden semantic
 * copy of the page and side-car files (JSON / HTML).
 *
 * All geometry in this module is in "visual page space": PDF points, origin at
 * the top-left corner of the (rotated, cropped) page, y growing downwards.
 */

#ifndef SEMANTIC_H__
#define SEMANTIC_H__

#include <string>
#include <vector>
#include <map>
#include <memory>

#include "Param.h"

namespace pdf2htmlEX {
namespace semantic {

struct Box
{
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    Box() { }
    Box(double x0, double y0, double x1, double y1) : x0(x0), y0(y0), x1(x1), y1(y1) { }

    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
    double cx() const { return (x0 + x1) / 2; }
    double cy() const { return (y0 + y1) / 2; }
    bool valid() const { return x1 >= x0 && y1 >= y0 && !(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0); }
    bool contains(double x, double y, double tol = 0) const
    {
        return x >= x0 - tol && x <= x1 + tol && y >= y0 - tol && y <= y1 + tol;
    }
    void expand(const Box & b)
    {
        if (!b.valid()) return;
        if (!valid()) { *this = b; return; }
        if (b.x0 < x0) x0 = b.x0;
        if (b.y0 < y0) y0 = b.y0;
        if (b.x1 > x1) x1 = b.x1;
        if (b.y1 > y1) y1 = b.y1;
    }
};

// one drawn glyph
struct Char
{
    int index = -1;            // page level drawing order
    int line = -1;             // index of the HTML text line containing it (-1: none)
    Box box;                   // glyph box (visual page space)
    double em = 0;             // font size in pt
    std::string text;          // UTF-8, empty for padding glyphs
    bool space_before = false; // a word gap precedes the glyph in its HTML line
    int mcid = -1;             // marked content id (tagged PDF)
    int dir = 0;               // writing direction: 0 left to right, 1 bottom to top, 2 right to left (upside down), 3 top to bottom
    bool is_space() const { return text == " " || text == "\xc2\xa0"; }
};

// axis-aligned ruling line (normalized: x0 <= x1, y0 <= y1)
struct Rule
{
    double x0, y0, x1, y1;
    bool horizontal;
    int kind = 0;          // 0: stroked line, 1: edge of a filled box
    double pos() const { return horizontal ? y0 : x0; }
    double lo() const { return horizontal ? x0 : y0; }
    double hi() const { return horizontal ? x1 : y1; }
};

// a raw drawing reported by DrawingTracer (in tracer space)
struct RawRule
{
    double x0, y0, x1, y1, width;
    int kind;
    bool white = false; // invisible on a white page
};

struct Cell
{
    int row = 0, col = 0, rowspan = 1, colspan = 1;
    bool header = false;
    Box box;
    std::vector<int> chars;          // indices into PageInput::chars
    std::vector<std::string> lines;  // visual lines of text
    std::string text;                // lines joined
};

struct Table
{
    std::string id;           // unique in the document, e.g. "p2t0"
    int page = 0;
    std::string source;       // "tagged", "ruled", "hrules" or "stream"
    int rows = 0, cols = 0;
    Box box;
    std::vector<Cell> cells;
    std::string struct_key;   // tagged tables: identity of the structure element
    std::string continued_from; // id of the table on a previous page this one continues
    std::string group;        // id of the first table of a continued chain
    bool repeated_header = false; // first row repeats the header of the chain's first table
    int order = 0;            // reading order among the blocks of the page
    std::vector<double> col_bounds; // column boundaries (x), size cols+1, if known
};

struct Paragraph
{
    std::string id;           // e.g. "p2p5"
    int page = 0;
    std::string tag = "p";    // p, h1..h6, li, caption
    int order = 0;            // reading order among the blocks of the page
    Box box;
    std::vector<int> chars;
    std::vector<std::string> lines;
    std::string text;
};

struct PageInput
{
    int page = 0;
    double width = 0, height = 0;     // pt
    std::vector<Char> chars;          // indexed by Char::index
    std::vector<Rule> rules;
    // tagged PDF: mcid -> table cell / block
    struct CellTag { std::string table_key; int row, col, rowspan, colspan; bool header; };
    struct BlockTag { std::string key; std::string tag; };
    std::map<int, CellTag> mcid_cells;
    std::map<int, BlockTag> mcid_blocks;
};

struct PageResult
{
    int page = 0;
    double width = 0, height = 0;
    std::vector<Table> tables;
    std::vector<Paragraph> paragraphs;
    std::vector<int> char_group;            // per char index: index into group_attrs or -1
    std::vector<std::string> group_attrs;   // attribute string (starting with a space)
    std::vector<std::string> group_keys;    // compact key: "t:<table>:<row>:<col>" or "p:<para>"
    // chars, kept for copy-paste reconstruction etc.
    std::vector<Char> chars;
};

/*
 * Options derived from Param
 */
struct Options
{
    bool detect_tagged = true;
    bool detect_ruled = true;
    bool detect_hrules = true;
    bool detect_stream = false;
    bool paragraphs = true;
    bool dehyphenate = false;
    bool cell_join_br = false;

    static Options from_param(const Param & param);
};

/*
 * Text layout helpers
 */
struct VLine
{
    Box box;
    std::vector<int> chars; // in reading order
    double em = 0;
    int dir = 0;            // writing direction of the chars (see Char::dir)
};

// box of a char in its reading frame: x along the baseline, y across (next line)
Box reading_box(const Char & c);

struct Segment
{
    Box box;
    std::vector<int> chars;
    int vline = -1;
};

// Group chars (indices into `chars`) into visual lines, top to bottom.
std::vector<VLine> build_visual_lines(const std::vector<Char> & chars, const std::vector<int> & subset);
// Split a visual line into segments at horizontal gaps larger than gap_em * em.
std::vector<Segment> split_segments(const std::vector<Char> & chars, const VLine & line, double gap_em);
// Text of a sequence of chars (in reading order), inserting spaces at word gaps.
std::string chars_text(const std::vector<Char> & chars, const std::vector<int> & seq);
// Join visual lines of a block into one string.
std::string join_lines(const std::vector<std::string> & lines, bool dehyphenate);

/*
 * Table detectors. `taken` marks chars already assigned to a table.
 */
std::vector<Table> detect_ruled_tables(const PageInput & in, std::vector<bool> & taken, std::vector<Rule> & used_rules_out);
std::vector<Table> detect_hrule_tables(const PageInput & in, const std::vector<Rule> & free_rules, std::vector<bool> & taken);
std::vector<Table> detect_stream_tables(const PageInput & in, std::vector<bool> & taken);

/*
 * Run the whole analysis for one page.
 */
PageResult analyze_page(const PageInput & in, const Options & opt);

// fill lines/text of the cells, and table/cell boxes
void finalize_table(const PageInput & in, Table & t, const Options & opt);

/*
 * Escaping helpers
 */
std::string json_escape(const std::string & s);
std::string html_escape(const std::string & s);

} // namespace semantic
} // namespace pdf2htmlEX

#endif // SEMANTIC_H__
