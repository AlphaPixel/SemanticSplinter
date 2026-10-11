#include "common.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/formats/ifd.hpp>
#include <semsplinter/formats/tiff.hpp>

#include <tiffio.h>
#include <zlib.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace semsplinter::tiff {

namespace {

using namespace rastermisc;
constexpr std::string_view kName = "TIFF";

// Tags that are encoding or shape, regenerated on write and never merged as keys (raster-misc.md §3.1).
const std::set<std::uint16_t> kStructural{256, 257, 258, 259, 262, 266, 273, 277, 278, 279, 284,
                                          292, 293, 317, 320, 322, 323, 324, 325, 338, 339, 347};
constexpr std::uint16_t kExif = 34665, kGps = 34853, kSubIfds = 330, kGeoDir = 34735, kGeoDouble = 34736,
                        kGeoAscii = 34737, kLayers = 37724, kMakerNote = 0x927C;

std::string tag_key(std::uint16_t tag) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "/tags/0x%04x", tag);
    return buf;
}

std::string compression_name(unsigned c) {
    switch (c) {
    case 1:
        return "none";
    case 5:
        return "lzw";
    case 7:
        return "jpeg";
    case 8:
    case 32946:
        return "deflate";
    case 32773:
        return "packbits";
    case 50000:
        return "zstd";
    case 34925:
        return "lzma";
    default:
        return std::to_string(c);
    }
}

// A value's bytes in little-endian element order, so byte order does not show as a change.
std::vector<std::byte> le_value(const ifd::Tiff& t, const ifd::Entry& e) {
    std::vector<std::byte> v(e.value.begin(), e.value.end());
    unsigned sz = ifd::type_size(e.type);
    if (e.type == ifd::RATIONAL || e.type == ifd::SRATIONAL)
        sz = 4;
    if (t.big_endian && sz > 1)
        for (std::size_t k = 0; k + sz <= v.size(); k += sz)
            std::reverse(v.begin() + static_cast<std::ptrdiff_t>(k),
                         v.begin() + static_cast<std::ptrdiff_t>(k + sz));
    return v;
}

std::vector<std::byte> to_order(std::vector<std::byte> v, std::uint16_t type, bool big) {
    unsigned sz = ifd::type_size(type);
    if (type == ifd::RATIONAL || type == ifd::SRATIONAL)
        sz = 4;
    if (big && sz > 1)
        for (std::size_t k = 0; k + sz <= v.size(); k += sz)
            std::reverse(v.begin() + static_cast<std::ptrdiff_t>(k),
                         v.begin() + static_cast<std::ptrdiff_t>(k + sz));
    return v;
}

void hash_ifd(const ifd::Tiff& t, const ifd::Ifd& d, Hasher& h) {
    for (auto& e : d.entries) {
        h.add(e.tag).add(e.type).add(e.count);
        if (ifd::is_pointer_tag(e.tag))
            for (auto& s : e.sub)
                hash_ifd(t, s, h);
        else
            h.raw(le_value(t, e));
    }
}

std::string render(const ifd::Tiff& t, const ifd::Entry& e) {
    if (e.type == ifd::ASCII) {
        std::string s = ifd::text(e), out = "\"";
        for (char c : s.substr(0, 200))
            out += c == '"' ? std::string("\\\"") : c == '\n' ? std::string("\\n") : std::string(1, c);
        return out + (s.size() > 200 ? "...\"" : "\"");
    }
    const unsigned sz = ifd::type_size(e.type);
    if (sz && e.type != ifd::FLOAT && e.type != ifd::DOUBLE && e.type != ifd::UNDEFINED && e.count <= 8) {
        std::string s;
        const bool rational = e.type == ifd::RATIONAL || e.type == ifd::SRATIONAL;
        for (std::size_t i = 0; i < e.count * (rational ? 2 : 1); ++i)
            s += (i ? (rational && i % 2 ? "/" : ",") : "") + std::to_string(t.number(e, i));
        return s;
    }
    return bytes_text(e.value);
}

struct Geo {
    std::uint16_t loc = 0, count = 0;
    std::vector<std::byte> data; // shorts (LE), doubles (LE), or ASCII as stored
};

struct Page {
    std::size_t index = 0; // in the IFD chain
    std::uint32_t w = 0, h = 0, rps = 0, tw = 0, th = 0;
    std::uint16_t spp = 1, bits = 1, photometric = 1, compression = 1, predictor = 1, planar = 1, format = 1,
                  fill = 1;
    std::vector<std::uint16_t> extra;
    bool tiled = false, alpha = false;
    std::vector<std::uint64_t> offsets, counts;
    std::vector<raster::Cell> colormap;
    std::map<std::uint16_t, Geo> geo;
    bool decodable() const {
        const unsigned base = photometric == 2 ? 3u : 1u;
        return format == 1 && fill == 1 && (bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16) &&
               photometric <= 3 && (photometric != 3 || bits <= 8) && spp >= base && spp - base <= 1 &&
               (spp == base || alpha) && TIFFIsCODECConfigured(compression);
    }
};

struct TiffDoc final : Doc {
    ifd::Tiff t;
    std::vector<Page> pages;
};

const TiffDoc& tdoc(const Doc& d) {
    return static_cast<const TiffDoc&>(d);
}

// ---- libtiff over memory ----

struct Mem {
    std::span<const std::byte> d;
    std::uint64_t pos = 0;
    std::string error;
};

tmsize_t mem_read(thandle_t h, void* buf, tmsize_t n) {
    auto* m = static_cast<Mem*>(h);
    const auto k = std::min<std::uint64_t>(static_cast<std::uint64_t>(n),
                                           m->pos < m->d.size() ? m->d.size() - m->pos : 0);
    std::memcpy(buf, m->d.data() + m->pos, k);
    m->pos += k;
    return static_cast<tmsize_t>(k);
}
tmsize_t mem_write(thandle_t, void*, tmsize_t) {
    return 0;
}
toff_t mem_seek(thandle_t h, toff_t off, int whence) {
    auto* m = static_cast<Mem*>(h);
    m->pos = whence == SEEK_SET ? off : whence == SEEK_CUR ? m->pos + off : m->d.size() + off;
    return m->pos;
}
int mem_close(thandle_t) {
    return 0;
}
toff_t mem_size(thandle_t h) {
    return static_cast<Mem*>(h)->d.size();
}
int mem_map(thandle_t, void**, toff_t*) {
    return 0;
}
void mem_unmap(thandle_t, void*, toff_t) {}
int on_error(TIFF*, void* user, const char* module, const char* fmt, va_list ap) {
    char buf[512];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    auto* s = static_cast<std::string*>(user);
    if (s->empty())
        *s = std::string(module ? module : "") + ": " + buf;
    return 1;
}
int on_warning(TIFF*, void*, const char*, const char*, va_list) {
    return 1;
}

