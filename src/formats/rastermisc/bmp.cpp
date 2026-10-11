#include "common.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/formats/bmp.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

namespace semsplinter::bmp {

namespace {

using namespace rastermisc;
constexpr std::string_view kName = "BMP";

constexpr std::uint32_t kRgb = 0, kRle8 = 1, kRle4 = 2, kBitfields = 3, kAlphaBitfields = 6;
constexpr std::uint32_t kLinked = 0x4C494E4B, kEmbedded = 0x4D424544; // 'LINK', 'MBED'

using Masks = std::array<std::uint32_t, 4>; // R, G, B, A

struct BmpDoc final : Doc {
    Encoding enc;
    Masks masks{};
    std::vector<Rgba8> palette;
    std::vector<std::byte> header; // the info header as stored
    std::uint32_t xppm = 0, yppm = 0, important = 0;
    std::array<std::byte, 4> reserved{};
    std::vector<std::byte> profile; // V5 profile data (embedded profile or linked file name)
    bool alpha_bytes = false;       // 32 bpp RGB with a nonzero fourth byte (Q-RAST-8)
};

const BmpDoc& bmp(const Doc& d) {
    return static_cast<const BmpDoc&>(d);
}

// v (n bits) scaled to 8 bits by bit replication, and back.
std::uint8_t widen(std::uint32_t v, unsigned n) {
    if (!n)
        return 0;
    std::uint32_t r = 0;
    for (int s = 8 - static_cast<int>(n); s > -static_cast<int>(n); s -= static_cast<int>(n))
        r |= s >= 0 ? v << s : v >> -s;
    return static_cast<std::uint8_t>(r & 0xFF);
}

std::uint32_t pack(const Rgba8& c, const Masks& m) {
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k)
        if (m[k]) {
            const unsigned n = static_cast<unsigned>(std::popcount(m[k]));
            v |= (std::uint32_t{c[k]} >> (8 - n)) << std::countr_zero(m[k]);
        }
    return v;
}

Rgba8 unpack(std::uint32_t v, const Masks& m) {
    Rgba8 c{0, 0, 0, 255};
    for (int k = 0; k < 4; ++k)
        if (m[k])
            c[k] = widen((v & m[k]) >> std::countr_zero(m[k]), static_cast<unsigned>(std::popcount(m[k])));
    return c;
}

bool representable(const Rgba8& c, const Masks& m) {
    return unpack(pack(c, m), m) == c;
}

std::string header_name(std::uint32_t hs) {
    switch (hs) {
    case 12:
        return "core";
    case 40:
        return "info";
    case 52:
        return "v2";
    case 56:
        return "v3";
    case 64:
        return "os2";
    case 108:
        return "v4";
    default:
        return "v5";
    }
}

std::string compression_name(const Encoding& e, const Masks& m) {
    if (e.compression == kRle8)
        return "rle8";
    if (e.compression == kRle4)
        return "rle4";
    if (e.compression == kBitfields || e.compression == kAlphaBitfields) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "bitfields(%x,%x,%x,%x)", m[0], m[1], m[2], m[3]);
        return buf;
    }
    return "rgb";
}

std::uint32_t stride(std::uint32_t w, unsigned bpp) {
    return static_cast<std::uint32_t>((std::uint64_t{w} * bpp + 31) / 32 * 4);
}

// RLE8/RLE4 decode into bottom-up rows of indices (raster-misc.md §3.2). Skipped pixels keep index 0.
void rle_decode(const Reader& r, std::uint64_t at, std::uint32_t w, std::uint32_t h, bool four,
                std::vector<std::uint8_t>& idx) {
    std::uint64_t x = 0, y = 0;
    auto put = [&](std::uint8_t v) {
        if (x >= w || y >= h)
            bad(kName, "RLE data runs outside the image");
        idx[(h - 1 - y) * std::uint64_t{w} + x++] = v;
    };
    while (at + 2 <= r.d.size() && y < h) { // a missing end-of-bitmap ends the image
        const auto n = r.u8(at), v = r.u8(at + 1);
        at += 2;
        if (n) {
            for (unsigned k = 0; k < n; ++k)
                put(four ? static_cast<std::uint8_t>(k % 2 ? v & 0xF : v >> 4) : v);
        } else if (v == 0) {
            x = 0, ++y;
        } else if (v == 1) {
            return;
        } else if (v == 2) {
            x += r.u8(at), y += r.u8(at + 1);
            at += 2;
        } else {
            for (unsigned k = 0; k < v; ++k) {
                const auto b = r.u8(at + (four ? k / 2 : k));
                put(four ? static_cast<std::uint8_t>(k % 2 ? b & 0xF : b >> 4) : b);
            }
            const unsigned bytes = four ? (v + 1u) / 2 : v;
            at += bytes + bytes % 2;
        }
    }
}

