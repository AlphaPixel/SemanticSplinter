#pragma once

// The text module (docs/formats/text.md): line model, GNU-compatible diff, and diff3 merges in three
// flavours: GNU diff3 (byte-compatible), Git merge-file (Git-compatible), and native splinter.

#include <semsplinter/format.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/linediff.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/model.hpp>
#include <semsplinter/render.hpp>
#include <semsplinter/text.hpp>

#include <array>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace semsplinter::text {

struct TextDoc final : Document {
    ByteSource src;
    EncodingInfo enc;
    std::vector<Line> lines;
    std::vector<std::string> decoded; // UTF-8 line content, only when the file is UTF-16/32

    std::string_view line(std::size_t i) const; // content without terminator, as UTF-8
    bool missing_newline() const noexcept { return !lines.empty() && lines.back().eol == Eol::none; }
    Eol dominant_eol() const noexcept; // most frequent terminator; lf when there is none
};

struct LoadOptions {
    SplitMode split = SplitMode::lf; // `any` for --eol=auto
    bool strip_trailing_cr = false;  // --strip-trailing-cr: drop CR before LF from the content
    bool bom_is_content = false;     // GNU emulation: a UTF-8 BOM is part of the first line
};

std::unique_ptr<TextDoc> load(ByteSource, const LoadOptions& = {});

// ---- Diff ----

struct DiffOptions {
    LineNormalize normalize;
    LineDiffOptions algorithm;
    bool ignore_blank_lines = false;     // -B
    bool ignore_missing_newline = false; // ed output cannot express it; GNU compares as if present
    std::vector<std::regex> ignore_re;   // -I (repeatable)
};

struct TextDiff {
    std::vector<Edit> edits;
    std::vector<char> ignorable; // parallel to edits: dropped by -B / -I
    bool differs() const noexcept;
};

TextDiff diff(const TextDoc& a, const TextDoc& b, const DiffOptions& = {});
DiffSide diff_side(const TextDoc&, std::string label);
ChangeSet changes(const TextDoc& a, const TextDoc& b, const TextDiff&); // for the JSON report
nlohmann::ordered_json metrics(const TextDoc& a, const TextDoc& b, const TextDiff&);

// ---- Merge (text.md §6) ----

enum class MarkerStyle : std::uint8_t { merge, diff3, zdiff3 };

// GNU diff3 output selection: -A, -E, -e, -x (GNU 3.12 prints -X like -x), -3.
enum class Diff3Mode : std::uint8_t { show_all, show_overlap, ed, overlap_only, easy_only };

enum class Engine : std::uint8_t {
    gnu,    // GNU diff3: no refinement; -A brackets identical changes; Diff3Mode applies
    git,    // git merge-file: zealous-alnum refinement (merge style only)
    native, // splinter merge: like git, histogram by default
};

struct MergeOptions {
    Engine engine = Engine::native;
    LineDiffOptions algorithm{DiffAlgorithm::histogram, true, Compaction::gnu};
    MarkerStyle style = MarkerStyle::merge;
    Diff3Mode gnu_mode = Diff3Mode::show_all;
    std::size_t marker_size = 7;
    std::array<std::string, 3> labels; // ours, base, theirs (marker text)
    ConflictOptions conflict{Policy::mark};
    bool eol_auto = false;     // --eol=auto: compare without terminators, lift whole-file EOL conversions
    bool crlf_markers = false; // markers end with CRLF when ours' dominant terminator is CRLF (Git)
};

struct TextMerge {
    std::vector<std::byte> bytes;
    MergeResult result;
    std::size_t marked = 0; // conflict blocks written with markers
};

TextMerge merge(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs, const MergeOptions&);

// GNU diff3 without -m: the "====" report, or an ed script for Diff3Mode (with -i: "w" and "q" appended).
// Labels: mine, older, yours.
std::string diff3_report(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs,
                         const std::array<std::string, 3>& labels, bool initial_tab);
std::string diff3_ed(const TextDoc& base, const TextDoc& ours, const TextDoc& theirs, Diff3Mode,
                     const std::array<std::string, 3>& labels, bool write_quit);

// ---- Format module ----

std::unique_ptr<Format> make_format();

} // namespace semsplinter::text
