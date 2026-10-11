#pragma once

// The TGA module (docs/formats/raster-misc.md §3.3): own codec for color-mapped, truecolor and gray images,
// raw or RLE, any origin, with the TGA 2.0 extension and developer areas, on the shared raster-misc engine.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace semsplinter::tga {

struct Encoding {
    std::uint8_t type = 2; // 1 color-mapped, 2 truecolor, 3 gray
    std::uint8_t bpp = 24; // pixel depth (8 for color-mapped, 8/16 gray, 15/16/24/32 truecolor)
    bool rle = false;
    bool top = false, right = false; // origin bits
    bool footer = true;              // write a TGA 2.0 footer (with no extension area)
};

// A TGA of `r` (8-bit values) in `enc`, promoted if the pixels need it. Used by tests to build fixtures.
std::vector<std::byte> write(const raster::Raster& r, Encoding enc = {});

std::unique_ptr<Format> make_format();

} // namespace semsplinter::tga
