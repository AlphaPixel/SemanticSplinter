#include <semsplinter/formats/raster.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <unordered_set>

namespace semsplinter::raster {

Raster::Raster(std::uint32_t w, std::uint32_t h, bool wide_storage)
    : width(w), height(h), wide(wide_storage) {
    data.assign(cells() * cell_bytes(), 0);
}

Cell Raster::get(std::uint64_t i) const noexcept {
    Cell c;
    if (wide) {
        std::memcpy(c.data(), at(i), 8);
    } else {
        auto* p = at(i);
        for (int k = 0; k < 4; ++k)
            c[k] = static_cast<std::uint16_t>(p[k] * 257);
    }
    return c;
}

void Raster::set(std::uint64_t i, const Cell& c) noexcept {
    auto* p = data.data() + i * cell_bytes();
    if (wide)
        std::memcpy(p, c.data(), 8);
    else
        for (int k = 0; k < 4; ++k)
            p[k] = static_cast<std::uint8_t>(c[k] >> 8);
}

Raster Raster::widened() const {
    if (wide)
        return *this;
    Raster r(width, height, true);
    for (std::uint64_t i = 0; i < cells(); ++i)
        r.set(i, get(i));
    return r;
}

void unify(std::span<Raster* const> rs) {
    if (std::any_of(rs.begin(), rs.end(), [](const Raster* r) { return r->wide; }))
        for (auto* r : rs)
            if (!r->wide)
                *r = r->widened();
}

bool CellCompare::same(const Raster& a, const Raster& b, std::uint64_t i) const noexcept {
    if (!tolerance && !alpha_zero_equal && a.wide == b.wide)
        return std::memcmp(a.at(i), b.at(i), a.cell_bytes()) == 0;
    auto x = a.get(i), y = b.get(i);
    if (alpha_zero_equal && x[3] == 0 && y[3] == 0)
        return true;
    for (int k = 0; k < 4; ++k)
        if (std::abs(int{x[k]} - int{y[k]}) > tolerance)
            return false;
    return true;
}

namespace {

void add_cell(Hasher& h, const Cell& c) {
    std::array<std::byte, 8> be;
    for (int k = 0; k < 4; ++k)
        be[2 * k] = std::byte(c[k] >> 8), be[2 * k + 1] = std::byte(c[k] & 0xFF);
    h.raw(be);
}

Hash128 hash_rect(const Raster& r, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h) {
    Hasher hs;
    hs.add(w).add(h);
    std::vector<std::byte> row(std::size_t{w} * 8);
    for (std::uint32_t y = y0; y < y0 + h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto c = r.get(std::uint64_t{y} * r.width + x0 + x);
            for (int k = 0; k < 4; ++k)
                row[8 * x + 2 * k] = std::byte(c[k] >> 8), row[8 * x + 2 * k + 1] = std::byte(c[k] & 0xFF);
        }
        hs.raw(row);
    }
    return hs.digest();
}

std::string hex16(const Hash128& h) {
    return h.hex().substr(0, 16);
}

} // namespace

Hash128 hash_cells(const Raster& r) {
    return hash_rect(r, 0, 0, r.width, r.height);
}

Hash128 hash_cells(const Raster& r, std::span<const std::uint64_t> cells) {
    Hasher h;
    h.add(cells.size());
    for (auto i : cells)
        add_cell(h, r.get(i));
    return h.digest();
}

std::vector<GridRegion> diff_regions(const Raster& a, const Raster& b, const CellCompare& cmp, unsigned gap,
                                     Limits* budget) {
    GridShape s{a.width, a.height};
    // b plays "ours" against a as base and theirs; its regions are the changes.
    auto res = grid_merge(s, s, s,
                          [&](Side x, Side y, std::uint64_t i) {
                              bool bx = x == Side::ours, by = y == Side::ours;
                              return bx == by || cmp.same(bx ? b : a, by ? b : a, i);
                          },
                          {gap, 0, budget});
    return std::move(res.ours);
}