// Deterministic RLE: runs of two or more equal indices, absolute mode for three or more others, EOL per
// row and EOB at the end.
std::vector<std::byte> rle_encode(const std::vector<std::uint8_t>& idx, std::uint32_t w, std::uint32_t h,
                                  bool four) {
    std::vector<std::byte> o;
    auto b = [&](unsigned v) { o.push_back(std::byte(v & 0xFF)); };
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint8_t* p = idx.data() + std::uint64_t{h - 1 - row} * w;
        std::uint32_t x = 0;
        while (x < w) {
            std::uint32_t run = 1;
            while (x + run < w && run < 255 && p[x + run] == p[x])
                ++run;
            if (run >= 2) {
                b(run), b(four ? (p[x] << 4) | p[x] : p[x]);
                x += run;
                continue;
            }
            std::uint32_t lit = 1;
            while (x + lit < w && lit < 255 && (x + lit + 1 >= w || p[x + lit] != p[x + lit + 1]))
                ++lit;
            if (lit < 3) {
                for (std::uint32_t k = 0; k < lit; ++k)
                    b(1), b(four ? p[x + k] << 4 : p[x + k]);
            } else {
                b(0), b(lit);
                const auto start = o.size();
                for (std::uint32_t k = 0; k < lit; k += four ? 2 : 1)
                    b(four ? (p[x + k] << 4) | (k + 1 < lit ? p[x + k + 1] : 0) : p[x + k]);
                if ((o.size() - start) % 2)
                    b(0);
            }
            x += lit;
        }
        b(0), b(row + 1 == h ? 1 : 0);
    }
    return o;
}

// The keys a written file carries, from the sides the merge chose.
struct KeyValues {
    std::uint32_t xppm = 0, yppm = 0;
    std::vector<std::byte> colorspace; // header bytes 56.. (V4: 52 bytes, V5: 56 with the intent)
    const BmpDoc* icc = nullptr;       // the side whose V5 profile is written
};

