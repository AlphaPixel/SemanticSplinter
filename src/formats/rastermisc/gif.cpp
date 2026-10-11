#include "common.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/formats/gif.hpp>

#include <gif_lib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace semsplinter::gif {

namespace {

using namespace rastermisc;
constexpr std::string_view kName = "GIF";
constexpr Rgba8 kClear{0, 0, 0, 0}; // transparent pixels are canonical (0,0,0,0): GIF stores no color there

struct Gce {
    std::uint8_t disposal = 0;
    bool user = false;
    int transparent = -1;
    std::uint16_t delay = 0;
};

struct GifFrame {
    std::uint16_t left = 0, top = 0, w = 0, h = 0;
    bool interlace = false, local = false;
    std::vector<Rgba8> table;                          // the effective table as stored (opaque colors)
    std::uint64_t lct_at = 0, lzw_at = 0, lzw_end = 0; // local table; min code size through terminator
    Gce gce;
};

struct GifDoc final : Doc {
    std::string version;
    std::uint16_t sw = 0, sh = 0;
    std::uint8_t packed = 0, bg = 0, aspect = 0;
    std::vector<Rgba8> gct;
    std::vector<GifFrame> gf;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> blocks; // extension keys -> block bytes
};

const GifDoc& gifdoc(const Doc& d) {
    return static_cast<const GifDoc&>(d);
}

// The canonical palette of a frame: its table with the transparent entry as kClear.
std::vector<Rgba8> canonical(const GifFrame& f) {
    auto p = f.table;
    if (f.gce.transparent >= 0 && static_cast<std::size_t>(f.gce.transparent) < p.size())
        p[static_cast<std::size_t>(f.gce.transparent)] = kClear;
    return p;
}

// Sub-blocks from `at` to the terminator; returns the offset after it.
std::uint64_t skip_blocks(const Reader& r, std::uint64_t at, std::vector<std::byte>* data = nullptr) {
    for (;;) {
        const auto n = r.u8(at++);
        if (!n)
            return at;
        auto b = r.sub(at, n);
        if (data)
            data->insert(data->end(), b.begin(), b.end());
        at += n;
    }
}

unsigned table_bits(std::size_t n) { // N with 2^(N+1) >= n
    unsigned b = 0;
    while ((std::size_t{2} << b) < n)
        ++b;
    return b;
}

// GIF LZW with the gif.h policy: a clear code first, codes widen when the next code reaches 2^width, a
// clear code when the table is full, and clear + end-of-information at the end (Q-RAST-4).
std::vector<std::byte> lzw(const std::vector<std::uint8_t>& idx, unsigned min_bits) {
    std::vector<std::byte> out;
    out.push_back(std::byte(min_bits));
    std::vector<std::byte> data;
    std::uint32_t acc = 0;
    unsigned nacc = 0;
    auto put = [&](unsigned code, unsigned width) {
        acc |= std::uint32_t{code} << nacc;
        nacc += width;
        while (nacc >= 8)
            data.push_back(std::byte(acc & 0xFF)), acc >>= 8, nacc -= 8;
    };
    const unsigned clear = 1u << min_bits;
    unsigned width = min_bits + 1, max_code = clear + 1;
    std::vector<std::uint16_t> next(4096 * 256u, 0); // child code of (code, value)
    std::vector<std::size_t> used;                   // entries to reset on a clear code
    put(clear, width);
    int cur = -1;
    for (auto v : idx) {
        if (cur < 0) {
            cur = v;
        } else if (auto c = next[static_cast<std::size_t>(cur) * 256 + v]) {
            cur = c;
        } else {
            put(static_cast<unsigned>(cur), width);
            used.push_back(static_cast<std::size_t>(cur) * 256 + v);
            next[used.back()] = static_cast<std::uint16_t>(++max_code);
            if (max_code >= (1u << width))
                ++width;
            if (max_code == 4095) {
                put(clear, width);
                for (auto u : used)
                    next[u] = 0;
                used.clear();
                width = min_bits + 1, max_code = clear + 1;
            }
            cur = v;
        }
    }
    if (cur >= 0)
        put(static_cast<unsigned>(cur), width);
    put(clear, width);
    put(clear + 1, min_bits + 1);
    if (nacc)
        data.push_back(std::byte(acc & 0xFF));
    for (std::size_t at = 0; at < data.size(); at += 255) {
        const auto n = std::min<std::size_t>(255, data.size() - at);
        out.push_back(std::byte(n));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + n));
    }
    out.push_back(std::byte{0});
    return out;
}

