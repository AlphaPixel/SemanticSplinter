#pragma once

// Shared engine of the miscellaneous raster formats (raster-misc.md §2): every module loads into a Doc of
// canonical frames plus keyed metadata, and supplies only parsing, encoding, and the envelope map. Diff,
// merge (GridMerge per frame, SequenceMerge of frames, KeyedMerge of metadata), passthrough, validation,
// dump, inspect, and visuals live here.

#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace semsplinter::rastermisc {

using raster::Raster;
using Rgba8 = std::array<std::uint8_t, 4>;

// One keyed metadata unit. Doc keys use absolute paths ("/tga/id"); frame keys use paths relative to their
// frame ("/delay"), prefixed with "/frames/<i>" in reports when the document is a sequence.
struct Key {
    std::string key;
    Hash128 value;
    std::string text;              // rendering for reports and dump
    std::string kind = "metadata"; // metadata, color (pixels depend on it), text, animation, ...
    bool time = false;             // volatile timestamp: both changed -> newest (text order) wins
};

struct Frame {
    Raster pixels;                     // canonical RGBA; empty when `opaque`
    bool opaque = false;               // not decodable to canonical pixels: compared by `coded` only
    bool lossy = false;                // decoded, but no lossless pixel merge: whole-frame merge
    Hash128 coded;                     // the coded bytes (opaque and lossy frames)
    std::vector<Key> keys;             // frame control fields
    std::string encoding;              // per-frame encoding ("lossless", "lzw"), not compared semantically
    std::vector<std::uint8_t> indices; // the stored palette indices of an indexed frame
    std::vector<Rgba8> palette;        // its effective palette
};

struct Doc : Document {
    ByteSource src;
    bool sequence = false; // frames are addressed /<word>/<i>, even when there is one
    std::vector<Frame> frames;
    std::vector<Key> keys;
    std::vector<std::pair<std::string, std::string>> encoding; // /encoding/<name> = value
    EnvelopeMap map;
    std::vector<Warning> warnings;
    nlohmann::ordered_json structure = nlohmann::ordered_json::object(); // extra inspect() fields
};

// Merged output, handed to the module's writer.
struct OutFrame {
    Raster pixels;                                 // merged, unified storage (empty for opaque frames)
    std::array<std::optional<std::size_t>, 3> src; // the frame it descends from in each side, by Side
    std::optional<Side> same; // a side whose source frame has exactly these pixels (ours first)
    std::vector<Side> take;   // per cell, when grid-merged
    std::vector<std::pair<std::string, Side>> keys; // frame keys in output order, side whose value is written
};

struct Plan {
    std::array<const Doc*, 3> docs{}; // by Side
    Side enc = Side::ours;            // side whose encoding is written
    std::vector<OutFrame> frames;
    std::vector<std::pair<std::string, Side>> keys; // doc keys in output order
    std::vector<std::string> dropped;     // keys the writer could not carry or had to rewrite (warned)
    std::vector<std::size_t> approximate; // frames the writer re-encoded lossily (--reencode)

    const Doc& doc(Side s) const { return *docs[static_cast<std::size_t>(s)]; }
    const Frame& frame(const OutFrame& f, Side s) const {
        return doc(s).frames[*f.src[static_cast<std::size_t>(s)]];
    }
};

// The exact palette indices of a merged frame, when every side it takes cells from is indexed with one
// and the same palette (indexed mode, png.md §3.2); that palette is returned too.
std::optional<std::pair<std::vector<std::uint8_t>, std::vector<Rgba8>>> merged_indices(const Plan&,
                                                                                       const OutFrame&);

// Palette for `r` that starts with `base` in order and appends new colors in raster order (png.md §7.3);
// nullopt past `max` entries. `index` receives each cell's entry.
std::optional<std::vector<Rgba8>> palettize(const Raster& r, std::vector<Rgba8> base, std::size_t max,
                                            std::vector<std::uint8_t>& index);

Rgba8 rgba8(const raster::Cell&) noexcept; // high bytes
raster::Cell cell(const Rgba8&) noexcept;  // v * 257
bool narrow_ok(const Raster&) noexcept;    // every 16-bit value is v * 257

const Key* find_key(std::span<const Key>, std::string_view path) noexcept;

// What a module supplies.
class Codec {
public:
    virtual ~Codec() = default;
    virtual const FormatInfo& info() const = 0;
    virtual int probe(std::span<const std::byte> head, std::string_view name) const = 0;
    // Throws Error{invalid_input} on bad data; charges rasters to `budget`.
    virtual std::unique_ptr<Doc> load(ByteSource, Limits& budget, const OptionValues&) const = 0;
    // The merged file. Only called when no input already is the result (passthrough).
    virtual std::vector<std::byte> write(Plan&, std::vector<Warning>&) const = 0;
    virtual std::string_view frame_word() const { return "frames"; } // "pages" for TIFF
    virtual std::string_view ignore_option() const { return {}; }    // diff-only key filter option
};

std::unique_ptr<Format> make_format(std::unique_ptr<Codec>);

// Helpers for the modules.
[[noreturn]] void bad(std::string_view format, const std::string& what);
std::string hex16(const Hash128&);
std::string bytes_text(std::span<const std::byte>);                             // "bytes=N xxh3=..."
Raster new_raster(std::uint32_t w, std::uint32_t h, bool wide, Limits& budget); // checks --max-pixels

struct Reader { // bounds-checked little/big-endian reads; throws invalid_input naming the format
    std::span<const std::byte> d;
    std::string_view format;
    std::uint64_t u(std::uint64_t at, unsigned width, bool big = false) const;
    std::uint8_t u8(std::uint64_t at) const { return static_cast<std::uint8_t>(u(at, 1)); }
    std::uint16_t le16(std::uint64_t at) const { return static_cast<std::uint16_t>(u(at, 2)); }
    std::uint32_t le32(std::uint64_t at) const { return static_cast<std::uint32_t>(u(at, 4)); }
    std::uint32_t be32(std::uint64_t at) const { return static_cast<std::uint32_t>(u(at, 4, true)); }
    std::span<const std::byte> sub(std::uint64_t at, std::uint64_t len) const;
};

void put_le(std::vector<std::byte>& o, std::uint64_t v, unsigned width);
void put_be(std::vector<std::byte>& o, std::uint64_t v, unsigned width);

} // namespace semsplinter::rastermisc
