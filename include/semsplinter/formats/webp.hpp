#pragma once

// The WebP module (docs/formats/raster-misc.md §3.5): own RIFF layer and writer, libwebp for the VP8 and
// VP8L bitstreams; still, lossless and lossy, and animated files, on the shared raster-misc engine.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace semsplinter::webp {

struct FrameSpec {
    raster::Raster pixels;                    // 8-bit values
    std::uint32_t x = 0, y = 0, duration = 0; // animation frame placement (even x, y) and duration in ms
    bool lossless = true;
};

// A WebP of the frames: a still image for one frame without `loop`, else an animation (canvas = union).
// Lossless frames use the Q-RAST-5 settings; lossy frames quality 90. Used by tests to build fixtures.
std::vector<std::byte> write(const std::vector<FrameSpec>& frames, int loop = -1);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::webp
