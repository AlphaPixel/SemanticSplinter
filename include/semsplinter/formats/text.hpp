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
#include <semsplinter/textopts.hpp>

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

// The option types live in core (textopts.hpp) so they travel through the Format contract.
using LoadOptions = TextLoadOptions;
using DiffOptions = TextDiffOptions;
using MergeOptions = TextMergeOptions;
using Engine = MergeEngine;
using semsplinter::Diff3Mode;
using semsplinter::MarkerStyle;

std::unique_ptr<TextDoc> load(ByteSource, const LoadOptions& = {});

// ---- Diff ----

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
