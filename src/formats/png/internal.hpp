#pragma once

// Private to the PNG module: the chunk layer, the loaded document, and the codec (png.md §2, §7).

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/png.hpp>
#include <semsplinter/formats/raster.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/limits.hpp>
#include <semsplinter/model.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace semsplinter::png {

inline constexpr std::array<std::uint8_t, 8> kSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

struct Chunk {
    std::uint64_t offset = 0; // of the length field
    std::uint32_t length = 0;
    std::string type; // four ASCII letters
    std::uint32_t crc = 0;
    bool crc_ok = true;

    std::uint64_t data() const noexcept { return offset + 8; }
    std::uint64_t end() const noexcept { return offset + 12 + length; }
    bool ancillary() const noexcept { return type[0] & 0x20; }
    bool safe_to_copy() const noexcept { return type[3] & 0x20; }
};

// Splits the file into chunks and checks framing: signature, lengths inside the file and <= 2^31-1,
// IHDR first with 13 bytes, IEND present. CRCs are verified, not enforced. Throws Error{invalid_input}.
std::vector<Chunk> parse_chunks(std::span<const std::byte> file);

// One ancillary unit (png.md §3): an address, the hash of its canonical value, and how to show it.
struct Entry {
    std::string key;                 // /color/gAMA, /chunks/tEXt:Author, /animation, ...
    Hash128 value;                   // canonical value (decompressed text, profile, chunk data)
    std::string text;                // rendering for reports and dump
    std::string kind;                // color, text, metadata, animation
    std::string encoding;            // chunk type, plus "+z" when compressed: tEXt, zTXt, iTXt+z, ...
    std::vector<std::size_t> chunks; // indices into PngDoc::chunks (several for /animation)
};

using Rgba8 = std::array<std::uint8_t, 4>;

struct PngDoc final : Document {
    ByteSource src;
    std::vector<Chunk> chunks;
    std::uint32_t width = 0, height = 0;
    Encoding enc;
    std::vector<Rgba8> palette;                      // PLTE with tRNS alpha (color type 3)
    std::optional<std::array<std::uint16_t, 3>> key; // tRNS key color as stored samples (types 0, 2)
    raster::Raster pixels;
    std::vector<Entry> entries;
    EnvelopeMap map;
    std::vector<std::string> crc_mismatch; // addresses of chunks whose stored CRC is wrong
    std::vector<Warning> warnings;

    std::span<const std::byte> bytes(const Chunk& c) const {
        return src.bytes().subspan(c.offset, c.end() - c.offset);
    }
    std::span<const std::byte> data(const Chunk& c) const { return src.bytes().subspan(c.data(), c.length); }
    std::vector<std::byte> idat_stream() const; // the concatenated IDAT payload
};

// Decodes the critical chunks through libpng into the canonical grid (png.md §3.1): no gamma, ICC, or
// cICP transform; bit replication; tRNS applied. Charges the raster to `budget`.
raster::Raster decode_pixels(const PngDoc&, Limits& budget);

// Bounded zlib inflate. Throws Error{invalid_input} on bad data, Error{limit} past `limit` bytes.
std::vector<std::byte> inflate(std::span<const std::byte> stream, std::uint64_t limit);

struct DeflateParams {
    int level = 9, window_bits = 15, mem_level = 9, strategy = 0; // fallback (png.md §7.3)
    bool inferred = false;                                        // reproduced the source's compressed prefix
};
std::vector<std::byte> deflate(std::span<const std::byte>, const DeflateParams&);

// What a re-encode reuses from the input the output is built from (png.md §7.1).
struct Source {
    const PngDoc* doc = nullptr;
    std::vector<std::uint8_t> filters; // per-row filter types of a non-interlaced stream
    std::vector<std::byte> filtered;   // the inflated IDAT stream
};
Source source_of(const PngDoc&); // never throws; leaves filters empty when the stream cannot be read

// Trial-compresses the start of the source's filtered data inside its zlib header's FLEVEL class and
// keeps the first setting that reproduces its compressed prefix; else the fallback.
DeflateParams infer_params(const Source&);

// The critical part of an encoded image.
struct Image {
    Encoding enc;
    std::vector<std::byte> ihdr, plte, trns, stream;
    std::vector<std::uint16_t> hist; // palette usage for hIST (indexed output only)
    DeflateParams params;
};

// Encodes `r` as `want`, promoting the encoding minimally when the pixels need it (warning
// encoding-promoted). `src` supplies the palette order, tRNS key, per-row filters of unchanged rows,
// and the zlib parameters; without one, adaptive filters and the fallback settings are used.
// `params` overrides the zlib settings.
Image encode(const raster::Raster& r, Encoding want, const Source* src, std::vector<Warning>& warnings,
             const DeflateParams* params = nullptr);

std::string escape_text(std::string_view, std::size_t max = 200); // quoted, truncated, \n \t \" escaped

} // namespace semsplinter::png
