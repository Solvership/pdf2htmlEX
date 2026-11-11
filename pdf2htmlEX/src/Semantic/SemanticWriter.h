/*
 * SemanticWriter.h
 *
 * Output of the semantic layer: hidden in-page semantic HTML, and the
 * <name>.semantic.json / <name>.tables.html side-car files.
 */

#ifndef SEMANTICWRITER_H__
#define SEMANTICWRITER_H__

#include <ostream>
#include <string>
#include <vector>

#include "Semantic.h"

namespace pdf2htmlEX {
namespace semantic {

class SemanticWriter
{
public:
    explicit SemanticWriter(const Param & param);

    // hidden semantic copy of a page, written inside the page container
    void write_inline(std::ostream & out, const PageResult & page) const;

    // remember a page for the side-car files (chars are dropped)
    void add_page(const PageResult & page);

    // link tables continued across pages and write the side-car files
    void finish();

    bool want(const char * what) const;

private:
    void link_continuations();
    void write_json(const std::string & path) const;
    void write_tables_html(const std::string & path) const;
    void write_table_html(std::ostream & out, const std::vector<const Table *> & parts, bool hidden_ids) const;

    const Param & param;
    Options opt;
    std::vector<PageResult> pages;
};

} // namespace semantic
} // namespace pdf2htmlEX

#endif // SEMANTICWRITER_H__