std::uint8_t widen8(unsigned v, unsigned bits) {
    if (bits >= 8)
        return static_cast<std::uint8_t>(v);
    unsigned r = 0;
    for (int s = 8 - static_cast<int>(bits); s > -static_cast<int>(bits); s -= static_cast<int>(bits))
        r |= s >= 0 ? v << s : v >> -s;
    return static_cast<std::uint8_t>(r);
}

// Decodes the page's samples through libtiff into the canonical grid.
void decode(TIFF* tif, const Page& pg, Frame& f, Limits& budget, const std::string& err) {
    const std::uint32_t w = pg.w, h = pg.h;
    const unsigned spp = pg.spp, bits = pg.bits;
    const bool sep = pg.planar == 2;
    std::vector<std::uint16_t> s(std::uint64_t{w} * h * spp);
    budget.charge(s.size() * 2);
    auto sample = [&](const std::uint8_t* row, std::uint64_t i) -> std::uint16_t {
        if (bits == 8)
            return row[i];
        if (bits == 16) {
            std::uint16_t v;
            std::memcpy(&v, row + 2 * i, 2);
            return v;
        }
        const std::uint64_t bit = i * bits;
        return static_cast<std::uint16_t>((row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1));
    };
    const unsigned planes = sep ? spp : 1, per = sep ? 1 : spp;
    std::vector<std::uint8_t> buf;
    auto fail = [&](const char* what) { bad(kName, std::string(what) + (err.empty() ? "" : ": " + err)); };
    if (!pg.tiled) {
        const std::uint32_t rps = std::min(pg.rps ? pg.rps : h, h);
        const std::uint32_t strips = (h + rps - 1) / rps;
        buf.resize(static_cast<std::size_t>(TIFFStripSize(tif)));
        const std::uint64_t rowbytes = (std::uint64_t{w} * per * bits + 7) / 8;
        for (unsigned p = 0; p < planes; ++p)
            for (std::uint32_t k = 0; k < strips; ++k) {
                const std::uint32_t rows = std::min(rps, h - k * rps);
                const auto got =
                    TIFFReadEncodedStrip(tif, p * strips + k, buf.data(), static_cast<tmsize_t>(buf.size()));
                if (got < 0 || static_cast<std::uint64_t>(got) < rowbytes * rows)
                    fail(("cannot decode strip " + std::to_string(p * strips + k) + " got " +
                          std::to_string(got) + " want " + std::to_string(rowbytes * rows))
                             .c_str());
                for (std::uint32_t r = 0; r < rows; ++r) {
                    const std::uint8_t* row = buf.data() + r * rowbytes;
                    const std::uint64_t y = std::uint64_t{k} * rps + r;
                    for (std::uint32_t x = 0; x < w; ++x)
                        for (unsigned c = 0; c < per; ++c)
                            s[(y * w + x) * spp + p + c] = sample(row, std::uint64_t{x} * per + c);
                }
            }
    } else {
        buf.resize(static_cast<std::size_t>(TIFFTileSize(tif)));
        const std::uint64_t rowbytes = (std::uint64_t{pg.tw} * per * bits + 7) / 8;
        for (unsigned p = 0; p < planes; ++p)
            for (std::uint32_t ty = 0; ty < h; ty += pg.th)
                for (std::uint32_t tx = 0; tx < w; tx += pg.tw) {
                    const auto tile = TIFFComputeTile(tif, tx, ty, 0, static_cast<std::uint16_t>(p));
                    if (TIFFReadEncodedTile(tif, tile, buf.data(), static_cast<tmsize_t>(buf.size())) < 0)
                        fail("cannot decode a tile");
                    for (std::uint32_t r = 0; r < pg.th && ty + r < h; ++r) {
                        const std::uint8_t* row = buf.data() + r * rowbytes;
                        for (std::uint32_t x = 0; x < pg.tw && tx + x < w; ++x)
                            for (unsigned c = 0; c < per; ++c)
                                s[((std::uint64_t{ty} + r) * w + tx + x) * spp + p + c] =
                                    sample(row, std::uint64_t{x} * per + c);
                    }
                }
    }
    const bool wide =
        bits == 16 || std::any_of(pg.colormap.begin(), pg.colormap.end(), [](const raster::Cell& c) {
            return c[0] % 257 || c[1] % 257 || c[2] % 257;
        });
    f.pixels = new_raster(w, h, wide, budget);
    auto v16 = [&](std::uint16_t v) {
        return bits == 16 ? v : static_cast<std::uint16_t>(widen8(v, bits) * 257);
    };
    if (pg.photometric == 3 && !wide)
        f.indices.resize(f.pixels.cells());
    for (std::uint64_t i = 0; i < f.pixels.cells(); ++i) {
        const std::uint16_t* px = s.data() + i * spp;
        raster::Cell c;
        if (pg.photometric == 3) {
            if (px[0] >= pg.colormap.size())
                bad(kName, "palette index outside the color map");
            c = pg.colormap[px[0]];
            if (!f.indices.empty())
                f.indices[i] = static_cast<std::uint8_t>(px[0]);
        } else if (pg.photometric == 2) {
            c = {v16(px[0]), v16(px[1]), v16(px[2]), pg.alpha ? v16(px[3]) : std::uint16_t{0xFFFF}};
        } else {
            c = {v16(px[0]), v16(px[0]), v16(px[0]), pg.alpha ? v16(px[1]) : std::uint16_t{0xFFFF}};
        }
        f.pixels.set(i, c);
    }
    if (!f.indices.empty())
        for (auto& c : pg.colormap)
            f.palette.push_back(rgba8(c));
    budget.release(s.size() * 2);
}

// ---- Encoding (raster-misc.md §7: own encoders, validated by libtiff on reload) ----

std::vector<std::byte> packbits(std::span<const std::uint8_t> in) {
    std::vector<std::byte> o;
    for (std::size_t i = 0; i < in.size();) {
        std::size_t run = 1;
        while (i + run < in.size() && run < 128 && in[i + run] == in[i])
            ++run;
        if (run >= 2) {
            o.push_back(std::byte(static_cast<std::uint8_t>(1 - static_cast<int>(run))));
            o.push_back(std::byte(in[i]));
            i += run;
            continue;
        }
        std::size_t lit = 1;
        while (i + lit < in.size() && lit < 128 &&
               !(i + lit + 1 < in.size() && in[i + lit] == in[i + lit + 1]))
            ++lit;
        o.push_back(std::byte(lit - 1));
        for (std::size_t k = 0; k < lit; ++k)
            o.push_back(std::byte(in[i + k]));
        i += lit;
    }
    return o;
}

