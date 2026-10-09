#pragma once

// Shared raster core (png.md §3.4): the canonical grid, cell comparison, change regions and their
// metrics, crop detection, the tile-hash dump, and the diff and merge visuals. No format types and no
// external dependencies, so JPEG and the other raster formats reuse it.

#include <semsplinter/hash.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/model.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace semsplinter::raster {

using Cell = std::array<std::uint16_t, 4>; // R, G, B, A on the 16-bit scale

// Canonical pixels: RGBA, straight alpha, 16-bit semantics (png.md §3.1). Storage is 8 bits per
// channel when every input is 8-bit or less (Q-PNG-1); the 16-bit value is then v * 257.
struct Raster {
    std::uint32_t width = 0, height = 0;
    bool wide = false;              // 16-bit storage
    std::vector<std::uint8_t> data; // row-major; 4 bytes per cell, or 4 native-endian u16 when wide

    Raster() = default;
    Raster(std::uint32_t w, std::uint32_t h, bool wide_storage);

    std::uint64_t cells() const noexcept { return std::uint64_t{width} * height; }
    std::size_t cell_bytes() const noexcept { return wide ? 8 : 4; }
    const std::uint8_t* at(std::uint64_t i) const noexcept { return data.data() + i * cell_bytes(); }
    Cell get(std::uint64_t i) const noexcept;
    // Narrow storage keeps the high byte; callers only store representable values there.
    void set(std::uint64_t i, const Cell&) noexcept;
    Raster widened() const; // the same image with 16-bit storage
    bool operator==(const Raster&) const = default;
};

// Brings two or three rasters to one storage width (16-bit if any is wide).
void unify(std::span<Raster* const>);

// Cell equality (png.md §4): exact, or within `tolerance` per channel on the 16-bit scale; with
// `alpha_zero_equal`, two fully transparent cells are equal whatever their color.
struct CellCompare {
    std::uint16_t tolerance = 0;
    bool alpha_zero_equal = false;
    bool same(const Raster& a, const Raster& b, std::uint64_t i) const noexcept;
};

// Hash of the canonical 16-bit values of all cells, or of the listed cells, independent of storage.
Hash128 hash_cells(const Raster&);
Hash128 hash_cells(const Raster&, std::span<const std::uint64_t> cells);

// Changed regions of b against a (equal shapes), 8-connected, with same-side cells closer than `gap`
// joined (png.md §5).
std::vector<GridRegion> diff_regions(const Raster& a, const Raster& b, const CellCompare&, unsigned gap = 0);

// pixels, bbox, depth (8 or 16), max_delta and mean_delta per channel on that depth's scale, alpha_changed,
// delta_e00_max/mean (sRGB, D65).
nlohmann::ordered_json region_metrics(const Raster& a, const Raster& b, const GridRegion&);

// CIEDE2000 between two 16-bit sRGB colors (alpha ignored), and between two CIELAB colors.
double delta_e00(const Cell& a, const Cell& b) noexcept;
double delta_e00_lab(const std::array<double, 3>&, const std::array<double, 3>&) noexcept;

// Where `small` sits inside `big` if it equals big's cells there (a pure crop or canvas extension,
// Q-PNG-3). Returns the offset of small's origin in big.
// ponytail: brute-force offsets with early exit and a work cap; FFT correlation if large crops miss it.
std::optional<std::array<std::uint32_t, 2>> find_offset(const Raster& big, const Raster& small);

// "pixels xxh3=... alpha=... colors=N" then one "tile x,y,w,h xxh3=..." line per tile. The tile size
// starts at 64 and doubles until there are at most 4096 tiles (png.md §5).
void dump_tiles(const Raster&, std::ostream&);

// Diff visual: a's luminance at 35% over a checkerboard, changed cells on a log heat ramp of the max
// channel delta, region boxes outlined.
Raster heatmap(const Raster& a, const Raster& b, std::span<const GridRegion>);

// Merge visual: ours-only blue, theirs-only orange, identical green, conflicts magenta, guard-band
// groups outlined yellow (Okabe-Ito colors).
Raster merge_visual(const Raster& base, const GridResult&);

} // namespace semsplinter::raster
