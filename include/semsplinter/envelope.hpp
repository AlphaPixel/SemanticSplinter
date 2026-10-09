#pragma once

#include <semsplinter/io.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

// Byte classes of SPEC §12.1. `unexplained` covers bytes no range claims (churn only).
enum class ByteClass : std::uint8_t {
    content,
    framing,
    integrity,
    authenticity,
    encoding,
    derived,
    volatile_,
    unexplained
};
inline constexpr std::size_t kByteClasses = 8;

std::string_view to_string(ByteClass) noexcept;

struct EnvRange {
    std::uint64_t offset = 0, length = 0;
    ByteClass cls = ByteClass::content;
    std::string unit;  // address of the semantic unit the bytes belong to
    std::string layer; // codec layer for encoding ranges ("zlib", "deflate", ...)
};

// Sorted, non-overlapping classified ranges of one file (SPEC §12.3).
struct EnvelopeMap {
    std::vector<EnvRange> ranges;

    // Throws Error{internal} unless ranges are sorted, non-overlapping, and within `file_size`.
    void validate(std::uint64_t file_size) const;
    static EnvelopeMap whole_content(std::uint64_t file_size);
};

struct Churn {
    std::array<std::uint64_t, kByteClasses> by_class{};
    std::uint64_t total() const noexcept;
    nlohmann::ordered_json to_json() const; // {"bytes_changed": n, "content": n, ...} as in SPEC §7.1
};

// Changed bytes between two files, attributed by aligning their maps: bytes are compared per
// (unit, class, layer); bytes no range covers count as `unexplained` (SPEC §12.6).
// ponytail: per-key compare is positional for equal lengths, else prefix/suffix trimmed; an aligned
// byte diff per key is the upgrade if insertions inside one unit overstate churn.
Churn attribute_churn(std::span<const std::byte> a, const EnvelopeMap& ma, std::span<const std::byte> b,
                      const EnvelopeMap& mb);

// Checksums in core (SPEC §12.8). Incremental: pass the previous result to continue.
std::uint32_t crc32(std::span<const std::byte>, std::uint32_t crc = 0) noexcept; // zlib/PNG CRC-32
std::uint32_t adler32(std::span<const std::byte>, std::uint32_t adler = 1) noexcept;

} // namespace semsplinter
