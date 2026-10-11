#pragma once

// The BMP module (docs/formats/raster-misc.md §3.2): own codec for every header version, 1-32 bpp, bitfields,
// and RLE4/RLE8, on the shared raster-misc engine.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace semsplinter::bmp {

// What the writer produces: info header size (12, 40, 56, 108, 124), bits per pixel, compression
// (0 RGB, 1 RLE8, 2 RLE4, 3 BITFIELDS), row order.
struct Encoding {
    std::uint32_t header = 40;
    std::uint16_t bpp = 24;
    std::uint32_t compression = 0;
    bool top_down = false;
};

// A BMP of `r` (8-bit values) in `enc`, promoted if the pixels need it. Used by tests to build fixtures.
std::vector<std::byte> write(const raster::Raster& r, Encoding enc = {});

std::unique_ptr<Format> make_format();

} // namespace semsplinter::bmp
