#pragma once

// The SQLite module (docs/formats/sqlite.md, MVP): schema and row diff through the SQLite library, rows
// keyed by primary key or rowid and merged per cell, and a writer that applies the merge to a copy of ours
// so that pages SQLite does not touch keep their bytes.

#include <semsplinter/format.hpp>

#include <memory>

namespace semsplinter::sqlite {

std::unique_ptr<Format> make_format();

} // namespace semsplinter::sqlite