std::vector<std::uint8_t> interlaced(const std::vector<std::uint8_t>& idx, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> o;
    o.reserve(idx.size());
    for (auto [start, step] : {std::pair{0u, 8u}, {4u, 8u}, {2u, 4u}, {1u, 2u}})
        for (std::uint32_t y = start; y < h; y += step)
            o.insert(o.end(), idx.begin() + std::ptrdiff_t{y} * w, idx.begin() + std::ptrdiff_t{y + 1} * w);
    return o;
}

std::vector<std::byte> table_bytes(const std::vector<Rgba8>& t, std::size_t size) {
    std::vector<std::byte> o;
    for (std::size_t k = 0; k < size; ++k) {
        Rgba8 c = k < t.size() ? t[k] : Rgba8{0, 0, 0, 0};
        o.push_back(std::byte(c[0])), o.push_back(std::byte(c[1])), o.push_back(std::byte(c[2]));
    }
    return o;
}

// One frame to write: a raw copy of a source frame's table and LZW data, or indices to encode.
struct OutFrame2 {
    const GifDoc* raw = nullptr;
    std::size_t raw_index = 0;
    std::vector<std::uint8_t> idx;
    std::vector<Rgba8> pal; // canonical
    std::uint16_t left = 0, top = 0, w = 0, h = 0, delay = 0;
    std::uint8_t disposal = 0;
    bool user = false, interlace = false;
};

struct Out {
    std::string version = "89a";
    std::uint16_t sw = 0, sh = 0;
    std::uint8_t bg = 0, aspect = 0, flags = 0x70; // color resolution and sort bits of the packed field
    std::vector<Rgba8> gct;                        // opaque colors, empty for none
    std::vector<emit::Seg> extensions;             // application and comment blocks
    std::vector<OutFrame2> frames;
};

std::vector<std::byte> assemble(Out o) {
    std::vector<emit::Seg> file;
    std::vector<std::byte> head;
    for (char c : "GIF" + o.version)
        head.push_back(std::byte(c));
    put_le(head, o.sw, 2), put_le(head, o.sh, 2);
    const unsigned gbits = table_bits(o.gct.size());
    head.push_back(std::byte(o.gct.empty() ? (o.flags & 0x70) : 0x80 | (o.flags & 0x78) | gbits));
    head.push_back(std::byte(o.bg)), head.push_back(std::byte(o.aspect));
    file.push_back(emit::bytes(head));
    if (!o.gct.empty())
        file.push_back(emit::bytes(table_bytes(o.gct, std::size_t{2} << gbits)));
    for (auto& s : o.extensions)
        file.push_back(std::move(s));
    for (auto& f : o.frames) {
        const GifFrame* src = f.raw ? &f.raw->gf[f.raw_index] : nullptr;
        int transparent = -1;
        if (src) {
            transparent = src->gce.transparent;
        } else {
            auto it = std::find(f.pal.begin(), f.pal.end(), kClear);
            if (it != f.pal.end())
                transparent = static_cast<int>(it - f.pal.begin());
        }
        if (transparent >= 0 || f.delay || f.disposal || f.user) {
            std::vector<std::byte> g{std::byte{0x21}, std::byte{0xF9}, std::byte{4}};
            g.push_back(std::byte((f.disposal & 7) << 2 | (f.user ? 2 : 0) | (transparent >= 0 ? 1 : 0)));
            put_le(g, f.delay, 2);
            g.push_back(std::byte(transparent >= 0 ? transparent : 0)), g.push_back(std::byte{0});
            file.push_back(emit::bytes(g));
        }
        // A frame uses the global table when its palette fits it, else a local one.
        bool local;
        std::vector<Rgba8> lct;
        if (src) {
            local = src->local, lct = src->table;
        } else {
            local = f.pal.size() > o.gct.size();
            for (std::size_t k = 0; !local && k < f.pal.size(); ++k)
                local = f.pal[k] != kClear && f.pal[k] != o.gct[k];
            local = local || (o.gct.empty());
            lct = f.pal;
        }
        std::vector<std::byte> d{std::byte{0x2C}};
        put_le(d, f.left, 2), put_le(d, f.top, 2), put_le(d, f.w, 2), put_le(d, f.h, 2);
        const unsigned lbits = table_bits(lct.size());
        const bool il = src ? src->interlace : f.interlace;
        d.push_back(std::byte((local ? 0x80 | lbits : 0) | (il ? 0x40 : 0)));
        file.push_back(emit::bytes(d));
        if (local)
            file.push_back(emit::bytes(table_bytes(lct, std::size_t{2} << lbits)));
        if (src) {
            file.push_back(emit::raw(f.raw->src, src->lzw_at, src->lzw_end - src->lzw_at));
        } else {
            const unsigned bits = std::max(2u, table_bits(local ? lct.size() : o.gct.size()) + 1);
            file.push_back(emit::bytes(lzw(il ? interlaced(f.idx, f.w, f.h) : f.idx, bits)));
        }
    }
    file.push_back(emit::bytes(std::vector<std::byte>{std::byte{0x3B}}));
    return emit::emit(emit::group(1, std::move(file)));
}