std::vector<std::byte> encode(const Raster& px,
                              std::optional<std::pair<std::vector<std::uint8_t>, std::vector<Rgba8>>> exact,
                              const BmpDoc* src, Encoding enc, const KeyValues& keys,
                              std::vector<Warning>& warnings) {
    const std::uint32_t w = px.width, h = px.height;
    const std::uint64_t n = px.cells();
    std::vector<Rgba8> cells(n);
    bool has_alpha = false, any_visible = false;
    for (std::uint64_t i = 0; i < n; ++i) {
        cells[i] = rgba8(px.get(i));
        has_alpha |= cells[i][3] != 255;
        any_visible |= cells[i][3] != 0;
    }
    const Encoding asked = enc;
    Masks masks = src && src->enc.bpp == enc.bpp ? src->masks : Masks{};
    if (enc.bpp == 16 && !masks[0])
        masks = {0x7C00, 0x03E0, 0x001F, 0};
    if (enc.bpp == 32 && !masks[0])
        masks = {0xFF0000, 0xFF00, 0xFF, 0};
    std::vector<Rgba8> palette;
    std::vector<std::uint8_t> index;
    if (enc.bpp <= 8) {
        bool ok = !has_alpha;
        if (ok && exact && exact->second.size() <= (std::size_t{1} << enc.bpp) &&
            std::all_of(exact->second.begin(), exact->second.end(),
                        [](const Rgba8& c) { return c[3] == 255; })) {
            index = std::move(exact->first), palette = std::move(exact->second);
        } else if (ok) {
            std::vector<Rgba8> base = src ? src->palette : std::vector<Rgba8>{};
            for (unsigned bpp : {unsigned{enc.bpp}, 8u}) {
                if (bpp < enc.bpp || base.size() > (std::size_t{1} << bpp))
                    continue;
                if (auto p = palettize(px, base, std::size_t{1} << bpp, index)) {
                    palette = std::move(*p);
                    if (bpp != enc.bpp)
                        enc.bpp = 8, enc.compression = enc.compression == kRle4 ? kRle8 : enc.compression;
                    break;
                }
            }
            ok = !palette.empty();
        }
        if (!ok)
            enc.bpp = has_alpha ? 32 : 24, enc.compression = kRgb;
    }
    if (enc.bpp == 16 ||
        (enc.bpp == 32 && (enc.compression == kBitfields || enc.compression == kAlphaBitfields))) {
        bool fits =
            std::all_of(cells.begin(), cells.end(), [&](const Rgba8& c) { return representable(c, masks); });
        if (!fits)
            enc.bpp = 32, enc.compression = kRgb, masks = {0xFF0000, 0xFF00, 0xFF, 0};
    }
    if (enc.bpp == 24 && has_alpha)
        enc.bpp = 32, enc.compression = kRgb;
    if (enc.bpp == 32 && enc.compression == kRgb) {
        masks = {0xFF0000, 0xFF00, 0xFF, enc.header >= 56 ? masks[3] : 0};
        if (has_alpha && !masks[3] && !any_visible) // all-zero fourth bytes read as padding: declare alpha
            enc.compression = kBitfields, masks[3] = 0xFF000000;
    }
    if (enc.compression == kBitfields && masks[3] && enc.header < 56)
        enc.header = 56;
    if (enc.compression == kRle8 || enc.compression == kRle4)
        enc.top_down = false;
    // Header version from the keys it must carry (raster-misc.md §3.2).
    if (keys.icc || keys.colorspace.size() == 56)
        enc.header = 124;
    else if (!keys.colorspace.empty())
        enc.header = std::max<std::uint32_t>(enc.header, 108);
    else if (enc.header >= 108 || enc.header == 64)
        enc.header = masks[3] ? 56 : 40;
    if (enc.header == 12 && (keys.xppm || keys.yppm || enc.top_down || enc.bpp == 16 || enc.bpp == 32 ||
                             enc.bpp == 2 || enc.compression != kRgb))
        enc.header = 40;
    if (enc.header == 52 && masks[3])
        enc.header = 56;
    if (enc.bpp != asked.bpp || enc.compression != asked.compression)
        warnings.push_back(
            {"encoding-promoted", "/encoding",
             "BMP written as " + std::to_string(enc.bpp) + " bpp " + compression_name(enc, masks)});
    const bool bitfields = enc.compression == kBitfields || enc.compression == kAlphaBitfields;
    if (enc.compression == kAlphaBitfields)
        enc.compression = kBitfields, enc.header = std::max<std::uint32_t>(enc.header, 56);

    // Pixels.
    std::vector<std::byte> pix;
    if (enc.compression == kRle8 || enc.compression == kRle4) {
        pix = rle_encode(index, w, h, enc.compression == kRle4);
    } else {
        const std::uint32_t st = stride(w, enc.bpp);
        pix.assign(std::uint64_t{st} * h, std::byte{0});
        const bool alpha_byte = has_alpha || (src && src->alpha_bytes);
        for (std::uint32_t k = 0; k < h; ++k) {
            const std::uint32_t y = enc.top_down ? k : h - 1 - k;
            std::byte* row = pix.data() + std::uint64_t{k} * st;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint64_t i = std::uint64_t{y} * w + x;
                const auto& c = cells[i];
                if (enc.bpp <= 8) {
                    const unsigned bit = x * enc.bpp;
                    row[bit / 8] |= std::byte(index[i] << (8 - enc.bpp - bit % 8));
                } else if (enc.bpp == 24) {
                    row[3 * x] = std::byte(c[2]), row[3 * x + 1] = std::byte(c[1]),
                            row[3 * x + 2] = std::byte(c[0]);
                } else if (enc.bpp == 32 && !bitfields) {
                    row[4 * x] = std::byte(c[2]), row[4 * x + 1] = std::byte(c[1]),
                            row[4 * x + 2] = std::byte(c[0]);
                    row[4 * x + 3] = std::byte(alpha_byte ? c[3] : 0);
                } else {
                    const std::uint32_t v = pack(c, masks);
                    for (unsigned b = 0; b < enc.bpp / 8u; ++b)
                        row[enc.bpp / 8u * x + b] = std::byte((v >> (8 * b)) & 0xFF);
                }
            }
        }
    }

    // Layout on the Emitter (raster-misc.md §7.1): bfSize, bfOffBits, biSizeImage, and the V5 profile offset
    // are fields.
    enum : emit::GroupId { kFile = 1, kHeader, kPixels, kProfile };
    using emit::Endian;
    auto le = [](std::uint64_t v, unsigned width) {
        std::vector<std::byte> o;
        put_le(o, v, width);
        return emit::bytes(o);
    };
    std::vector<emit::Seg> hdr;
    const std::uint32_t hs = enc.header;
    hdr.push_back(le(hs, 4));
    if (hs == 12) {
        hdr.push_back(le(w, 2)), hdr.push_back(le(h, 2)), hdr.push_back(le(1, 2)),
            hdr.push_back(le(enc.bpp, 2));
    } else {
        hdr.push_back(le(w, 4));
        hdr.push_back(le(enc.top_down ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(h)) : h, 4));
        hdr.push_back(le(1, 2)), hdr.push_back(le(enc.bpp, 2)), hdr.push_back(le(enc.compression, 4));
        hdr.push_back(emit::field(4, Endian::little, emit::size_of(kPixels)));
        hdr.push_back(le(keys.xppm, 4)), hdr.push_back(le(keys.yppm, 4));
        hdr.push_back(
            le(enc.bpp <= 8 && palette.size() < (std::size_t{1} << enc.bpp) ? palette.size() : 0, 4));
        hdr.push_back(
            le(src ? std::min<std::uint32_t>(src->important,
                                             enc.bpp <= 8 ? static_cast<std::uint32_t>(palette.size()) : 0)
                   : 0,
               4));
        if (hs >= 52)
            for (int k = 0; k < (hs >= 56 ? 4 : 3); ++k)
                hdr.push_back(le(bitfields ? masks[static_cast<std::size_t>(k)] : 0, 4));
        if (hs >= 108) {
            std::vector<std::byte> cs = keys.colorspace;
            if (keys.icc)
                cs.assign(keys.icc->header.begin() + 56, keys.icc->header.begin() + 112);
            cs.resize(hs == 124 ? 56 : 52); // a V4 color space in a V5 header gets intent 0
            hdr.push_back(emit::bytes(cs));
        }
        if (hs == 124) {
            if (keys.icc) {
                hdr.push_back(emit::field(4, Endian::little, emit::offset_of(kProfile, kHeader)));
                hdr.push_back(emit::field(4, Endian::little, emit::size_of(kProfile)));
            } else {
                hdr.push_back(le(0, 8));
            }
            hdr.push_back(le(0, 4));
        }
    }
    std::vector<emit::Seg> file{
        emit::bytes(std::string_view("BM")), emit::field(4, Endian::little, emit::size_of(kFile)),
        emit::bytes(src ? std::span<const std::byte>(src->reserved)
                        : std::span<const std::byte>(std::array<std::byte, 4>{})),
        emit::field(4, Endian::little, emit::offset_of(kPixels)), emit::group(kHeader, std::move(hdr))};
    if (hs == 40 && bitfields)
        for (int k = 0; k < 3; ++k)
            file.push_back(le(masks[static_cast<std::size_t>(k)], 4));
    if (enc.bpp <= 8) {
        std::vector<std::byte> t;
        for (auto& c : palette) {
            t.push_back(std::byte(c[2])), t.push_back(std::byte(c[1])), t.push_back(std::byte(c[0]));
            if (hs != 12)
                t.push_back(std::byte{0});
        }
        file.push_back(emit::bytes(t));
    }
    file.push_back(emit::group(kPixels, {emit::bytes(pix)}));
    if (keys.icc && hs == 124)
        file.push_back(emit::group(kProfile, {emit::bytes(keys.icc->profile)}));
    return emit::emit(emit::group(kFile, std::move(file)));
}

