/*
 * StructureMapper.h
 *
 * Reads the structure tree of a tagged PDF and maps marked content ids
 * (MCID) of every page to table cells and text blocks.
 */

#ifndef STRUCTUREMAPPER_H__
#define STRUCTUREMAPPER_H__

#include <map>
#include <set>
#include <string>

#include "Semantic.h"

class PDFDoc;
class StructElement;

namespace pdf2htmlEX {
namespace semantic {

class StructureMapper
{
public:
    // returns false if the document has no usable structure tree
    bool load(PDFDoc * doc);
    // copy the tags of `page` into `in`
    void fill(int page, PageInput & in) const;

    int table_count() const { return table_counter; }

private:
    struct Ctx;
    void walk(const StructElement * e, Ctx & ctx);
    void process_table(const StructElement * table, Ctx & ctx);
    void process_block(const StructElement * e, const std::string & tag, Ctx & ctx);
    // collect (page, mcid) pairs of a subtree
    void collect_mcids(const StructElement * e, Ctx & ctx, std::vector<std::pair<int, int>> & out);
    // remember the mcids of the inline Span elements of a subtree
    void collect_spans(const StructElement * e, Ctx & ctx);

    std::map<int, std::map<int, PageInput::CellTag>> cells;   // page -> mcid -> cell
    std::map<int, std::map<int, PageInput::BlockTag>> blocks; // page -> mcid -> block
    std::map<int, std::set<int>> span_mcids; // page -> mcids of inline Span elements of blocks
    int table_counter = 0;
    int block_counter = 0;
    int seq_counter = 0; // order of blocks and tables in the structure tree
};

} // namespace semantic
} // namespace pdf2htmlEX

#endif // STRUCTUREMAPPER_H__