std::string text_of(const std::vector<std::byte>& d, std::size_t max = 200) {
    std::string s(reinterpret_cast<const char*>(d.data()), d.size());
    std::string out = "\"";
    for (char c : s.substr(0, max))
        out += c == '\n' ? std::string("\\n") : c == '"' ? std::string("\\\"") : std::string(1, c);
    return out + (s.size() > max ? "...\"" : "\"");
}

int read_mem(GifFileType* g, GifByteType* buf, int n) {
    auto* s = static_cast<std::span<const std::byte>*>(g->UserData);
    const auto k = std::min<std::size_t>(static_cast<std::size_t>(n), s->size());
    std::memcpy(buf, s->data(), k);
    *s = s->subspan(k);
    return static_cast<int>(k);
}

class GifCodec final : public Codec {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{"gif",
                                  "Graphics Interchange Format (still and animated)",
                                  {".gif"},
                                  {true, true, false, true, true},
                                  {}};
        return i;
    }

    int probe(std::span<const std::byte> head, std::string_view) const override {
        if (head.size() < 13)
            return 0;
        std::string_view s(reinterpret_cast<const char*>(head.data()), 6);
        return s == "GIF87a" || s == "GIF89a" ? 100 : 0;
    }

    std::unique_ptr<Doc> load(ByteSource src, Limits& budget, const OptionValues&) const override {
        auto d = std::make_unique<GifDoc>();
        d->src = std::move(src);
        d->sequence = true;
        const Reader r{d->src.bytes(), kName};
        auto sig = r.sub(0, 6);
        d->version.assign(reinterpret_cast<const char*>(sig.data()) + 3, 3);
        if (std::memcmp(sig.data(), "GIF", 3) != 0 || (d->version != "87a" && d->version != "89a"))
            bad(kName, "missing GIF87a/GIF89a signature");
        d->sw = r.le16(6), d->sh = r.le16(8), d->packed = r.u8(10), d->bg = r.u8(11), d->aspect = r.u8(12);
        auto& m = d->map.ranges;
        auto add = [&](std::uint64_t a, std::uint64_t len, ByteClass c, std::string unit,
                       std::string layer = {}) {
            if (len)
                m.push_back({a, len, c, std::move(unit), std::move(layer)});
        };
        add(0, 6, ByteClass::framing, "/signature");
        add(6, 4, ByteClass::content, "/screen/size");
        add(10, 1, ByteClass::framing, "/palette");
        add(11, 1, ByteClass::content, "/screen/background");
        add(12, 1, ByteClass::content, "/screen/aspect");
        std::uint64_t at = 13;
        auto read_table = [&](unsigned bits) {
            const std::size_t n = std::size_t{2} << bits;
            auto t = r.sub(at, 3 * n);
            std::vector<Rgba8> v;
            for (std::size_t k = 0; k < n; ++k)
                v.push_back({std::to_integer<std::uint8_t>(t[3 * k]),
                             std::to_integer<std::uint8_t>(t[3 * k + 1]),
                             std::to_integer<std::uint8_t>(t[3 * k + 2]), 255});
            add(at, 3 * n, ByteClass::encoding, "/palette", "palette");
            at += 3 * n;
            return v;
        };
        if (d->packed & 0x80)
            d->gct = read_table(d->packed & 7);
        std::optional<Gce> gce;
        std::map<std::string, int> counts;
        std::uint64_t pixels_total = 0;
        for (bool done = false; !done;) {
            const auto intro = r.u8(at);
            if (intro == 0x3B) {
                add(at, 1, ByteClass::framing, "/end");
                ++at;
                done = true;
            } else if (intro == 0x21) {
                const auto label = r.u8(at + 1);
                const std::uint64_t start = at;
                if (label == 0xF9) {
                    if (r.u8(at + 2) != 4)
                        bad(kName, "bad graphic control extension");
                    Gce g;
                    const auto p = r.u8(at + 3);
                    g.disposal = (p >> 2) & 7, g.user = p & 2, g.delay = r.le16(at + 4);
                    g.transparent = p & 1 ? r.u8(at + 6) : -1;
                    gce = g;
                    const std::string f = "/frames/" + std::to_string(d->gf.size());
                    add(at, 3, ByteClass::framing, f);
                    add(at + 3, 4, ByteClass::content, f);
                    at = skip_blocks(r, at + 7);
                    add(at - 1, 1, ByteClass::framing, f);
                    continue;
                }
                std::vector<std::byte> data;
                std::string key;
                if (label == 0xFF && r.u8(at + 2) == 11) {
                    auto id = r.sub(at + 3, 11);
                    std::string name(reinterpret_cast<const char*>(id.data()), 11);
                    at = skip_blocks(r, at + 14, &data);
                    if ((name == "NETSCAPE2.0" || name == "ANIMEXTS1.0") && data.size() == 3 &&
                        data[0] == std::byte{1}) {
                        key = "/animation/loop";
                        const auto loop = Reader{data, kName}.le16(1);
                        d->keys.push_back(
                            {key, hash128(data), loop ? std::to_string(loop) : "forever", "animation"});
                    } else {
                        key = "/app/" + pointer_escape(name);
                        Hasher h;
                        h.add(name).raw(data);
                        d->keys.push_back({key, h.digest(), bytes_text(data)});
                    }
                } else {
                    at = skip_blocks(r, at + 2, &data);
                    if (label == 0xFE) {
                        key = "/comments/" + std::to_string(counts["c"]++);
                        d->keys.push_back({key, hash128(data), text_of(data), "text"});
                    } else {
                        char buf[16];
                        std::snprintf(buf, sizeof buf, "%02x", label);
                        key =
                            (label == 0x01 ? std::string("/plaintext/") : "/ext/" + std::string(buf) + ":") +
                            std::to_string(counts[buf]++);
                        Hasher h;
                        h.add(label).raw(data);
                        d->keys.push_back({key, h.digest(), bytes_text(data)});
                        if (label == 0x01)
                            gce.reset(); // a plain text block takes the pending control
                    }
                }
                d->blocks[key] = {start, at - start};
                add(start, at - start, ByteClass::content, key);
            } else if (intro == 0x2C) {
                GifFrame f;
                f.left = r.le16(at + 1), f.top = r.le16(at + 3), f.w = r.le16(at + 5), f.h = r.le16(at + 7);
                const auto p = r.u8(at + 9);
                f.interlace = p & 0x40, f.local = p & 0x80;
                const std::string unit = "/frames/" + std::to_string(d->gf.size());
                add(at, 1, ByteClass::framing, unit);
                add(at + 1, 8, ByteClass::content, unit);
                add(at + 9, 1, ByteClass::encoding, unit);
                at += 10;
                if (f.local) {
                    f.lct_at = at;
                    f.table = read_table(p & 7);
                } else {
                    f.table = d->gct;
                }
                if (f.table.empty())
                    bad(kName, "frame without a color table");
                if (!f.w || !f.h)
                    bad(kName, "empty frame");
                pixels_total += std::uint64_t{f.w} * f.h;
                if (pixels_total > budget.max_pixels)
                    throw Error(ErrorCategory::limit, "GIF frames exceed --max-pixels");
                f.lzw_at = at;
                const auto min_bits = r.u8(at);
                if (min_bits < 2 || min_bits > 11)
                    bad(kName, "bad LZW minimum code size");
                f.lzw_end = skip_blocks(r, at + 1);
                add(at, 1, ByteClass::encoding, unit + "/pixels", "lzw");
                // Sub-block lengths are framing; the code stream is encoding (raster-misc.md §7.1).
                for (std::uint64_t b = at + 1; b < f.lzw_end;) {
                    const auto n = r.u8(b);
                    add(b, 1, ByteClass::framing, unit + "/pixels");
                    add(b + 1, n, ByteClass::encoding, unit + "/pixels", "lzw");
                    b += 1u + n;
                }
                at = f.lzw_end;
                if (gce)
                    f.gce = *gce;
                gce.reset();
                d->gf.push_back(std::move(f));
            } else {
                bad(kName, "unknown block 0x" + std::to_string(intro) + " at " + std::to_string(at));
            }
        }
        if (d->gf.empty())
            bad(kName, "no image");
        if (at < r.d.size())
            d->warnings.push_back({"framing-inconsistent", "/end", "bytes after the trailer are ignored"});

        // Pixels through giflib.
        std::span<const std::byte> rest = r.d.first(at);
        int err = 0;
        GifFileType* g = DGifOpen(&rest, read_mem, &err);
        if (!g)
            bad(kName, std::string("giflib: ") + GifErrorString(err));
        struct Close {
            GifFileType* g;
            ~Close() {
                int e = 0;
                DGifCloseFile(g, &e);
            }
        } close{g};
        if (DGifSlurp(g) != GIF_OK)
            bad(kName, std::string("giflib: ") + GifErrorString(g->Error));
        if (static_cast<std::size_t>(g->ImageCount) != d->gf.size())
            bad(kName, "giflib found a different number of frames");
        for (std::size_t k = 0; k < d->gf.size(); ++k) {
            auto& gf = d->gf[k];
            Frame f;
            f.pixels = new_raster(gf.w, gf.h, false, budget);
            f.palette = canonical(gf);
            const auto* bits = g->SavedImages[k].RasterBits;
            f.indices.assign(bits, bits + std::size_t{gf.w} * gf.h);
            for (std::uint64_t i = 0; i < f.indices.size(); ++i) {
                if (f.indices[i] >= f.palette.size())
                    bad(kName, "index outside the color table");
                f.pixels.set(i, cell(f.palette[f.indices[i]]));
            }
            if (gf.left || gf.top)
                f.keys.push_back({"/rect", hash128(std::to_string(gf.left) + "," + std::to_string(gf.top)),
                                  std::to_string(gf.left) + "," + std::to_string(gf.top)});
            if (gf.gce.delay)
                f.keys.push_back({"/delay", hash128(std::to_string(gf.gce.delay)),
                                  std::to_string(gf.gce.delay) + "cs", "animation"});
            if (gf.gce.disposal)
                f.keys.push_back({"/disposal", hash128(std::to_string(gf.gce.disposal)),
                                  std::to_string(gf.gce.disposal), "animation"});
            if (gf.gce.user)
                f.keys.push_back({"/user-input", hash128(std::string_view("1")), "1", "animation"});
            f.encoding = std::string(gf.local ? "local" : "global") + (gf.interlace ? " interlaced" : "");
            d->frames.push_back(std::move(f));
        }
        d->keys.push_back(
            {"/screen/size", hash128(r.sub(6, 4)), std::to_string(d->sw) + "x" + std::to_string(d->sh)});
        if (d->bg)
            d->keys.push_back(
                {"/screen/background", hash128(r.sub(11, 1)), "index " + std::to_string(d->bg)});
        if (d->aspect)
            d->keys.push_back({"/screen/aspect", hash128(r.sub(12, 1)), std::to_string(d->aspect)});
        d->encoding = {{"version", d->version},
                       {"gct", d->gct.empty() ? "none"
                                              : std::to_string(d->gct.size()) +
                                                    " xxh3=" + hex16(hash128(r.sub(13, 3 * d->gct.size())))}};
        std::sort(m.begin(), m.end(),
                  [](const EnvRange& a, const EnvRange& b) { return a.offset < b.offset; });
        d->map.validate(r.d.size());
        return d;
    }

    std::vector<std::byte> write(Plan& p, std::vector<Warning>&) const override {
        const GifDoc& src = gifdoc(p.doc(p.enc));
        Out o;
        o.version = src.version;
        o.gct = src.gct;
        o.flags = src.packed;
        bool extensions = false;
        for (auto& [k, s] : p.keys) {
            const GifDoc& d = gifdoc(p.doc(s));
            if (k == "/screen/size")
                o.sw = d.sw, o.sh = d.sh;
            else if (k == "/screen/background")
                o.bg = d.bg;
            else if (k == "/screen/aspect")
                o.aspect = d.aspect;
            else if (auto b = d.blocks.find(k); b != d.blocks.end())
                o.extensions.push_back(emit::raw(d.src, b->second.first, b->second.second)),
                    extensions = true;
        }
        for (std::size_t i = 0; i < p.frames.size(); ++i) {
            const OutFrame& of = p.frames[i];
            OutFrame2 f;
            f.w = static_cast<std::uint16_t>(of.pixels.width),
            f.h = static_cast<std::uint16_t>(of.pixels.height);
            for (auto& [k, s] : of.keys) {
                auto& sf = gifdoc(p.doc(s)).gf[*of.src[static_cast<std::size_t>(s)]];
                if (k == "/rect")
                    f.left = sf.left, f.top = sf.top;
                else if (k == "/delay")
                    f.delay = sf.gce.delay;
                else if (k == "/disposal")
                    f.disposal = sf.gce.disposal;
                else if (k == "/user-input")
                    f.user = sf.gce.user;
            }
            extensions |= f.delay || f.disposal || f.user;
            // Unchanged frames keep their table and LZW bytes when the table they use is still there.
            if (of.same) {
                const GifDoc& sd = gifdoc(p.doc(*of.same));
                const GifFrame& sf = sd.gf[*of.src[static_cast<std::size_t>(*of.same)]];
                if (sf.local || sd.gct == o.gct) {
                    f.raw = &sd, f.raw_index = *of.src[static_cast<std::size_t>(*of.same)];
                    extensions |= sf.gce.transparent >= 0;
                    o.frames.push_back(std::move(f));
                    continue;
                }
            }
            const Doc& base_doc = p.doc(of.same ? *of.same : p.enc);
            const auto base_idx = of.src[static_cast<std::size_t>(of.same ? *of.same : p.enc)];
            if (auto ex = merged_indices(p, of)) {
                f.idx = std::move(ex->first), f.pal = std::move(ex->second);
            } else {
                std::vector<Rgba8> base =
                    base_idx ? base_doc.frames[*base_idx].palette : std::vector<Rgba8>{};
                if (base.empty())
                    for (auto& c : o.gct)
                        base.push_back(c);
                auto pal = palettize(of.pixels, base, 256, f.idx);
                if (!pal)
                    throw Error(ErrorCategory::unsupported,
                                "GIF: merged frame " + std::to_string(i) +
                                    " has more than 256 colors (encoding-mismatch; GIF merges never dither)");
                f.pal = std::move(*pal);
            }
            if (std::count(f.pal.begin(), f.pal.end(), kClear) > 0)
                extensions = true;
            f.interlace = base_idx && gifdoc(base_doc).gf[*base_idx].interlace;
            o.frames.push_back(std::move(f));
        }
        if (extensions)
            o.version = "89a";
        return assemble(std::move(o));
    }
};

} // namespace

