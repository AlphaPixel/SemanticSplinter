#pragma once

// Shared text helpers (SPEC §2.3 `src/core/text`): encoding detection and line splitting with byte
// offsets into the original buffer. Used by the text module and by structured text formats.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

enum class Encoding : std::uint8_t { utf8, utf16le, utf16be, utf32le, utf32be };
enum class Eol : std::uint8_t { none, lf, crlf, cr };

std::string_view to_string(Encoding) noexcept;
std::string_view to_string(Eol) noexcept;

struct EncodingInfo {
    Encoding encoding = Encoding::utf8;
    std::size_t bom = 0; // BOM length in bytes (0 when absent)
};

// BOM first, then the BOM-less UTF-16 NUL-parity heuristic (text.md §2, Q-TEXT-10), else UTF-8.
EncodingInfo detect_encoding(std::span<const std::byte>) noexcept;

// GNU rule: a NUL byte in the first 8 KiB means binary. UTF-16/32 (with BOM or detected) is text.
bool looks_binary(std::span<const std::byte>) noexcept;

// A line as a byte span of the original buffer. `len` excludes the terminator.
struct Line {
    std::size_t off = 0, len = 0;
    Eol eol = Eol::none;
};

enum class SplitMode : std::uint8_t {
    lf,  // GNU: only LF terminates a line; a CR before it stays in the content
    any, // --eol=auto: LF, CRLF, and lone CR
};

// Splits the bytes after the BOM. For UTF-16/32 the terminators are matched as whole code units.
std::vector<Line> split_lines(std::span<const std::byte>, const EncodingInfo&, SplitMode = SplitMode::lf);

// The bytes of a line (without terminator) as UTF-8. UTF-8 input is returned as is (no validation);
// UTF-16/32 is transcoded, with U+FFFD for ill-formed sequences.
std::string line_utf8(std::span<const std::byte>, const Line&, Encoding);

// Encodes UTF-8 text in `enc` (used to write merged output and markers in the file's encoding).
std::string encode_utf8_as(std::string_view utf8, Encoding enc);

} // namespace semsplinter
