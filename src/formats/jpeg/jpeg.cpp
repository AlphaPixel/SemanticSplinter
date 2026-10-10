#include "internal.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/formats/png.hpp>
#include <semsplinter/merge.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace semsplinter::jpeg {

namespace {

using raster::Raster;

constexpr std::size_t kMaxScans = 1000; // progressive scan cap for untrusted input (jpeg.md §11)

std::uint16_t be16(std::span<const std::byte> d, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(d[at]) << 8 |
                                      std::to_integer<unsigned>(d[at + 1]));
}
std::string hex16(const Hash128& h) {
    return h.hex().substr(0, 16);
}
std::string str(std::span<const std::byte> d) {
    return {reinterpret_cast<const char*>(d.data()), d.size()};
}

[[noreturn]] void bad(const std::string& what) {
    throw Error(ErrorCategory::invalid_input, "JPEG: " + what);
}
[[noreturn]] void unsupported(const std::string& what) {
    throw Error(ErrorCategory::unsupported, "JPEG: " + what);
}

bool standalone(std::uint8_t m) {
    return (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == kSOI || m == kEOI;
}
bool sof_marker(std::uint8_t m) {
    return m >= 0xC0 && m <= 0xCF && m != kDHT && m != 0xC8 && m != kDAC;
}

std::string marker_name(std::uint8_t m) {
    static const std::map<std::uint8_t, const char*> names{
        {kSOI, "SOI"}, {kEOI, "EOI"}, {kSOS, "SOS"}, {kDQT, "DQT"}, {kDHT, "DHT"}, {kDAC, "DAC"},
        {kDRI, "DRI"}, {kCOM, "COM"}, {0xDC, "DNL"}, {0xDE, "DHP"}, {0xDF, "EXP"}, {0x01, "TEM"}};
    if (auto it = names.find(m); it != names.end())
        return it->second;
    if (sof_marker(m))
        return "SOF" + std::to_string(m - 0xC0);
    if (m >= 0xD0 && m <= 0xD7)
        return "RST" + std::to_string(m - 0xD0);
    if (m >= 0xE0 && m <= 0xEF)
        return "APP" + std::to_string(m - 0xE0);
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", unsigned{m});
    return buf;
}

std::string process_name(std::uint8_t sof) {
    switch (sof) {
    case 0xC0:
        return "baseline";
    case 0xC1:
        return "extended";
    case 0xC2:
        return "progressive";
    case 0xC9:
        return "arithmetic";
    case 0xCA:
        return "arithmetic-progressive";
    default:
        return marker_name(sof);
    }
}

// Quoted, truncated, with control characters escaped (COM text).
std::string escape_text(std::string_view s, std::size_t max = 200) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size() && i < max; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\')
            out += '\\', out += static_cast<char>(c);
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", unsigned{c});
            out += buf;
        } else
            out += static_cast<char>(c);
    }
    return out + (s.size() > max ? "\"..." : "\"");
}

// The leading printable ASCII of an APPn payload up to its NUL: "JFIF", "Exif", "ICC_PROFILE", ...
std::string signature(std::span<const std::byte> d) {
    std::string s;
    for (std::size_t i = 0; i < d.size() && i < 64; ++i) {
        const auto c = std::to_integer<unsigned char>(d[i]);
        if (c < 0x20 || c > 0x7E)
            break;
        s += static_cast<char>(c);
    }
    return s;
}

std::string bytes_text(std::uint64_t n, const Hash128& h) {
    return "bytes=" + std::to_string(n) + " xxh3=" + hex16(h);
}

} // namespace

std::pair<std::vector<Segment>, std::uint64_t> parse_segments(std::span<const std::byte> b) {
    const std::size_t n = b.size();
    auto at = [&](std::size_t i) { return std::to_integer<std::uint8_t>(b[i]); };
    if (n < 4 || at(0) != 0xFF || at(1) != kSOI)
        bad("missing SOI");
    std::vector<Segment> segs{{0, 0, kSOI, 0, 2}};
    std::size_t pos = 2, scans = 0;
    for (;;) {
        Segment s;
        s.start = pos;
        if (pos >= n)
            bad("no EOI");
        if (at(pos) != 0xFF)
            bad("expected a marker at offset " + std::to_string(pos));
        while (pos < n && at(pos) == 0xFF)
            ++pos;
        if (pos >= n)
            bad("no EOI");
        s.offset = pos - 1, s.marker = at(pos++);
        if (s.marker == 0x00 || s.marker == kSOI)
            bad("bad marker at offset " + std::to_string(s.offset));
        if (standalone(s.marker)) {
            s.end = pos;
            segs.push_back(s);
            if (s.marker == kEOI)
                return {std::move(segs), pos};
            continue;
        }
        if (n - pos < 2 || be16(b, pos) < 2 || n - pos < be16(b, pos))
            bad(marker_name(s.marker) + " segment length at offset " + std::to_string(s.offset));
        s.length = be16(b, pos) - 2u;
        pos += s.length + 2u;
        if (s.marker == kSOS) {
            if (++scans > kMaxScans)
                throw Error(ErrorCategory::limit, "JPEG: more than " + std::to_string(kMaxScans) + " scans");
            // Entropy-coded data: FF00 is a stuffed byte, FFD0..FFD7 a restart marker, fill bytes may
            // precede either; any other marker ends the scan.
            for (;;) {
                if (pos >= n)
                    bad("truncated scan data");
                if (at(pos) != 0xFF) {
                    ++pos;
                    continue;
                }
                std::size_t q = pos;
                while (q < n && at(q) == 0xFF)
                    ++q;
                if (q >= n)
                    bad("truncated scan data");
                if (at(q) == 0x00 || (at(q) >= 0xD0 && at(q) <= 0xD7)) {
                    pos = q + 1;
                    continue;
                }
                break;
            }
        }
        s.end = pos;
        segs.push_back(s);
    }
}

