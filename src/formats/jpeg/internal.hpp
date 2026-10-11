#pragma once

// Private to the JPEG module: the segment layer, the loaded document, and the libjpeg-turbo codec
// (jpeg.md §2, §5, §7).

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/jpeg.hpp>
#include <semsplinter/formats/raster.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/limits.hpp>
#include <semsplinter/model.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace semsplinter::jpeg {

inline constexpr std::uint8_t kSOI = 0xD8, kEOI = 0xD9, kSOS = 0xDA, kDQT = 0xDB, kDHT = 0xC4, kDAC = 0xCC,
                              kDRI = 0xDD, kCOM = 0xFE;

struct Segment {
    std::uint64_t start = 0;  // first fill byte before the marker
    std::uint64_t offset = 0; // the FF of the marker
    std::uint8_t marker = 0;
    std::uint32_t length = 0; // payload bytes after the length field; 0 for standalone markers
    std::uint64_t end = 0;    // past the payload, or past the entropy-coded data that follows SOS

    std::uint64_t payload() const noexcept { return offset + 4; }
    bool metadata() const noexcept { return (marker >= 0xE0 && marker <= 0xEF) || marker == kCOM; }
};

// Splits the file into segments: SOI, marker segments, entropy-coded data after each SOS, EOI. Checks
// framing (lengths inside the file, EOI present, at most 1000 scans). Throws Error{invalid_input}.
// Returns the segments and the offset where the trailer after EOI begins.
std::pair<std::vector<Segment>, std::uint64_t> parse_segments(std::span<const std::byte>);

struct Component {
    int id = 0, h = 1, v = 1, tq = 0;
};

// Quantized DCT coefficients of one component in natural order, padded to whole MCUs. Blocks at or past
// real_w/real_h are the dummy blocks of edge MCUs: the encoder regenerates them, so they are never
// compared or kept (jpeg.md §6.1).
struct Plane {
    std::uint32_t bw = 0, bh = 0, real_w = 0, real_h = 0;
    std::vector<std::int16_t> coef; // bw * bh blocks of 64

    const std::int16_t* block(std::uint32_t bx, std::uint32_t by) const noexcept {
        return coef.data() + (std::uint64_t{by} * bw + bx) * 64;
    }
    std::int16_t* block(std::uint32_t bx, std::uint32_t by) noexcept {
        return coef.data() + (std::uint64_t{by} * bw + bx) * 64;
    }
};

// One metadata unit (jpeg.md §3): its address, the hash of its value, and how to show it.
struct Unit {
    std::string key;                   // /exif, /icc, /app/APP11:JP, /com/0, /trailer, ...
    Hash128 value;                     // payloads in order (ICC: the reassembled profile)
    std::string text;                  // rendering for reports and dump
    std::string kind;                  // metadata, color, text
    std::vector<std::size_t> segments; // indices into JpegDoc::segs; empty for /trailer
};

struct JpegDoc final : Document {
    ByteSource src;
    std::vector<Segment> segs;
    std::uint64_t trailer = 0; // offset of the bytes after EOI
    std::uint8_t sof = 0;
    int precision = 8;
    std::uint32_t width = 0, height = 0;
    std::vector<Component> comps;
    std::uint32_t mcu_w = 8, mcu_h = 8, mcus_x = 0, mcus_y = 0;
    std::string colorspace; // gray, ycbcr, rgb, cmyk, ycck, unknown (from libjpeg-turbo)
    unsigned restart = 0;   // DRI interval in MCUs
    std::vector<Plane> planes;
    // /shape and /coding/* (gate the DCT-domain merge), /encoding/* (never conflict), as address and text.
    std::vector<std::pair<std::string, std::string>> coding, encoding;
    std::vector<Unit> units;
    EnvelopeMap map;
    std::vector<Warning> warnings;

    std::span<const std::byte> bytes(const Segment& s) const {
        return s.length ? src.bytes().subspan(s.payload(), s.length) : std::span<const std::byte>{};
    }
    std::uint64_t mcus() const noexcept { return std::uint64_t{mcus_x} * mcus_y; }
    bool progressive() const noexcept { return sof == 0xC2 || sof == 0xC6 || sof == 0xCA || sof == 0xCE; }
    bool arithmetic() const noexcept { return sof >= 0xC9; }
};

// ---- codec.cpp (libjpeg-turbo) ----

// Reads the coefficient planes and the color space. Warnings of the decoder go to doc.warnings.
// Throws Error{invalid_input} with libjpeg-turbo's message.
void read_coefficients(JpegDoc&);

// Decoded pixels (JDCT_ISLOW, fancy upsampling), 8-bit storage, or 16-bit for 12-bit data. CMYK and YCCK
// are shown as RGB assuming Adobe's inverted values. nullopt when the color space has no RGB view.
std::optional<raster::Raster> decode_pixels(const JpegDoc&, Limits& budget);

// A complete JPEG (SOI..EOI, no APPn) of `planes` with ours' critical parameters (quantization tables,
// sampling, component IDs), restart interval, arithmetic coding, and, when asked, ours' scan script and
// Huffman tables (jpeg.md §7 item 3). nullopt with `error` set when libjpeg-turbo refuses (a scan
// script it cannot reproduce, a quantization slot redefined between scans).
std::optional<std::vector<std::byte>> write_coefficients(const JpegDoc& ours,
                                                         const std::vector<Plane>& planes, bool ours_script,
                                                         bool ours_tables, std::string& error);

// True when ours' Huffman tables can be handed to libjpeg-turbo: one sequential Huffman scan over every
// component, no table slot defined twice.
bool tables_reusable(const JpegDoc&);

} // namespace semsplinter::jpeg