// TIFF LZW (MSB-first, early change) with libtiff's code-width and table-full policy, without its
// ratio-based early clears.
std::vector<std::byte> tiff_lzw(std::span<const std::uint8_t> in) {
    std::vector<std::byte> o;
    std::uint32_t acc = 0;
    unsigned nacc = 0, width = 9;
    auto put = [&](unsigned code) {
        acc = acc << width | code;
        nacc += width;
        while (nacc >= 8)
            o.push_back(std::byte((acc >> (nacc - 8)) & 0xFF)), nacc -= 8;
    };
    std::map<std::uint32_t, std::uint16_t> dict;
    unsigned next = 258, maxcode = 511;
    put(256);
    int cur = -1;
    for (auto b : in) {
        if (cur < 0) {
            cur = b;
            continue;
        }
        const std::uint32_t key = static_cast<std::uint32_t>(cur) << 8 | b;
        if (auto f = dict.find(key); f != dict.end()) {
            cur = f->second;
            continue;
        }
        put(static_cast<unsigned>(cur));
        cur = b;
        dict[key] = static_cast<std::uint16_t>(next++);
        if (next == 4094) {
            put(256);
            dict.clear();
            next = 258, width = 9, maxcode = 511;
        } else if (next > maxcode) {
            ++width;
            maxcode = (1u << width) - 1;
        }
    }
    if (cur >= 0) {
        put(static_cast<unsigned>(cur));
        ++next;
        if (next == 4094) {
            put(256);
            width = 9;
        } else if (next > maxcode) {
            ++width;
        }
    }
    put(257);
    if (nacc)
        o.push_back(std::byte((acc << (8 - nacc)) & 0xFF));
    return o;
}

std::vector<std::byte> deflate_strip(std::span<const std::uint8_t> in, int level) {
    uLongf n = compressBound(static_cast<uLong>(in.size()));
    std::vector<std::byte> o(n);
    if (compress2(reinterpret_cast<Bytef*>(o.data()), &n, in.data(), static_cast<uLong>(in.size()), level) !=
        Z_OK)
        throw Error(ErrorCategory::internal, "TIFF: deflate failed");
    o.resize(n);
    return o;
}

struct Params {
    std::uint16_t compression = 1, predictor = 1, bits = 8, photometric = 2;
    std::uint32_t rps = 0;
    bool alpha = false;
    std::uint16_t extra = 2; // ExtraSamples value for alpha: 1 associated, 2 unassociated
    int level = 6;           // zlib level for Deflate
};

struct Encoded {
    Params p;
    std::uint16_t spp = 3;
    std::vector<std::vector<std::byte>> strips;
    std::vector<raster::Cell> colormap; // photometric 3
};

// Encodes `r` with `want`, promoting minimally when the pixels need it (warning encoding-promoted).
Encoded encode(const Raster& r, Params want,
               std::optional<std::pair<std::vector<std::uint8_t>, std::vector<Rgba8>>> exact,
               const std::vector<Rgba8>& base_palette, bool big, std::vector<Warning>& warnings) {
    Encoded e;
    Params p = want;
    const std::uint64_t n = r.cells();
    bool has_alpha = false, gray = true;
    for (std::uint64_t i = 0; i < n; ++i) {
        auto c = r.get(i);
        has_alpha |= c[3] != 0xFFFF;
        gray &= c[0] == c[1] && c[1] == c[2];
    }
    const bool narrow = narrow_ok(r);
    if (p.compression != 1 && p.compression != 5 && p.compression != 8 && p.compression != 32946 &&
        p.compression != 32773)
        p.compression = 5;
    std::vector<std::uint8_t> index;
    if (p.photometric == 3) {
        std::vector<Rgba8> pal;
        if (!has_alpha && narrow) {
            const std::size_t max = std::size_t{1} << std::min<unsigned>(p.bits, 8);
            if (exact && exact->second.size() <= max)
                index = std::move(exact->first), pal = std::move(exact->second);
            else if (auto q = palettize(r, base_palette, max, index))
                pal = std::move(*q);
            else if (auto q8 = palettize(r, base_palette, 256, index))
                pal = std::move(*q8), p.bits = 8;
        }
        if (pal.empty()) {
            p.photometric = 2, p.bits = narrow ? 8 : 16;
        } else {
            e.colormap.resize(std::size_t{1} << p.bits, raster::Cell{0, 0, 0, 0xFFFF});
            for (std::size_t k = 0; k < pal.size(); ++k)
                e.colormap[k] = cell(pal[k]);
        }
    }
    if (p.photometric <= 1 && !gray)
        p.photometric = 2;
    if (p.photometric != 3) {
        if (p.bits < 8) {
            const unsigned b = p.bits;
            bool ok = true;
            for (std::uint64_t i = 0; ok && i < n; ++i)
                for (auto v : r.get(i))
                    ok &= v % 257 == 0 && widen8((v >> 8) >> (8 - b), b) == (v >> 8);
            if (!ok || p.photometric == 2)
                p.bits = 8;
        }
        if (!narrow)
            p.bits = 16;
        if (has_alpha)
            p.alpha = true;
    }
    if (p.bits < 8 || p.photometric == 3 ||
        (p.compression != 5 && p.compression != 8 && p.compression != 32946))
        p.predictor = 1; // libtiff applies predictors to LZW and Deflate only
    if (p.photometric != want.photometric || p.bits != want.bits || p.compression != want.compression ||
        p.alpha != want.alpha)
        warnings.push_back({"encoding-promoted", "/encoding",
                            "TIFF page written as photometric " + std::to_string(p.photometric) + ", " +
                                std::to_string(p.bits) + "-bit, " + compression_name(p.compression)});
    e.spp =
        static_cast<std::uint16_t>((p.photometric == 2 ? 3 : 1) + (p.alpha && p.photometric != 3 ? 1 : 0));
    const std::uint64_t rowbytes = (std::uint64_t{r.width} * e.spp * p.bits + 7) / 8;
    if (!p.rps || p.rps > r.height)
        p.rps = static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(8192 / std::max<std::uint64_t>(rowbytes, 1), 1, r.height));
    e.p = p;
    std::vector<std::uint16_t> vals(std::uint64_t{r.width} * e.spp);
    for (std::uint32_t y0 = 0; y0 < r.height; y0 += p.rps) {
        std::vector<std::uint8_t> raw;
        for (std::uint32_t y = y0; y < std::min(r.height, y0 + p.rps); ++y) {
            for (std::uint32_t x = 0; x < r.width; ++x) {
                const std::uint64_t i = std::uint64_t{y} * r.width + x;
                auto c = r.get(i);
                std::uint16_t* v = vals.data() + std::uint64_t{x} * e.spp;
                auto q = [&](std::uint16_t s) {
                    return p.bits == 16 ? s : static_cast<std::uint16_t>((s >> 8) >> (8 - p.bits));
                };
                if (p.photometric == 3) {
                    v[0] = index[i];
                } else if (p.photometric == 2) {
                    v[0] = q(c[0]), v[1] = q(c[1]), v[2] = q(c[2]);
                    if (p.alpha)
                        v[3] = q(c[3]);
                } else {
                    v[0] = q(c[0]);
                    if (p.alpha)
                        v[1] = q(c[3]);
                }
            }
            if (p.predictor == 2)
                for (std::size_t k = vals.size(); k-- > e.spp;)
                    vals[k] = static_cast<std::uint16_t>(vals[k] - vals[k - e.spp]);
            const auto start = raw.size();
            raw.resize(start + rowbytes);
            for (std::size_t k = 0; k < vals.size(); ++k) {
                if (p.bits == 16) {
                    raw[start + 2 * k + (big ? 0 : 1)] = static_cast<std::uint8_t>(vals[k] >> 8);
                    raw[start + 2 * k + (big ? 1 : 0)] = static_cast<std::uint8_t>(vals[k] & 0xFF);
                } else if (p.bits == 8) {
                    raw[start + k] = static_cast<std::uint8_t>(vals[k]);
                } else {
                    const std::uint64_t bit = k * p.bits;
                    raw[start + bit / 8] |= static_cast<std::uint8_t>(vals[k] << (8 - p.bits - bit % 8));
                }
            }
        }
        if (p.compression == 5)
            e.strips.push_back(tiff_lzw(raw));
        else if (p.compression == 8 || p.compression == 32946)
            e.strips.push_back(deflate_strip(raw, p.level));
        else if (p.compression == 32773)
            e.strips.push_back(packbits(raw));
        else
            e.strips.push_back({reinterpret_cast<const std::byte*>(raw.data()),
                                reinterpret_cast<const std::byte*>(raw.data()) + raw.size()});
    }
    return e;
}

