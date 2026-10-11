#pragma once

// The TIFF module (docs/formats/raster-misc.md §3.1): own IFD layer (ifd.hpp), libtiff for decoding, own
// strip encoders, stable-layout writer that appends rewritten IFDs and strips, on the raster-misc engine.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace semsplinter::tiff {

struct Encoding {
    std::uint16_t compression = 1;    // 1 none, 5 LZW, 8 Deflate, 32773 PackBits
    std::uint16_t predictor = 1;      // 1 none, 2 horizontal
    std::uint16_t bits = 8;           // 1, 2, 4, 8, 16 (palette: up to 8)
    std::uint16_t photometric = 2;    // 0/1 gray, 2 RGB, 3 palette
    std::uint32_t rows_per_strip = 0; // 0: about 8 KiB per strip
    bool big_endian = false, bigtiff = false;
    struct Tag {
        std::uint16_t tag = 0, type = 0;
        std::uint64_t count = 0;
        std::vector<std::byte> value; // little-endian elements
    };
    std::vector<Tag> tags; // further tags for every page (Software, GeoTIFF keys, ...)
};

// A single- or multi-page TIFF of the rasters in `enc` (promoted if the pixels need it). Used by tests.
std::vector<std::byte> write(const std::vector<raster::Raster>& pages, Encoding enc = {});

std::unique_ptr<Format> make_format();

} // namespace semsplinter::tiff
