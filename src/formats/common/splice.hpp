#pragma once

// Span-splice writer (SPEC §2.3): re-emit the source verbatim except for replaced byte ranges.

#include <semsplinter/error.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

// Replace source[begin, end) with text; begin == end inserts.
struct Splice {
    std::size_t begin = 0, end = 0;
    std::string text;
};

// Edits may come in any order but must not overlap. Insertions at one offset keep their given order.
inline std::string apply_splices(std::string_view source, std::vector<Splice> edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const Splice& x, const Splice& y) { return x.begin < y.begin; });
    std::string out;
    std::size_t at = 0;
    for (auto& e : edits) {
        if (e.begin < at || e.end < e.begin || e.end > source.size())
            throw Error(ErrorCategory::internal, "overlapping or out-of-range splice");
        out.append(source.substr(at, e.begin - at));
        out += e.text;
        at = e.end;
    }
    out.append(source.substr(at));
    return out;
}

} // namespace semsplinter