namespace {

// ---- Loading ----

void build_units(JpegDoc& doc) {
    std::map<std::string, std::size_t> index;
    int com = 0;
    for (std::size_t i = 0; i < doc.segs.size(); ++i) {
        const auto& s = doc.segs[i];
        if (!s.metadata())
            continue;
        auto d = doc.bytes(s);
        std::string key, kind = "metadata";
        if (s.marker == kCOM) {
            key = "/com/" + std::to_string(com++), kind = "text";
        } else {
            const int n = s.marker - 0xE0;
            const std::string sig = signature(d);
            if (n == 0 && (sig == "JFIF" || sig == "JFXX"))
                key = sig == "JFIF" ? "/jfif" : "/jfxx";
            else if (n == 1 && sig == "Exif")
                key = "/exif";
            else if (n == 1 && sig == "http://ns.adobe.com/xap/1.0/")
                key = "/xmp";
            else if (n == 1 && sig == "http://ns.adobe.com/xmp/extension/")
                key = "/xmp/extended";
            else if (n == 2 && sig == "ICC_PROFILE")
                key = "/icc", kind = "color";
            else if (n == 2 && sig == "MPF")
                key = "/mpf";
            else if (n == 13 && sig == "Photoshop 3.0")
                key = "/irb";
            else
                key = "/app/APP" + std::to_string(n) + (sig.empty() ? "" : ":" + pointer_escape(sig));
        }
        auto [it, fresh] = index.emplace(key, doc.units.size());
        if (fresh)
            doc.units.push_back({key, {}, {}, kind, {}});
        doc.units[it->second].segments.push_back(i);
    }
    for (auto& u : doc.units) {
        Hasher h;
        std::uint64_t total = 0;
        auto segs = u.segments;
        std::size_t skip = 0;
        if (u.key == "/icc") { // the profile, reassembled in sequence order (jpeg.md §6.5)
            skip = 14;
            std::stable_sort(segs.begin(), segs.end(), [&](std::size_t x, std::size_t y) {
                auto sx = doc.bytes(doc.segs[x]), sy = doc.bytes(doc.segs[y]);
                return (sx.size() > 12 ? std::to_integer<int>(sx[12]) : 0) <
                       (sy.size() > 12 ? std::to_integer<int>(sy[12]) : 0);
            });
        }
        for (auto i : segs) {
            auto d = doc.bytes(doc.segs[i]);
            d = d.subspan(std::min(skip, d.size()));
            h.raw(d), total += d.size();
        }
        u.value = h.digest();
        u.text = bytes_text(total, u.value);
        auto first = doc.bytes(doc.segs[u.segments[0]]);
        if (u.kind == "text")
            u.text = escape_text(str(first));
        else if (u.key == "/jfif" && first.size() >= 14) {
            static const char* units[] = {"no-units", "dpi", "dpcm"};
            const auto unit = std::to_integer<unsigned>(first[7]);
            u.text = "density=" + std::to_string(be16(first, 8)) + "x" + std::to_string(be16(first, 10)) +
                     " " + (unit < 3 ? units[unit] : "unit-" + std::to_string(unit)) + " " + u.text;
        }
        if (u.segments.size() > 1)
            u.text += " segments=" + std::to_string(u.segments.size());
    }
    if (doc.trailer < doc.src.size()) {
        auto t = doc.src.bytes().subspan(doc.trailer);
        auto h = hash128(t);
        doc.units.push_back({"/trailer", h, bytes_text(t.size(), h), "metadata", {}});
    }
}

// /shape and /coding/* gate the DCT-domain merge; /encoding/* are free choices (jpeg.md §3, §4).
void build_coding(JpegDoc& doc) {
    std::map<int, std::span<const std::byte>> quant; // slot -> Pq and values, last definition before SOS
    std::string dht;
    bool scanned = false;
    for (auto& s : doc.segs) {
        auto p = doc.bytes(s);
        if (s.marker == kSOS)
            scanned = true;
        else if (s.marker == kDHT)
            dht += str(p);
        else if (s.marker == kDQT && !scanned)
            for (std::size_t at = 0; at < p.size();) {
                const auto pq = std::to_integer<int>(p[at]) >> 4, tq = std::to_integer<int>(p[at]) & 15;
                const std::size_t len = 1 + 64 * std::size_t(pq ? 2 : 1);
                if (pq > 1 || tq > 3 || p.size() - at < len)
                    bad("bad DQT segment");
                quant[tq] = p.subspan(at, len);
                at += len;
            }
    }
    std::string ids, sampling;
    std::set<int> slots;
    for (auto& c : doc.comps) {
        ids += (ids.empty() ? "" : ",") + std::to_string(c.id);
        sampling += (sampling.empty() ? "" : ",") + std::to_string(c.h) + "x" + std::to_string(c.v);
        slots.insert(c.tq);
    }
    doc.coding = {{"/shape", std::to_string(doc.width) + "x" + std::to_string(doc.height)},
                  {"/coding/precision", std::to_string(doc.precision)},
                  {"/coding/components", ids},
                  {"/coding/sampling", sampling},
                  {"/coding/colorspace", doc.colorspace}};
    for (int slot : slots) {
        auto it = quant.find(slot);
        if (it == quant.end())
            bad("component uses undefined quantization table " + std::to_string(slot));
        doc.coding.emplace_back("/coding/quant/" + std::to_string(slot),
                                "xxh3=" + hex16(hash128(it->second)));
    }
    doc.encoding = {{"/encoding/process", process_name(doc.sof)},
                    {"/encoding/huffman", doc.arithmetic() ? "none" : "xxh3=" + hex16(hash128(dht))},
                    {"/encoding/restart", doc.restart ? std::to_string(doc.restart) : "none"}};
}

// The EnvelopeMap (jpeg.md §7.1, SPEC §12.3).
void build_map(JpegDoc& doc) {
    auto& r = doc.map.ranges;
    std::map<std::size_t, std::string> unit_of;
    for (auto& u : doc.units)
        for (auto i : u.segments)
            unit_of[i] = u.key;
    auto piece = [&](std::uint64_t off, std::uint64_t len, ByteClass cls, const std::string& unit,
                     const char* layer = "") {
        if (len)
            r.push_back({off, len, cls, unit, layer});
    };
    const auto b = doc.src.bytes();
    const char* coder = doc.arithmetic() ? "arithmetic" : "huffman";
    for (std::size_t i = 0; i < doc.segs.size(); ++i) {
        const auto& s = doc.segs[i];
        const std::string unit = unit_of.count(i)                           ? unit_of[i]
                                 : s.marker == kDQT || sof_marker(s.marker) ? "/coding"
                                 : standalone(s.marker)                     ? "/markers"
                                                                            : "/encoding";
        piece(s.start, s.offset - s.start, ByteClass::encoding, unit, "fill");
        piece(s.offset, standalone(s.marker) ? 2 : 4, ByteClass::framing, unit);
        if (standalone(s.marker))
            continue;
        if (s.metadata()) {
            const std::uint64_t head = unit == "/icc" ? std::min<std::uint64_t>(14, s.length) : 0;
            piece(s.payload(), head, ByteClass::framing, unit);
            piece(s.payload() + head, s.length - head, ByteClass::content, unit);
        } else {
            piece(s.payload(), s.length, unit == "/coding" ? ByteClass::content : ByteClass::encoding, unit,
                  unit == "/coding" ? "" : coder);
        }
        if (s.marker == kSOS) { // entropy-coded data, restart markers are framing
            std::uint64_t from = s.payload() + s.length;
            for (std::uint64_t k = from; k + 1 < s.end; ++k)
                if (b[k] == std::byte{0xFF} && std::to_integer<int>(b[k + 1]) >= 0xD0 &&
                    std::to_integer<int>(b[k + 1]) <= 0xD7) {
                    piece(from, k - from, ByteClass::encoding, "/pixels", coder);
                    piece(k, 2, ByteClass::framing, "/pixels");
                    from = k + 2, ++k;
                }
            piece(from, s.end - from, ByteClass::encoding, "/pixels", coder);
        }
    }
    piece(doc.trailer, doc.src.size() - doc.trailer, ByteClass::content, "/trailer");
    doc.map.validate(doc.src.size());
}

// `strict`: decoder warnings are errors (validation of our own output, jpeg.md §8).
std::unique_ptr<JpegDoc> load_doc(ByteSource src, Limits& budget, bool strict = false) {
    auto doc = std::make_unique<JpegDoc>();
    doc->src = std::move(src);
    std::tie(doc->segs, doc->trailer) = parse_segments(doc->src.bytes());
    const Segment* sof = nullptr;
    for (auto& s : doc->segs) {
        if (sof_marker(s.marker)) {
            if (sof)
                bad("more than one SOF segment");
            sof = &s;
        } else if (s.marker == kSOS && !sof) {
            bad("SOS before SOF");
        } else if (s.marker == kDRI && s.length >= 2 && !doc->restart) {
            doc->restart = be16(doc->bytes(s), 0);
        }
    }
    if (!sof)
        bad("no SOF segment");
    doc->sof = sof->marker;
    if (doc->sof == 0xC3 || doc->sof == 0xCB)
        unsupported("lossless JPEG (SOF3) is not supported yet");
    if ((doc->sof >= 0xC5 && doc->sof <= 0xC7) || doc->sof >= 0xCD)
        unsupported("hierarchical JPEG is not supported");
    auto p = doc->bytes(*sof);
    if (p.size() < 6)
        bad("short SOF segment");
    doc->precision = std::to_integer<int>(p[0]);
    doc->height = be16(p, 1), doc->width = be16(p, 3);
    const std::size_t nf = std::to_integer<std::size_t>(p[5]);
    if (nf < 1 || nf > 4 || p.size() < 6 + 3 * nf)
        bad("bad SOF component count");
    if (!doc->height)
        unsupported("height defined by a DNL segment");
    if (!doc->width)
        bad("zero image width");
    if (doc->precision != 8 && doc->precision != 12)
        unsupported(std::to_string(doc->precision) + "-bit DCT data");
    int hmax = 1, vmax = 1;
    for (std::size_t c = 0; c < nf; ++c) {
        const auto hv = std::to_integer<int>(p[7 + 3 * c]);
        Component k{std::to_integer<int>(p[6 + 3 * c]), hv >> 4, hv & 15, std::to_integer<int>(p[8 + 3 * c])};
        if (k.h < 1 || k.h > 4 || k.v < 1 || k.v > 4 || k.tq > 3)
            bad("bad SOF sampling factors");
        hmax = std::max(hmax, k.h), vmax = std::max(vmax, k.v);
        doc->comps.push_back(k);
    }
    doc->mcu_w = 8u * unsigned(hmax), doc->mcu_h = 8u * unsigned(vmax);
    doc->mcus_x = (doc->width + doc->mcu_w - 1) / doc->mcu_w;
    doc->mcus_y = (doc->height + doc->mcu_h - 1) / doc->mcu_h;
    if (std::uint64_t{doc->width} * doc->height > budget.max_pixels)
        throw Error(ErrorCategory::limit, "JPEG: " + std::to_string(doc->width) + "x" +
                                              std::to_string(doc->height) + " exceeds --max-pixels");
    std::uint64_t coef_bytes = 0; // charged before libjpeg-turbo allocates (jpeg.md §5 cost)
    for (auto& c : doc->comps)
        coef_bytes += doc->mcus() * unsigned(c.h * c.v) * 128;
    budget.charge(coef_bytes);
    read_coefficients(*doc);
    if (strict && !doc->warnings.empty())
        throw Error(ErrorCategory::invalid_input, "JPEG: " + doc->warnings[0].message);
    build_coding(*doc);
    build_units(*doc);
    build_map(*doc);
    return doc;
}

// ---- Coefficient grid (jpeg.md §5, §6.1) ----

// Calls fn(component, bx, by) for every coded block of MCU i; the dummy blocks of edge MCUs are skipped.
template <class F>
void for_blocks(const JpegDoc& g, const std::vector<Plane>& planes, std::uint64_t i, F&& fn) {
    const auto mx = static_cast<std::uint32_t>(i % g.mcus_x), my = static_cast<std::uint32_t>(i / g.mcus_x);
    for (std::size_t c = 0; c < g.comps.size(); ++c) {
        const auto h = static_cast<std::uint32_t>(g.comps[c].h), v = static_cast<std::uint32_t>(g.comps[c].v);
        for (auto by = my * v; by < std::min((my + 1) * v, planes[c].real_h); ++by)
            for (auto bx = mx * h; bx < std::min((mx + 1) * h, planes[c].real_w); ++bx)
                fn(c, bx, by);
    }
}

bool mcu_equal(const JpegDoc& g, const std::vector<Plane>& a, const std::vector<Plane>& b, std::uint64_t i) {
    bool same = true;
    for_blocks(g, a, i, [&](std::size_t c, std::uint32_t bx, std::uint32_t by) {
        same = same && std::memcmp(a[c].block(bx, by), b[c].block(bx, by), 128) == 0;
    });
    return same;
}

// Equal coefficient grids: same layout and every coded block equal.
bool planes_equal(const JpegDoc& g, const std::vector<Plane>& a, const std::vector<Plane>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t c = 0; c < a.size(); ++c)
        if (a[c].bw != b[c].bw || a[c].bh != b[c].bh || a[c].real_w != b[c].real_w ||
            a[c].real_h != b[c].real_h)
            return false;
    for (std::uint64_t i = 0; i < g.mcus(); ++i)
        if (!mcu_equal(g, a, b, i))
            return false;
    return true;
}

