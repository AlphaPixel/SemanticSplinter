#pragma once

// Options for text-bearing units, carried through the Format contract so that every route to a text
// merge (top level, container members, the library API) behaves the same (CLI.md §2.1, text.md §4-§6).

#include <semsplinter/linediff.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/text.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace semsplinter {

struct TextLoadOptions {
    SplitMode split = SplitMode::lf; // `any` for --eol=auto
    bool strip_trailing_cr = false;  // --strip-trailing-cr: drop CR before LF from the content
    bool bom_is_content = false;     // GNU emulation: a UTF-8 BOM is part of the first line
};

struct TextDiffOptions {
    LineNormalize normalize;             // -i -E -Z -b -w
    LineDiffOptions algorithm;           // --diff-algorithm, -d, --horizon-lines
    bool ignore_blank_lines = false;     // -B
    bool ignore_missing_newline = false; // ed output cannot express it; GNU compares as if present
    std::vector<std::regex> ignore_re;   // -I (repeatable)
};

enum class MarkerStyle : std::uint8_t { merge, diff3, zdiff3 };

// GNU diff3 output selection: -A, -E, -e, -x (GNU 3.12 prints -X like -x), -3.
enum class Diff3Mode : std::uint8_t { show_all, show_overlap, ed, overlap_only, easy_only };

enum class MergeEngine : std::uint8_t {
    gnu,    // GNU diff3: no refinement; -A brackets identical changes; Diff3Mode applies
    git,    // git merge-file: zealous-alnum refinement (merge style only)
    native, // splinter merge: like git, histogram by default
};

struct TextMergeOptions {
    MergeEngine engine = MergeEngine::native;
    LineDiffOptions algorithm{DiffAlgorithm::histogram, true, Compaction::gnu};
    MarkerStyle style = MarkerStyle::merge;
    Diff3Mode gnu_mode = Diff3Mode::show_all;
    std::size_t marker_size = 7;
    std::array<std::string, 3> labels; // ours, base, theirs (marker text)
    ConflictOptions conflict{Policy::mark};
    bool eol_auto = false;     // --eol=auto: compare without terminators, lift whole-file EOL conversions
    bool crlf_markers = false; // markers end with CRLF when ours' dominant terminator is CRLF (Git)
};

} // namespace semsplinter
