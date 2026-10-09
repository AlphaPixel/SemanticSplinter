#pragma once

// The PNG module (docs/formats/png.md): own chunk layer, libpng pixel decode, canonical RGBA grid,
// GridMerge on pixels and KeyedMerge on ancillary chunks, and an encoder that reuses the source's
// per-row filters and inferred zlib parameters.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace semsplinter::png {

// The encoding attributes (/encoding/*): IHDR color type, bit depth, and interlace method.
struct Encoding {
    std::uint8_t color_type = 6, bit_depth = 8, interlace = 0;
    bool operator==(const Encoding&) const = default;
    std::string name() const; // "rgba8", "gray4", "index8", ...
};

// A complete PNG of `r` in `enc` (promoted if the pixels need it), with the fallback zlib settings.
// Used for visuals and by tests to build fixtures.
std::vector<std::byte> write(const raster::Raster& r, Encoding enc = {});

std::unique_ptr<Format> make_format();

} // namespace semsplinter::png
