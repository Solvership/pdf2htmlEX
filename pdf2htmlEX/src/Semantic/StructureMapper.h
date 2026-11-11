/*
 * StructureMapper.h
 *
 * Reads the structure tree of a tagged PDF and maps marked content ids
 * (MCID) of every page to table cells and text blocks.
 */

#ifndef STRUCTUREMAPPER_H__
#define STRUCTUREMAPPER_H__

#include <map>
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

    std::map<int, std::map<int, PageInput::CellTag>> cells;   // page -> mcid -> cell
    std::map<int, std::map<int, PageInput::BlockTag>> blocks; // page -> mcid -> block
    int table_counter = 0;
    int block_counter = 0;
};

} // namespace semantic
} // namespace pdf2htmlEX

#endif // STRUCTUREMAPPER_H__
