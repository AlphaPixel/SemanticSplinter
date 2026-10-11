#pragma once

// The JPEG module (docs/formats/jpeg.md): own segment layer, libjpeg-turbo coefficient access, GridMerge
// over whole MCUs in the DCT domain (no recompression), and OpaqueLeaf metadata segments.

#include <semsplinter/format.hpp>
#include <semsplinter/formats/raster.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace semsplinter::jpeg {

// Test and fixture encoder settings (jpeg.md §11).
struct EncodeOptions {
    int quality = 90;
    int h_samp = 2, v_samp = 2; // luma sampling factors: 2x2 is 4:2:0, 1x1 is 4:4:4
    bool gray = false, progressive = false, arithmetic = false;
    unsigned restart = 0; // restart interval in MCUs
    // Extra segments written after the JFIF header: marker (0xE1, 0xFE, ...) and payload.
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> markers;
};

// A JPEG of the 8-bit values of `r` (alpha ignored), written by libjpeg-turbo.
std::vector<std::byte> encode(const raster::Raster& r, const EncodeOptions& = {});

// `file` with `delta` added to the DC coefficient of every block in the listed MCUs (x, y in MCU units),
// written by the module's coefficient writer: every other MCU keeps its exact coefficients. Gives tests
// lossless edits with exact ground truth (jpeg.md §11).
std::vector<std::byte> edit_mcus(std::span<const std::byte> file,
                                 std::span<const std::array<std::uint32_t, 2>> mcus, int delta);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::jpeg