// Changed MCU regions of b against a (equal coding), 8-connected, `gap` applied (grid_merge's labeling).
std::vector<GridRegion> mcu_regions(const JpegDoc& g, const std::vector<Plane>& a,
                                    const std::vector<Plane>& b, unsigned gap, Limits& budget) {
    const GridShape s{g.mcus_x, g.mcus_y};
    auto gr = grid_merge(s, s, s,
                         [&](Side x, Side y, std::uint64_t i) {
                             return mcu_equal(g, x == Side::theirs ? b : a, y == Side::theirs ? b : a, i);
                         },
                         {gap, 0, &budget});
    return std::move(gr.theirs);
}

Rect pixel_rect(const JpegDoc& g, const Rect& m) {
    const std::uint64_t x = m.x * g.mcu_w, y = m.y * g.mcu_h;
    return {x, y, std::min<std::uint64_t>(g.width, (m.x + m.w) * g.mcu_w) - x,
            std::min<std::uint64_t>(g.height, (m.y + m.h) * g.mcu_h) - y};
}

// The pixels of a region's MCUs, for pixel metrics and visuals.
// ponytail: one cell index per pixel; per-MCU iteration if huge regions must stay within --max-memory.
GridRegion to_pixels(const JpegDoc& g, const GridRegion& r) {
    GridRegion out{pixel_rect(g, std::get<Rect>(r.bounds)), {}};
    for (auto i : r.cells) {
        const Rect px = pixel_rect(g, {i % g.mcus_x, i / g.mcus_x, 1, 1});
        for (auto y = px.y; y < px.y + px.h; ++y)
            for (auto x = px.x; x < px.x + px.w; ++x)
                out.cells.push_back(y * g.width + x);
    }
    std::sort(out.cells.begin(), out.cells.end());
    return out;
}

Change region_change(const JpegDoc& g, const std::vector<Plane>& a, const std::vector<Plane>& b,
                     const GridRegion& r, Side side) {
    const Rect m = std::get<Rect>(r.bounds);
    auto c = make_change(side, Op::update, {"/pixels", pixel_rect(g, m)}, "pixels");
    Hasher ha, hb;
    std::vector<std::uint64_t> blocks(g.comps.size());
    std::uint64_t l1 = 0;
    for (auto i : r.cells)
        for_blocks(g, a, i, [&](std::size_t k, std::uint32_t bx, std::uint32_t by) {
            const auto *x = a[k].block(bx, by), *y = b[k].block(bx, by);
            ha.raw({reinterpret_cast<const std::byte*>(x), 128}),
                hb.raw({reinterpret_cast<const std::byte*>(y), 128});
            if (std::memcmp(x, y, 128) != 0) {
                ++blocks[k];
                for (int n = 0; n < 64; ++n)
                    l1 += static_cast<std::uint64_t>(std::abs(int{x[n]} - int{y[n]}));
            }
        });
    c.before.hash = ha.digest(), c.after.hash = hb.digest();
    c.depends_on = {"/icc"};
    c.metrics["mcus"] = r.cells.size();
    c.metrics["mcu_rect"] = {m.x, m.y, m.w, m.h};
    c.metrics["blocks_changed"] = blocks;
    c.metrics["coef_l1"] = l1;
    return c;
}

