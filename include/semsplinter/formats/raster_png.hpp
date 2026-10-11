#pragma once

// A dependency-free PNG writer for report artifacts (heatmaps, merge visuals) of raster formats that cannot
// use the PNG module (D-28).

#include <semsplinter/formats/raster.hpp>

#include <cstddef>
#include <vector>

namespace semsplinter::raster {

// RGBA8 PNG with stored (uncompressed) deflate blocks: large, but needs no zlib.
std::vector<std::byte> encode_png(const Raster&);

} // namespace semsplinter::raster