std::vector<std::byte> write(const std::vector<FrameSpec>& frames, int loop) {
    Out o;
    std::vector<Rgba8> gct;
    for (auto& f : frames) {
        o.sw = std::max<std::uint16_t>(o.sw, static_cast<std::uint16_t>(f.left + f.pixels.width));
        o.sh = std::max<std::uint16_t>(o.sh, static_cast<std::uint16_t>(f.top + f.pixels.height));
        OutFrame2 x;
        x.w = static_cast<std::uint16_t>(f.pixels.width), x.h = static_cast<std::uint16_t>(f.pixels.height);
        x.left = f.left, x.top = f.top, x.delay = f.delay, x.disposal = f.disposal, x.interlace = f.interlace;
        Raster r = f.pixels;
        for (std::uint64_t i = 0; i < r.cells(); ++i)
            if (rgba8(r.get(i))[3] == 0)
                r.set(i, cell(kClear));
        auto pal = palettize(r, gct, 256, x.idx);
        if (!pal) // too many colors for one global table: a local one
            pal = palettize(r, {}, 256, x.idx);
        if (!pal)
            throw Error(ErrorCategory::unsupported, "GIF: a frame has more than 256 colors");
        if (pal->size() <= 256 && pal->size() >= gct.size() &&
            std::equal(gct.begin(), gct.end(), pal->begin()))
            gct = *pal;
        x.pal = std::move(*pal);
        o.frames.push_back(std::move(x));
    }
    if (loop >= 0) {
        std::vector<std::byte> b{std::byte{0x21}, std::byte{0xFF}, std::byte{11}};
        for (char c : std::string_view("NETSCAPE2.0"))
            b.push_back(std::byte(c));
        b.push_back(std::byte{3}), b.push_back(std::byte{1});
        put_le(b, static_cast<std::uint64_t>(loop), 2);
        b.push_back(std::byte{0});
        o.extensions.push_back(emit::bytes(b));
    }
    o.gct = gct; // a transparent entry is written black; frames say which index is transparent
    return assemble(std::move(o));
}

std::unique_ptr<Format> make_format() {
    return rastermisc::make_format(std::make_unique<GifCodec>());
}

} // namespace semsplinter::gif
