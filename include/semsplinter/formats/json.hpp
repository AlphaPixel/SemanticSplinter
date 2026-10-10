#pragma once

// The JSON module (docs/formats/json.md): own span-recording parser (strict JSON plus JSONC), tree diff
// with JSON Pointer addresses, KeyedMerge on objects, SequenceMerge on arrays with automatic identity keys,
// and a span-splice writer that keeps ours' bytes everywhere outside the merged units.

#include <semsplinter/format.hpp>

#include <memory>

namespace semsplinter::json {

std::unique_ptr<Format> make_format();

} // namespace semsplinter::json