// max_delta, mean_delta, delta_e00 over the pixels of the region's MCUs (jpeg.md §5 step 3).
void pixel_metrics(const JpegDoc& g, const Raster& ra, const Raster& rb, const GridRegion& r, Change& c) {
    auto m = raster::region_metrics(ra, rb, to_pixels(g, r));
    for (auto& [k, v] : m.items())
        if (k != "pixels" && k != "bbox" && k != "alpha_changed")
            c.metrics[k] = v;
}

// ---- Options ----

struct Opts {
    unsigned gap = 0, guard = 0, tolerance = 0;
    bool strip_signatures = false;
};

unsigned number(const OptionValues& o, const char* name) {
    auto it = o.find(name);
    if (it == o.end())
        return 0;
    unsigned v = 0;
    auto [p, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    if (ec != std::errc{} || p != it->second.data() + it->second.size())
        throw Error(ErrorCategory::unsupported, std::string("JPEG: --") + name + "=" + it->second);
    return v;
}

Opts options(const OptionValues& o) {
    Opts r;
    r.gap = number(o, "gap"), r.guard = number(o, "guard"), r.tolerance = number(o, "tolerance");
    auto it = o.find("signatures");
    r.strip_signatures = it != o.end() && it->second == "strip";
    return r;
}

// ---- Diff ----

using KeyList = std::vector<std::pair<std::string, Hash128>>;

KeyList key_list(const JpegDoc& d) {
    KeyList v;
    for (auto& u : d.units)
        v.emplace_back(u.key, u.value);
    return v;
}

KeyList sorted(KeyList v) {
    std::sort(v.begin(), v.end());
    return v;
}

const Unit* unit(const JpegDoc& d, std::string_view key) {
    for (auto& u : d.units)
        if (u.key == key)
            return &u;
    return nullptr;
}

// Shape, coding, encoding, and metadata changes of b against a.
void meta_changes(const JpegDoc& a, const JpegDoc& b, Side side, ChangeSet& out) {
    using Fields = std::vector<std::pair<std::string, std::string>>;
    auto fields = [&](const Fields& fa, const Fields& fb) {
        std::map<std::string, std::string> ma(fa.begin(), fa.end()), mb(fb.begin(), fb.end());
        auto kind = [](const std::string& k) {
            return k == "/shape" ? "shape" : k.starts_with("/coding/") ? "coding" : "encoding";
        };
        for (auto& [k, v] : ma) {
            auto f = mb.find(k);
            if (f != mb.end() && f->second == v)
                continue;
            auto c = make_change(side, f == mb.end() ? Op::erase : Op::update, {k}, kind(k));
            c.before.text = v;
            if (f != mb.end())
                c.after.text = f->second;
            out.push_back(std::move(c));
        }
        for (auto& [k, v] : mb)
            if (!ma.count(k)) {
                auto c = make_change(side, Op::insert, {k}, kind(k));
                c.after.text = v;
                out.push_back(std::move(c));
            }
    };
    fields(a.coding, b.coding);
    fields(a.encoding, b.encoding);
    for (auto& u : a.units) {
        auto* v = unit(b, u.key);
        if (!v) {
            auto c = make_change(side, Op::erase, {u.key}, u.kind);
            c.before = {u.text, u.value};
            out.push_back(std::move(c));
        } else if (v->value != u.value) {
            auto c = make_change(side, Op::update, {u.key}, u.kind);
            c.before = {u.text, u.value}, c.after = {v->text, v->value};
            out.push_back(std::move(c));
        }
    }
    for (auto& u : b.units)
        if (!unit(a, u.key)) {
            auto c = make_change(side, Op::insert, {u.key}, u.kind);
            c.after = {u.text, u.value};
            out.push_back(std::move(c));
        }
}

ChangeSet diff_docs(const JpegDoc& a, const JpegDoc& b, const Opts& opt, Limits& budget) {
    ChangeSet out;
    meta_changes(a, b, Side::theirs, out);
    if (a.coding == b.coding) { // DCT domain (jpeg.md §5 step 2)
        if (opt.tolerance)
            unsupported(
                "--tolerance applies only when the coding parameters differ (DCT-mode tolerance is v1.1)");
        auto regions = mcu_regions(a, a.planes, b.planes, opt.gap, budget);
        std::optional<Raster> ra, rb;
        if (!regions.empty())
            ra = decode_pixels(a, budget), rb = decode_pixels(b, budget);
        for (auto& r : regions) {
            out.push_back(region_change(a, a.planes, b.planes, r, Side::theirs));
            if (ra && rb)
                pixel_metrics(a, *ra, *rb, r, out.back());
        }
    } else if (a.width == b.width && a.height == b.height) { // pixel mode (jpeg.md §5 step 4)
        auto ra = decode_pixels(a, budget), rb = decode_pixels(b, budget);
        bool requantized = false;
        for (auto& [k, v] : a.coding)
            requantized |= k.starts_with("/coding/quant/") &&
                           std::find(b.coding.begin(), b.coding.end(), std::pair{k, v}) == b.coding.end();
        if (ra && rb) {
            std::array<Raster*, 2> both{&*ra, &*rb};
            raster::unify(both);
            const raster::CellCompare cmp{static_cast<std::uint16_t>(std::min(65535u, opt.tolerance * 257)),
                                          false};
            for (auto& r : raster::diff_regions(*ra, *rb, cmp, opt.gap, &budget)) {
                auto c = make_change(Side::theirs, Op::update, {"/pixels", r.bounds}, "pixels");
                c.before.hash = raster::hash_cells(*ra, r.cells),
                c.after.hash = raster::hash_cells(*rb, r.cells);
                c.depends_on = {"/icc"};
                c.metrics = raster::region_metrics(*ra, *rb, r);
                if (requantized)
                    c.metrics["requantized"] = true;
                out.push_back(std::move(c));
            }
        } else { // no RGB view: the whole image, unmeasured
            auto c =
                make_change(Side::theirs, Op::update, {"/pixels", Rect{0, 0, a.width, a.height}}, "pixels");
            c.metrics["decoded"] = false;
            out.push_back(std::move(c));
        }
    }
    sort_canonical(out);
    return out;
}

// ---- Writing (jpeg.md §7) ----

// Exif with an IFD1 (its thumbnail), from the TIFF header after "Exif\0\0".
bool exif_has_thumbnail(std::span<const std::byte> d) {
    if (d.size() < 14)
        return false;
    auto t = d.subspan(6);
    const bool le = t[0] == std::byte{'I'};
    auto u16 = [&](std::size_t at) {
        return le ? static_cast<std::uint32_t>(std::to_integer<unsigned>(t[at]) |
                                               std::to_integer<unsigned>(t[at + 1]) << 8)
                  : static_cast<std::uint32_t>(be16(t, at));
    };
    auto u32 = [&](std::size_t at) { return le ? u16(at) | u16(at + 2) << 16 : u16(at) << 16 | u16(at + 2); };
    const std::uint64_t ifd0 = u32(4);
    if (ifd0 + 2 > t.size())
        return false;
    const std::uint64_t next = ifd0 + 2 + 12ull * u16(static_cast<std::size_t>(ifd0));
    return next + 4 <= t.size() && u32(static_cast<std::size_t>(next)) != 0;
}

bool is_c2pa(const JpegDoc& d, const Unit& u) {
    if (!u.key.starts_with("/app/APP11"))
        return false;
    for (auto i : u.segments)
        if (str(d.bytes(d.segs[i])).find("c2pa") != std::string::npos)
            return true;
    return false;
}

struct Plan {
    std::array<const JpegDoc*, 3> docs{};           // by Side; ours gives the layout
    std::optional<Side> image;                      // the output's coefficients are this side's
    const std::vector<Side>* take = nullptr;        // or per MCU (equal coding in all three)
    std::optional<std::vector<Plane>> planes;       // or these (edit_mcus)
    std::vector<std::pair<std::string, Side>> keys; // output order; side whose unit is written
};

const JpegDoc& at(const Plan& p, Side s) {
    return *p.docs[static_cast<std::size_t>(s)];
}

// SOI, ours' segment order with each metadata unit from the side that wins it, the image segments
// (tables, SOF, scans) from `img`, EOI, and the trailer. Everything is copied raw.
std::vector<std::byte> assemble(const Plan& p, const ByteSource& img, const std::vector<Segment>& img_segs,
                                bool img_is_ours) {
    const JpegDoc& lay = at(p, Side::ours);
    std::map<std::string, Side> take;
    std::map<std::string, std::vector<std::string>> after; // keys ours lacks, after the ours key they follow
    std::string anchor;
    for (auto& [k, s] : p.keys) {
        if (k == "/trailer")
            continue;
        take[k] = s;
        if (unit(lay, k))
            anchor = k;
        else
            after[anchor].push_back(k);
    }
    std::map<std::size_t, const Unit*> owner;
    for (auto& u : lay.units)
        for (auto i : u.segments)
            owner[i] = &u;

    std::vector<emit::Seg> out{emit::raw(lay.src, 0, 2)};
    auto seg = [&](const ByteSource& src, const Segment& s) {
        out.push_back(emit::raw(src, s.start, s.end - s.start));
    };
    auto add = [&](const std::string& key) {
        const JpegDoc& d = at(p, take.at(key));
        for (auto i : unit(d, key)->segments)
            seg(d.src, d.segs[i]);
    };
    for (auto& k : after[""])
        add(k);
    bool image_done = false;
    for (std::size_t i = 1; i < lay.segs.size(); ++i) {
        const Segment& s = lay.segs[i];
        if (s.marker == kEOI) {
            seg(lay.src, s);
        } else if (s.metadata()) {
            const Unit* u = owner.at(i);
            if (i != u->segments[0])
                continue;
            if (take.count(u->key))
                add(u->key);
            for (auto& k : after[u->key])
                add(k);
        } else if (img_is_ours) {
            seg(lay.src, s);
        } else if (!image_done) {
            image_done = true;
            for (auto& x : img_segs)
                if (!x.metadata() && x.marker != kSOI && x.marker != kEOI)
                    seg(img, x);
        }
    }
    for (auto& [k, s] : p.keys)
        if (k == "/trailer") {
            const JpegDoc& d = at(p, s);
            out.push_back(emit::raw(d.src, d.trailer, d.src.size() - d.trailer));
        }
    return emit::emit(emit::group(1, std::move(out)));
}

// Writes the plan, reloads the bytes, and checks that they hold the planned document (jpeg.md §8).
std::unique_ptr<JpegDoc> write_plan(const Plan& p, std::vector<Warning>& warnings, Limits& budget) {
    const JpegDoc& lay = at(p, Side::ours);
    std::vector<Plane> merged;
    const std::vector<Plane>* expect = nullptr;
    const JpegDoc* geom = &lay; // coding of the output
    if (p.image) {
        geom = &at(p, *p.image);
        expect = &geom->planes;
    } else if (p.planes) {
        expect = &*p.planes;
    } else {
        for (auto& pl : lay.planes)
            budget.charge(pl.coef.size() * 2);
        merged = lay.planes;
        for (std::uint64_t i = 0; i < lay.mcus(); ++i) {
            const Side s = (*p.take)[i];
            if (s != Side::ours)
                for_blocks(lay, merged, i, [&](std::size_t c, std::uint32_t bx, std::uint32_t by) {
                    std::memcpy(merged[c].block(bx, by), at(p, s).planes[c].block(bx, by), 128);
                });
        }
        expect = &merged;
    }
    KeyList chosen;
    for (auto& [k, s] : p.keys)
        chosen.emplace_back(k, unit(at(p, s), k)->value);
    const KeyList expected = sorted(chosen);

    // The inputs that already hold the merged coefficients: passthrough, else their image segments.
    std::vector<Side> holders;
    for (Side s : {Side::ours, Side::theirs, Side::base})
        if (at(p, s).coding == geom->coding && planes_equal(*geom, at(p, s).planes, *expect))
            holders.push_back(s);
    for (Side s : holders)
        if (sorted(key_list(at(p, s))) == expected) {
            Limits fresh = budget.fresh();
            return load_doc(at(p, s).src, fresh);
        }

    struct Attempt {
        bool script, tables;
    };
    std::vector<Attempt> attempts{{true, true}};
    if (holders.empty()) { // re-encode the coefficients losslessly (jpeg.md §7 item 3)
        const bool reuse = tables_reusable(lay);
        attempts = {{true, reuse}};
        if (reuse)
            attempts.push_back({true, false});
        attempts.push_back({false, false});
    }
    std::string last_error;
    for (auto a : attempts) {
        ByteSource img;
        std::vector<Segment> img_segs;
        if (!holders.empty()) {
            img = at(p, holders[0]).src, img_segs = at(p, holders[0]).segs;
        } else {
            auto enc = write_coefficients(lay, *expect, a.script, a.tables, last_error);
            if (!enc)
                continue;
            img = ByteSource::from_memory(std::move(*enc));
            img_segs = parse_segments(img.bytes()).first;
        }
        auto bytes = assemble(p, img, img_segs, !holders.empty() && holders[0] == Side::ours);
        Limits fresh = budget.fresh(); // the result is a new document (#51)
        std::unique_ptr<JpegDoc> out;
        try {
            out = load_doc(ByteSource::from_memory(std::move(bytes), "merged.jpg"), fresh, true);
        } catch (const Error& e) {
            last_error = e.what();
            continue;
        }
        if (out->coding != geom->coding || !planes_equal(*out, out->planes, *expect) ||
            sorted(key_list(*out)) != expected) {
            last_error = "the output does not load to the merged document";
            continue;
        }
        if (holders.empty()) {
            const bool ours_tables = !lay.arithmetic() && !lay.progressive() && a.tables;
            if (!ours_tables && !lay.arithmetic())
                warnings.push_back({"encoding-reencoded", "/pixels",
                                    "scan data re-encoded losslessly with optimized Huffman tables"});
            if (!a.script)
                warnings.push_back(
                    {"scan-script-changed", "/pixels",
                     "ours' scan script could not be reproduced; libjpeg-turbo's default is used"});
        }
        return out;
    }
    throw Error(ErrorCategory::internal, "JPEG writer: " + last_error);
}

// Stale derived data and invalidated signatures in the written output (jpeg.md §7).
void output_warnings(const Plan& p, const JpegDoc& out, std::vector<Warning>& warnings) {
    for (auto& [k, s] : p.keys) {
        const JpegDoc& d = at(p, s);
        const Unit* u = unit(d, k);
        const bool image_kept = d.coding == out.coding && planes_equal(d, d.planes, out.planes);
        auto first = u->segments.empty() ? std::span<const std::byte>{} : d.bytes(d.segs[u->segments[0]]);
        const bool thumb =
            (k == "/exif" && exif_has_thumbnail(first)) ||
            (k == "/jfif" && first.size() >= 14 && first[12] != std::byte{0} && first[13] != std::byte{0}) ||
            k == "/jfxx" || k == "/mpf";
        if (thumb && !image_kept)
            warnings.push_back({"derived-stale", k,
                                k == "/mpf"
                                    ? "MPF image offsets and the images after EOI were not updated (v1.1)"
                                    : "the embedded thumbnail shows the pre-merge image"});
        if (is_c2pa(d, *u) && (out.src.size() != d.src.size() ||
                               std::memcmp(out.src.bytes().data(), d.src.bytes().data(), d.src.size()) != 0))
            warnings.push_back({"signature-invalidated", k, "the C2PA manifest no longer matches the file"});
    }
}

std::vector<std::byte> write_png(const Raster& r) {
    return png::write(r, png::Encoding{2, static_cast<std::uint8_t>(r.wide ? 16 : 8), 0});
}

class JpegFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{"jpeg",
                                  "JPEG (merged in whole MCUs in the DCT domain, no recompression)",
                                  {".jpg", ".jpeg", ".jpe", ".jfif"},
                                  {true, true, false, true, true},
                                  {}};
        return i;
    }

    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        auto at = [&](std::size_t i) { return std::to_integer<std::uint8_t>(head[i]); };
        if (head.size() < 3 || at(0) != 0xFF || at(1) != kSOI || at(2) != 0xFF)
            return {0};
        for (std::size_t pos = 2; pos + 4 <= head.size();) { // a SOFn within the probe window: 100
            if (at(pos) != 0xFF)
                return {60};
            while (pos + 1 < head.size() && at(pos + 1) == 0xFF)
                ++pos;
            if (pos + 4 > head.size())
                break;
            const auto m = at(pos + 1);
            if (sof_marker(m))
                return {100};
            if (m == kSOS || m == kEOI || m == 0x00)
                return {60};
            pos += standalone(m) ? 2 : 2 + be16(head, pos + 2);
        }
        return {60};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return load_doc(std::move(src), o.ctx.budget);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        return diff_docs(doc(a), doc(b), options(o.ctx.format_options), o.ctx.budget);
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& o) const override;

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions&) const override {
        auto& j = doc(d);
        std::string sampling;
        for (auto& [k, v] : j.coding)
            if (k == "/coding/sampling")
                sampling = v;
        os << "jpeg " << j.width << "x" << j.height << " sof=" << process_name(j.sof)
           << " precision=" << j.precision << " colorspace=" << j.colorspace << " sampling=" << sampling
           << "\n";
        for (auto& [k, v] : j.coding)
            if (k.starts_with("/coding/quant/"))
                os << "quant " << k.substr(14) << " " << v << "\n";
        std::vector<const Unit*> us;
        for (auto& u : j.units)
            us.push_back(&u);
        std::sort(us.begin(), us.end(), [](const Unit* x, const Unit* y) { return x->key < y->key; });
        for (auto* u : us)
            os << "app " << u->key.substr(u->key.starts_with("/app/") ? 5 : 1) << " " << u->text << "\n";
        // Tiles of whole MCUs, hashed over coefficients: at least 64 pixels, doubled to <= 4096 tiles.
        std::uint32_t tx = std::max(1u, 64 / j.mcu_w), ty = std::max(1u, 64 / j.mcu_h);
        while (std::uint64_t{(j.mcus_x + tx - 1) / tx} * ((j.mcus_y + ty - 1) / ty) > 4096)
            tx *= 2, ty *= 2;
        for (std::uint32_t y0 = 0; y0 < j.mcus_y; y0 += ty)
            for (std::uint32_t x0 = 0; x0 < j.mcus_x; x0 += tx) {
                Hasher h;
                for (auto y = y0; y < std::min(j.mcus_y, y0 + ty); ++y)
                    for (auto x = x0; x < std::min(j.mcus_x, x0 + tx); ++x)
                        for_blocks(
                            j, j.planes, std::uint64_t{y} * j.mcus_x + x,
                            [&](std::size_t c, std::uint32_t bx, std::uint32_t by) {
                                h.raw({reinterpret_cast<const std::byte*>(j.planes[c].block(bx, by)), 128});
                            });
                const Rect px =
                    pixel_rect(j, {x0, y0, std::min(tx, j.mcus_x - x0), std::min(ty, j.mcus_y - y0)});
                os << "mcutile " << px.x << "," << px.y << "," << px.w << "," << px.h
                   << " xxh3=" << hex16(h.digest()) << "\n";
            }
    }

    const EnvelopeMap* envelope(const Document& d) const override { return &doc(d).map; }

    std::vector<std::byte> visual(const Document& a, const Document& b, const DiffOptions& o) const override {
        auto& ja = doc(a);
        auto& jb = doc(b);
        auto ra = decode_pixels(ja, o.ctx.budget), rb = decode_pixels(jb, o.ctx.budget);
        if (!ra || !rb)
            unsupported("no RGB view of this color space for --visual");
        std::array<Raster*, 2> both{&*ra, &*rb};
        raster::unify(both);
        if (ja.width != jb.width || ja.height != jb.height)
            return write_png(raster::heatmap(*rb, *rb, {}));
        const Opts opt = options(o.ctx.format_options);
        std::vector<GridRegion> regions;
        if (ja.coding == jb.coding)
            for (auto& r : mcu_regions(ja, ja.planes, jb.planes, opt.gap, o.ctx.budget))
                regions.push_back(to_pixels(ja, r));
        else
            regions = raster::diff_regions(*ra, *rb, {}, opt.gap, &o.ctx.budget);
        return write_png(raster::heatmap(*ra, *rb, regions));
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& j = doc(d);
        nlohmann::ordered_json r;
        r["width"] = j.width, r["height"] = j.height, r["process"] = process_name(j.sof);
        r["precision"] = j.precision, r["colorspace"] = j.colorspace;
        r["mcu"] = {j.mcu_w, j.mcu_h};
        r["restart"] = j.restart;
        r["integrity"] = "ok"; // no checksums in JPEG
        auto& segs = r["segments"] = nlohmann::ordered_json::array();
        for (auto& s : j.segs) {
            nlohmann::ordered_json e{
                {"offset", s.offset}, {"marker", marker_name(s.marker)}, {"length", s.length}};
            if (s.marker == kSOS)
                e["scan_bytes"] = s.end - s.payload() - s.length;
            segs.push_back(std::move(e));
        }
        auto& units = r["units"] = nlohmann::ordered_json::array();
        for (auto& [k, v] : j.coding)
            units.push_back({{"at", k}, {"kind", "coding"}, {"value", v}});
        for (auto& [k, v] : j.encoding)
            units.push_back({{"at", k}, {"kind", "encoding"}, {"value", v}});
        for (auto& u : j.units)
            units.push_back({{"at", u.key}, {"kind", u.kind}, {"value", u.text}});
        auto& warn = r["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : j.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return r;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json j;
        std::uint64_t regions = 0, mcus = 0;
        for (auto& c : cs)
            if (c.kind == "pixels")
                ++regions, mcus += c.metrics.value("mcus", std::uint64_t{0});
        j["width"] = {doc(a).width, doc(b).width};
        j["height"] = {doc(a).height, doc(b).height};
        j["mode"] = doc(a).coding == doc(b).coding ? "dct" : "pixels";
        j["regions"] = regions;
        j["mcus_changed"] = mcus;
        return j;
    }

private:
    static const JpegDoc& doc(const Document& d) {
        auto* p = dynamic_cast<const JpegDoc*>(&d);
        if (!p)
            throw Error(ErrorCategory::internal, "jpeg: not a JPEG document");
        return *p;
    }
};