// Structural entries of an encoded page; strip offsets come from `strip_at` (one field per strip).
std::vector<ifd::OutEntry> structural(const ifd::Writer& w, const Encoded& e, std::uint32_t width,
                                      std::uint32_t height, std::vector<emit::Seg> strip_at,
                                      const std::vector<std::uint64_t>& counts) {
    std::vector<ifd::OutEntry> v;
    const std::uint16_t lt = w.bigtiff ? ifd::LONG8 : ifd::LONG;
    v.push_back(w.numbers(256, ifd::LONG, {width}));
    v.push_back(w.numbers(257, ifd::LONG, {height}));
    v.push_back(w.numbers(258, ifd::SHORT, std::vector<std::uint64_t>(e.spp, e.p.bits)));
    v.push_back(w.numbers(259, ifd::SHORT, {e.p.compression}));
    v.push_back(w.numbers(262, ifd::SHORT, {e.p.photometric}));
    v.push_back({273, lt, strip_at.size(), std::move(strip_at), std::nullopt});
    v.push_back(w.numbers(277, ifd::SHORT, {e.spp}));
    v.push_back(w.numbers(278, ifd::LONG, {e.p.rps}));
    v.push_back(w.numbers(279, lt, counts));
    v.push_back(w.numbers(284, ifd::SHORT, {1}));
    if (e.p.predictor != 1)
        v.push_back(w.numbers(317, ifd::SHORT, {e.p.predictor}));
    if (!e.colormap.empty()) {
        std::vector<std::uint64_t> m;
        for (int c = 0; c < 3; ++c)
            for (auto& x : e.colormap)
                m.push_back(x[static_cast<std::size_t>(c)]);
        v.push_back(w.numbers(320, ifd::SHORT, m));
    }
    if (e.p.alpha && e.p.photometric != 3)
        v.push_back(w.numbers(338, ifd::SHORT, {e.p.extra}));
    return v;
}

// A deep copy of a sub-IFD from another file (Exif, GPS, Interop), values in the output byte order.
emit::Seg copy_ifd(const ifd::Writer& w, const ifd::Tiff& t, const ifd::Ifd& d, emit::GroupId gid,
                   std::vector<emit::Seg>& tail, std::vector<Warning>& warnings, const std::string& at) {
    std::vector<ifd::OutEntry> es;
    for (auto& e : d.entries) {
        if (e.tag == kMakerNote)
            warnings.push_back(
                {"opaque-offsets-moved", at, "the MakerNote moved; offsets inside it may be wrong"});
        if (ifd::is_pointer_tag(e.tag) && e.sub.size() == 1) {
            const auto sub = w.id();
            tail.push_back(copy_ifd(w, t, e.sub[0], sub, tail, warnings, at));
            es.push_back({e.tag, w.bigtiff ? ifd::IFD8 : ifd::LONG, 1, {w.pointer(sub)}, std::nullopt});
        } else if (ifd::type_size(e.type)) {
            es.push_back({e.tag,
                          e.type,
                          e.count,
                          {emit::bytes(to_order(le_value(t, e), e.type, w.big_endian))},
                          std::nullopt});
        }
    }
    return w.write(gid, std::move(es), w.pointer(std::uint64_t{0}));
}

std::vector<std::byte> geo_shorts(std::span<const std::uint64_t> v) {
    std::vector<std::byte> o;
    for (auto x : v)
        put_le(o, x, 2);
    return o;
}

