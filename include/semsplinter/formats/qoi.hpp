#pragma once

// The QOI module (docs/formats/raster-misc.md §3.6): own codec on the shared raster-misc engine.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace semsplinter::qoi {

// A QOI of `r` (8-bit values) with 3 or 4 channels (4 when the pixels need alpha) and colorspace 0 or 1.
// The encoder follows the reference op choice. Used by tests to build fixtures.
std::vector<std::byte> write(const raster::Raster& r, std::uint8_t channels = 4, std::uint8_t colorspace = 0);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::qoi