// ---- Merge (jpeg.md §6) ----

MergeResult JpegFormat::merge(const Document& base_d, const Document& ours_d, const Document& theirs_d,
                              const MergeOptions& mo) const {
    const JpegDoc& b = doc(base_d);
    const JpegDoc& o = doc(ours_d);
    const JpegDoc& t = doc(theirs_d);
    const Opts opt = options(mo.ctx.format_options);
    if (opt.tolerance)
        unsupported("--tolerance in a JPEG merge is v1.1 (jpeg.md §6.2)");
    Limits& budget = mo.ctx.budget;
    MergeResult res;
    for (auto [d, name] : {std::pair{&b, "base"}, std::pair{&o, "ours"}, std::pair{&t, "theirs"}})
        for (auto w : d->warnings) {
            w.message = std::string(name) + ": " + w.message;
            res.warnings.push_back(std::move(w));
        }
    const std::array<const JpegDoc*, 3> docs{&b, &o, &t};
    auto D = [&](Side s) -> const JpegDoc& { return *docs[static_cast<std::size_t>(s)]; };
    auto& co = res.changes_ours;
    auto& ct = res.changes_theirs;
    meta_changes(b, o, Side::ours, co);
    meta_changes(b, t, Side::theirs, ct);

    struct Pending {
        enum Kind { grid, image, key, dependency } kind;
        std::size_t index = 0;
    };
    std::vector<Pending> pend;
    auto add_conflict = [&](Address at, std::vector<std::string> ids, std::string reason, Pending p) {
        ids.erase(std::remove(ids.begin(), ids.end(), std::string{}), ids.end());
        res.conflicts.push_back(make_conflict(std::move(at), std::move(ids), std::move(reason)));
        pend.push_back(p);
    };
    auto change_at = [](const ChangeSet& cs, std::string_view path) -> const Change* {
        for (auto& c : cs)
            if (c.at.path == path && !c.at.region)
                return &c;
        return nullptr;
    };

    // Pixels: GridMerge over MCUs when the coding matches in all three (jpeg.md §6.1), else a whole side
    // or conflict encoding-mismatch / shape-mismatch (§6.3).
    const bool grid = b.coding == o.coding && b.coding == t.coding;
    GridResult gr;
    std::optional<Side> whole;
    std::map<std::string, std::pair<Side, std::size_t>> region_of; // pixel change id -> (side, region)
    if (grid) {
        const GridShape s{b.mcus_x, b.mcus_y};
        gr = grid_merge(
            s, s, s,
            [&](Side x, Side y, std::uint64_t i) { return mcu_equal(b, D(x).planes, D(y).planes, i); },
            {opt.gap, opt.guard, &budget});
        std::vector<std::string> ids_o, ids_t;
        for (std::size_t k = 0; k < gr.ours.size(); ++k) {
            co.push_back(region_change(b, b.planes, o.planes, gr.ours[k], Side::ours));
            ids_o.push_back(co.back().id), region_of[co.back().id] = {Side::ours, k};
        }
        for (std::size_t k = 0; k < gr.theirs.size(); ++k) {
            ct.push_back(region_change(b, b.planes, t.planes, gr.theirs[k], Side::theirs));
            ids_t.push_back(ct.back().id), region_of[ct.back().id] = {Side::theirs, k};
        }
        std::set<std::string> conflicted;
        for (std::size_t k = 0; k < gr.conflicts.size(); ++k) {
            auto& g = gr.conflicts[k];
            std::vector<std::string> ids;
            for (auto r : g.ours)
                ids.push_back(ids_o[r]);
            for (auto r : g.theirs)
                ids.push_back(ids_t[r]);
            conflicted.insert(ids.begin(), ids.end());
            add_conflict({"/pixels", pixel_rect(b, std::get<Rect>(g.bounds))}, ids, g.reason,
                         {Pending::grid, k});
        }
        auto note = [&](const std::vector<GridRegion>& regs, const std::vector<std::string>& ids) {
            for (std::size_t k = 0; k < regs.size(); ++k)
                if (!conflicted.count(ids[k])) {
                    bool same = std::all_of(regs[k].cells.begin(), regs[k].cells.end(), [&](std::uint64_t i) {
                        return mcu_equal(b, o.planes, t.planes, i);
                    });
                    res.auto_merged.push_back({ids[k], same ? "identical" : "independent"});
                }
        };
        note(gr.ours, ids_o), note(gr.theirs, ids_t);
        for (auto [r, k] : gr.adjacent) {
            auto ro = pixel_rect(b, std::get<Rect>(gr.ours[r].bounds)),
                 rt = pixel_rect(b, std::get<Rect>(gr.theirs[k].bounds));
            res.warnings.push_back({"adjacent-edits", Address{"/pixels", ro}.to_string(),
                                    "ours and theirs edit adjacent MCUs (" + to_string(Region{ro}) + ", " +
                                        to_string(Region{rt}) + "); --guard=1 makes this a conflict"});
        }
    } else {
        const bool oc = o.coding != b.coding, tc = t.coding != b.coding;
        for (Side s : {Side::ours, Side::theirs}) // pixel edits of a side that kept base's coding
            if (D(s).coding == b.coding) {
                auto& cs = s == Side::ours ? co : ct;
                for (auto& r : mcu_regions(b, b.planes, D(s).planes, opt.gap, budget))
                    cs.push_back(region_change(b, b.planes, D(s).planes, r, s));
            }
        auto painted = [](const ChangeSet& cs) {
            return std::any_of(cs.begin(), cs.end(), [](const Change& c) { return c.kind == "pixels"; });
        };
        if (oc && tc && o.coding == t.coding && planes_equal(o, o.planes, t.planes))
            whole = Side::ours;
        else if (oc && !tc && !painted(ct))
            whole = Side::ours;
        else if (tc && !oc && !painted(co))
            whole = Side::theirs;
        std::vector<std::string> ids;
        for (auto* cs : {&co, &ct})
            for (auto& c : *cs)
                if (c.kind == "pixels" || c.kind == "shape" || c.kind == "coding")
                    ids.push_back(c.id);
        if (whole) {
            for (auto& id : ids)
                res.auto_merged.push_back({id, oc && tc ? "identical" : "independent"});
        } else {
            const bool reshaped =
                o.width != b.width || o.height != b.height || t.width != b.width || t.height != b.height;
            add_conflict({"/pixels"}, ids, reshaped ? "shape-mismatch" : "encoding-mismatch",
                         {Pending::image, 0});
        }
    }

    // Metadata units (KeyedMerge over OpaqueLeaf values) and encoding choices (never conflict).
    auto lb = key_list(b), lo = key_list(o), lt = key_list(t);
    auto rows = keyed_merge<std::string, Hash128>(lb, lo, lt);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        auto& row = rows[k];
        auto* xo = change_at(co, row.key);
        auto* xt = change_at(ct, row.key);
        if (row.outcome.rule == MergeRule::conflict)
            add_conflict({row.key}, {xo ? xo->id : "", xt ? xt->id : ""}, row.outcome.reason,
                         {Pending::key, k});
        else if (row.outcome.rule != MergeRule::unchanged)
            for (auto* x : {xo, xt})
                if (x)
                    res.auto_merged.push_back(
                        {x->id, row.outcome.rule == MergeRule::identical ? "identical" : "independent"});
    }
    for (auto& [f, v] : b.encoding) {
        auto* xo = change_at(co, f);
        auto* xt = change_at(ct, f);
        if (xo && xt)
            res.auto_merged.push_back(
                {xo->id, xo->after.text == xt->after.text ? "identical" : "encoding-ours"});
        else if (xo || xt)
            res.auto_merged.push_back({(xo ? xo : xt)->id, "independent"});
    }

    // Pixels depend on the ICC profile: a one-sided profile change against pixel edits conflicts (jpeg.md
    // §3).
    const std::size_t before_deps = res.conflicts.size();
    check_dependencies(co, ct, res.auto_merged, res.conflicts, [](const Change& c, const Change& other) {
        return c.kind == "pixels" && path_within(other.at.path, "/icc");
    });
    for (std::size_t k = before_deps; k < res.conflicts.size(); ++k)
        pend.push_back({Pending::dependency, 0});
    {
        std::set<std::string> in_conflict;
        for (auto& c : res.conflicts)
            in_conflict.insert(c.changes.begin(), c.changes.end());
        std::erase_if(res.auto_merged, [&](const AutoMerge& a) {
            return in_conflict.count(a.change) && a.rule != "dependency-touched";
        });
    }

    apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);

    std::vector<std::optional<Side>> key_take(rows.size());
    for (std::size_t k = 0; k < rows.size(); ++k)
        key_take[k] = rows[k].outcome.take;
    auto choice = [](Side s) { return s == Side::base ? "base" : s == Side::ours ? "ours" : "theirs"; };
    for (std::size_t k = 0; k < res.conflicts.size(); ++k) {
        auto& c = res.conflicts[k];
        const Side s = c.resolved_as == "theirs" ? Side::theirs
                       : c.resolved_as == "base" ? Side::base
                                                 : Side::ours;
        auto& p = pend[k];
        if (p.kind == Pending::grid) {
            grid_resolve(gr, p.index, s);
        } else if (p.kind == Pending::image) {
            whole = s;
        } else if (p.kind == Pending::key) {
            auto& row = rows[p.index];
            key_take[p.index] =
                take_for(c.resolved_as, row.base.has_value(), row.ours.has_value(), row.theirs.has_value());
        } else { // dependency: {pixel change, profile change}; the losing side's part reverts to the winner's
            for (auto& id : c.changes) {
                if (auto f = region_of.find(id); f != region_of.end() && f->second.first != s) {
                    auto& regs = f->second.first == Side::ours ? gr.ours : gr.theirs;
                    for (auto cell : regs[f->second.second].cells)
                        gr.take[cell] = s;
                }
                for (auto* cs : {&co, &ct})
                    for (auto& ch : *cs)
                        if (ch.id == id && ch.kind == "color" && ch.side != s)
                            for (std::size_t r = 0; r < rows.size(); ++r)
                                if (rows[r].key == ch.at.path)
                                    key_take[r] =
                                        take_for(choice(s), rows[r].base.has_value(),
                                                 rows[r].ours.has_value(), rows[r].theirs.has_value());
            }
        }
    }

    Plan plan;
    plan.docs = docs;
    if (grid)
        plan.take = &gr.take;
    else
        plan.image = whole.value_or(Side::ours);
    for (std::size_t k = 0; k < rows.size(); ++k)
        if (key_take[k] &&
            !(opt.strip_signatures && is_c2pa(D(*key_take[k]), *unit(D(*key_take[k]), rows[k].key)))) {
            plan.keys.emplace_back(rows[k].key, *key_take[k]);
        } else if (key_take[k]) {
            res.warnings.push_back(
                {"signature-stripped", rows[k].key, "C2PA manifest removed (--signatures=strip)"});
        }
    auto out = write_plan(plan, res.warnings, budget);
    output_warnings(plan, *out, res.warnings);

    if (mo.visual == MergeOptions::Visual::always ||
        (mo.visual == MergeOptions::Visual::on_conflict && res.unresolved_conflicts())) {
        Limits vb = budget.fresh();
        auto rb = decode_pixels(b, vb);
        if (rb) {
            GridResult px; // the merge visual works on pixels
            if (grid) {
                for (auto& r : gr.ours)
                    px.ours.push_back(to_pixels(b, r));
                for (auto& r : gr.theirs)
                    px.theirs.push_back(to_pixels(b, r));
                for (auto c : gr.conflicts) {
                    c.bounds = pixel_rect(b, std::get<Rect>(c.bounds));
                    px.conflicts.push_back(std::move(c));
                }
            } else {
                px.shape_conflict = std::any_of(pend.begin(), pend.end(),
                                                [](const Pending& p) { return p.kind == Pending::image; });
            }
            res.visual = write_png(raster::merge_visual(*rb, px));
        }
    }
    sort_canonical(co), sort_canonical(ct);
    res.document = std::move(out);
    return res;
}

} // namespace

