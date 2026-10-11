#pragma once

// The shared TIFF/Exif IFD layer (raster-misc.md §3.1, jpeg.md §7.2): parses a TIFF structure (a TIFF or
// BigTIFF file, or the Exif payload after "Exif\0\0") into IFDs with their raw values, and writes IFDs on the
// Emitter. Part of the raster helper, so TIFF and JPEG share it without depending on each other (D-28).

#include <semsplinter/emit.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace semsplinter::ifd {

// Field types (TIFF 6.0, BigTIFF). type_size() is 0 for unknown types.
enum Type : std::uint16_t {
    BYTE = 1,
    ASCII,
    SHORT,
    LONG,
    RATIONAL,
    SBYTE,
    UNDEFINED,
    SSHORT,
    SLONG,
    SRATIONAL,
    FLOAT,
    DOUBLE,
    IFD,
    LONG8 = 16,
    SLONG8,
    IFD8
};
unsigned type_size(std::uint16_t type) noexcept;

// Tags whose values point at further IFDs: SubIFDs, Exif, GPS, Interoperability.
bool is_pointer_tag(std::uint16_t tag) noexcept;

struct Ifd;

struct Entry {
    std::uint16_t tag = 0, type = 0;
    std::uint64_t count = 0;
    std::uint64_t at = 0; // offset of the value bytes in the block
    bool in_line = true;  // the value sits inside the entry
    std::span<const std::byte>
        value;            // count * type_size bytes in the block's byte order (unknown type: empty)
    std::vector<Ifd> sub; // the IFDs a pointer tag points at
};

struct Ifd {
    std::uint64_t offset = 0; // in the block
    std::uint64_t size = 0;   // count field, entries, and next pointer
    std::vector<Entry> entries;
    std::uint64_t next = 0; // NextIFD as stored
    const Entry* find(std::uint16_t tag) const noexcept;
};

struct Tiff {
    bool big_endian = false, bigtiff = false;
    std::uint64_t first = 0; // the header's IFD0 offset
    std::vector<Ifd> chain;  // IFD0, IFD1, ... by NextIFD
    unsigned offset_size() const noexcept { return bigtiff ? 8 : 4; }

    std::uint64_t get(std::span<const std::byte>, unsigned width) const noexcept; // an unsigned number
    // The value's i-th element as an unsigned integer (BYTE, SHORT, LONG, LONG8, IFD, IFD8, and the signed
    // variants reinterpreted). RATIONAL counts 32-bit words: numerator at 2k, denominator at 2k + 1.
    std::uint64_t number(const Entry&, std::size_t i = 0) const;
    std::vector<std::uint64_t> numbers(const Entry&) const;
};

// Parses the header and the IFD chain with its pointer-tag sub-IFDs. Offsets are relative to the block.
// Rejects values or IFDs outside the block, IFD cycles, and more than `max_ifds` IFDs. Throws
// Error{invalid_input}.
Tiff parse(std::span<const std::byte> block, std::size_t max_ifds = 4096);

// ASCII value up to its first NUL.
std::string text(const Entry&);

// One entry of an IFD being written. `value` holds count * type_size bytes in the block's byte order, as
// bytes or fields (offsets to other groups); when `kept` is set, an out-of-line value stays where it is in
// the block and the entry stores that offset (stable layout).
struct OutEntry {
    std::uint16_t tag = 0, type = 0;
    std::uint64_t count = 0;
    std::vector<emit::Seg> value;
    std::optional<std::uint64_t> kept;
};

// Writes IFDs for one TIFF structure. Offsets are absolute, or relative to `relative_to` (the TIFF header
// of an Exif block).
struct Writer {
    bool big_endian = false, bigtiff = false;
    std::optional<emit::GroupId> relative_to;
    emit::GroupId* ids = nullptr; // group id allocator shared with the caller

    emit::GroupId id() const { return (*ids)++; }
    emit::Endian endian() const { return big_endian ? emit::Endian::big : emit::Endian::little; }
    emit::Expr offset(emit::GroupId g) const;
    emit::Seg number(std::uint64_t v, unsigned width) const;
    emit::Seg pointer(emit::GroupId g) const;  // an offset field (4 or 8 bytes)
    emit::Seg pointer(std::uint64_t at) const; // a constant offset field

    OutEntry copy(const Entry&) const; // the value bytes as stored
    OutEntry keep(const Entry&) const; // out-of-line values stay at their offset
    OutEntry numbers(std::uint16_t tag, std::uint16_t type, const std::vector<std::uint64_t>&) const;

    // The IFD as group `id`, entries sorted by tag, followed by its out-of-line values (word-aligned by
    // absolute position, so a block that starts at an odd file offset needs its own padding). `next` is the
    // NextIFD field: pointer(group) or pointer(constant).
    emit::Seg write(emit::GroupId id, std::vector<OutEntry> entries, emit::Seg next) const;
};

} // namespace semsplinter::ifd