namespace {

std::array<double, 3> lab(const Cell& c) {
    auto lin = [](std::uint16_t v) {
        double x = v / 65535.0;
        return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    double r = lin(c[0]), g = lin(c[1]), b = lin(c[2]);
    double xyz[3] = {(0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047,
                     0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
                     (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883};
    auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116; };
    double fx = f(xyz[0]), fy = f(xyz[1]), fz = f(xyz[2]);
    return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

double round3(double v) { // the nearest double to v rounded to 3 decimals, so it prints short
    return std::round(v * 1000) / 1000;
}

} // namespace

double delta_e00_lab(const std::array<double, 3>& p, const std::array<double, 3>& q) noexcept {
    constexpr double pi = std::numbers::pi, k25 = 6103515625.0; // 25^7
    auto deg = [](double r) { return r * 180 / pi; };
    auto rad = [](double d) { return d * pi / 180; };
    double c1 = std::hypot(p[1], p[2]), c2 = std::hypot(q[1], q[2]), cb = (c1 + c2) / 2;
    double cb7 = std::pow(cb, 7), g = 0.5 * (1 - std::sqrt(cb7 / (cb7 + k25)));
    double a1 = (1 + g) * p[1], a2 = (1 + g) * q[1];
    double c1p = std::hypot(a1, p[2]), c2p = std::hypot(a2, q[2]);
    auto hue = [&](double b, double a) {
        if (a == 0 && b == 0)
            return 0.0;
        double h = deg(std::atan2(b, a));
        return h < 0 ? h + 360 : h;
    };
    double h1 = hue(p[2], a1), h2 = hue(q[2], a2);
    double dl = q[0] - p[0], dc = c2p - c1p, dh = 0;
    if (c1p * c2p != 0) {
        dh = h2 - h1;
        dh = dh > 180 ? dh - 360 : dh < -180 ? dh + 360 : dh;
    }
    double dH = 2 * std::sqrt(c1p * c2p) * std::sin(rad(dh / 2));
    double lbp = (p[0] + q[0]) / 2, cbp = (c1p + c2p) / 2, hbp = h1 + h2;
    if (c1p * c2p != 0)
        hbp = std::abs(h1 - h2) <= 180 ? (h1 + h2) / 2
              : h1 + h2 < 360          ? (h1 + h2 + 360) / 2
                                       : (h1 + h2 - 360) / 2;
    double t = 1 - 0.17 * std::cos(rad(hbp - 30)) + 0.24 * std::cos(rad(2 * hbp)) +
               0.32 * std::cos(rad(3 * hbp + 6)) - 0.20 * std::cos(rad(4 * hbp - 63));
    double dtheta = 30 * std::exp(-std::pow((hbp - 275) / 25, 2));
    double cbp7 = std::pow(cbp, 7), rc = 2 * std::sqrt(cbp7 / (cbp7 + k25));
    double sl = 1 + 0.015 * std::pow(lbp - 50, 2) / std::sqrt(20 + std::pow(lbp - 50, 2));
    double sc = 1 + 0.045 * cbp, sh = 1 + 0.015 * cbp * t;
    double rt = -std::sin(rad(2 * dtheta)) * rc;
    double x = dl / sl, y = dc / sc, z = dH / sh;
    return std::sqrt(x * x + y * y + z * z + rt * y * z);
}

double delta_e00(const Cell& a, const Cell& b) noexcept {
    return delta_e00_lab(lab(a), lab(b));
}

nlohmann::ordered_json region_metrics(const Raster& a, const Raster& b, const GridRegion& r) {
    const bool wide = a.wide || b.wide;
    std::array<int, 4> max{};
    std::array<double, 4> sum{};
    std::uint64_t alpha = 0;
    double de_max = 0, de_sum = 0;
    for (auto i : r.cells) {
        auto x = a.get(i), y = b.get(i);
        for (int k = 0; k < 4; ++k) {
            int d = std::abs(int{x[k]} - int{y[k]}) / (wide ? 1 : 257);
            max[k] = std::max(max[k], d), sum[k] += d;
        }
        alpha += x[3] != y[3];
        double de = delta_e00(x, y);
        de_max = std::max(de_max, de), de_sum += de;
    }
    auto n = static_cast<double>(r.cells.size());
    auto& box = std::get<Rect>(r.bounds);
    nlohmann::ordered_json m;
    m["pixels"] = r.cells.size();
    m["bbox"] = {box.x, box.y, box.w, box.h};
    m["depth"] = wide ? 16 : 8;
    m["max_delta"] = max;
    m["mean_delta"] = {round3(sum[0] / n), round3(sum[1] / n), round3(sum[2] / n), round3(sum[3] / n)};
    m["alpha_changed"] = alpha;
    m["delta_e00_max"] = round3(de_max);
    m["delta_e00_mean"] = round3(de_sum / n);
    return m;
}

std::optional<std::array<std::uint32_t, 2>> find_offset(const Raster& big, const Raster& small) {
    if (small.width > big.width || small.height > big.height || big.wide != small.wide || !small.cells())
        return std::nullopt;
    const std::size_t row = std::size_t{small.width} * small.cell_bytes();
    std::int64_t budget = std::int64_t{1} << 28; // bytes compared
    for (std::uint32_t oy = 0; oy + small.height <= big.height; ++oy)
        for (std::uint32_t ox = 0; ox + small.width <= big.width; ++ox) {
            std::uint32_t y = 0;
            for (; y < small.height; ++y) {
                if (std::memcmp(big.at(std::uint64_t{oy + y} * big.width + ox),
                                small.at(std::uint64_t{y} * small.width), row) != 0)
                    break;
                budget -= static_cast<std::int64_t>(row);
            }
            if (y == small.height)
                return std::array{ox, oy};
            if (--budget < 0)
                return std::nullopt;
        }
    return std::nullopt;
}

void dump_tiles(const Raster& r, std::ostream& os) {
    bool opaque = true, binary = true;
    std::unordered_set<std::uint64_t> colors;
    constexpr std::size_t kMaxColors = 65536;
    for (std::uint64_t i = 0; i < r.cells(); ++i) {
        auto c = r.get(i);
        opaque &= c[3] == 0xFFFF;
        binary &= c[3] == 0xFFFF || c[3] == 0;
        if (colors.size() <= kMaxColors)
            colors.insert(std::uint64_t{c[0]} << 48 | std::uint64_t{c[1]} << 32 | std::uint64_t{c[2]} << 16 |
                          c[3]);
    }
    os << "pixels xxh3=" << hex16(hash_cells(r)) << " alpha="
       << (opaque   ? "opaque"
           : binary ? "binary"
                    : "varies")
       << " colors=" << (colors.size() > kMaxColors ? std::string(">65536") : std::to_string(colors.size()))
       << "\n";
    std::uint32_t tile = 64;
    auto count = [&](std::uint64_t t) { return ((r.width + t - 1) / t) * ((r.height + t - 1) / t); };
    while (count(tile) > 4096)
        tile *= 2;
    for (std::uint32_t y = 0; y < r.height; y += tile)
        for (std::uint32_t x = 0; x < r.width; x += tile) {
            auto w = std::min(tile, r.width - x), h = std::min(tile, r.height - y);
            os << "tile " << x << ',' << y << ',' << w << ',' << h
               << " xxh3=" << hex16(hash_rect(r, x, y, w, h)) << "\n";
        }
}

namespace {

using Rgb = std::array<std::uint8_t, 3>;
constexpr Rgb kBlue{0, 114, 178}, kOrange{230, 159, 0}, kGreen{0, 158, 115}, kMagenta{204, 121, 167},
    kYellow{240, 228, 66}, kSky{86, 180, 233};

// Base luminance at 35% over an 8-pixel checkerboard.
Raster backdrop(const Raster& base) {
    Raster out(base.width, base.height, false);
    for (std::uint32_t y = 0; y < base.height; ++y)
        for (std::uint32_t x = 0; x < base.width; ++x) {
            auto i = std::uint64_t{y} * base.width + x;
            auto c = base.get(i);
            double lum = (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 257, a = 0.35 * c[3] / 65535;
            double checker = ((x / 8 + y / 8) % 2) ? 204 : 255;
            auto v = static_cast<std::uint16_t>(std::lround(a * lum + (1 - a) * checker) * 257);
            out.set(i, {v, v, v, 0xFFFF});
        }
    return out;
}

void paint(Raster& r, std::uint64_t i, const Rgb& c) {
    r.set(i, {static_cast<std::uint16_t>(c[0] * 257), static_cast<std::uint16_t>(c[1] * 257),
              static_cast<std::uint16_t>(c[2] * 257), 0xFFFF});
}

// A 1-pixel box just outside `b`, clipped to the image.
void outline(Raster& r, const Rect& b, const Rgb& c) {
    auto x0 = static_cast<std::int64_t>(b.x) - 1, y0 = static_cast<std::int64_t>(b.y) - 1;
    auto x1 = static_cast<std::int64_t>(b.x + b.w), y1 = static_cast<std::int64_t>(b.y + b.h);
    auto put = [&](std::int64_t x, std::int64_t y) {
        if (x >= 0 && y >= 0 && x < r.width && y < r.height)
            paint(r, static_cast<std::uint64_t>(y) * r.width + static_cast<std::uint64_t>(x), c);
    };
    for (auto x = x0; x <= x1; ++x)
        put(x, y0), put(x, y1);
    for (auto y = y0; y <= y1; ++y)
        put(x0, y), put(x1, y);
}

Rgb heat(double t) { // inferno-like, perceptually ordered
    static const Rgb stops[] = {{87, 16, 110}, {188, 55, 84}, {249, 142, 9}, {252, 255, 164}};
    t = std::clamp(t, 0.0, 1.0) * 3;
    auto k = std::min<int>(static_cast<int>(t), 2);
    double f = t - k;
    Rgb out;
    for (int j = 0; j < 3; ++j)
        out[j] = static_cast<std::uint8_t>(std::lround(stops[k][j] + f * (stops[k + 1][j] - stops[k][j])));
    return out;
}

} // namespace

Raster heatmap(const Raster& a, const Raster& b, std::span<const GridRegion> regions) {
    Raster out = backdrop(a);
    for (auto& r : regions)
        for (auto i : r.cells) {
            auto x = a.get(i), y = b.get(i);
            int d = 0;
            for (int k = 0; k < 4; ++k)
                d = std::max(d, std::abs(int{x[k]} - int{y[k]}));
            paint(out, i, heat(std::log1p(d / 257.0) / std::log1p(255.0)));
        }
    for (auto& r : regions)
        outline(out, std::get<Rect>(r.bounds), kSky);
    return out;
}

Raster merge_visual(const Raster& base, const GridResult& res) {
    Raster out = backdrop(base);
    if (res.shape_conflict) {
        outline(out, {1, 1, std::max(base.width, 2u) - 2, std::max(base.height, 2u) - 2}, kMagenta);
        return out;
    }
    std::vector<std::uint8_t> mark(base.cells());
    for (auto& r : res.ours)
        for (auto i : r.cells)
            mark[i] |= 1;
    for (auto& r : res.theirs)
        for (auto i : r.cells)
            mark[i] |= 2;
    for (auto& c : res.conflicts) {
        for (auto r : c.ours)
            for (auto i : res.ours[r].cells)
                mark[i] |= 4;
        for (auto r : c.theirs)
            for (auto i : res.theirs[r].cells)
                mark[i] |= 4;
    }
    for (std::uint64_t i = 0; i < mark.size(); ++i)
        if (mark[i])
            paint(out, i, mark[i] & 4 ? kMagenta : mark[i] == 3 ? kGreen : mark[i] == 1 ? kBlue : kOrange);
    for (auto& c : res.conflicts)
        if (c.reason == "guard-band")
            outline(out, std::get<Rect>(c.bounds), kYellow);
    return out;
}

} // namespace semsplinter::raster