std::string colorspace_text(std::span<const std::byte> cs) {
    Reader r{cs, kName};
    const std::uint32_t type = r.le32(0);
    std::string s = "type=";
    if (type == 0)
        s += "calibrated";
    else if (type >= 0x20202020 && type <= 0x7A7A7A7A)
        for (int k = 3; k >= 0; --k)
            s += static_cast<char>((type >> (8 * k)) & 0xFF);
    else
        s += std::to_string(type);
    if (cs.size() >= 56)
        s += " intent=" + std::to_string(r.le32(52));
    return s + " xxh3=" + hex16(hash128(cs));
}

class BmpCodec final : public Codec {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "bmp", "Windows bitmap (BMP, DIB)", {".bmp", ".dib"}, {true, true, false, true, true}, {}};
        return i;
    }

    int probe(std::span<const std::byte> head, std::string_view) const override {
        if (head.size() < 18 || head[0] != std::byte{'B'} || head[1] != std::byte{'M'})
            return 0;
        const auto hs = Reader{head, kName}.le32(14);
        return hs == 12 || hs == 40 || hs == 52 || hs == 56 || hs == 64 || hs == 108 || hs == 124 ? 90 : 0;
    }

    std::unique_ptr<Doc> load(ByteSource src, Limits& budget, const OptionValues&) const override {
        auto d = std::make_unique<BmpDoc>();
        d->src = std::move(src);
        const Reader r{d->src.bytes(), kName};
        if (r.u8(0) != 'B' || r.u8(1) != 'M')
            bad(kName, "missing BM signature");
        const std::uint32_t file_size = r.le32(2), off = r.le32(10), hs = r.le32(14);
        if (hs != 12 && hs != 40 && hs != 52 && hs != 56 && hs != 64 && hs != 108 && hs != 124)
            bad(kName, "unknown info header size " + std::to_string(hs));
        auto hb = r.sub(14, hs);
        d->header.assign(hb.begin(), hb.end());
        std::memcpy(d->reserved.data(), r.sub(6, 4).data(), 4);
        auto& e = d->enc;
        e.header = hs;
        std::int64_t w, h;
        std::uint32_t size_image = 0, used = 0;
        if (hs == 12) {
            w = r.le16(18), h = r.le16(20), e.bpp = r.le16(24);
        } else {
            w = static_cast<std::int32_t>(r.le32(18)), h = static_cast<std::int32_t>(r.le32(22));
            e.bpp = r.le16(28), e.compression = r.le32(30), size_image = r.le32(34);
            d->xppm = r.le32(38), d->yppm = r.le32(42), used = r.le32(46), d->important = r.le32(50);
        }
        e.top_down = h < 0;
        h = h < 0 ? -h : h;
        if (w <= 0 || h <= 0 || w > 0x7FFFFFFF || h > 0x7FFFFFFF)
            bad(kName, "bad image size");
        if (hs == 64 && e.compression > kRle4)
            throw Error(ErrorCategory::unsupported,
                        "BMP: OS/2 Huffman and RLE24 compression are not supported");
        const unsigned bpp = e.bpp;
        const bool rle = e.compression == kRle8 || e.compression == kRle4;
        const bool bitfields = e.compression == kBitfields || e.compression == kAlphaBitfields;
        if (e.compression == 4 || e.compression == 5)
            throw Error(ErrorCategory::unsupported, "BMP: embedded JPEG or PNG data is not supported");
        if ((bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) ||
            e.compression > kAlphaBitfields || (e.compression == kRle8 && bpp != 8) ||
            (e.compression == kRle4 && bpp != 4) || (bitfields && bpp != 16 && bpp != 32) ||
            (rle && e.top_down))
            bad(kName,
                "illegal bpp " + std::to_string(bpp) + " with compression " + std::to_string(e.compression));
        // Masks: in the header from V2 on, else after an INFO header with BITFIELDS.
        std::uint64_t table = 14 + hs;
        if (bitfields && hs == 40) {
            for (int k = 0; k < (e.compression == kAlphaBitfields ? 4 : 3); ++k)
                d->masks[static_cast<std::size_t>(k)] = r.le32(table + 4u * static_cast<unsigned>(k));
            table += e.compression == kAlphaBitfields ? 16 : 12;
        } else if (hs >= 52) {
            for (int k = 0; k < (hs >= 56 ? 4 : 3); ++k)
                d->masks[static_cast<std::size_t>(k)] = r.le32(54 + 4u * static_cast<unsigned>(k));
            if (!bitfields)
                d->masks = {0, 0, 0, bpp == 32 ? d->masks[3] : 0}; // an alpha mask declares alpha (Q-RAST-8)
        }
        if (!bitfields && bpp == 16)
            d->masks = {0x7C00, 0x03E0, 0x001F, 0};
        if (!bitfields && bpp == 32)
            d->masks = {0xFF0000, 0xFF00, 0xFF, d->masks[3]};
        for (auto m : d->masks)
            if (m && (std::popcount(m) > 8 ||
                      ((m >> std::countr_zero(m)) & ((m >> std::countr_zero(m)) + 1)) != 0))
                throw Error(ErrorCategory::unsupported,
                            "BMP: bitfield masks must be contiguous and at most 8 bits");
        if (bpp <= 8) {
            const std::uint32_t n = used && used < (1u << bpp) ? used : 1u << bpp;
            const unsigned es = hs == 12 ? 3 : 4;
            const auto avail = off > table ? (off - table) / es : 0;
            if (avail == 0)
                bad(kName, "missing color table");
            for (std::uint32_t k = 0; k < std::min<std::uint64_t>(n, avail); ++k) {
                auto c = r.sub(table + std::uint64_t{k} * es, 3);
                d->palette.push_back({std::to_integer<std::uint8_t>(c[2]),
                                      std::to_integer<std::uint8_t>(c[1]),
                                      std::to_integer<std::uint8_t>(c[0]), 255});
            }
        }
        const auto W = static_cast<std::uint32_t>(w), H = static_cast<std::uint32_t>(h);
        Frame f;
        f.pixels = new_raster(W, H, false, budget);
        const std::uint64_t cells = f.pixels.cells();
        std::uint64_t pixel_bytes;
        if (off > r.d.size())
            bad(kName, "bfOffBits outside the file");
        if (rle) {
            f.indices.assign(cells, 0);
            rle_decode(r, off, W, H, e.compression == kRle4, f.indices);
            pixel_bytes = r.d.size() - off; // up to EOB; refined below for the map
        } else {
            const std::uint32_t st = stride(W, bpp);
            pixel_bytes = std::uint64_t{st} * H;
            auto data = r.sub(off, pixel_bytes);
            if (bpp <= 8)
                f.indices.resize(cells);
            bool fourth = false;
            for (std::uint32_t k = 0; k < H; ++k) {
                const std::uint32_t y = e.top_down ? k : H - 1 - k;
                auto row = data.subspan(std::uint64_t{k} * st, st);
                for (std::uint32_t x = 0; x < W; ++x) {
                    const std::uint64_t i = std::uint64_t{y} * W + x;
                    auto at = [&](std::size_t j) { return std::to_integer<std::uint32_t>(row[j]); };
                    if (bpp <= 8) {
                        const unsigned bit = x * bpp;
                        f.indices[i] = static_cast<std::uint8_t>((at(bit / 8) >> (8 - bpp - bit % 8)) &
                                                                 ((1u << bpp) - 1));
                    } else if (bpp == 24) {
                        f.pixels.set(i, cell({static_cast<std::uint8_t>(at(3 * x + 2)),
                                              static_cast<std::uint8_t>(at(3 * x + 1)),
                                              static_cast<std::uint8_t>(at(3 * x)), 255}));
                    } else {
                        std::uint32_t v = 0;
                        for (unsigned b = 0; b < bpp / 8; ++b)
                            v |= at(bpp / 8 * x + b) << (8 * b);
                        auto c = unpack(v, d->masks);
                        if (bpp == 32 && !bitfields && !d->masks[3]) {
                            c[3] = static_cast<std::uint8_t>(v >> 24);
                            fourth |= c[3] != 0;
                        }
                        f.pixels.set(i, cell(c));
                    }
                }
            }
            if (bpp == 32 && !bitfields && !d->masks[3]) {
                d->alpha_bytes = fourth;
                if (!fourth) // all fourth bytes zero: padding, opaque (Q-RAST-8)
                    for (std::uint64_t i = 0; i < cells; ++i) {
                        auto c = f.pixels.get(i);
                        c[3] = 0xFFFF;
                        f.pixels.set(i, c);
                    }
            }
        }
        if (!f.indices.empty()) {
            for (std::uint64_t i = 0; i < cells; ++i) {
                if (f.indices[i] >= d->palette.size())
                    bad(kName, "palette index " + std::to_string(f.indices[i]) + " outside the color table");
                f.pixels.set(i, cell(d->palette[f.indices[i]]));
            }
            f.palette = d->palette;
        }
        f.encoding = {};
        d->frames.push_back(std::move(f));
        d->encoding = {{"header", header_name(hs)},
                       {"bpp", std::to_string(bpp)},
                       {"compression", compression_name(e, d->masks)},
                       {"rows", e.top_down ? "top-down" : "bottom-up"}};

        // Keys (raster-misc.md §3.2).
        if (d->xppm || d->yppm) {
            Hasher hh;
            hh.add(d->xppm).add(d->yppm);
            d->keys.push_back({"/bmp/ppm", hh.digest(),
                               std::to_string(d->xppm) + "x" + std::to_string(d->yppm) + " per-metre"});
        }
        std::uint64_t profile_at = 0;
        if (hs >= 108) {
            auto cs = std::span<const std::byte>(d->header).subspan(56, hs == 124 ? 56 : 52);
            const std::uint32_t type = Reader{cs, kName}.le32(0);
            if (hs == 124 && (type == kEmbedded || type == kLinked)) {
                profile_at = 14 + std::uint64_t{Reader{d->header, kName}.le32(112)};
                auto p = r.sub(profile_at, Reader{d->header, kName}.le32(116));
                d->profile.assign(p.begin(), p.end());
                Hasher hh;
                hh.raw(cs).raw(p);
                d->keys.push_back({"/icc", hh.digest(),
                                   std::string(type == kEmbedded ? "embedded " : "linked ") + bytes_text(p),
                                   "color"});
            } else {
                d->keys.push_back({"/bmp/colorspace", hash128(cs), colorspace_text(cs), "color"});
            }
        }
        // Advisory size fields (Q-RAST-9).
        if (file_size != r.d.size())
            d->warnings.push_back({"framing-inconsistent", "/bmp",
                                   "bfSize " + std::to_string(file_size) + " differs from the file size " +
                                       std::to_string(r.d.size())});
        if (size_image && !rle && size_image != pixel_bytes)
            d->warnings.push_back({"framing-inconsistent", "/bmp",
                                   "biSizeImage " + std::to_string(size_image) + " differs from " +
                                       std::to_string(pixel_bytes)});
        if (rle)
            pixel_bytes = size_image && size_image <= r.d.size() - off ? size_image : r.d.size() - off;

        // Envelope map (raster-misc.md §7.1).
        auto& m = d->map.ranges;
        auto add = [&](std::uint64_t at, std::uint64_t len, ByteClass c, std::string unit,
                       std::string layer = {}) {
            if (len)
                m.push_back({at, len, c, std::move(unit), std::move(layer)});
        };
        add(0, 2, ByteClass::framing, "/signature");
        add(2, 4, ByteClass::framing, "/bmp");
        add(6, 4, ByteClass::content, "/bmp");
        add(10, 4, ByteClass::framing, "/bmp");
        add(14, 4, ByteClass::framing, "/bmp");
        if (hs == 12) {
            add(18, 4, ByteClass::content, "/shape");
            add(22, 4, ByteClass::encoding, "/encoding");
        } else {
            add(18, 8, ByteClass::content, "/shape");
            add(26, 8, ByteClass::encoding, "/encoding");
            add(34, 4, ByteClass::framing, "/pixels");
            add(38, 8, ByteClass::content, "/bmp/ppm");
            add(46, 4, ByteClass::framing, "/pixels");
            add(50, 4, ByteClass::encoding, "/encoding");
            if (hs != 64)
                add(54, std::min(hs, 56u) - 40, ByteClass::encoding, "/encoding", "bitfields");
            if (hs >= 108) {
                const bool icc = !d->profile.empty() || find_key(d->keys, "/icc");
                add(70, hs == 124 ? 56 : 52, ByteClass::content, icc ? "/icc" : "/bmp/colorspace");
            }
            if (hs == 124) {
                add(126, 8, ByteClass::framing, "/icc");
                add(134, 4, ByteClass::content, "/bmp");
            }
            if (hs == 64)
                add(54, 24, ByteClass::content, "/bmp");
        }
        if (table > 14 + hs)
            add(14 + hs, table - 14 - hs, ByteClass::encoding, "/encoding", "bitfields");
        if (bpp <= 8)
            add(table,
                std::min<std::uint64_t>(off, table + std::uint64_t{d->palette.size()} * (hs == 12 ? 3 : 4)) -
                    table,
                ByteClass::encoding, "/pixels", "palette");
        if (off >= table + (bpp <= 8 ? d->palette.size() * (hs == 12 ? 3u : 4u) : 0))
            add(off, pixel_bytes, rle ? ByteClass::encoding : ByteClass::content, "/pixels",
                rle ? "rle" : "");
        if (!d->profile.empty() && profile_at >= off + pixel_bytes)
            add(profile_at, d->profile.size(), ByteClass::content, "/icc");
        std::sort(m.begin(), m.end(),
                  [](const EnvRange& a, const EnvRange& b) { return a.offset < b.offset; });
        // Drop overlaps from odd layouts (a profile inside the pixel area): the map stays advisory.
        for (std::size_t k = 1; k < m.size();)
            if (m[k].offset < m[k - 1].offset + m[k - 1].length)
                m.erase(m.begin() + static_cast<std::ptrdiff_t>(k));
            else
                ++k;
        d->map.validate(r.d.size());
        return d;
    }

    std::vector<std::byte> write(Plan& p, std::vector<Warning>& warnings) const override {
        const BmpDoc& src = bmp(p.doc(p.enc));
        KeyValues kv;
        for (auto& [k, s] : p.keys) {
            const BmpDoc& d = bmp(p.doc(s));
            if (k == "/bmp/ppm")
                kv.xppm = d.xppm, kv.yppm = d.yppm;
            else if (k == "/bmp/colorspace")
                kv.colorspace.assign(d.header.begin() + 56,
                                     d.header.begin() + (d.enc.header == 124 ? 112 : 108));
            else if (k == "/icc")
                kv.icc = &d;
        }
        if (kv.icc && !kv.colorspace.empty()) {
            p.dropped.push_back("/bmp/colorspace");
            warnings.push_back(
                {"key-dropped", "/bmp/colorspace", "a V5 header holds either a profile or a color space"});
            kv.colorspace.clear();
        }
        const OutFrame& f = p.frames.at(0);
        return encode(f.pixels, merged_indices(p, f), &src, src.enc, kv, warnings);
    }
};

} // namespace

std::vector<std::byte> write(const raster::Raster& r, Encoding enc) {
    std::vector<Warning> ignore;
    KeyValues kv;
    if (enc.header >= 108) { // sRGB, and for V5 the intent LCS_GM_IMAGES
        put_le(kv.colorspace, 0x73524742, 4);
        kv.colorspace.resize(enc.header == 124 ? 56 : 52);
        if (enc.header == 124)
            kv.colorspace[52] = std::byte{4};
    }
    return encode(r, std::nullopt, nullptr, enc, kv, ignore);
}

std::unique_ptr<Format> make_format() {
    return rastermisc::make_format(std::make_unique<BmpCodec>());
}

} // namespace semsplinter::bmp
