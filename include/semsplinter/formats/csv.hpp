#pragma once

// The CSV/TSV module (docs/formats/csv.md): own tokenizer with raw field spans, dialect sniffing (comma,
// semicolon, tab, pipe; BOM, UTF-16, CRLF, `sep=`), rows merged by key column or by position with a
// three-way merge per cell, column add/delete, and a span-splice writer that keeps ours' bytes outside
// the rebuilt records.

#include <semsplinter/format.hpp>

#include <memory>

namespace semsplinter::csv {

std::unique_ptr<Format> make_format();

} // namespace semsplinter::csv
