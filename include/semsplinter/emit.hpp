#pragma once

#include <semsplinter/io.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The fix-up Emitter (SPEC §12.4): formats describe content and envelope fields as a segment tree;
// the Emitter lays it out and computes sizes, offsets, and checksums. Later formats add expressions
// (encode(), count_of, digests) as they need them.
namespace semsplinter::emit {

using GroupId = std::uint32_t;
enum class Endian : std::uint8_t { little, big };
enum class VarEncoding : std::uint8_t { decimal, uleb128 }; // decimal: ASCII digits, zero-padded when grown

struct Expr {
    enum class Kind : std::uint8_t { constant, size_of, offset_of, crc32, adler32 } kind = Kind::constant;
    std::uint64_t value = 0;
    GroupId group = 0;
    GroupId relative_to = 0;
    bool relative = false;
};

Expr constant(std::uint64_t);
Expr size_of(GroupId);
Expr offset_of(GroupId);                      // absolute file offset
Expr offset_of(GroupId, GroupId relative_to); // offset of g minus offset of relative_to (must be >= 0)
Expr crc32(GroupId);
Expr adler32(GroupId);

struct Seg {
    enum class Kind : std::uint8_t { bytes, raw, group, align, field, varfield } kind = Kind::bytes;
    std::vector<std::byte> data; // bytes
    ByteSource source;           // raw
    std::uint64_t offset = 0, length = 0;
    GroupId id = 0; // group
    std::vector<Seg> children;
    unsigned boundary = 1; // align
    std::byte fill{0};
    unsigned width = 0; // field: 1, 2, 4, or 8 bytes
    Endian endian = Endian::little;
    VarEncoding encoding = VarEncoding::decimal;
    Expr expr;
};

Seg bytes(std::span<const std::byte>);
Seg bytes(std::string_view);
Seg raw(const ByteSource&, std::uint64_t offset, std::uint64_t length); // verbatim copy from an input
Seg group(GroupId, std::vector<Seg>);                                   // addressable region
Seg align(unsigned boundary, std::byte fill = std::byte{0});            // pad to an absolute boundary
Seg field(unsigned width, Endian, Expr);                                // fixed-width number
Seg varfield(VarEncoding, Expr); // variable-width number; set .width first for a zero-padded minimum
                                 // (PDF's 10-digit xref offsets)

// Lays out to a fixed point (at most 16 iterations; variable widths only grow), then evaluates fields,
// checksums innermost first; a checksum field inside its own group is zero while it is summed.
// Throws Error{internal} on duplicate or unknown groups, overflowing fields, or no fixed point.
// ponytail: builds the whole output in memory; stream raw segments > 1 MiB when large files arrive.
std::vector<std::byte> emit(const Seg& root);

} // namespace semsplinter::emit
