#pragma once

// Patch files (SPEC §6): splinter envelope sections and unified diffs, their parser and writer, the payload
// codec, and GNU-compatible application of unified hunks to lines.

#include <semsplinter/hash.hpp>
#include <semsplinter/model.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

// Payload codec (D-7): zlib, then base85 with Git's alphabet. encode_payload() returns the compressed size
// and a newline, then lines of at most 80 characters (64 bytes).
std::string encode_payload(std::span<const std::byte>);
// Decodes `n` compressed bytes from base85 `lines`; inflates at most `limit` bytes.
std::vector<std::byte> decode_payload(std::string_view lines, std::size_t n, std::uint64_t limit);

// One touched unit of an envelope section, with its value on each side (nullopt: the unit is absent).
struct UnitHunk {
    Op op = Op::update;
    Address at;
    std::string id;
    std::optional<std::string> before_text, after_text; // informational "- " / "+ " lines
    std::optional<std::vector<std::byte>> before, after;
};

// One unified hunk: `lines` start with ' ', '-', or '+'; `no_eol_old` / `no_eol_new` record a
// "\ No newline at end of file" after the last old / new line.
struct TextHunk {
    std::size_t old_start = 0, old_len = 0, new_start = 0, new_len = 0;
    std::string tail; // after the closing "@@"
    std::vector<std::string> lines;
    bool no_eol_old = false, no_eol_new = false;
};

struct PatchSection {
    std::string old_name, new_name; // as written, without timestamps or hashes; "/dev/null" for absent
    std::size_t line = 0;           // 1-based line of the "---" header, for messages
    std::vector<std::string>
        preamble; // the text up to the first hunk, headers included (shown when the file is missing)
    // Envelope sections only.
    std::string format; // empty: a unified diff
    std::optional<Hash128> old_hash, new_hash;
    std::vector<UnitHunk> units;
    // Unified diffs only.
    std::vector<TextHunk> hunks;

    std::size_t hunk_count() const noexcept { return format.empty() ? hunks.size() : units.size(); }
    void reverse(); // swaps the sides: names, hashes, and every hunk
};

// Splits a patch into sections. Text that belongs to no section (mail headers, "diff" lines) is skipped, as
// GNU patch does. Throws Error{invalid_input} on a malformed envelope or hunk.
std::vector<PatchSection> parse_patch(std::string_view, std::uint64_t payload_limit);

// An envelope section ("splinter-patch 1" first), or a unified diff when `format` is empty.
std::string write_section(const PatchSection&);

// GNU patch's matching and application of unified hunks (patch 2.7 locate_hunk / apply_hunk).
struct TextApplyOptions {
    std::size_t fuzz = 2;
    bool ignore_whitespace = false;                                                        // -l
    enum class Reversed : std::uint8_t { skip, assume, ignore } reversed = Reversed::skip; // -N, -t, -f
    bool reverse_given = false; // -R: no reversed-patch detection
};

struct TextApplyResult {
    std::vector<std::string> lines;  // the patched file, each line with its terminator
    std::string messages;            // "Hunk #2 succeeded at 14 (offset 3 lines)." ...
    std::vector<std::size_t> failed; // indices of hunks that did not apply
    bool skipped = false;            // a reversed patch was skipped: every hunk is ignored
    bool reversed = false;           // a reversed patch was detected and applied as -R
    bool mismatch = false;           // some hunk needed an offset or fuzz, or failed
};

// `file` holds the lines of the target with their terminators. `section` may be reversed in place (-t).
TextApplyResult apply_text(std::vector<std::string> file, PatchSection& section, const TextApplyOptions&);

// Splits text into lines that keep their "\n".
std::vector<std::string> split_lines(std::string_view);

} // namespace semsplinter
