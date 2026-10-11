#include "common.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/formats/tga.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace semsplinter::tga {

namespace {

using namespace rastermisc;
constexpr std::string_view kName = "TGA";
constexpr std::string_view kSignature{"TRUEVISION-XFILE.\0", 18};
constexpr std::uint64_t kExtSize = 495;

// TGA 2.0 extension-area fields that are keys (raster-misc.md §3.3); offsets within the area.
struct ExtField {
    const char* name;
    std::size_t at, len;
    bool text, time, color;
};
constexpr ExtField kExt[] = {
    {"author", 2, 41, true, false, false},     {"comments", 43, 324, true, false, false},
    {"datetime", 367, 12, false, true, false}, {"job", 379, 41, true, false, false},
    {"jobtime", 420, 6, false, false, false},  {"software", 426, 41, true, false, false},
    {"version", 467, 3, false, false, false},  {"keycolor", 470, 4, false, false, false},
    {"aspect", 474, 4, false, false, false},   {"gamma", 478, 4, false, false, true},
    {"attribute", 494, 1, false, false, true},
};

struct TgaDoc final : Doc {
    Encoding enc;
    std::uint8_t map_bits = 0, alpha_bits = 0;
    std::uint16_t map_first = 0;
    std::vector<Rgba8> palette;
    std::vector<std::byte> id, ext;           // ID field; extension area (495 bytes) or empty
    std::array<std::byte, 4> origin{};        // x and y origin
    std::vector<std::byte> stamp, correction; // postage stamp (w, h, pixels), color-correction table
    std::map<std::uint16_t, std::vector<std::byte>> dev; // developer-area entries by tag
};

const TgaDoc& tga(const Doc& d) {
    return static_cast<const TgaDoc&>(d);
}

std::uint8_t five(unsigned v) {
    return static_cast<std::uint8_t>((v << 3) | (v >> 2));
}

// One stored pixel of `bpp` bits as RGBA (gray when `gray`).
Rgba8 read_px(std::span<const std::byte> p, unsigned bpp, bool gray, bool alpha) {
    auto at = [&](std::size_t k) { return std::to_integer<std::uint8_t>(p[k]); };
    if (gray)
        return {at(0), at(0), at(0), bpp == 16 && alpha ? at(1) : std::uint8_t{255}};
    if (bpp == 15 || bpp == 16) {
        const unsigned v = at(0) | at(1) << 8;
        return {five(v >> 10 & 31), five(v >> 5 & 31), five(v & 31),
                static_cast<std::uint8_t>(bpp == 16 && alpha ? (v >> 15 ? 255 : 0) : 255)};
    }
    return {at(2), at(1), at(0), bpp == 32 && alpha ? at(3) : std::uint8_t{255}};
}

void put_px(std::vector<std::byte>& o, const Rgba8& c, unsigned bpp, bool gray, bool alpha) {
    if (gray) {
        o.push_back(std::byte(c[0]));
        if (bpp == 16)
            o.push_back(std::byte(c[3]));
    } else if (bpp == 15 || bpp == 16) {
        const unsigned v = (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3 | (alpha && c[3] ? 0x8000u : 0u);
        o.push_back(std::byte(v & 0xFF)), o.push_back(std::byte(v >> 8));
    } else {
        o.push_back(std::byte(c[2])), o.push_back(std::byte(c[1])), o.push_back(std::byte(c[0]));
        if (bpp == 32)
            o.push_back(std::byte(alpha ? c[3] : 255));
    }
}

bool fits(const Rgba8& c, unsigned bpp, bool gray, bool alpha) {
    std::vector<std::byte> o;
    put_px(o, c, bpp, gray, alpha);
    return read_px(o, bpp, gray, alpha) == c;
}

std::string ext_text(const ExtField& f, std::span<const std::byte> v) {
    const Reader r{v, kName};
    char buf[64];
    if (f.text) {
        std::string s(reinterpret_cast<const char*>(v.data()), v.size());
        if (std::string_view(f.name) == "comments") { // four lines of 81 bytes
            std::string out;
            for (std::size_t k = 0; k < 4; ++k) {
                auto line = s.substr(k * 81, 81);
                line = line.substr(0, line.find('\0'));
                out += (k ? "\\n" : "") + line;
            }
            while (out.ends_with("\\n"))
                out.resize(out.size() - 2);
            return "\"" + out + "\"";
        }
        return "\"" + s.substr(0, s.find('\0')) + "\"";
    }
    if (std::string_view(f.name) == "datetime") { // month, day, year, hour, minute, second
        std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", r.le16(4), r.le16(0), r.le16(2),
                      r.le16(6), r.le16(8), r.le16(10));
        return buf;
    }
    if (std::string_view(f.name) == "version") {
        std::snprintf(buf, sizeof buf, "%u.%02u%c", r.le16(0) / 100, r.le16(0) % 100,
                      std::to_integer<char>(v[2]) ? std::to_integer<char>(v[2]) : ' ');
        return buf;
    }
    if (std::string_view(f.name) == "attribute")
        return std::to_string(r.u8(0));
    std::string s;
    for (std::size_t k = 0; k + 1 < v.size(); k += 2)
        s += (k ? "," : "") + std::to_string(r.le16(k));
    return s.empty() ? std::to_string(r.u8(0)) : s;
}

bool zero(std::span<const std::byte> v) {
    return std::all_of(v.begin(), v.end(), [](std::byte b) { return b == std::byte{0}; });
}

// What the writer emits besides the pixels: the keys' bytes from the sides the merge chose.
struct Extras {
    std::vector<std::byte> id, ext; // ext: 495 bytes or empty
    std::array<std::byte, 4> origin{};
    const TgaDoc* stamp = nullptr;
    std::vector<std::byte> correction;
    std::map<std::uint16_t, std::vector<std::byte>> dev;
    bool footer = true;
};

std::vector<std::byte> encode(const Raster& px,
                              std::optional<std::pair<std::vector<std::uint8_t>, std::vector<Rgba8>>> exact,
                              const TgaDoc* src, Encoding enc, std::uint8_t map_bits, Extras x,
                              std::vector<Warning>& warnings, bool& attribute_rewritten) {
    const std::uint32_t w = px.width, h = px.height;
    std::vector<Rgba8> cells(px.cells());
    bool has_alpha = false, gray = true;
    for (std::uint64_t i = 0; i < cells.size(); ++i) {
        cells[i] = rgba8(px.get(i));
        has_alpha |= cells[i][3] != 255;
        gray &= cells[i][0] == cells[i][1] && cells[i][1] == cells[i][2];
    }
    const Encoding asked = enc;
    std::vector<Rgba8> palette;
    std::vector<std::uint8_t> index;
    if (enc.type == 3 && (!gray || (has_alpha && enc.bpp != 16)))
        enc.type = 2, enc.bpp = has_alpha ? 32 : 24;
    if (enc.type == 1) {
        if (!map_bits)
            map_bits = has_alpha ? 32 : 24;
        if (has_alpha && map_bits == 24)
            map_bits = 32;
        if (exact && exact->second.size() <= 256)
            index = std::move(exact->first), palette = std::move(exact->second);
        else if (auto p = palettize(px, src ? src->palette : std::vector<Rgba8>{}, 256, index))
            palette = std::move(*p);
        const bool alpha = map_bits == 32 || map_bits == 16;
        if (palette.empty() || !std::all_of(palette.begin(), palette.end(),
                                            [&](const Rgba8& c) { return fits(c, map_bits, false, alpha); }))
            enc.type = 2, enc.bpp = has_alpha ? 32 : 24, palette.clear();
        else
            enc.bpp = 8;
    }
    bool alpha = enc.bpp == 32 || (enc.type == 3 && enc.bpp == 16) ||
                 (enc.type == 2 && enc.bpp == 16 && (has_alpha || (src && src->alpha_bits)));
    if (enc.type == 2 && !std::all_of(cells.begin(), cells.end(),
                                      [&](const Rgba8& c) { return fits(c, enc.bpp, false, alpha); }))
        enc.bpp = has_alpha ? 32 : 24, alpha = has_alpha;
    if (enc.type == 2 && enc.bpp == 24 && has_alpha)
        enc.bpp = 32, alpha = true;
    if (enc.type != asked.type || enc.bpp != asked.bpp)
        warnings.push_back(
            {"encoding-promoted", "/encoding",
             "TGA written as type " + std::to_string(enc.type) + " " + std::to_string(enc.bpp) + " bpp"});
    // A TGA 2.0 attribute type below 3 says the alpha bits are not alpha: say straight alpha instead.
    if (!x.ext.empty() && has_alpha && std::to_integer<int>(x.ext[494]) != 3 &&
        std::to_integer<int>(x.ext[494]) != 4) {
        x.ext[494] = std::byte{3};
        attribute_rewritten = true;
        warnings.push_back({"encoding-promoted", "/tga/ext/attribute",
                            "attribute type set to 3 (alpha) for the merged alpha"});
    }
    const std::uint8_t alpha_bits = !alpha ? 0 : enc.bpp == 16 && enc.type == 2 ? 1 : 8;
    const unsigned map_alpha_bits =
        enc.type == 1 && (map_bits == 32 || map_bits == 16) ? (map_bits == 32 ? 8 : 1) : 0;

    // Pixels, row by row in the stored order; RLE packets never cross rows (TGA 2.0).
    const unsigned bytes_pp = (enc.bpp + 7) / 8;
    std::vector<std::byte> pix;
    std::vector<std::byte> row;
    for (std::uint32_t k = 0; k < h; ++k) {
        const std::uint32_t y = enc.top ? k : h - 1 - k;
        row.clear();
        for (std::uint32_t j = 0; j < w; ++j) {
            const std::uint32_t xx = enc.right ? w - 1 - j : j;
            const std::uint64_t i = std::uint64_t{y} * w + xx;
            if (enc.type == 1)
                row.push_back(std::byte(index[i]));
            else
                put_px(row, cells[i], enc.bpp, enc.type == 3, alpha);
        }
        if (!enc.rle) {
            pix.insert(pix.end(), row.begin(), row.end());
            continue;
        }
        for (std::uint32_t j = 0; j < w;) {
            auto same = [&](std::uint32_t a, std::uint32_t b) {
                return std::memcmp(row.data() + a * bytes_pp, row.data() + b * bytes_pp, bytes_pp) == 0;
            };
            std::uint32_t run = 1;
            while (j + run < w && run < 128 && same(j, j + run))
                ++run;
            if (run >= 2) {
                pix.push_back(std::byte(0x80 | (run - 1)));
                pix.insert(pix.end(), row.begin() + j * bytes_pp, row.begin() + (j + 1) * bytes_pp);
                j += run;
                continue;
            }
            std::uint32_t lit = 1;
            while (j + lit < w && lit < 128 && !(j + lit + 1 < w && same(j + lit, j + lit + 1)))
                ++lit;
            pix.push_back(std::byte(lit - 1));
            pix.insert(pix.end(), row.begin() + j * bytes_pp, row.begin() + (j + lit) * bytes_pp);
            j += lit;
        }
    }

    enum : emit::GroupId { kFile = 1, kPixels, kExt, kDev, kStamp, kCorrection, kFirstDev = 100 };
    using emit::Endian;
    std::vector<std::byte> head;
    head.push_back(std::byte(x.id.size()));
    head.push_back(std::byte(enc.type == 1 ? 1 : 0));
    head.push_back(std::byte(enc.type + (enc.rle ? 8 : 0)));
    put_le(head, 0, 2);
    put_le(head, enc.type == 1 ? palette.size() : 0, 2);
    head.push_back(std::byte(enc.type == 1 ? map_bits : 0));
    head.insert(head.end(), x.origin.begin(), x.origin.end());
    put_le(head, w, 2), put_le(head, h, 2);
    head.push_back(std::byte(enc.bpp == 15 ? 16 : enc.bpp));
    head.push_back(std::byte((enc.type == 1 ? map_alpha_bits : alpha_bits) | (enc.right ? 0x10 : 0) |
                             (enc.top ? 0x20 : 0)));
    std::vector<emit::Seg> file{emit::bytes(head), emit::bytes(x.id)};
    if (enc.type == 1) {
        std::vector<std::byte> map;
        for (auto& c : palette)
            put_px(map, c, map_bits, false, map_bits == 32 || map_bits == 16);
        file.push_back(emit::bytes(map));
    }
    file.push_back(emit::group(kPixels, {emit::bytes(pix)}));
    const bool has_ext = !x.ext.empty();
    if (!x.dev.empty()) {
        std::vector<emit::Seg> dir{emit::field(2, Endian::little, emit::constant(x.dev.size()))};
        emit::GroupId g = kFirstDev;
        for (auto& [tag, data] : x.dev) {
            dir.push_back(emit::field(2, Endian::little, emit::constant(tag)));
            dir.push_back(emit::field(4, Endian::little, emit::offset_of(g)));
            dir.push_back(emit::field(4, Endian::little, emit::size_of(g)));
            ++g;
        }
        g = kFirstDev;
        for (auto& [tag, data] : x.dev)
            file.push_back(emit::group(g++, {emit::bytes(data)}));
        file.push_back(emit::group(kDev, std::move(dir)));
    }
    if (has_ext) {
        if (x.stamp)
            file.push_back(emit::group(kStamp, {emit::bytes(x.stamp->stamp)}));
        if (!x.correction.empty())
            file.push_back(emit::group(kCorrection, {emit::bytes(x.correction)}));
        auto e = std::span<const std::byte>(x.ext);
        file.push_back(emit::group(
            kExt, {emit::field(2, Endian::little, emit::constant(kExtSize)), emit::bytes(e.subspan(2, 480)),
                   x.correction.empty() ? emit::field(4, Endian::little, emit::constant(0))
                                        : emit::field(4, Endian::little, emit::offset_of(kCorrection)),
                   x.stamp ? emit::field(4, Endian::little, emit::offset_of(kStamp))
                           : emit::field(4, Endian::little, emit::constant(0)),
                   emit::field(4, Endian::little, emit::constant(0)), // scan-line table: not written
                   emit::bytes(e.subspan(494, 1))}));
    }
    if (has_ext || !x.dev.empty() || x.footer) {
        file.push_back(has_ext ? emit::field(4, Endian::little, emit::offset_of(kExt))
                               : emit::field(4, Endian::little, emit::constant(0)));
        file.push_back(!x.dev.empty() ? emit::field(4, Endian::little, emit::offset_of(kDev))
                                      : emit::field(4, Endian::little, emit::constant(0)));
        file.push_back(emit::bytes(kSignature));
    }
    return emit::emit(emit::group(kFile, std::move(file)));
}

class TgaCodec final : public Codec {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "tga", "Truevision TGA", {".tga", ".icb", ".vda", ".vst"}, {true, true, false, true, true}, {}};
        return i;
    }

    // No magic: header plausibility, 100 with the TGA 2.0 footer (raster-misc.md, probe <= 40 without it).
    int probe(std::span<const std::byte> head, std::string_view) const override {
        if (head.size() < 18)
            return 0;
        const Reader r{head, kName};
        const auto cmt = r.u8(1), type = r.u8(2), bits = r.u8(7), bpp = r.u8(16);
        const bool ok =
            cmt <= 1 && (type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11) &&
            (cmt == 1) == (type % 8 == 1) && r.le16(12) && r.le16(14) &&
            (bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32) &&
            (!cmt || bits == 15 || bits == 16 || bits == 24 || bits == 32) && (r.u8(17) & 0xC0) == 0;
        if (!ok)
            return 0;
        if (head.size() >= 26 &&
            std::memcmp(head.data() + head.size() - 18, kSignature.data(), kSignature.size()) == 0)
            return 100;
        return 30;
    }

    std::unique_ptr<Doc> load(ByteSource src, Limits& budget, const OptionValues&) const override {
        auto d = std::make_unique<TgaDoc>();
        d->src = std::move(src);
        const Reader r{d->src.bytes(), kName};
        const auto size = r.d.size();
        const std::uint8_t idlen = r.u8(0), cmt = r.u8(1), type = r.u8(2), map_bits = r.u8(7), bpp = r.u8(16),
                           desc = r.u8(17);
        const std::uint16_t first = r.le16(3), count = r.le16(5), w = r.le16(12), h = r.le16(14);
        auto& e = d->enc;
        e.type = type & 7, e.rle = type & 8, e.bpp = bpp, e.top = desc & 0x20, e.right = desc & 0x10;
        if (cmt > 1 || (e.type != 1 && e.type != 2 && e.type != 3) || (type & ~11) || !w || !h)
            bad(kName, "unsupported image type " + std::to_string(type));
        if ((e.type == 1 && (bpp != 8 && bpp != 16)) ||
            (e.type == 2 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) ||
            (e.type == 3 && bpp != 8 && bpp != 16))
            bad(kName, "illegal pixel depth " + std::to_string(bpp) + " for type " + std::to_string(e.type));
        if (e.type == 2 && bpp == 16 && (desc & 15) == 0)
            e.bpp = 15;
        d->alpha_bits = desc & 15, d->map_first = first, d->map_bits = cmt ? map_bits : 0;
        auto id = r.sub(18, idlen);
        d->id.assign(id.begin(), id.end());
        std::memcpy(d->origin.data(), r.sub(8, 4).data(), 4);
        // Footer, extension and developer areas.
        bool footer =
            size >= 18 + 26 && std::memcmp(r.d.data() + size - 18, kSignature.data(), kSignature.size()) == 0;
        d->enc.footer = footer;
        std::uint64_t ext_at = 0, dev_at = 0;
        if (footer) {
            ext_at = r.le32(size - 26), dev_at = r.le32(size - 22);
            if (ext_at) {
                auto ext = r.sub(ext_at, kExtSize);
                if (r.le16(ext_at) < kExtSize)
                    bad(kName, "extension area too small");
                d->ext.assign(ext.begin(), ext.end());
            }
        }
        const Reader er{d->ext, kName};
        const int attribute = d->ext.empty() ? -1 : er.u8(494);
        const bool alpha = d->alpha_bits && (attribute < 0 || attribute == 3 || attribute == 4);
        // Color map.
        std::uint64_t at = 18 + std::uint64_t{idlen};
        const unsigned mbytes = (map_bits + 7) / 8;
        if (cmt) {
            if (map_bits != 15 && map_bits != 16 && map_bits != 24 && map_bits != 32)
                bad(kName, "bad color-map entry size");
            auto m = r.sub(at, std::uint64_t{count} * mbytes);
            const bool malpha = map_bits == 32 || (map_bits == 16 && d->alpha_bits);
            for (std::uint32_t k = 0; k < count; ++k)
                d->palette.push_back(
                    read_px(m.subspan(k * mbytes, mbytes), map_bits, false, malpha && alpha));
            at += m.size();
        }
        const std::uint64_t map_at = 18 + std::uint64_t{idlen}, pix_at = at;
        Frame f;
        f.pixels = new_raster(w, h, false, budget);
        const unsigned bpx = (bpp + 7) / 8;
        const std::uint64_t n = f.pixels.cells();
        if (e.type == 1)
            f.indices.resize(n);
        std::uint64_t i = 0;
        auto put = [&](std::span<const std::byte> p) {
            const std::uint64_t k = i++, row = k / w, col = k % w;
            const std::uint64_t y = e.top ? row : h - 1 - row, x = e.right ? w - 1 - col : col;
            const std::uint64_t c = y * w + x;
            if (e.type == 1) {
                const unsigned v =
                    std::to_integer<unsigned>(p[0]) | (bpp == 16 ? std::to_integer<unsigned>(p[1]) << 8 : 0u);
                if (v < first || v - first >= d->palette.size() || v - first > 255)
                    bad(kName, "color-map index outside the map");
                f.indices[c] = static_cast<std::uint8_t>(v - first);
                f.pixels.set(c, cell(d->palette[v - first]));
            } else {
                f.pixels.set(c, cell(read_px(p, e.bpp, e.type == 3, alpha)));
            }
        };
        while (i < n) {
            if (!e.rle) {
                auto all = r.sub(at, n * bpx);
                for (std::uint64_t k = 0; k < n; ++k)
                    put(all.subspan(k * bpx, bpx));
                at += all.size();
                break;
            }
            const auto hdr = r.u8(at++);
            const unsigned cnt = (hdr & 0x7F) + 1u;
            if (cnt > n - i)
                bad(kName, "RLE packet past the last pixel");
            if (hdr & 0x80) {
                auto p = r.sub(at, bpx);
                for (unsigned k = 0; k < cnt; ++k)
                    put(p);
                at += bpx;
            } else {
                auto p = r.sub(at, std::uint64_t{cnt} * bpx);
                for (unsigned k = 0; k < cnt; ++k)
                    put(p.subspan(k * bpx, bpx));
                at += p.size();
            }
        }
        if (e.type == 1)
            f.palette = d->palette;
        const std::uint64_t pix_end = at;
        d->frames.push_back(std::move(f));
        d->encoding = {{"type", e.type == 1   ? "colormap"
                                : e.type == 2 ? "truecolor"
                                              : "gray"},
                       {"bpp", std::to_string(e.bpp) + (e.type == 1 ? "/" + std::to_string(map_bits) : "")},
                       {"rle", e.rle ? "yes" : "no"},
                       {"origin", std::string(e.top ? "top" : "bottom") + (e.right ? "-right" : "-left")}};

        // Keys.
        if (!d->id.empty())
            d->keys.push_back({"/tga/id", hash128(d->id), bytes_text(d->id)});
        if (!zero(d->origin))
            d->keys.push_back({"/tga/origin", hash128(d->origin),
                               std::to_string(r.le16(8)) + "," + std::to_string(r.le16(10))});
        std::uint64_t stamp_at = 0, cc_at = 0;
        if (!d->ext.empty()) {
            for (auto& fd : kExt) {
                auto v = std::span<const std::byte>(d->ext).subspan(fd.at, fd.len);
                if (!zero(v))
                    d->keys.push_back({std::string("/tga/ext/") + fd.name, hash128(v), ext_text(fd, v),
                                       fd.color ? "color" : "metadata", fd.time});
            }
            cc_at = er.le32(482), stamp_at = er.le32(486);
            if (cc_at) {
                auto cc = r.sub(cc_at, 2048);
                d->correction.assign(cc.begin(), cc.end());
                d->keys.push_back({"/tga/ext/colorcorrection", hash128(cc), bytes_text(cc)});
            }
            if (stamp_at) {
                const auto sw = r.u8(stamp_at), sh = r.u8(stamp_at + 1);
                auto s = r.sub(stamp_at, 2 + std::uint64_t{sw} * sh * bpx);
                d->stamp.assign(s.begin(), s.end());
                d->keys.push_back({"/tga/ext/stamp", hash128(s),
                                   std::to_string(sw) + "x" + std::to_string(sh) + " " + bytes_text(s),
                                   "derived"});
            }
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> dev_ranges;
        if (dev_at) {
            const auto cnt = r.le16(dev_at);
            for (std::uint32_t k = 0; k < cnt; ++k) {
                const std::uint64_t ent = dev_at + 2 + 10ull * k;
                const auto tag = r.le16(ent);
                auto v = r.sub(r.le32(ent + 2), r.le32(ent + 6));
                d->dev[tag].assign(v.begin(), v.end());
                d->keys.push_back({"/tga/dev/" + std::to_string(tag), hash128(v), bytes_text(v)});
                dev_ranges.emplace_back(r.le32(ent + 2), v.size());
            }
        }

        // Envelope map (raster-misc.md §7.1).
        auto& m = d->map.ranges;
        auto add = [&](std::uint64_t a, std::uint64_t len, ByteClass c, std::string unit,
                       std::string layer = {}) {
            if (len)
                m.push_back({a, len, c, std::move(unit), std::move(layer)});
        };
        add(0, 1, ByteClass::framing, "/tga/id");
        add(1, 2, ByteClass::encoding, "/encoding");
        add(3, 5, ByteClass::framing, "/pixels", "palette");
        add(8, 4, ByteClass::content, "/tga/origin");
        add(12, 4, ByteClass::content, "/shape");
        add(16, 2, ByteClass::encoding, "/encoding");
        add(18, idlen, ByteClass::content, "/tga/id");
        add(map_at, pix_at - map_at, ByteClass::encoding, "/pixels", "palette");
        add(pix_at, pix_end - pix_at, e.rle ? ByteClass::encoding : ByteClass::content, "/pixels",
            e.rle ? "rle" : "");
        if (ext_at) {
            add(ext_at, 2, ByteClass::framing, "/tga/ext");
            for (auto& fd : kExt)
                if (fd.at != 494)
                    add(ext_at + fd.at, fd.len, fd.time ? ByteClass::volatile_ : ByteClass::content,
                        std::string("/tga/ext/") + fd.name);
            add(ext_at + 482, 12, ByteClass::framing, "/tga/ext");
            add(ext_at + 494, 1, ByteClass::content, "/tga/ext/attribute");
        }
        if (cc_at)
            add(cc_at, 2048, ByteClass::content, "/tga/ext/colorcorrection");
        if (stamp_at)
            add(stamp_at, d->stamp.size(), ByteClass::derived, "/tga/ext/stamp");
        if (dev_at)
            add(dev_at, 2 + 10ull * dev_ranges.size(), ByteClass::framing, "/tga/dev");
        for (auto [a, len] : dev_ranges)
            add(a, len, ByteClass::content, "/tga/dev");
        if (footer)
            add(size - 26, 26, ByteClass::framing, "/tga/footer");
        std::sort(m.begin(), m.end(),
                  [](const EnvRange& a, const EnvRange& b) { return a.offset < b.offset; });
        for (std::size_t k = 1; k < m.size();) // overlapping areas in odd files: the map stays advisory
            if (m[k].offset < m[k - 1].offset + m[k - 1].length)
                m.erase(m.begin() + static_cast<std::ptrdiff_t>(k));
            else
                ++k;
        d->map.validate(size);
        return d;
    }

    std::vector<std::byte> write(Plan& p, std::vector<Warning>& warnings) const override {
        const TgaDoc& src = tga(p.doc(p.enc));
        const OutFrame& f = p.frames.at(0);
        Extras x;
        x.footer = src.enc.footer;
        bool any_ext = false;
        std::vector<std::byte> ext(kExtSize);
        for (auto& [k, s] : p.keys) {
            const TgaDoc& d = tga(p.doc(s));
            if (k == "/tga/id") {
                x.id = d.id;
            } else if (k == "/tga/origin") {
                x.origin = d.origin;
            } else if (k == "/tga/ext/colorcorrection") {
                x.correction = d.correction, any_ext = true;
            } else if (k == "/tga/ext/stamp") {
                x.stamp = &d, any_ext = true;
                if (!d.frames.empty() && d.frames[0].pixels.data != f.pixels.data)
                    warnings.push_back(
                        {"derived-stale", k, "the postage stamp shows the pixels before the merge"});
            } else if (k.starts_with("/tga/dev/")) {
                x.dev[static_cast<std::uint16_t>(std::stoul(k.substr(9)))] =
                    d.dev.at(static_cast<std::uint16_t>(std::stoul(k.substr(9))));
            } else if (k.starts_with("/tga/ext/")) {
                for (auto& fd : kExt)
                    if (k.substr(9) == fd.name)
                        std::memcpy(ext.data() + fd.at, d.ext.data() + fd.at, fd.len);
                any_ext = true;
            }
        }
        if (any_ext)
            x.ext = std::move(ext);
        bool rewritten = false;
        auto out = encode(f.pixels, merged_indices(p, f), &src, src.enc, src.map_bits, std::move(x), warnings,
                          rewritten);
        if (rewritten)
            p.dropped.push_back("/tga/ext/attribute");
        return out;
    }
};

} // namespace

std::vector<std::byte> write(const raster::Raster& r, Encoding enc) {
    std::vector<Warning> ignore;
    bool rewritten = false;
    Extras x;
    x.footer = enc.footer;
    return encode(r, std::nullopt, nullptr, enc, 0, std::move(x), ignore, rewritten);
}

std::unique_ptr<Format> make_format() {
    return rastermisc::make_format(std::make_unique<TgaCodec>());
}

} // namespace semsplinter::tga
