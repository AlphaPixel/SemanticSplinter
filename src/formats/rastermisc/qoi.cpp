#include "common.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/formats/qoi.hpp>

#include <algorithm>

namespace semsplinter::qoi {

namespace {

using namespace rastermisc;
constexpr std::string_view kName = "QOI";
constexpr std::uint8_t kIndex = 0x00, kDiff = 0x40, kLuma = 0x80, kRun = 0xC0, kRgb = 0xFE, kRgba = 0xFF;

struct QoiDoc final : Doc {
    std::uint8_t channels = 4, colorspace = 0;
};

unsigned slot(const Rgba8& c) {
    return (c[0] * 3u + c[1] * 5u + c[2] * 7u + c[3] * 11u) % 64;
}

// The reference encoder's op choice (qoi.h), so output is deterministic and matches other encoders.
std::vector<std::byte> encode(const Raster& r, std::uint8_t channels, std::uint8_t colorspace) {
    std::vector<Rgba8> cells(r.cells());
    for (std::uint64_t i = 0; i < cells.size(); ++i) {
        cells[i] = rgba8(r.get(i));
        if (cells[i][3] != 255)
            channels = 4; // alpha needs four channels (encoding-promoted by the caller)
    }
    std::vector<std::byte> ops;
    auto b = [&](unsigned v) { ops.push_back(std::byte(v & 0xFF)); };
    std::array<Rgba8, 64> index{};
    Rgba8 prev{0, 0, 0, 255};
    unsigned run = 0;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Rgba8 px = cells[i];
        if (px == prev) {
            if (++run == 62 || i + 1 == cells.size())
                b(kRun | (run - 1)), run = 0;
            continue;
        }
        if (run)
            b(kRun | (run - 1)), run = 0;
        const unsigned h = slot(px);
        if (index[h] == px) {
            b(kIndex | h);
        } else {
            index[h] = px;
            if (px[3] == prev[3]) {
                const int dr = int(px[0]) - prev[0], dg = int(px[1]) - prev[1], db = int(px[2]) - prev[2];
                const int r8 = static_cast<std::int8_t>(dr), g8 = static_cast<std::int8_t>(dg),
                          b8 = static_cast<std::int8_t>(db);
                const int dr_dg = r8 - g8, db_dg = b8 - g8;
                if (r8 > -3 && r8 < 2 && g8 > -3 && g8 < 2 && b8 > -3 && b8 < 2)
                    b(kDiff | unsigned((r8 + 2) << 4 | (g8 + 2) << 2 | (b8 + 2)));
                else if (dr_dg > -9 && dr_dg < 8 && g8 > -33 && g8 < 32 && db_dg > -9 && db_dg < 8)
                    b(kLuma | unsigned(g8 + 32)), b(unsigned((dr_dg + 8) << 4 | (db_dg + 8)));
                else
                    b(kRgb), b(px[0]), b(px[1]), b(px[2]);
            } else {
                b(kRgba), b(px[0]), b(px[1]), b(px[2]), b(px[3]);
            }
        }
        prev = px;
    }
    std::vector<std::byte> head;
    for (char c : std::string_view("qoif"))
        head.push_back(std::byte(c));
    put_be(head, r.width, 4), put_be(head, r.height, 4);
    head.push_back(std::byte(channels)), head.push_back(std::byte(colorspace));
    std::vector<std::byte> end(8);
    end[7] = std::byte{1};
    return emit::emit(emit::group(1, {emit::bytes(head), emit::bytes(ops), emit::bytes(end)}));
}

class QoiCodec final : public Codec {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "qoi", "Quite OK Image format", {".qoi"}, {true, true, false, true, true}, {}};
        return i;
    }

    int probe(std::span<const std::byte> head, std::string_view) const override {
        return head.size() >= 14 &&
                       std::equal(head.begin(), head.begin() + 4, as_bytes(std::string_view("qoif")).begin())
                   ? 100
                   : 0;
    }

    std::unique_ptr<Doc> load(ByteSource src, Limits& budget, const OptionValues&) const override {
        auto d = std::make_unique<QoiDoc>();
        d->src = std::move(src);
        const Reader r{d->src.bytes(), kName};
        if (r.be32(0) != 0x716F6966)
            bad(kName, "missing qoif signature");
        const std::uint32_t w = r.be32(4), h = r.be32(8);
        d->channels = r.u8(12), d->colorspace = r.u8(13);
        if (!w || !h || (d->channels != 3 && d->channels != 4) || d->colorspace > 1)
            bad(kName, "bad header");
        Frame f;
        f.pixels = new_raster(w, h, false, budget);
        std::array<Rgba8, 64> index{};
        Rgba8 px{0, 0, 0, 255};
        std::uint64_t at = 14;
        const std::uint64_t n = f.pixels.cells();
        for (std::uint64_t i = 0; i < n;) {
            const std::uint8_t op = r.u8(at++);
            unsigned run = 1;
            if (op == kRgb) {
                px[0] = r.u8(at), px[1] = r.u8(at + 1), px[2] = r.u8(at + 2), at += 3;
            } else if (op == kRgba) {
                px = {r.u8(at), r.u8(at + 1), r.u8(at + 2), r.u8(at + 3)}, at += 4;
            } else if ((op & 0xC0) == kIndex) {
                px = index[op & 0x3F];
            } else if ((op & 0xC0) == kDiff) {
                px[0] = static_cast<std::uint8_t>(px[0] + ((op >> 4) & 3) - 2);
                px[1] = static_cast<std::uint8_t>(px[1] + ((op >> 2) & 3) - 2);
                px[2] = static_cast<std::uint8_t>(px[2] + (op & 3) - 2);
            } else if ((op & 0xC0) == kLuma) {
                const int dg = (op & 0x3F) - 32, x = r.u8(at++);
                px[0] = static_cast<std::uint8_t>(px[0] + dg - 8 + (x >> 4));
                px[1] = static_cast<std::uint8_t>(px[1] + dg);
                px[2] = static_cast<std::uint8_t>(px[2] + dg - 8 + (x & 0xF));
            } else {
                run = (op & 0x3F) + 1u;
                if (run > n - i)
                    bad(kName, "run past the last pixel");
            }
            index[slot(px)] = px;
            for (unsigned k = 0; k < run; ++k)
                f.pixels.set(i++, cell(px));
        }
        auto end = r.sub(at, 8);
        if (std::any_of(end.begin(), end.end() - 1, [](std::byte x) { return x != std::byte{0}; }) ||
            end[7] != std::byte{1})
            bad(kName, "missing end marker");
        if (at + 8 != r.d.size())
            d->warnings.push_back({"framing-inconsistent", "/qoi", "bytes after the end marker are ignored"});
        d->frames.push_back(std::move(f));
        d->encoding = {{"channels", std::to_string(d->channels)}};
        d->keys.push_back({"/qoi/colorspace", hash128(r.sub(13, 1)),
                           d->colorspace ? "linear" : "srgb-linear-alpha", "color"});
        auto& m = d->map.ranges;
        m.push_back({0, 4, ByteClass::framing, "/signature", {}});
        m.push_back({4, 8, ByteClass::content, "/shape", {}});
        m.push_back({12, 1, ByteClass::encoding, "/encoding", {}});
        m.push_back({13, 1, ByteClass::content, "/qoi/colorspace", {}});
        m.push_back({14, at - 14, ByteClass::encoding, "/pixels", "qoi"});
        m.push_back({at, 8, ByteClass::framing, "/end", {}});
        d->map.validate(r.d.size());
        return d;
    }

    std::vector<std::byte> write(Plan& p, std::vector<Warning>& warnings) const override {
        auto& src = static_cast<const QoiDoc&>(p.doc(p.enc));
        std::uint8_t cs = 0;
        for (auto& [k, s] : p.keys)
            if (k == "/qoi/colorspace")
                cs = static_cast<const QoiDoc&>(p.doc(s)).colorspace;
        auto out = encode(p.frames.at(0).pixels, src.channels, cs);
        if (std::to_integer<std::uint8_t>(out[12]) != src.channels)
            warnings.push_back(
                {"encoding-promoted", "/encoding/channels", "QOI written with 4 channels for alpha"});
        return out;
    }
};

} // namespace

std::vector<std::byte> write(const raster::Raster& r, std::uint8_t channels, std::uint8_t colorspace) {
    return encode(r, channels, colorspace);
}

std::unique_ptr<Format> make_format() {
    return rastermisc::make_format(std::make_unique<QoiCodec>());
}

} // namespace semsplinter::qoi