class TiffCodec final : public Codec {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "tiff",
            "Tagged Image File Format (TIFF, BigTIFF)",
            {".tif", ".tiff"},
            {true, true, false, true, true},
            {{"tiff-ignore-tags", OptionSpec::Arg::required, "diff: tag keys to ignore (0x0131,0x0132)"}}};
        return i;
    }
    std::string_view frame_word() const override { return "pages"; }
    std::string_view ignore_option() const override { return "tiff-ignore-tags"; }

    int probe(std::span<const std::byte> head, std::string_view) const override {
        if (head.size() < 8)
            return 0;
        static const std::array<std::array<unsigned char, 4>, 4> magic{
            {{'I', 'I', 42, 0}, {'M', 'M', 0, 42}, {'I', 'I', 43, 0}, {'M', 'M', 0, 43}}};
        for (auto& m : magic)
            if (std::memcmp(head.data(), m.data(), 4) == 0)
                return 100;
        return 0;
    }

    std::unique_ptr<Doc> load(ByteSource src, Limits& budget, const OptionValues&) const override {
        auto d = std::make_unique<TiffDoc>();
        d->src = std::move(src);
        d->sequence = true;
        auto bytes = d->src.bytes();
        d->t = ifd::parse(bytes);
        const auto& t = d->t;
        auto& m = d->map.ranges;
        auto add = [&](std::uint64_t a, std::uint64_t len, ByteClass c, std::string unit,
                       std::string layer = {}) {
            if (len)
                m.push_back({a, len, c, std::move(unit), std::move(layer)});
        };
        add(0, 4, ByteClass::framing, "/signature");
        add(t.bigtiff ? 8 : 4, t.offset_size(), ByteClass::framing, "/pages");
        if (t.bigtiff)
            add(4, 4, ByteClass::framing, "/signature");
        Mem mem{bytes};
        TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
        TIFFOpenOptionsSetErrorHandlerExtR(opts, on_error, &mem.error);
        TIFFOpenOptionsSetWarningHandlerExtR(opts, on_warning, nullptr);
        TIFFOpenOptionsSetMaxSingleMemAlloc(
            opts, static_cast<tmsize_t>(std::min<std::uint64_t>(budget.max_memory, 1ull << 40)));
        TIFF* tif = TIFFClientOpenExt("mem", "rmc", &mem, mem_read, mem_write, mem_seek, mem_close, mem_size,
                                      mem_map, mem_unmap, opts);
        TIFFOpenOptionsFree(opts);
        if (!tif)
            bad(kName, "libtiff cannot open the file: " + mem.error);
        struct Close {
            TIFF* t;
            ~Close() { TIFFClose(t); }
        } close{tif};
        for (std::size_t k = 0; k < t.chain.size(); ++k) {
            const auto& ifd = t.chain[k];
            const std::string unit = "/pages/" + std::to_string(k);
            Page pg;
            pg.index = k;
            auto num = [&](std::uint16_t tag, std::uint64_t dflt) {
                auto* e = ifd.find(tag);
                return e && e->count ? t.number(*e) : dflt;
            };
            pg.w = static_cast<std::uint32_t>(num(256, 0)), pg.h = static_cast<std::uint32_t>(num(257, 0));
            pg.spp = static_cast<std::uint16_t>(num(277, 1)),
            pg.bits = static_cast<std::uint16_t>(num(258, 1));
            pg.photometric = static_cast<std::uint16_t>(num(262, 1)),
            pg.compression = static_cast<std::uint16_t>(num(259, 1));
            pg.predictor = static_cast<std::uint16_t>(num(317, 1)),
            pg.planar = static_cast<std::uint16_t>(num(284, 1));
            pg.format = static_cast<std::uint16_t>(num(339, 1)),
            pg.fill = static_cast<std::uint16_t>(num(266, 1));
            pg.rps = static_cast<std::uint32_t>(std::min<std::uint64_t>(num(278, pg.h), pg.h ? pg.h : 1));
            if (auto* e = ifd.find(258))
                for (auto b : t.numbers(*e))
                    if (b != pg.bits)
                        pg.format = 0; // mixed sample sizes: not canonical
            if (auto* e = ifd.find(338))
                for (auto x : t.numbers(*e))
                    pg.extra.push_back(static_cast<std::uint16_t>(x));
            pg.alpha = pg.extra.size() == 1 && (pg.extra[0] == 1 || pg.extra[0] == 2);
            pg.tiled = ifd.find(322) != nullptr;
            pg.tw = static_cast<std::uint32_t>(num(322, 0)), pg.th = static_cast<std::uint32_t>(num(323, 0));
            if (!pg.w || !pg.h || (pg.tiled && (!pg.tw || !pg.th)))
                bad(kName, "page " + std::to_string(k) + " has no size");
            if (auto* e = ifd.find(pg.tiled ? 324 : 273))
                pg.offsets = t.numbers(*e);
            if (auto* e = ifd.find(pg.tiled ? 325 : 279))
                pg.counts = t.numbers(*e);
            if (pg.offsets.size() != pg.counts.size() || pg.offsets.empty())
                bad(kName, "page " + std::to_string(k) + ": strip offsets and byte counts disagree");
            if (auto* e = ifd.find(320)) {
                auto v = t.numbers(*e);
                const std::size_t n = v.size() / 3;
                for (std::size_t i = 0; i < n; ++i)
                    pg.colormap.push_back({static_cast<std::uint16_t>(v[i]),
                                           static_cast<std::uint16_t>(v[n + i]),
                                           static_cast<std::uint16_t>(v[2 * n + i]), 0xFFFF});
            }
            Frame f;
            // Pixels: canonical when the layout maps onto RGBA, else opaque (compared by coded bytes).
            Hasher coded;
            for (std::size_t s = 0; s < pg.offsets.size(); ++s) {
                if (pg.offsets[s] > bytes.size() || bytes.size() - pg.offsets[s] < pg.counts[s])
                    bad(kName, "strip outside the file");
                coded.raw(bytes.subspan(pg.offsets[s], pg.counts[s]));
                add(pg.offsets[s], pg.counts[s],
                    pg.compression == 1 ? ByteClass::content : ByteClass::encoding, unit + "/pixels",
                    pg.compression == 1 ? "" : compression_name(pg.compression));
            }
            f.coded = coded.digest();
            if (pg.decodable() &&
                (pg.photometric != 3 || pg.colormap.size() >= (std::size_t{1} << pg.bits))) {
                if (!TIFFSetDirectory(tif, static_cast<tdir_t>(k)))
                    bad(kName, "libtiff cannot read page " + std::to_string(k) + ": " + mem.error);
                decode(tif, pg, f, budget, mem.error);
            } else {
                f.opaque = true;
            }
            f.encoding = "compression=" + compression_name(pg.compression) +
                         (pg.predictor != 1 ? " predictor=" + std::to_string(pg.predictor) : "") +
                         (pg.tiled ? " tiles=" + std::to_string(pg.tw) + "x" + std::to_string(pg.th)
                                   : " rows=" + std::to_string(pg.rps)) +
                         (pg.planar == 2 ? " planar" : "") +
                         " photometric=" + std::to_string(pg.photometric) +
                         " bits=" + std::to_string(pg.bits);
            // GeoTIFF keys (raster-misc.md §3.1).
            if (auto* dir = ifd.find(kGeoDir); dir && dir->type == ifd::SHORT && dir->count >= 4) {
                auto v = t.numbers(*dir);
                const auto* dbl = ifd.find(kGeoDouble);
                const auto* asc = ifd.find(kGeoAscii);
                const std::size_t n = std::min<std::size_t>(v[3], (v.size() - 4) / 4);
                for (std::size_t i = 0; i < n; ++i) {
                    const auto id = static_cast<std::uint16_t>(v[4 + 4 * i]),
                               loc = static_cast<std::uint16_t>(v[5 + 4 * i]);
                    const auto cnt = static_cast<std::uint16_t>(v[6 + 4 * i]);
                    const std::uint64_t off = v[7 + 4 * i];
                    Geo g{loc, cnt, {}};
                    std::string text;
                    if (loc == 0) {
                        g.data = geo_shorts(std::vector<std::uint64_t>{off});
                        text = std::to_string(off);
                    } else if (loc == kGeoDir && off + cnt <= v.size()) {
                        g.data = geo_shorts(std::span(v).subspan(off, cnt));
                        text = "shorts=" + std::to_string(cnt);
                    } else if (loc == kGeoDouble && dbl && (off + cnt) * 8 <= dbl->value.size()) {
                        auto all = le_value(t, *dbl);
                        g.data.assign(all.begin() + static_cast<std::ptrdiff_t>(off * 8),
                                      all.begin() + static_cast<std::ptrdiff_t>((off + cnt) * 8));
                        for (std::size_t j = 0; j < cnt && j < 4; ++j) {
                            double x;
                            std::memcpy(&x, g.data.data() + 8 * j, 8);
                            char buf[32];
                            std::snprintf(buf, sizeof buf, "%.17g", x);
                            text += (j ? "," : "") + std::string(buf);
                        }
                    } else if (loc == kGeoAscii && asc && off + cnt <= asc->value.size()) {
                        g.data.assign(asc->value.begin() + static_cast<std::ptrdiff_t>(off),
                                      asc->value.begin() + static_cast<std::ptrdiff_t>(off + cnt));
                        text = "\"" +
                               std::string(reinterpret_cast<const char*>(g.data.data()), g.data.size()) +
                               "\"";
                    } else {
                        bad(kName, "GeoKey " + std::to_string(id) + " points outside its tag");
                    }
                    Hasher h;
                    h.add(loc).add(cnt).raw(g.data);
                    f.keys.push_back({"/geo/" + std::to_string(id), h.digest(), text});
                    pg.geo[id] = std::move(g);
                }
            }
            // Tags (KeyedMerge by numeric ID), private IFDs as opaque units (Exif keyed merge is v2).
            for (auto& e : ifd.entries) {
                if (kStructural.count(e.tag) ||
                    (pg.geo.size() && (e.tag == kGeoDir || e.tag == kGeoDouble || e.tag == kGeoAscii)))
                    continue;
                if (ifd::is_pointer_tag(e.tag)) {
                    Hasher h;
                    for (auto& s : e.sub)
                        hash_ifd(t, s, h);
                    const std::string key = e.tag == kExif      ? "/exif"
                                            : e.tag == kGps     ? "/gps"
                                            : e.tag == kSubIfds ? "/subifds"
                                                                : tag_key(e.tag);
                    f.keys.push_back({key, h.digest(), std::to_string(e.sub.size()) + " IFD(s)",
                                      e.tag == kSubIfds ? "derived" : "metadata"});
                    continue;
                }
                Hasher h;
                h.add(e.type).add(e.count).raw(le_value(t, e));
                f.keys.push_back({tag_key(e.tag), h.digest(), render(t, e), "metadata", e.tag == 306});
            }
            // Map: the IFD and its out-of-line values.
            add(ifd.offset, ifd.size, ByteClass::framing, unit);
            for (auto& e : ifd.entries)
                if (!e.in_line && e.value.size()) {
                    const bool strip_arrays = e.tag == 273 || e.tag == 279 || e.tag == 324 || e.tag == 325;
                    add(e.at, e.value.size(),
                        strip_arrays                   ? ByteClass::framing
                        : e.tag == 305 || e.tag == 306 ? ByteClass::volatile_
                        : kStructural.count(e.tag)     ? ByteClass::encoding
                                                       : ByteClass::content,
                        strip_arrays || kStructural.count(e.tag) ? unit + "/pixels" : unit + tag_key(e.tag));
                }
            d->frames.push_back(std::move(f));
            d->pages.push_back(std::move(pg));
        }
        d->encoding = {{"byteorder", t.big_endian ? "MM" : "II"}, {"bigtiff", t.bigtiff ? "yes" : "no"}};
        std::sort(m.begin(), m.end(),
                  [](const EnvRange& a, const EnvRange& b) { return a.offset < b.offset; });
        for (std::size_t k = 1; k < m.size();) // shared or overlapping values: the map stays advisory
            if (m[k].offset < m[k - 1].offset + m[k - 1].length)
                m.erase(m.begin() + static_cast<std::ptrdiff_t>(k));
            else
                ++k;
        d->map.validate(bytes.size());
        return d;
    }

    // Stable layout (Q-RAST-7): ours' bytes stay where they are; rewritten IFDs and new strips are appended
    // and the one pointer to each rewritten IFD is patched.
    std::vector<std::byte> write(Plan& p, std::vector<Warning>& warnings) const override {
        const TiffDoc& lay = tdoc(p.doc(Side::ours));
        const auto& lt = lay.t;
        emit::GroupId ids = 1000;
        ifd::Writer w{lt.big_endian, lt.bigtiff, std::nullopt, &ids};
        const std::size_t n = p.frames.size();
        auto key_set = [](const Frame& f) {
            std::vector<std::pair<std::string, Hash128>> v;
            for (auto& k : f.keys)
                v.emplace_back(k.key, k.value);
            std::sort(v.begin(), v.end());
            return v;
        };
        // Which pages stay in place: unchanged against an ours page whose NextIFD already points right.
        std::vector<bool> kept(n);
        std::vector<std::uint64_t> orig(n);
        for (std::size_t i = n; i-- > 0;) {
            const OutFrame& of = p.frames[i];
            const auto j = of.src[static_cast<std::size_t>(Side::ours)];
            if (!j || of.same != Side::ours)
                continue;
            const auto& ifd = lt.chain[lay.pages[*j].index];
            orig[i] = ifd.offset;
            std::vector<std::pair<std::string, Hash128>> want;
            for (auto& [k, s] : of.keys)
                want.emplace_back(k, find_key(p.frame(of, s).keys, k)->value);
            std::sort(want.begin(), want.end());
            const bool next_ok = i + 1 == n ? ifd.next == 0 : kept[i + 1] && ifd.next == orig[i + 1];
            kept[i] = next_ok && want == key_set(lay.frames[*j]);
        }
        std::vector<emit::GroupId> gid(n);
        for (std::size_t i = 0; i < n; ++i)
            if (!kept[i])
                gid[i] = w.id();
        auto pointer_to = [&](std::size_t i) { return kept[i] ? w.pointer(orig[i]) : w.pointer(gid[i]); };

        std::vector<emit::Seg> appended;
        for (std::size_t i = 0; i < n; ++i) {
            if (kept[i])
                continue;
            OutFrame& of = p.frames[i];
            const std::string unit = "/pages/" + std::to_string(i);
            std::vector<ifd::OutEntry> es;
            // Pixels: an input page's strips as they are, or re-encoded.
            const Side ps = of.same.value_or(p.enc);
            const auto pj = of.src[static_cast<std::size_t>(ps)];
            const TiffDoc* pd = pj ? &tdoc(p.doc(ps)) : nullptr;
            bool copied = false;
            if (of.same && pd) {
                const Page& pg = pd->pages[*pj];
                const auto& sifd = pd->t.chain[pg.index];
                const bool same_file = ps == Side::ours;
                if (same_file || pd->t.big_endian == lt.big_endian ||
                    (pg.bits <= 8 && !p.frame(of, ps).opaque)) {
                    for (auto& e : sifd.entries)
                        if (kStructural.count(e.tag) && e.tag != 273 && e.tag != 279 && e.tag != 324 &&
                            e.tag != 325 && ifd::type_size(e.type)) {
                            if (same_file)
                                es.push_back(w.keep(e));
                            else
                                es.push_back(
                                    {e.tag,
                                     e.type,
                                     e.count,
                                     {emit::bytes(to_order(le_value(pd->t, e), e.type, lt.big_endian))},
                                     std::nullopt});
                        }
                    const std::uint16_t otag = pg.tiled ? 324 : 273, ctag = pg.tiled ? 325 : 279;
                    const std::uint16_t ltype = lt.bigtiff ? ifd::LONG8 : ifd::LONG;
                    if (same_file && !lt.bigtiff == !pd->t.bigtiff) {
                        es.push_back(w.keep(*sifd.find(otag)));
                        es.push_back(w.keep(*sifd.find(ctag)));
                    } else {
                        std::vector<emit::Seg> at;
                        for (std::size_t s = 0; s < pg.offsets.size(); ++s) {
                            if (same_file) {
                                at.push_back(w.number(pg.offsets[s], ifd::type_size(ltype)));
                            } else {
                                const auto g = w.id();
                                appended.push_back(
                                    emit::group(g, {emit::raw(pd->src, pg.offsets[s], pg.counts[s])}));
                                at.push_back(emit::field(ifd::type_size(ltype), w.endian(), w.offset(g)));
                            }
                        }
                        es.push_back({otag, ltype, at.size(), std::move(at), std::nullopt});
                        es.push_back(w.numbers(ctag, ltype, pg.counts));
                    }
                    copied = true;
                }
            }
            if (!copied) {
                if (of.pixels.width == 0)
                    throw Error(ErrorCategory::unsupported,
                                "TIFF: page " + std::to_string(i) + " cannot be re-encoded");
                // Encoding parameters from the source page (ours' when it has one).
                const Side bs = of.src[static_cast<std::size_t>(Side::ours)] ? Side::ours : ps;
                const auto bj = of.src[static_cast<std::size_t>(bs)];
                const TiffDoc& bd = tdoc(p.doc(bs));
                Params want;
                std::vector<Rgba8> base_pal;
                const Page* bp = bj ? &bd.pages[*bj] : nullptr;
                if (bp) {
                    want.compression = bp->compression, want.predictor = bp->predictor, want.bits = bp->bits;
                    want.photometric = bp->photometric, want.rps = bp->tiled ? 0 : bp->rps,
                    want.alpha = bp->alpha;
                    if (bp->alpha)
                        want.extra = bp->extra[0];
                    if (bp->predictor == 3)
                        want.predictor = 1;
                    if (bp->compression == 8 || bp->compression == 32946)
                        if (bp->counts[0] >= 2) { // FLEVEL of the first strip's zlib header picks the level
                            const auto flg =
                                std::to_integer<unsigned>(bd.src.bytes()[bp->offsets[0] + 1]) >> 6;
                            want.level = flg == 0 ? 1 : flg == 1 ? 5 : flg == 2 ? 6 : 9;
                        }
                    base_pal = p.frame(of, bs).palette;
                }
                auto enc = encode(of.pixels, want, merged_indices(p, of), base_pal, lt.big_endian, warnings);
                // Unchanged strips of ours' page keep their bytes and offsets (raster-misc.md §6).
                const bool reuse = bp && bs == Side::ours && !bp->tiled && bp->planar == 1 &&
                                   enc.p.compression == bp->compression && enc.p.predictor == bp->predictor &&
                                   enc.p.bits == bp->bits && enc.p.photometric == bp->photometric &&
                                   enc.p.rps == bp->rps && enc.p.alpha == bp->alpha &&
                                   !p.frame(of, Side::ours).opaque &&
                                   p.frame(of, Side::ours).pixels.width == of.pixels.width &&
                                   p.frame(of, Side::ours).pixels.height == of.pixels.height &&
                                   bp->offsets.size() == enc.strips.size();
                std::vector<emit::Seg> at;
                std::vector<std::uint64_t> counts;
                const std::uint16_t ltype = lt.bigtiff ? ifd::LONG8 : ifd::LONG;
                for (std::size_t s = 0; s < enc.strips.size(); ++s) {
                    bool same_rows = reuse;
                    if (reuse) {
                        const Raster& src = p.frame(of, Side::ours).pixels;
                        const std::uint64_t c0 = std::uint64_t{enc.p.rps} * s * of.pixels.width,
                                            c1 = std::min<std::uint64_t>(of.pixels.cells(),
                                                                         c0 + std::uint64_t{enc.p.rps} *
                                                                                  of.pixels.width);
                        for (auto c = c0; same_rows && c < c1; ++c)
                            same_rows = src.get(c) == of.pixels.get(c);
                    }
                    if (same_rows) {
                        at.push_back(w.number(bp->offsets[s], ifd::type_size(ltype)));
                        counts.push_back(bp->counts[s]);
                    } else {
                        const auto g = w.id();
                        counts.push_back(enc.strips[s].size());
                        appended.push_back(emit::group(g, {emit::bytes(enc.strips[s])}));
                        at.push_back(emit::field(ifd::type_size(ltype), w.endian(), w.offset(g)));
                    }
                }
                es = structural(w, enc, of.pixels.width, of.pixels.height, std::move(at), counts);
            }
            // Keys: ours' values stay where they are; others are copied (pointer tags deep-copied).
            std::map<std::uint16_t, std::pair<const Geo*, Side>> geo;
            bool geo_changed = false;
            for (auto& [k, s] : of.keys) {
                const auto sj = *of.src[static_cast<std::size_t>(s)];
                const TiffDoc& sd = tdoc(p.doc(s));
                const Page& spg = sd.pages[sj];
                const auto& sifd = sd.t.chain[spg.index];
                if (k.starts_with("/geo/")) {
                    const auto id = static_cast<std::uint16_t>(std::stoul(k.substr(5)));
                    geo[id] = {&spg.geo.at(id), s};
                    geo_changed |= s != Side::ours;
                    continue;
                }
                const std::uint16_t tag =
                    k == "/exif"      ? kExif
                    : k == "/gps"     ? kGps
                    : k == "/subifds" ? kSubIfds
                                      : static_cast<std::uint16_t>(std::stoul(k.substr(6), nullptr, 16));
                const auto* e = sifd.find(tag);
                if (tag == kLayers && !(of.same && *of.same == s) &&
                    p.frame(of, s).pixels.data != of.pixels.data) {
                    warnings.push_back({"layers-dropped", unit + k,
                                        "Photoshop layer data dropped after a pixel merge (Q-RAST-3)"});
                    p.dropped.push_back(unit + k);
                    continue;
                }
                if (s == Side::ours) {
                    es.push_back(w.keep(*e));
                } else if (ifd::is_pointer_tag(tag)) {
                    if (tag == kSubIfds) {
                        warnings.push_back(
                            {"derived-stale", unit + k, "SubIFDs from another input are not carried over"});
                        p.dropped.push_back(unit + k);
                        continue;
                    }
                    const auto g = w.id();
                    std::vector<emit::Seg> tail;
                    appended.push_back(copy_ifd(w, sd.t, e->sub.at(0), g, tail, warnings, unit + k));
                    for (auto& x : tail)
                        appended.push_back(std::move(x));
                    es.push_back({tag, lt.bigtiff ? ifd::IFD8 : ifd::LONG, 1, {w.pointer(g)}, std::nullopt});
                } else {
                    es.push_back({e->tag,
                                  e->type,
                                  e->count,
                                  {emit::bytes(to_order(le_value(sd.t, *e), e->type, lt.big_endian))},
                                  std::nullopt});
                }
            }
            // GeoTIFF: ours' three tags when the keys are ours' exactly, else regenerated from the merged
            // keys.
            if (!geo.empty()) {
                const auto oj = of.src[static_cast<std::size_t>(Side::ours)];
                if (oj && !geo_changed && geo.size() == lay.pages[*oj].geo.size()) {
                    for (auto tag : {kGeoDir, kGeoDouble, kGeoAscii})
                        if (auto* e = lt.chain[lay.pages[*oj].index].find(tag))
                            es.push_back(w.keep(*e));
                } else {
                    std::vector<std::uint64_t> dir{1, 1, 0, geo.size()}, extra;
                    std::vector<std::byte> dbl, asc;
                    for (auto& [id, gs] : geo) {
                        const Geo& g = *gs.first;
                        dir.push_back(id), dir.push_back(g.loc), dir.push_back(g.count);
                        if (g.loc == 0) {
                            dir.push_back(Reader{g.data, kName}.le16(0));
                        } else if (g.loc == kGeoDir) {
                            dir.push_back(extra.size()); // fixed up below
                            for (std::size_t k = 0; k < g.data.size(); k += 2)
                                extra.push_back(Reader{g.data, kName}.le16(k));
                        } else if (g.loc == kGeoDouble) {
                            dir.push_back(dbl.size() / 8);
                            dbl.insert(dbl.end(), g.data.begin(), g.data.end());
                        } else {
                            dir.push_back(asc.size());
                            asc.insert(asc.end(), g.data.begin(), g.data.end());
                        }
                    }
                    for (std::size_t k = 4; k < dir.size(); k += 4) // shorts stored after the key entries
                        if (dir[k + 1] == kGeoDir)
                            dir[k + 3] += dir.size();
                    dir.insert(dir.end(), extra.begin(), extra.end());
                    es.push_back(w.numbers(kGeoDir, ifd::SHORT, dir));
                    if (!dbl.empty())
                        es.push_back({kGeoDouble,
                                      ifd::DOUBLE,
                                      dbl.size() / 8,
                                      {emit::bytes(to_order(dbl, ifd::DOUBLE, lt.big_endian))},
                                      std::nullopt});
                    if (!asc.empty())
                        es.push_back({kGeoAscii, ifd::ASCII, asc.size(), {emit::bytes(asc)}, std::nullopt});
                }
            }
            appended.push_back(
                w.write(gid[i], std::move(es), i + 1 < n ? pointer_to(i + 1) : w.pointer(std::uint64_t{0})));
        }
        // Reduced-resolution pages copied unchanged next to a changed full page go stale (derived-stale).
        for (std::size_t i = 0; i < n; ++i) {
            auto* k = find_key(p.frame(p.frames[i], p.frames[i].same.value_or(p.enc)).keys, "/tags/0x00fe");
            if (k && p.frames[i].same &&
                std::any_of(p.frames.begin(), p.frames.end(), [](const OutFrame& f) { return !f.same; }))
                warnings.push_back({"derived-stale", "/pages/" + std::to_string(i),
                                    "reduced-resolution page kept after a pixel merge"});
        }
        const std::uint64_t at = lt.bigtiff ? 8 : 4;
        auto src = lay.src.bytes();
        std::vector<emit::Seg> file{
            emit::raw(lay.src, 0, at), pointer_to(0),
            emit::raw(lay.src, at + lt.offset_size(), src.size() - at - lt.offset_size())};
        for (auto& s : appended)
            file.push_back(std::move(s));
        return emit::emit(emit::group(ids++, std::move(file)));
    }
};

} // namespace