std::vector<std::byte> edit_mcus(std::span<const std::byte> file,
                                 std::span<const std::array<std::uint32_t, 2>> mcus, int delta) {
    Limits budget;
    auto d = load_doc(ByteSource::from_memory({file.begin(), file.end()}, "edit.jpg"), budget);
    Plan p;
    p.docs = {d.get(), d.get(), d.get()};
    p.planes = d->planes;
    for (auto [x, y] : mcus) {
        if (x >= d->mcus_x || y >= d->mcus_y)
            throw Error(ErrorCategory::invalid_input, "edit_mcus: MCU outside the image");
        for_blocks(*d, *p.planes, std::uint64_t{y} * d->mcus_x + x,
                   [&](std::size_t c, std::uint32_t bx, std::uint32_t by) {
                       auto* blk = p.planes->at(c).block(bx, by);
                       blk[0] = static_cast<std::int16_t>(blk[0] + delta);
                   });
    }
    for (auto& u : d->units)
        p.keys.emplace_back(u.key, Side::ours);
    std::vector<Warning> warnings;
    auto out = write_plan(p, warnings, budget);
    auto b = out->src.bytes();
    return {b.begin(), b.end()};
}

std::unique_ptr<Format> make_format() {
    return std::make_unique<JpegFormat>();
}

} // namespace semsplinter::jpeg
