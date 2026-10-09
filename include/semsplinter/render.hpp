#pragma once

// GNU diff output formats over two line sequences and an edit script (text.md §5.2, SPEC §7.2). Used by the
// text module directly and by structured formats over their dump lines.

#include <semsplinter/linediff.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

struct DiffSide {
    std::vector<std::string_view> lines; // without terminators
    bool missing_newline = false;        // the last line has no terminator
    std::string label;                   // header text after "--- " / "*** ", timestamp included if any
};

enum class OutputStyle : std::uint8_t { normal, unified, context, side_by_side, ed, forward_ed, rcs };

// GNU --palette keys. Empty strings disable a color.
struct Palette {
    std::string rs = "0", hd = "1", ad = "32", de = "31", ln = "36";
};

struct RenderOptions {
    std::size_t context = 3;
    bool initial_tab = false;          // -T
    bool expand_tabs = false;          // -t
    bool suppress_blank_empty = false; // --suppress-blank-empty
    std::size_t tabsize = 8;
    // -p / -F: true for lines that may head a hunk. Null when off.
    std::function<bool(std::string_view)> is_function;
    std::size_t width = 130;            // -W (side by side)
    bool left_column = false;           // --left-column
    bool suppress_common_lines = false; // --suppress-common-lines
    const Palette* color = nullptr;     // --color: null when off
    // Optional text appended to hunk headers ("@@ ... @@ ADDRESS" for structured formats).
    std::function<std::string(const Edit& first, const Edit& last)> hunk_address;
};

// `ignorable` (same length as `edits`, or empty) marks changes dropped by -B / -I: they never start a
// hunk, but are shown when they fall inside one, as GNU does.
void render(std::string& out, OutputStyle, const DiffSide& a, const DiffSide& b, std::span<const Edit> edits,
            std::span<const char> ignorable, const RenderOptions& = {});

// Parses a GNU palette string ("ad=1;32:de=1;31"). Throws Error{invalid_input} on unknown keys.
Palette parse_palette(std::string_view, Palette base = {});

} // namespace semsplinter
