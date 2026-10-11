#pragma once

// The GIF module (docs/formats/raster-misc.md §3.4): own block parser and LZW writer, giflib for decoding,
// frames as the common frame sequence, on the shared raster-misc engine.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace semsplinter::gif {

struct FrameSpec {
    raster::Raster pixels; // 8-bit values; alpha 0 is transparent, other alpha must be 255
    std::uint16_t left = 0, top = 0;
    std::uint16_t delay = 0; // 1/100 s
    std::uint8_t disposal = 0;
    bool interlace = false;
};

// A GIF89a of the frames on a screen of their union size, with a global table built from the frames'
// colors (local tables when a frame does not fit), and a NETSCAPE2.0 loop count when `loop` >= 0.
// Used by tests to build fixtures.
std::vector<std::byte> write(const std::vector<FrameSpec>& frames, int loop = -1);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::gif
