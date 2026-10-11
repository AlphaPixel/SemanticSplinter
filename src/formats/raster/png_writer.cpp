#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/raster_png.hpp>

#include <algorithm>
#include <string_view>

namespace semsplinter::raster {

namespace {

void be32(std::vector<std::byte>& o, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        o.push_back(std::byte((v >> s) & 0xFF));
}

void chunk(std::vector<std::byte>& o, std::string_view type, const std::vector<std::byte>& data) {
    be32(o, static_cast<std::uint32_t>(data.size()));
    const auto at = o.size();
    for (char c : type)
        o.push_back(std::byte(c));
    o.insert(o.end(), data.begin(), data.end());
    be32(o, crc32(std::span(o).subspan(at)));
}

} // namespace

std::vector<std::byte> encode_png(const Raster& r) {
    std::vector<std::byte> rows;
    rows.reserve((std::size_t{r.width} * 4 + 1) * r.height);
    for (std::uint64_t y = 0; y < r.height; ++y) {
        rows.push_back(std::byte{0}); // filter None
        for (std::uint64_t x = 0; x < r.width; ++x)
            for (auto v : r.get(y * r.width + x))
                rows.push_back(std::byte(v >> 8));
    }
    std::vector<std::byte> z{std::byte{0x78}, std::byte{0x01}};
    for (std::size_t at = 0; at < rows.size() || at == 0; at += 65535) {
        const auto n = std::min<std::size_t>(65535, rows.size() - at);
        z.push_back(std::byte(at + n >= rows.size() ? 1 : 0));
        for (auto v : {n, n ^ 0xFFFF})
            z.push_back(std::byte(v & 0xFF)), z.push_back(std::byte((v >> 8) & 0xFF));
        z.insert(z.end(), rows.begin() + static_cast<std::ptrdiff_t>(at),
                 rows.begin() + static_cast<std::ptrdiff_t>(at + n));
        if (rows.empty())
            break;
    }
    be32(z, adler32(rows));
    std::vector<std::byte> ihdr;
    be32(ihdr, r.width), be32(ihdr, r.height);
    for (int v : {8, 6, 0, 0, 0})
        ihdr.push_back(std::byte(v));
    std::vector<std::byte> out{std::byte{0x89}, std::byte{'P'},  std::byte{'N'},  std::byte{'G'},
                               std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

} // namespace semsplinter::raster