std::vector<std::byte> write(const std::vector<raster::Raster>& pages, Encoding enc) {
    emit::GroupId ids = 1000;
    ifd::Writer w{enc.big_endian, enc.bigtiff, std::nullopt, &ids};
    std::vector<emit::Seg> file;
    std::vector<std::byte> head{std::byte(enc.big_endian ? 'M' : 'I'), std::byte(enc.big_endian ? 'M' : 'I')};
    file.push_back(emit::bytes(head));
    file.push_back(w.number(enc.bigtiff ? 43 : 42, 2));
    if (enc.bigtiff)
        file.push_back(w.number(8, 2)), file.push_back(w.number(0, 2));
    std::vector<emit::GroupId> gid;
    for (std::size_t i = 0; i < pages.size(); ++i)
        gid.push_back(w.id());
    file.push_back(w.pointer(gid[0]));
    std::vector<Warning> ignore;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        Params pr;
        pr.compression = enc.compression, pr.predictor = enc.predictor, pr.bits = enc.bits;
        pr.photometric = enc.photometric, pr.rps = enc.rows_per_strip;
        auto e = encode(pages[i], pr, std::nullopt, {}, enc.big_endian, ignore);
        std::vector<emit::Seg> at;
        std::vector<std::uint64_t> counts;
        const std::uint16_t ltype = enc.bigtiff ? ifd::LONG8 : ifd::LONG;
        for (auto& s : e.strips) {
            const auto g = w.id();
            file.push_back(emit::group(g, {emit::bytes(s)}));
            at.push_back(emit::field(ifd::type_size(ltype), w.endian(), w.offset(g)));
            counts.push_back(s.size());
        }
        auto es = structural(w, e, pages[i].width, pages[i].height, std::move(at), counts);
        for (auto& t : enc.tags)
            es.push_back({t.tag,
                          t.type,
                          t.count,
                          {emit::bytes(to_order(t.value, t.type, enc.big_endian))},
                          std::nullopt});
        file.push_back(w.write(gid[i], std::move(es),
                               i + 1 < pages.size() ? w.pointer(gid[i + 1]) : w.pointer(std::uint64_t{0})));
    }
    return emit::emit(emit::group(ids++, std::move(file)));
}

std::unique_ptr<Format> make_format() {
    return rastermisc::make_format(std::make_unique<TiffCodec>());
}

} // namespace semsplinter::tiff
