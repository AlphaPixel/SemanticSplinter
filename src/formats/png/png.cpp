#include "internal.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/report.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace semsplinter::png {

namespace {

using raster::Raster;

std::uint32_t be32(std::span<const std::byte> d, std::size_t at) {
    return std::uint32_t(d[at]) << 24 | std::uint32_t(d[at + 1]) << 16 | std::uint32_t(d[at + 2]) << 8 |
           std::uint32_t(d[at + 3]);
}
std::uint16_t be16(std::span<const std::byte> d, std::size_t at) {
    return static_cast<std::uint16_t>(std::uint32_t(d[at]) << 8 | std::uint32_t(d[at + 1]));
}
std::string str(std::span<const std::byte> d) {
    return {reinterpret_cast<const char*>(d.data()), d.size()};
}
std::string hex16(const Hash128& h) {
    return h.hex().substr(0, 16);
}

[[noreturn]] void bad(const std::string& what) {
    throw Error(ErrorCategory::invalid_input, "PNG: " + what);
}

// Chunk classes (png.md §2): color interpretation, the PLTE..IDAT group, anywhere before IDAT.
const std::set<std::string, std::less<>> kColor{"cICP", "iCCP", "sRGB", "gAMA",
                                                "cHRM", "sBIT", "mDCV", "cLLI"};
const std::set<std::string, std::less<>> kAfterPlte{"tRNS", "bKGD", "hIST"};
const std::set<std::string, std::less<>> kBeforeIdat{"pHYs", "sPLT", "eXIf", "acTL",
                                                     "oFFs", "pCAL", "sCAL", "sTER"};
const std::set<std::string, std::less<>> kSingleton{"pHYs", "bKGD", "tIME", "eXIf", "oFFs", "pCAL", "sCAL",
                                                    "sTER", "caBX", "dSIG", "gIFg", "gIFx", "gIFt"};
const std::set<std::string, std::less<>> kAnimation{"acTL", "fcTL", "fdAT"};
const std::set<std::string, std::less<>> kCritical{"IHDR", "PLTE", "IDAT", "IEND"};

bool known(std::string_view t) {
    return kColor.count(t) || kAfterPlte.count(t) || kBeforeIdat.count(t) || kSingleton.count(t) ||
           kAnimation.count(t) || kCritical.count(t) || t == "tEXt" || t == "zTXt" || t == "iTXt";
}

std::string latin1(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | c >> 6);
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string bytes_text(std::span<const std::byte> d) {
    return "bytes=" + std::to_string(d.size()) + " xxh3=" + hex16(hash128(d));
}

constexpr std::uint64_t kTextLimit = 64ull << 20; // zTXt/iCCP/iTXt inflation cap (png.md §11)

// "keyword\0rest": the keyword (1..79 bytes) and the offset of rest.
std::pair<std::string, std::size_t> keyword(std::span<const std::byte> d) {
    auto s = str(d.first(std::min<std::size_t>(d.size(), 80)));
    auto nul = s.find('\0');
    if (nul == std::string::npos || nul == 0)
        throw Error(ErrorCategory::invalid_input, "bad keyword");
    return {s.substr(0, nul), nul + 1};
}

std::string render(const std::string& type, std::span<const std::byte> d) {
    char buf[160];
    if (type == "gAMA" && d.size() == 4)
        return std::to_string(be32(d, 0));
    if (type == "sRGB" && d.size() == 1) {
        static const char* intents[] = {"perceptual", "relative", "saturation", "absolute"};
        auto i = std::to_integer<unsigned>(d[0]);
        return std::string("intent=") + (i < 4 ? intents[i] : std::to_string(i).c_str());
    }
    if (type == "cHRM" && d.size() == 32) {
        std::string s;
        for (int k = 0; k < 8; ++k)
            s += (k ? " " : "") + std::to_string(be32(d, 4 * k));
        return s;
    }
    if (type == "cICP" && d.size() == 4) {
        std::snprintf(buf, sizeof buf, "primaries=%u transfer=%u matrix=%u full_range=%u",
                      std::to_integer<unsigned>(d[0]), std::to_integer<unsigned>(d[1]),
                      std::to_integer<unsigned>(d[2]), std::to_integer<unsigned>(d[3]));
        return buf;
    }
    if (type == "pHYs" && d.size() == 9)
        return std::to_string(be32(d, 0)) + "x" + std::to_string(be32(d, 4)) +
               (std::to_integer<int>(d[8]) == 1 ? " per-metre" : " unit-unknown");
    if (type == "tIME" && d.size() == 7) {
        std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", unsigned{be16(d, 0)},
                      std::to_integer<unsigned>(d[2]), std::to_integer<unsigned>(d[3]),
                      std::to_integer<unsigned>(d[4]), std::to_integer<unsigned>(d[5]),
                      std::to_integer<unsigned>(d[6]));
        return buf;
    }
    if ((type == "sBIT" || type == "bKGD") && d.size() <= 8) {
        std::string s;
        for (std::size_t k = 0; k < d.size(); k += type == "bKGD" && d.size() > 1 ? 2 : 1)
            s +=
                (k ? "," : "") +
                std::to_string(type == "bKGD" && d.size() > 1 ? be16(d, k) : std::to_integer<unsigned>(d[k]));
        return s;
    }
    return bytes_text(d);
}

void build_entries(PngDoc& doc) {
    std::map<std::string, int> seen;
    auto add = [&](std::string key, Entry e, bool always_number = false) {
        int n = seen[key]++;
        if (n || always_number)
            key += ":" + std::to_string(n);
        e.key = std::move(key);
        doc.entries.push_back(std::move(e));
    };
    std::optional<std::size_t> anim;
    Hasher anim_hash;
    for (std::size_t i = 0; i < doc.chunks.size(); ++i) {
        const auto& c = doc.chunks[i];
        const std::string& t = c.type;
        if (kCritical.count(t) || t == "tRNS" || t == "hIST")
            continue;
        auto d = doc.data(c);
        if (kAnimation.count(t)) {
            if (!anim) {
                anim = doc.entries.size();
                doc.entries.push_back({"/animation", {}, {}, "animation", "APNG", {}});
            }
            doc.entries[*anim].chunks.push_back(i);
            anim_hash.add(t).raw(d);
            continue;
        }
        Entry e{{}, hash128(d), render(t, d), "metadata", t, {i}};
        try {
            if (kColor.count(t)) {
                e.kind = "color";
                if (t == "iCCP") {
                    auto [name, at] = keyword(d);
                    if (at >= d.size() || d[at] != std::byte{0})
                        throw Error(ErrorCategory::invalid_input, "iCCP compression method");
                    auto profile = inflate(d.subspan(at + 1), kTextLimit);
                    Hasher h;
                    h.add(name).raw(profile);
                    e.value = h.digest();
                    e.text = "name=" + escape_text(latin1(name)) + " " + bytes_text(profile);
                }
                add("/color/" + t, std::move(e));
            } else if (t == "tEXt" || t == "zTXt") {
                auto [kw, at] = keyword(d);
                std::string text;
                if (t == "tEXt") {
                    text = latin1(str(d.subspan(at)));
                } else {
                    if (at >= d.size() || d[at] != std::byte{0})
                        throw Error(ErrorCategory::invalid_input, "zTXt compression method");
                    text = latin1(str(inflate(d.subspan(at + 1), kTextLimit)));
                }
                e.kind = "text", e.value = hash128(text), e.text = escape_text(text);
                add("/chunks/tEXt:" + pointer_escape(latin1(kw)), std::move(e));
            } else if (t == "iTXt") {
                auto [kw, at] = keyword(d);
                if (at + 2 > d.size())
                    throw Error(ErrorCategory::invalid_input, "short iTXt");
                bool z = d[at] != std::byte{0};
                auto rest = str(d.subspan(at + 2));
                auto l1 = rest.find('\0'), l2 = l1 == std::string::npos ? l1 : rest.find('\0', l1 + 1);
                if (l2 == std::string::npos)
                    throw Error(ErrorCategory::invalid_input, "iTXt fields");
                auto lang = rest.substr(0, l1), translated = rest.substr(l1 + 1, l2 - l1 - 1);
                auto body = d.subspan(at + 2 + l2 + 1);
                std::string text = z ? str(inflate(body, kTextLimit)) : str(body);
                Hasher h;
                h.add(translated).add(text);
                e.kind = "text", e.value = h.digest(), e.text = escape_text(text);
                e.encoding = z ? "iTXt+z" : "iTXt";
                add("/chunks/iTXt:" + pointer_escape(latin1(kw) + (lang.empty() ? "" : "@" + lang)),
                    std::move(e));
            } else if (t == "sPLT") {
                add("/chunks/sPLT:" + pointer_escape(latin1(keyword(d).first)), std::move(e));
            } else if (kSingleton.count(t)) {
                add("/chunks/" + t, std::move(e));
            } else {
                add("/chunks/" + t, std::move(e), true);
            }
        } catch (const Error& err) {
            if (err.category() == ErrorCategory::limit)
                throw;
            doc.warnings.push_back({"chunk-unreadable", "/chunks/" + t, t + " chunk: " + err.what()});
            e.text = bytes_text(d);
            add("/chunks/" + t, std::move(e), true);
        }
    }
    if (anim) {
        auto& e = doc.entries[*anim];
        e.value = anim_hash.digest();
        e.text = "chunks=" + std::to_string(e.chunks.size());
        for (auto i : e.chunks) // acTL: num_frames, num_plays
            if (doc.chunks[i].type == "acTL" && doc.chunks[i].length >= 8)
                e.text = "frames=" + std::to_string(be32(doc.data(doc.chunks[i]), 0));
    }
}

// The EnvelopeMap (png.md §7.1, SPEC §12.3).
void build_map(PngDoc& doc) {
    auto& r = doc.map.ranges;
    r.push_back({0, 8, ByteClass::framing, "/signature", {}});
    std::map<std::size_t, std::string> unit_of;
    for (auto& e : doc.entries)
        for (auto i : e.chunks)
            unit_of[i] = e.key;
    std::uint64_t stream_total = 0, stream_at = 0;
    for (auto& c : doc.chunks)
        if (c.type == "IDAT")
            stream_total += c.length;
    auto piece = [&](std::uint64_t off, std::uint64_t len, ByteClass cls, const std::string& unit,
                     const char* layer) {
        if (len)
            r.push_back({off, len, cls, unit, layer ? layer : ""});
    };
    for (std::size_t i = 0; i < doc.chunks.size(); ++i) {
        const auto& c = doc.chunks[i];
        std::string unit = unit_of.count(i)   ? unit_of[i]
                           : c.type == "IHDR" ? "/encoding"
                           : c.type == "IEND" ? "/end"
                           : c.type == "hIST" ? "/hist"
                                              : "/pixels";
        piece(c.offset, 8, ByteClass::framing, unit, nullptr);
        if (c.type == "IDAT") {
            // Stream bytes: zlib header [0, 2), deflate data, Adler-32 [total - 4, total).
            const std::uint64_t s0 = stream_at, s1 = stream_at + c.length;
            const std::uint64_t cuts[] = {0, 2, stream_total >= 6 ? stream_total - 4 : 2, stream_total};
            const ByteClass cls[] = {ByteClass::encoding, ByteClass::encoding, ByteClass::integrity};
            const char* layers[] = {"zlib", "deflate", "zlib"};
            for (int k = 0; k < 3; ++k) {
                auto lo = std::max(s0, cuts[k]), hi = std::min(s1, cuts[k + 1]);
                if (lo < hi)
                    piece(c.data() + lo - s0, hi - lo, cls[k], "/pixels", layers[k]);
            }
            stream_at = s1;
        } else if (c.type == "PLTE" || c.type == "tRNS") {
            piece(c.data(), c.length, ByteClass::encoding, "/pixels", "palette");
        } else if (c.type == "tIME") {
            piece(c.data(), c.length, ByteClass::volatile_, unit, nullptr);
        } else if (c.type == "hIST") {
            piece(c.data(), c.length, ByteClass::derived, unit, nullptr);
        } else if (c.type == "zTXt" || c.type == "iCCP") {
            auto d = doc.data(c);
            auto nul = str(d.first(std::min<std::size_t>(d.size(), 80))).find('\0');
            auto head = nul == std::string::npos ? c.length : std::min<std::uint64_t>(nul + 2, c.length);
            piece(c.data(), head, ByteClass::content, unit, nullptr);
            piece(c.data() + head, c.length - head, ByteClass::encoding, unit, "zlib");
        } else {
            piece(c.data(), c.length, ByteClass::content, unit, nullptr);
        }
        piece(c.end() - 4, 4, ByteClass::integrity, unit, nullptr);
    }
    doc.map.validate(doc.src.size());
}

// ---- Options (png.md §4) ----

struct Opts {
    raster::CellCompare cmp;
    unsigned gap = 0, guard = 0;
    std::vector<std::string> ignore; // --png-ignore-chunks (diff only)
    bool color_dependency_warn = false;
    bool strict = false, repair = false; // --integrity
};

unsigned number(const OptionValues& o, const char* name) {
    auto it = o.find(name);
    if (it == o.end())
        return 0;
    unsigned v = 0;
    auto [p, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    if (ec != std::errc{} || p != it->second.data() + it->second.size())
        throw Error(ErrorCategory::unsupported, std::string("PNG: --") + name + "=" + it->second +
                                                    " (a whole number of 8-bit units is supported)");
    return v;
}

Opts options(const OptionValues& o) {
    Opts r;
    r.cmp.tolerance = static_cast<std::uint16_t>(std::min(65535u, number(o, "tolerance") * 257));
    r.cmp.alpha_zero_equal = o.count("alpha-zero-equal") > 0;
    r.gap = number(o, "gap"), r.guard = number(o, "guard");
    if (auto it = o.find("png-ignore-chunks"); it != o.end()) {
        std::stringstream ss(it->second);
        for (std::string item; std::getline(ss, item, ',');)
            if (!item.empty())
                r.ignore.push_back(item);
    }
    if (auto it = o.find("png-color-dependency"); it != o.end()) {
        if (it->second != "warn" && it->second != "conflict")
            throw Error(ErrorCategory::invalid_input, "--png-color-dependency must be conflict or warn");
        r.color_dependency_warn = it->second == "warn";
    }
    if (auto it = o.find("png-mode"); it != o.end()) {
        if (it->second == "index")
            throw Error(ErrorCategory::unsupported,
                        "--png-mode=index: indexed-mode merge is not implemented yet (png.md §3.2, v1.1)");
        if (it->second != "auto" && it->second != "rgba")
            throw Error(ErrorCategory::invalid_input, "--png-mode must be auto, rgba, or index");
    }
    if (auto it = o.find("integrity"); it != o.end()) {
        if (it->second != "recompute" && it->second != "repair" && it->second != "strict")
            throw Error(ErrorCategory::invalid_input, "--integrity must be recompute, repair, or strict");
        r.strict = it->second == "strict", r.repair = it->second == "repair";
    }
    return r;
}

// Save timestamps (png.md §6.2): tIME, and the ISO 8601 date:create/modify/timestamp text that ImageMagick
// writes.
bool volatile_key(std::string_view key) {
    return key == "/chunks/tIME" || key.starts_with("/chunks/tEXt:date:") ||
           key.starts_with("/chunks/iTXt:date:");
}

bool ignored(const std::string& key, const Opts& o) {
    for (auto& i : o.ignore)
        for (const char* root : {"/chunks/", "/color/"})
            if (key == root + i || key.starts_with(std::string(root) + i + ":"))
                return true;
    return false;
}

// ---- Loading ----

std::unique_ptr<PngDoc> load_doc(ByteSource src, Limits& budget, const Opts& opt) {
    auto doc = std::make_unique<PngDoc>();
    doc->src = std::move(src);
    doc->chunks = parse_chunks(doc->src.bytes());
    auto ihdr = doc->data(doc->chunks[0]);
    doc->width = be32(ihdr, 0), doc->height = be32(ihdr, 4);
    doc->enc = {std::to_integer<std::uint8_t>(ihdr[9]), std::to_integer<std::uint8_t>(ihdr[8]),
                std::to_integer<std::uint8_t>(ihdr[12])};
    const auto ct = doc->enc.color_type, bd = doc->enc.bit_depth;
    const bool legal = (ct == 0 && (bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16)) ||
                       (ct == 3 && (bd == 1 || bd == 2 || bd == 4 || bd == 8)) ||
                       ((ct == 2 || ct == 4 || ct == 6) && (bd == 8 || bd == 16));
    if (!doc->width || !doc->height || doc->width > 0x7FFFFFFFu || doc->height > 0x7FFFFFFFu)
        bad("bad image size");
    if (!legal)
        bad("illegal color type " + std::to_string(ct) + " with bit depth " + std::to_string(bd));
    if (ihdr[10] != std::byte{0} || ihdr[11] != std::byte{0} || doc->enc.interlace > 1)
        bad("unknown compression, filter, or interlace method");

    for (std::size_t i = 0; i < doc->chunks.size(); ++i) {
        const auto& c = doc->chunks[i];
        if (!c.crc_ok)
            doc->crc_mismatch.push_back("/chunks/" + std::to_string(i));
        auto d = doc->data(c);
        if (c.type == "PLTE") {
            if (c.length % 3 || c.length == 0 || c.length > 768)
                bad("bad PLTE length");
            for (std::size_t k = 0; k < c.length; k += 3)
                doc->palette.push_back({std::to_integer<std::uint8_t>(d[k]),
                                        std::to_integer<std::uint8_t>(d[k + 1]),
                                        std::to_integer<std::uint8_t>(d[k + 2]), 255});
        } else if (c.type == "tRNS") {
            if (ct == 3) {
                for (std::size_t k = 0; k < c.length && k < doc->palette.size(); ++k)
                    doc->palette[k][3] = std::to_integer<std::uint8_t>(d[k]);
            } else if ((ct == 0 && c.length == 2) || (ct == 2 && c.length == 6)) {
                std::array<std::uint16_t, 3> k{};
                for (std::size_t j = 0; j < c.length / 2; ++j)
                    k[j] = be16(d, 2 * j);
                doc->key = k;
            }
        }
    }
    if (ct == 3 && doc->palette.empty())
        bad("indexed image without PLTE");
    if (!doc->crc_mismatch.empty()) {
        if (opt.strict)
            bad("CRC mismatch in " + std::to_string(doc->crc_mismatch.size()) +
                " chunk(s) (--integrity=strict)");
        for (auto& at : doc->crc_mismatch)
            doc->warnings.push_back({"integrity-mismatch-input", at, "stored chunk CRC is wrong"});
    }
    doc->pixels = decode_pixels(*doc, budget);
    build_entries(*doc);
    build_map(*doc);
    return doc;
}

// Equal-storage views of up to three rasters (16-bit if any is). Widened copies are charged to `budget`.
class Views {
public:
    explicit Views(std::initializer_list<const Raster*> in, Limits* budget = nullptr) {
        bool wide = std::any_of(in.begin(), in.end(), [](const Raster* r) { return r->wide; });
        std::size_t k = 0;
        for (auto* r : in) {
            if (wide && !r->wide) {
                if (budget)
                    budget->charge(r->cells() * 8);
                own_[k] = r->widened();
                ptr_[k] = &*own_[k];
            } else {
                ptr_[k] = r;
            }
            ++k;
        }
    }
    Views(const Views&) = delete;
    const Raster& operator[](std::size_t k) const { return *ptr_[k]; }

private:
    std::array<std::optional<Raster>, 3> own_;
    std::array<const Raster*, 3> ptr_{};
};

constexpr const char* kEncodingAttrs[] = {"color_type", "bit_depth", "interlace"};
constexpr const char* kColorTypes[] = {"gray", "?", "rgb", "index", "graya", "?", "rgba"};

// The text of /encoding/NAME, and back.
std::string encoding_attr(const Encoding& e, std::string_view name) {
    if (name == "color_type")
        return e.color_type < 7 ? kColorTypes[e.color_type] : "?";
    if (name == "bit_depth")
        return std::to_string(e.bit_depth);
    return e.interlace ? "adam7" : "none";
}

void set_encoding_attr(Encoding& e, std::string_view name, std::string_view v) {
    if (name == "color_type") {
        for (std::uint8_t k = 0; k < 7; ++k)
            if (v == kColorTypes[k] && v != "?")
                return void(e.color_type = k);
    } else if (name == "bit_depth") {
        for (std::uint8_t d :
             {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}, std::uint8_t{8}, std::uint8_t{16}})
            if (v == std::to_string(d))
                return void(e.bit_depth = d);
    } else if (name == "interlace" && (v == "adam7" || v == "none")) {
        return void(e.interlace = v == "adam7");
    }
    bad("patch: bad value '" + std::string(v) + "' for /encoding/" + std::string(name));
}

std::string dims(const PngDoc& d) {
    return std::to_string(d.width) + "x" + std::to_string(d.height);
}

// A reshaped side that equals base at one integer offset (Q-PNG-3): reshaped coords = base + (dx, dy).
struct Shift {
    std::int64_t dx = 0, dy = 0;
    bool crop = false;
};

std::optional<Shift> pure_reshape(const Raster& base, const Raster& side) {
    if (side.width <= base.width && side.height <= base.height) {
        if (auto o = raster::find_offset(base, side))
            return Shift{-std::int64_t{(*o)[0]}, -std::int64_t{(*o)[1]}, true};
    } else if (side.width >= base.width && side.height >= base.height) {
        if (auto o = raster::find_offset(side, base))
            return Shift{(*o)[0], (*o)[1], false};
    }
    return std::nullopt;
}

Change region_change(const Raster& a, const Raster& b, const GridRegion& r, Side side) {
    auto c = make_change(side, Op::update, {"/pixels", r.bounds}, "pixels");
    c.before.hash = raster::hash_cells(a, r.cells);
    c.after.hash = raster::hash_cells(b, r.cells);
    c.depends_on = {"/color"};
    c.metrics = raster::region_metrics(a, b, r);
    return c;
}

// Encoding attributes, shape, and ancillary keys of b against a.
void meta_changes(const PngDoc& a, const PngDoc& b, const Raster& ra, const Raster& rb, Side side,
                  const Opts& opt, ChangeSet& out) {
    for (const char* name : kEncodingAttrs) {
        auto before = encoding_attr(a.enc, name), after = encoding_attr(b.enc, name);
        if (before == after)
            continue;
        auto c = make_change(side, Op::update, {std::string("/encoding/") + name}, "encoding");
        c.before.text = std::move(before), c.after.text = std::move(after);
        out.push_back(std::move(c));
    }
    if (a.width != b.width || a.height != b.height) {
        auto c = make_change(side, Op::update, {"/shape"}, "shape");
        c.before.text = dims(a), c.after.text = dims(b);
        if (auto s = pure_reshape(ra, rb)) {
            c.metrics["reshape"] = s->crop ? "crop" : "extension";
            c.metrics["offset"] = {s->crop ? -s->dx : s->dx, s->crop ? -s->dy : s->dy};
        }
        out.push_back(std::move(c));
    }
    std::map<std::string, const Entry*> ma, mb;
    for (auto& e : a.entries)
        ma[e.key] = &e;
    for (auto& e : b.entries)
        mb[e.key] = &e;
    auto value = [](const Entry& e) { return Value{e.text, e.value}; };
    for (auto& [k, e] : ma) {
        if (ignored(k, opt))
            continue;
        auto f = mb.find(k);
        if (f == mb.end()) {
            auto c = make_change(side, Op::erase, {k}, e->kind);
            c.before = value(*e);
            out.push_back(std::move(c));
        } else if (f->second->value != e->value || f->second->encoding != e->encoding) {
            auto c = make_change(side, Op::update, {k}, f->second->value == e->value ? "encoding" : e->kind);
            c.before = value(*e), c.after = value(*f->second);
            if (c.kind == "encoding")
                c.before.text = e->encoding, c.after.text = f->second->encoding;
            out.push_back(std::move(c));
        }
    }
    for (auto& [k, e] : mb)
        if (!ma.count(k) && !ignored(k, opt)) {
            auto c = make_change(side, Op::insert, {k}, e->kind);
            c.after = value(*e);
            out.push_back(std::move(c));
        }
}

ChangeSet diff_docs(const PngDoc& a, const PngDoc& b, const Opts& opt, Limits& budget) {
    ChangeSet out;
    Views v({&a.pixels, &b.pixels}, &budget);
    meta_changes(a, b, v[0], v[1], Side::theirs, opt, out);
    if (a.width == b.width && a.height == b.height)
        for (auto& r : raster::diff_regions(v[0], v[1], opt.cmp, opt.gap, &budget))
            out.push_back(region_change(v[0], v[1], r, Side::theirs));
    sort_canonical(out);
    return out;
}

// ---- Writing (png.md §7) ----

struct Writer {
    emit::GroupId next = 1;
    std::vector<emit::Seg> segs;

    void chunk(std::string_view type, std::span<const std::byte> data) {
        auto body = next++, crc = next++;
        segs.push_back(emit::field(4, emit::Endian::big, emit::size_of(body)));
        segs.push_back(emit::group(crc, {emit::bytes(type), emit::group(body, {emit::bytes(data)})}));
        segs.push_back(emit::field(4, emit::Endian::big, emit::crc32(crc)));
    }
    void raw(const PngDoc& d, const Chunk& c, bool repair) {
        if (repair)
            chunk(c.type, d.data(c));
        else
            segs.push_back(emit::raw(d.src, c.offset, c.end() - c.offset));
    }
    std::vector<std::byte> finish() {
        std::vector<emit::Seg> all{
            emit::bytes(std::span(reinterpret_cast<const std::byte*>(kSignature.data()), 8))};
        all.insert(all.end(), std::make_move_iterator(segs.begin()), std::make_move_iterator(segs.end()));
        return emit::emit(emit::group(next++, std::move(all)));
    }
};

void idat_chunks(Writer& w, std::span<const std::byte> stream, std::size_t size) {
    for (std::size_t at = 0; at < stream.size() || at == 0; at += size) {
        w.chunk("IDAT", stream.subspan(at, std::min(size, stream.size() - at)));
        if (stream.empty())
            break;
    }
}

// The order rank of a chunk type, clamped to its legal position class (png.md §7.2):
// 0 IHDR, 1 before PLTE, 2 PLTE, 3 after PLTE before IDAT, 4 IDAT, 5 after IDAT, 6 IEND.
std::pair<int, int> rank_range(std::string_view t, bool has_plte, bool pre_idat) {
    if (t == "IHDR")
        return {0, 0};
    if (t == "PLTE")
        return {2, 2};
    if (t == "IDAT")
        return {4, 4};
    if (t == "IEND")
        return {6, 6};
    if (kColor.count(t))
        return {1, 1};
    if (kAfterPlte.count(t))
        return {has_plte ? 3 : 1, 3};
    if (kBeforeIdat.count(t))
        return {1, 3};
    if (kAnimation.count(t) || !known(t))
        return pre_idat ? std::pair{1, 3} : std::pair{5, 5};
    return {1, 5}; // text, tIME, and the other anywhere chunks
}

struct Item {
    std::string type;
    const PngDoc* doc = nullptr; // raw copy of doc->chunks[chunk]
    std::size_t chunk = 0;
    std::vector<std::byte> data; // generated
    bool pre_idat = true;
    int rank = 0;
};

bool before_idat(const PngDoc& d, std::size_t chunk) {
    for (std::size_t i = 0; i < chunk; ++i)
        if (d.chunks[i].type == "IDAT")
            return false;
    return true;
}

// A generated IDAT stream in `lay`'s chunk size: all but the last chunk share it; one chunk stays one chunk.
void push_idat(std::vector<Item>& items, const std::vector<std::byte>& stream, const PngDoc& lay) {
    std::size_t n = 0, size = 0;
    for (auto& x : lay.chunks)
        if (x.type == "IDAT")
            size = n++ ? size : x.length;
    const std::size_t chunk = n > 1 && size ? size : n == 1 ? std::size_t{0x7FFFFFFF} : 65536;
    for (std::size_t at = 0; at < stream.size(); at += chunk)
        items.push_back({"IDAT",
                         nullptr,
                         0,
                         {stream.begin() + static_cast<long>(at),
                          stream.begin() + static_cast<long>(std::min(stream.size(), at + chunk))},
                         false});
}

// Ranks each item by the region it sits in, clamped to its legal class, sorts stably, and writes the file.
std::vector<std::byte> write_items(std::vector<Item> items, bool repair) {
    const bool has_plte =
        std::any_of(items.begin(), items.end(), [](const Item& x) { return x.type == "PLTE"; });
    int region = 1;
    for (auto& it : items) {
        if (it.type == "PLTE")
            region = 3;
        else if (it.type == "IDAT")
            region = 5;
        auto [lo, hi] = rank_range(it.type, has_plte, it.pre_idat);
        it.rank = std::clamp(it.type == "IHDR"   ? 0
                             : it.type == "IDAT" ? 4
                             : it.type == "IEND" ? 6
                             : it.type == "PLTE" ? 2
                                                 : region,
                             lo, hi);
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.rank < b.rank; });
    Writer w;
    for (auto& it : items) {
        if (it.doc)
            w.raw(*it.doc, it.doc->chunks[it.chunk], repair);
        else
            w.chunk(it.type, it.data);
    }
    return w.finish();
}

using KeyList = std::vector<std::pair<std::string, Hash128>>;

KeyList key_list(const PngDoc& d) {
    KeyList v;
    for (auto& e : d.entries)
        v.emplace_back(e.key, e.value);
    return v;
}

std::string strip_occurrence(const std::string& key) {
    auto c = key.rfind(':');
    if (c != std::string::npos && c + 1 < key.size() &&
        std::all_of(key.begin() + static_cast<long>(c) + 1, key.end(),
                    [](char ch) { return ch >= '0' && ch <= '9'; }))
        return key.substr(0, c);
    return key;
}

KeyList normalized(KeyList v) {
    for (auto& [k, h] : v)
        k = strip_occurrence(k);
    std::sort(v.begin(), v.end());
    return v;
}

// What the merged document holds, before it is written.
struct Plan {
    Raster pixels; // unified storage
    Encoding enc;
    std::vector<std::pair<std::string, Side>> keys; // output order; side whose entry is written
    std::array<const PngDoc*, 3> docs{};            // by Side
    std::array<const Raster*, 3> rasters{};         // unified views, by Side
};

const Entry* entry(const PngDoc& d, const std::string& key) {
    for (auto& e : d.entries)
        if (e.key == key)
            return &e;
    return nullptr;
}

std::vector<std::byte> write_plan(const Plan& p, const Opts& opt, std::vector<Warning>& warnings,
                                  KeyList& expected) {
    KeyList chosen;
    for (auto& [k, s] : p.keys)
        chosen.emplace_back(k, entry(*p.docs[static_cast<int>(s)], k)->value);
    expected = normalized(chosen);
    // Passthrough first (png.md §7.2): ours, theirs, base.
    for (Side s : {Side::ours, Side::theirs, Side::base}) {
        auto& d = *p.docs[static_cast<int>(s)];
        if (d.enc == p.enc && p.rasters[static_cast<int>(s)]->data == p.pixels.data &&
            d.width == p.pixels.width && d.height == p.pixels.height && normalized(key_list(d)) == expected &&
            !(opt.repair && !d.crc_mismatch.empty())) {
            auto b = d.src.bytes();
            return {b.begin(), b.end()};
        }
    }
    // Layout from ours; pixels from the side whose raster the output equals, if any.
    const PngDoc& lay = *p.docs[static_cast<int>(Side::ours)];
    const PngDoc* pix_src = nullptr;
    for (Side s : {Side::ours, Side::theirs, Side::base})
        if (p.rasters[static_cast<int>(s)]->data == p.pixels.data &&
            p.docs[static_cast<int>(s)]->width == p.pixels.width &&
            p.docs[static_cast<int>(s)]->height == p.pixels.height &&
            p.docs[static_cast<int>(s)]->enc == p.enc) {
            pix_src = p.docs[static_cast<int>(s)];
            break;
        }
    Image img;
    if (!pix_src) {
        const PngDoc* enc_src = &lay;
        for (Side s : {Side::ours, Side::theirs})
            if (p.docs[static_cast<int>(s)]->width == p.pixels.width &&
                p.docs[static_cast<int>(s)]->height == p.pixels.height) {
                enc_src = p.docs[static_cast<int>(s)];
                break;
            }
        Source src = source_of(*enc_src);
        img = encode(p.pixels, p.enc, &src, warnings);
        warnings.push_back({"encoding-reencoded", "/pixels",
                            std::string("IDAT stream re-encoded with ") +
                                (img.params.inferred ? "inferred" : "fallback") + " zlib settings"});
    }

    // Which side writes each key, and the keys that follow each of ours' keys in the merged order.
    std::map<std::string, Side> take(p.keys.begin(), p.keys.end());
    std::map<std::string, std::vector<std::string>> after;
    std::string anchor; // "" = before every ours key
    for (auto& [k, s] : p.keys) {
        if (entry(lay, k))
            anchor = k;
        else
            after[anchor].push_back(k);
    }
    std::vector<Item> items;
    auto add_entry = [&](const std::string& key) {
        Side s = take.at(key);
        auto& d = *p.docs[static_cast<int>(s)];
        auto* e = entry(d, key);
        const bool pixels_kept = p.rasters[static_cast<int>(s)]->data == p.pixels.data;
        for (auto i : e->chunks) {
            const auto& c = d.chunks[i];
            if (!c.safe_to_copy() && !known(c.type) && !pixels_kept) { // png.md §7.4
                warnings.push_back(
                    {"unsafe-chunk-dropped", key, c.type + " is not safe to copy after a pixel change"});
                auto it =
                    std::find(expected.begin(), expected.end(), std::pair{strip_occurrence(key), e->value});
                if (it != expected.end())
                    expected.erase(it);
                continue;
            }
            items.push_back({c.type, &d, i, {}, before_idat(d, i)});
        }
    };
    for (auto& k : after[""])
        add_entry(k);
    const Entry* lay_anim = entry(lay, "/animation");
    bool idat_done = false;
    for (std::size_t i = 0; i < lay.chunks.size(); ++i) {
        const std::string& t = lay.chunks[i].type;
        if (t == "IHDR" || t == "PLTE" || t == "tRNS" || t == "hIST") {
            items.push_back({t}); // placeholder, filled below
        } else if (t == "IDAT") {
            if (idat_done)
                continue;
            idat_done = true;
            if (pix_src) {
                for (std::size_t j = 0; j < pix_src->chunks.size(); ++j)
                    if (pix_src->chunks[j].type == "IDAT")
                        items.push_back({"IDAT", pix_src, j, {}, false});
                continue;
            }
            push_idat(items, img.stream, lay);
        } else if (t == "IEND") {
            items.push_back({t, &lay, i, {}, false});
        } else if (kAnimation.count(t)) {
            auto it = take.find("/animation");
            if (it != take.end() && it->second == Side::ours)
                items.push_back({t, &lay, i, {}, before_idat(lay, i)});
            if (i == lay_anim->chunks[0]) {
                if (it != take.end() && it->second != Side::ours)
                    add_entry("/animation");
                for (auto& k : after["/animation"])
                    add_entry(k);
            }
        } else {
            for (auto& e : lay.entries)
                if (e.chunks[0] == i) {
                    if (take.count(e.key))
                        add_entry(e.key);
                    for (auto& k : after[e.key])
                        add_entry(k);
                }
        }
    }
    if (!lay_anim && take.count("/animation"))
        add_entry("/animation");

    // Critical chunks: as they are in pix_src, or generated. Placeholders keep ours' positions.
    auto fill = [&](const std::string& type, std::optional<Item> content) {
        auto ph = std::find_if(items.begin(), items.end(),
                               [&](const Item& x) { return x.type == type && !x.doc && x.data.empty(); });
        if (!content) {
            if (ph != items.end())
                items.erase(ph);
        } else if (ph != items.end()) {
            *ph = std::move(*content);
        } else {
            auto at =
                std::find_if(items.begin(), items.end(), [](const Item& x) { return x.type == "IDAT"; });
            items.insert(at, std::move(*content));
        }
    };
    auto from_pix = [&](const std::string& type) -> std::optional<Item> {
        for (std::size_t j = 0; j < pix_src->chunks.size(); ++j)
            if (pix_src->chunks[j].type == type)
                return Item{type, pix_src, j};
        return std::nullopt;
    };
    auto generated = [](const std::string& type, const std::vector<std::byte>& d) -> std::optional<Item> {
        return d.empty() ? std::nullopt : std::optional<Item>(Item{type, nullptr, 0, d});
    };
    if (pix_src) {
        for (const char* type : {"IHDR", "PLTE", "tRNS", "hIST"})
            fill(type, from_pix(type));
    } else {
        fill("IHDR", generated("IHDR", img.ihdr));
        fill("PLTE", generated("PLTE", img.plte));
        fill("tRNS", generated("tRNS", img.trns));
        std::vector<std::byte> h;
        if (img.enc.color_type == 3 && std::any_of(lay.chunks.begin(), lay.chunks.end(),
                                                   [](const Chunk& c) { return c.type == "hIST"; })) {
            for (auto x : img.hist)
                h.push_back(std::byte(x >> 8)), h.push_back(std::byte(x & 0xFF));
            warnings.push_back({"derived-regenerated", "/hist", "hIST recomputed from the merged pixels"});
        }
        fill("hIST", generated("hIST", h));
    }
    return write_items(std::move(items), opt.repair);
}

// ---- Patch payloads (SPEC §6) ----
// /pixels#rect=...: a storage flag (1 = 16-bit), then the rectangle's cells row by row, as 8-bit or
// big-endian 16-bit RGBA. /shape: width and height (big-endian u32), then the same for the whole image.
// /encoding/NAME: the attribute's text. Ancillary keys: each chunk as length (u32), type, and data.

void put32(std::vector<std::byte>& o, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        o.push_back(std::byte((v >> s) & 0xFF));
}

void put_cells(std::vector<std::byte>& o, const Raster& r, const Rect& rc) {
    o.push_back(std::byte(r.wide));
    for (auto y = rc.y; y < rc.y + rc.h; ++y)
        for (auto x = rc.x; x < rc.x + rc.w; ++x)
            for (auto v : r.get(y * r.width + x)) {
                if (r.wide)
                    o.push_back(std::byte(v >> 8));
                o.push_back(std::byte(r.wide ? v & 0xFF : v >> 8));
            }
}

// Reads put_cells() data at `at` into `r` (widened if the payload is 16-bit).
void get_cells(std::span<const std::byte> v, std::size_t at, Raster& r, const Rect& rc) {
    if (at >= v.size())
        bad("patch: truncated pixel payload");
    const bool wide = v[at++] != std::byte{0};
    if (v.size() - at != rc.w * rc.h * (wide ? 8 : 4))
        bad("patch: pixel payload size does not match " + to_string(Region{rc}));
    if (wide && !r.wide)
        r = r.widened();
    for (auto y = rc.y; y < rc.y + rc.h; ++y)
        for (auto x = rc.x; x < rc.x + rc.w; ++x) {
            raster::Cell c;
            for (auto& ch : c) {
                ch = wide ? static_cast<std::uint16_t>(std::to_integer<unsigned>(v[at]) << 8 |
                                                       std::to_integer<unsigned>(v[at + 1]))
                          : static_cast<std::uint16_t>(std::to_integer<unsigned>(v[at]) * 257);
                at += wide ? 2 : 1;
            }
            r.set(y * r.width + x, c);
        }
}

std::optional<std::vector<std::byte>> unit_payload(const PngDoc& d, const Address& at) {
    std::vector<std::byte> o;
    if (at.path == "/pixels") {
        auto* rc = at.region ? std::get_if<Rect>(&*at.region) : nullptr;
        if (!rc || rc->x + rc->w > d.width || rc->y + rc->h > d.height)
            return std::nullopt;
        put_cells(o, d.pixels, *rc);
    } else if (at.path == "/shape") {
        put32(o, d.width), put32(o, d.height);
        put_cells(o, d.pixels, {0, 0, d.width, d.height});
    } else if (at.path.starts_with("/encoding/")) {
        auto s = encoding_attr(d.enc, std::string_view(at.path).substr(10));
        auto b = as_bytes(s);
        o.assign(b.begin(), b.end());
    } else if (at.path == "/animation") {
        throw Error(ErrorCategory::unsupported, "PNG: patches of APNG animation chunks are not supported");
    } else {
        auto* e = entry(d, at.path);
        if (!e)
            return std::nullopt;
        for (auto i : e->chunks) {
            auto& c = d.chunks[i];
            put32(o, c.length);
            auto t = as_bytes(c.type);
            o.insert(o.end(), t.begin(), t.end());
            auto data = d.data(c);
            o.insert(o.end(), data.begin(), data.end());
        }
    }
    return o;
}

// `t` with the units set, as a PNG file: pixels and encoding re-encoded when they change, replaced chunks at
// the position of the chunk they replace, new ones ranked into their legal place (png.md §7.2).
std::vector<std::byte> implant_units(const PngDoc& t, std::span<const UnitValue> units, const Opts& opt,
                                     std::vector<Warning>& warnings) {
    Raster px = t.pixels;
    Encoding enc = t.enc;
    std::map<std::string, const std::vector<std::byte>*> keys; // null: removed
    for (auto& u : units) {
        const auto& p = u.at.path;
        if (p == "/pixels" || p == "/shape") {
            if (!u.value)
                bad("patch: " + p + " cannot be removed");
            std::span<const std::byte> v(*u.value);
            if (p == "/shape") {
                if (v.size() < 9)
                    bad("patch: truncated /shape payload");
                const std::uint32_t w = be32(v, 0), h = be32(v, 4);
                if (!w || !h || std::uint64_t{w} * h > (std::uint64_t{1} << 40))
                    bad("patch: bad /shape size");
                px = Raster(w, h, px.wide);
                get_cells(v, 8, px, {0, 0, w, h});
            } else {
                auto* rc = u.at.region ? std::get_if<Rect>(&*u.at.region) : nullptr;
                if (!rc || rc->x + rc->w > px.width || rc->y + rc->h > px.height)
                    bad("patch: " + u.at.to_string() + " lies outside the " + std::to_string(px.width) + "x" +
                        std::to_string(px.height) + " image");
                get_cells(v, 0, px, *rc);
            }
        } else if (p.starts_with("/encoding/")) {
            if (!u.value)
                bad("patch: " + p + " cannot be removed");
            set_encoding_attr(enc, std::string_view(p).substr(10), str(*u.value));
        } else if (p == "/animation") {
            throw Error(ErrorCategory::unsupported,
                        "PNG: patches of APNG animation chunks are not supported");
        } else {
            keys[p] = u.value;
        }
    }
    auto chunks_of = [&](const std::vector<std::byte>& v, bool pre_idat) {
        std::vector<Item> out;
        for (std::size_t at = 0; at < v.size();) {
            if (v.size() - at < 8)
                bad("patch: truncated chunk payload");
            const std::uint32_t n = be32(v, at);
            if (v.size() - at - 8 < n)
                bad("patch: truncated chunk payload");
            std::string type = str(std::span(v).subspan(at + 4, 4));
            if (kCritical.count(type) || type == "tRNS" || type == "hIST")
                bad("patch: a key payload cannot carry " + type);
            out.push_back({type,
                           nullptr,
                           0,
                           {v.begin() + static_cast<long>(at + 8), v.begin() + static_cast<long>(at + 8 + n)},
                           pre_idat});
            at += 8 + n;
        }
        return out;
    };

    const bool reencode = px != t.pixels || enc != t.enc;
    Image img;
    if (reencode) {
        Source src = source_of(t);
        img = encode(px, enc, &src, warnings);
        warnings.push_back({"encoding-reencoded", "/pixels",
                            std::string("IDAT stream re-encoded with ") +
                                (img.params.inferred ? "inferred" : "fallback") + " zlib settings"});
    }
    std::vector<Item> items;
    std::set<std::string> placed;
    bool idat_done = false, had_hist = false;
    for (std::size_t i = 0; i < t.chunks.size(); ++i) {
        const std::string& type = t.chunks[i].type;
        had_hist |= type == "hIST";
        if (reencode &&
            (type == "IHDR" || type == "PLTE" || type == "tRNS" || type == "hIST" || type == "IDAT")) {
            if (type == "IHDR") {
                for (auto [ty, d] : {std::pair{"IHDR", &img.ihdr}, std::pair{"PLTE", &img.plte},
                                     std::pair{"tRNS", &img.trns}})
                    if (!d->empty())
                        items.push_back({ty, nullptr, 0, *d});
            } else if (type == "IDAT" && !idat_done) {
                idat_done = true;
                push_idat(items, img.stream, t);
            }
            continue;
        }
        const Entry* owner = nullptr;
        for (auto& e : t.entries)
            if (std::find(e.chunks.begin(), e.chunks.end(), i) != e.chunks.end())
                owner = &e;
        if (owner && keys.count(owner->key)) {
            if (i == owner->chunks[0] && keys[owner->key])
                for (auto& it : chunks_of(*keys[owner->key], before_idat(t, i)))
                    items.push_back(std::move(it));
            placed.insert(owner->key);
            continue;
        }
        items.push_back({type, &t, i, {}, before_idat(t, i)});
    }
    if (reencode && had_hist && img.enc.color_type == 3) {
        std::vector<std::byte> h;
        for (auto x : img.hist)
            h.push_back(std::byte(x >> 8)), h.push_back(std::byte(x & 0xFF));
        items.push_back({"hIST", nullptr, 0, h});
        warnings.push_back({"derived-regenerated", "/hist", "hIST recomputed from the patched pixels"});
    }
    auto iend = std::find_if(items.begin(), items.end(), [](const Item& x) { return x.type == "IEND"; });
    std::vector<Item> added;
    for (auto& [k, v] : keys)
        if (v && !placed.count(k))
            for (auto& it : chunks_of(*v, true))
                added.push_back(std::move(it));
    items.insert(iend, std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    return write_items(std::move(items), opt.repair);
}

std::vector<std::byte> write_image(const Raster&, Encoding, const DeflateParams&);
std::vector<std::byte> write_visual(const Raster&);

class PngFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "png",
            "Portable Network Graphics (APNG: default image)",
            {".png", ".apng"},
            {true, true, true, true, true},
            {{"png-mode", OptionSpec::Arg::required, "pixel domain: auto or rgba (index: v1.1)"},
             {"alpha-zero-equal", OptionSpec::Arg::none, "fully transparent pixels compare equal"},
             {"png-ignore-chunks", OptionSpec::Arg::required,
              "diff: chunk keys to ignore (tIME,tEXt:Software)"},
             {"png-color-dependency", OptionSpec::Arg::required,
              "conflict (default) or warn when one side changes color data and the other pixels", true}}};
        return i;
    }

    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        if (head.size() >= 16 && std::memcmp(head.data(), kSignature.data(), 8) == 0 && be32(head, 8) == 13 &&
            str(head.subspan(12, 4)) == "IHDR")
            return {100};
        return {0};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return load_doc(std::move(src), o.ctx.budget, options(o.ctx.format_options));
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        return diff_docs(doc(a), doc(b), options(o.ctx.format_options), o.ctx.budget);
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& o) const override;

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions& o) const override {
        auto& p = doc(d);
        if (o.line_at)
            o.line_at->push_back({"/shape"});
        os << "png width=" << p.width << " height=" << p.height << " encoding=" << p.enc.name()
           << " interlace=" << (p.enc.interlace ? "adam7" : "none") << "\n";
        std::vector<const Entry*> es;
        for (auto& e : p.entries)
            es.push_back(&e);
        auto group = [](const Entry* e) {
            return e->key.starts_with("/color/") ? 0 : e->key == "/animation" ? 2 : 1;
        };
        std::stable_sort(es.begin(), es.end(), [&](const Entry* x, const Entry* y) {
            return std::pair(group(x), x->key) < std::pair(group(y), y->key);
        });
        for (auto* e : es) {
            if (o.line_at)
                o.line_at->push_back({e->key});
            if (group(e) == 0)
                os << "color " << e->key.substr(7) << " " << e->text << "\n";
            else if (group(e) == 1)
                os << "chunk " << e->key.substr(8) << " " << e->text << "\n";
            else
                os << "animation " << e->text << " xxh3=" << hex16(e->value) << "\n";
        }
        raster::dump_tiles(p.pixels, os, o.line_at);
    }

    const EnvelopeMap* envelope(const Document& d) const override { return &doc(d).map; }

    std::optional<std::vector<std::byte>> payload(const Document& d, const Address& at) const override {
        return unit_payload(doc(d), at);
    }

    std::unique_ptr<Document> implant(const Document& t, std::span<const UnitValue> units,
                                      const LoadOptions& o) const override {
        const Opts opt = options(o.ctx.format_options);
        std::vector<Warning> warnings;
        auto bytes = implant_units(doc(t), units, opt, warnings);
        Limits budget = o.ctx.budget.fresh(); // a new document (#51)
        auto d = load_doc(ByteSource::from_memory(std::move(bytes), doc(t).src.name()), budget, opt);
        d->warnings.insert(d->warnings.end(), warnings.begin(), warnings.end());
        return d;
    }

    std::vector<std::byte> visual(const Document& a, const Document& b, const DiffOptions& o) const override {
        auto& pa = doc(a);
        auto& pb = doc(b);
        Views v({&pa.pixels, &pb.pixels}, &o.ctx.budget);
        if (pa.width != pb.width || pa.height != pb.height)
            return write_visual(raster::heatmap(v[1], v[1], {})); // reshaped: b alone
        const Opts opt = options(o.ctx.format_options);
        return write_visual(
            raster::heatmap(v[0], v[1], raster::diff_regions(v[0], v[1], opt.cmp, opt.gap, &o.ctx.budget)));
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& p = doc(d);
        nlohmann::ordered_json j;
        j["width"] = p.width, j["height"] = p.height, j["encoding"] = p.enc.name();
        j["interlace"] = p.enc.interlace ? "adam7" : "none";
        if (p.crc_mismatch.empty())
            j["integrity"] = "ok";
        else
            j["integrity"] = {{"status", "mismatch"}, {"at", p.crc_mismatch}};
        auto& chunks = j["chunks"] = nlohmann::ordered_json::array();
        for (auto& c : p.chunks)
            chunks.push_back({{"offset", c.offset},
                              {"type", c.type},
                              {"length", c.length},
                              {"crc", c.crc_ok ? "ok" : "mismatch"}});
        auto& units = j["units"] = nlohmann::ordered_json::array();
        for (auto& e : p.entries)
            units.push_back({{"at", e.key}, {"kind", e.kind}, {"value", e.text}});
        auto& warn = j["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : p.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return j;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json j;
        std::uint64_t regions = 0, pixels = 0;
        for (auto& c : cs)
            if (c.kind == "pixels")
                ++regions, pixels += c.metrics.value("pixels", std::uint64_t{0});
        j["width"] = {doc(a).width, doc(b).width};
        j["height"] = {doc(a).height, doc(b).height};
        j["regions"] = regions;
        j["pixels_changed"] = pixels;
        return j;
    }

private:
    static const PngDoc& doc(const Document& d) {
        auto* p = dynamic_cast<const PngDoc*>(&d);
        if (!p)
            throw Error(ErrorCategory::internal, "png: not a PNG document");
        return *p;
    }
};

// ---- Merge (png.md §6) ----

Side side_of(const std::string& choice) {
    return choice == "theirs" ? Side::theirs : choice == "base" ? Side::base : Side::ours;
}

MergeResult PngFormat::merge(const Document& base_d, const Document& ours_d, const Document& theirs_d,
                             const MergeOptions& mo) const {
    const PngDoc& b = doc(base_d);
    const PngDoc& o = doc(ours_d);
    const PngDoc& t = doc(theirs_d);
    const Opts opt = options(mo.ctx.format_options);
    MergeResult res;
    for (auto [d, name] : {std::pair{&b, "base"}, std::pair{&o, "ours"}, std::pair{&t, "theirs"}})
        for (auto w : d->warnings) {
            w.message = std::string(name) + ": " + w.message;
            res.warnings.push_back(std::move(w));
        }
    Limits& budget = mo.ctx.budget;
    Views v({&b.pixels, &o.pixels, &t.pixels}, &budget);
    const std::array<const PngDoc*, 3> docs{&b, &o, &t};
    auto R = [&](Side s) -> const Raster& { return v[static_cast<std::size_t>(s)]; };
    auto& co = res.changes_ours;
    auto& ct = res.changes_theirs;
    Opts meta_opt = opt;
    meta_opt.ignore.clear(); // merge still carries ignored chunks
    meta_changes(b, o, R(Side::base), R(Side::ours), Side::ours, meta_opt, co);
    meta_changes(b, t, R(Side::base), R(Side::theirs), Side::theirs, meta_opt, ct);
    auto change_at = [](const ChangeSet& cs, std::string_view path) -> const Change* {
        for (auto& c : cs)
            if (c.at.path == path && !c.at.region)
                return &c;
        return nullptr;
    };

    // What each conflict decides, by index into res.conflicts.
    struct Pending {
        enum Kind { grid, shape, cropped, key, dependency } kind;
        std::size_t index = 0; // grid conflict, crop region, or key row
    };
    std::vector<Pending> pend;
    auto add_conflict = [&](Address at, std::vector<std::string> ids, std::string reason, Pending p,
                            std::vector<std::string> choices = {"ours", "theirs", "base"}) {
        ids.erase(std::remove(ids.begin(), ids.end(), std::string{}), ids.end());
        auto c = make_conflict(std::move(at), std::move(ids), std::move(reason));
        c.choices = std::move(choices);
        res.conflicts.push_back(std::move(c));
        pend.push_back(p);
    };
    auto id_of = [](const Change* c) { return c ? c->id : std::string{}; };

    // Pixels.
    enum class Mode { grid, whole, shape_conflict, shifted } mode = Mode::grid;
    GridResult gr;
    std::optional<Side> whole;
    Side reshaped = Side::ours, painter = Side::theirs;
    Shift shift;
    std::vector<GridRegion> painted; // shifted mode: painter's regions in base coordinates
    std::vector<bool> painted_apply; // shifted mode: region lands inside the reshaped image
    std::map<std::string, std::pair<Side, std::size_t>> region_of; // pixel change id -> (side, region)

    const GridShape sb{b.width, b.height}, so{o.width, o.height}, st{t.width, t.height};
    const bool o_reshaped = so != sb, t_reshaped = st != sb;
    if (o_reshaped != t_reshaped) {
        reshaped = o_reshaped ? Side::ours : Side::theirs;
        painter = o_reshaped ? Side::theirs : Side::ours;
        auto s = pure_reshape(R(Side::base), R(reshaped));
        painted = raster::diff_regions(R(Side::base), R(painter), opt.cmp, opt.gap, &budget);
        if (s && !painted.empty()) {
            mode = Mode::shifted, shift = *s;
            auto& cs = painter == Side::ours ? co : ct;
            const Raster& rr = R(reshaped);
            for (std::size_t k = 0; k < painted.size(); ++k) {
                cs.push_back(region_change(R(Side::base), R(painter), painted[k], painter));
                region_of[cs.back().id] = {painter, k};
                bool inside =
                    std::all_of(painted[k].cells.begin(), painted[k].cells.end(), [&](std::uint64_t i) {
                        auto x = static_cast<std::int64_t>(i % b.width) + shift.dx,
                             y = static_cast<std::int64_t>(i / b.width) + shift.dy;
                        return x >= 0 && y >= 0 && x < rr.width && y < rr.height;
                    });
                painted_apply.push_back(inside);
                if (inside) {
                    res.auto_merged.push_back({cs.back().id, "independent"});
                } else {
                    auto& rcs = reshaped == Side::ours ? co : ct;
                    add_conflict({"/pixels", painted[k].bounds},
                                 {cs.back().id, id_of(change_at(rcs, "/shape"))}, "cropped-away",
                                 {Pending::cropped, k}, {"ours", "theirs"});
                }
            }
            if (auto* sc = change_at(reshaped == Side::ours ? co : ct, "/shape"))
                res.auto_merged.push_back({sc->id, "independent"});
        }
    }
    if (mode == Mode::grid) {
        gr = grid_merge(sb, so, st,
                        [&](Side x, Side y, std::uint64_t i) { return opt.cmp.same(R(x), R(y), i); },
                        {opt.gap, opt.guard, &budget});
        if (gr.whole) {
            mode = Mode::whole, whole = gr.whole;
            for (auto* cs : {&co, &ct})
                if (auto* sc = change_at(*cs, "/shape"))
                    res.auto_merged.push_back(
                        {sc->id, o_reshaped && t_reshaped ? "identical" : "independent"});
        } else if (gr.shape_conflict) {
            mode = Mode::shape_conflict;
            add_conflict({"/shape"}, {id_of(change_at(co, "/shape")), id_of(change_at(ct, "/shape"))},
                         "shape-mismatch", {Pending::shape, 0});
        } else {
            std::vector<std::string> ids_o, ids_t;
            for (std::size_t k = 0; k < gr.ours.size(); ++k) {
                co.push_back(region_change(R(Side::base), R(Side::ours), gr.ours[k], Side::ours));
                ids_o.push_back(co.back().id), region_of[co.back().id] = {Side::ours, k};
            }
            for (std::size_t k = 0; k < gr.theirs.size(); ++k) {
                ct.push_back(region_change(R(Side::base), R(Side::theirs), gr.theirs[k], Side::theirs));
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
                add_conflict({"/pixels", g.bounds}, ids, g.reason, {Pending::grid, k});
            }
            auto note = [&](const std::vector<GridRegion>& regs, const std::vector<std::string>& ids) {
                for (std::size_t k = 0; k < regs.size(); ++k)
                    if (!conflicted.count(ids[k])) {
                        bool same =
                            std::all_of(regs[k].cells.begin(), regs[k].cells.end(), [&](std::uint64_t i) {
                                return opt.cmp.same(R(Side::ours), R(Side::theirs), i);
                            });
                        res.auto_merged.push_back({ids[k], same ? "identical" : "independent"});
                    }
            };
            note(gr.ours, ids_o), note(gr.theirs, ids_t);
            for (auto [r, k] : gr.adjacent)
                res.warnings.push_back(
                    {"adjacent-edits", Address{"/pixels", gr.ours[r].bounds}.to_string(),
                     "ours and theirs edit adjacent pixels (" + to_string(gr.ours[r].bounds) + ", " +
                         to_string(gr.theirs[k].bounds) + "); --guard=1 makes this a conflict"});
        }
    }

    // Ancillary keys (KeyedMerge) and encoding attributes.
    auto lb = key_list(b), lo = key_list(o), lt = key_list(t);
    auto rows = keyed_merge<std::string, Hash128>(lb, lo, lt);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        auto& row = rows[k];
        auto* xo = change_at(co, row.key);
        auto* xt = change_at(ct, row.key);
        if (row.outcome.rule == MergeRule::conflict && volatile_key(row.key) && row.ours && row.theirs) {
            // Volatile timestamps: newest wins (SPEC §12.1). Both renderings sort by time as text.
            bool theirs_newer = entry(t, row.key)->text > entry(o, row.key)->text;
            row.outcome.take = theirs_newer ? Side::theirs : Side::ours;
            for (auto* x : {xo, xt})
                res.auto_merged.push_back({x->id, "newest"});
        } else if (row.outcome.rule == MergeRule::conflict) {
            add_conflict({row.key}, {id_of(xo), id_of(xt)},
                         row.key == "/animation" ? "unsupported" : row.outcome.reason, {Pending::key, k});
        } else if (row.outcome.rule != MergeRule::unchanged) {
            for (auto* x : {xo, xt})
                if (x)
                    res.auto_merged.push_back(
                        {x->id, row.outcome.rule == MergeRule::identical ? "identical" : "independent"});
        }
    }
    Encoding enc = o.enc;
    {
        auto pair = [](const Encoding& e) { return std::pair(e.color_type, e.bit_depth); };
        bool oc = pair(o.enc) != pair(b.enc), tc = pair(t.enc) != pair(b.enc);
        if (!oc && tc)
            enc.color_type = t.enc.color_type, enc.bit_depth = t.enc.bit_depth;
        if (o.enc.interlace == b.enc.interlace)
            enc.interlace = t.enc.interlace;
        for (const char* f : {"/encoding/color_type", "/encoding/bit_depth", "/encoding/interlace"}) {
            auto* xo = change_at(co, f);
            auto* xt = change_at(ct, f);
            if (xo && xt)
                res.auto_merged.push_back(
                    {xo->id, xo->after.text == xt->after.text ? "identical" : "encoding-ours"});
            else if (xo || xt)
                res.auto_merged.push_back({(xo ? xo : xt)->id, "independent"});
        }
    }

    // Color dependency (png.md §6.2): pixels painted under one interpretation, profile changed on the other.
    const std::size_t before_deps = res.conflicts.size();
    check_dependencies(co, ct, res.auto_merged, res.conflicts, [&](const Change& c, const Change& other) {
        return !opt.color_dependency_warn && c.kind == "pixels" && path_within(other.at.path, "/color");
    });
    for (std::size_t k = before_deps; k < res.conflicts.size(); ++k)
        pend.push_back({Pending::dependency, 0});
    // Notes for changes that ended up in a conflict are dropped.
    {
        std::set<std::string> in_conflict;
        for (auto& c : res.conflicts)
            in_conflict.insert(c.changes.begin(), c.changes.end());
        std::erase_if(res.auto_merged, [&](const AutoMerge& a) {
            return in_conflict.count(a.change) && a.rule != "dependency-touched";
        });
    }

    apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);

    // Apply the decisions.
    std::vector<std::optional<Side>> key_take(rows.size());
    for (std::size_t k = 0; k < rows.size(); ++k)
        key_take[k] = rows[k].outcome.take;
    auto row_index = [&](const std::string& key) -> std::optional<std::size_t> {
        for (std::size_t k = 0; k < rows.size(); ++k)
            if (rows[k].key == key)
                return k;
        return std::nullopt;
    };
    bool painter_wins = false;
    for (std::size_t k = 0; k < res.conflicts.size(); ++k) {
        auto& c = res.conflicts[k];
        Side s = side_of(c.resolved_as);
        auto& p = pend[k];
        switch (p.kind) {
        case Pending::grid:
            grid_resolve(gr, p.index, s);
            break;
        case Pending::shape:
            whole = s;
            break;
        case Pending::cropped:
            painter_wins |= s == painter;
            break;
        case Pending::key:
            key_take[p.index] = take_for(c.resolved_as, rows[p.index].base.has_value(),
                                         rows[p.index].ours.has_value(), rows[p.index].theirs.has_value());
            break;
        case Pending::dependency: {
            // changes = {pixel change, color change}: the losing side's part reverts to the winner's.
            for (auto& id : c.changes) {
                if (auto f = region_of.find(id); f != region_of.end() && f->second.first != s) {
                    auto [side, r] = f->second;
                    if (mode == Mode::grid) {
                        auto& regs = side == Side::ours ? gr.ours : gr.theirs;
                        for (auto cell : regs[r].cells)
                            gr.take[cell] = s;
                    } else if (mode == Mode::shifted) {
                        painted_apply[r] = false;
                    }
                }
                for (auto* cs : {&co, &ct})
                    for (auto& ch : *cs)
                        if (ch.id == id && ch.kind == "color" && ch.side != s)
                            if (auto ri = row_index(ch.at.path))
                                key_take[*ri] =
                                    take_for(s == Side::base   ? "base"
                                             : s == Side::ours ? "ours"
                                                               : "theirs",
                                             rows[*ri].base.has_value(), rows[*ri].ours.has_value(),
                                             rows[*ri].theirs.has_value());
            }
            break;
        }
        }
    }

    Plan plan;
    plan.docs = docs;
    for (int k = 0; k < 3; ++k)
        plan.rasters[static_cast<std::size_t>(k)] = &v[static_cast<std::size_t>(k)];
    plan.enc = enc;
    if (mode == Mode::grid) {
        const Raster& any = R(Side::base);
        budget.charge(any.cells() * any.cell_bytes());
        plan.pixels = Raster(any.width, any.height, any.wide);
        const auto cb = any.cell_bytes();
        for (std::uint64_t i = 0; i < any.cells(); ++i)
            std::memcpy(plan.pixels.data.data() + i * cb, R(gr.take[i]).at(i), cb);
    } else if (mode == Mode::shifted && !painter_wins) {
        plan.pixels = R(reshaped);
        const Raster& pr = R(painter);
        for (std::size_t k = 0; k < painted.size(); ++k)
            if (painted_apply[k])
                for (auto i : painted[k].cells) {
                    auto x = static_cast<std::int64_t>(i % b.width) + shift.dx,
                         y = static_cast<std::int64_t>(i / b.width) + shift.dy;
                    plan.pixels.set(static_cast<std::uint64_t>(y) * plan.pixels.width +
                                        static_cast<std::uint64_t>(x),
                                    pr.get(i));
                }
    } else {
        plan.pixels = R(mode == Mode::shifted ? painter : whole.value_or(Side::ours));
    }
    for (std::size_t k = 0; k < rows.size(); ++k)
        if (key_take[k])
            plan.keys.emplace_back(rows[k].key, *key_take[k]);

    KeyList expected;
    auto bytes = write_plan(plan, opt, res.warnings, expected);
    // Validation (png.md §8): the written file must load to the merged document.
    Limits out_budget = budget.fresh(); // the result is a new document (#51)
    std::unique_ptr<PngDoc> out;
    try {
        out = load_doc(ByteSource::from_memory(std::move(bytes), "merged.png"), out_budget, {});
    } catch (const Error& e) {
        throw Error(ErrorCategory::internal,
                    std::string("PNG writer produced an unreadable file: ") + e.what());
    }
    Views check{&out->pixels, &plan.pixels};
    if (check[0].data != check[1].data || normalized(key_list(*out)) != expected)
        throw Error(ErrorCategory::internal,
                    "PNG writer validation failed: output does not load to the merged document");
    if (mo.visual == MergeOptions::Visual::always ||
        (mo.visual == MergeOptions::Visual::on_conflict && res.unresolved_conflicts()))
        res.visual =
            write_visual(raster::merge_visual(R(Side::base), mode == Mode::grid ? gr : GridResult{}));
    sort_canonical(co), sort_canonical(ct);
    res.document = std::move(out);
    return res;
}

} // namespace

std::vector<std::byte> write(const Raster& r, Encoding enc) {
    return write_image(r, enc, {});
}

namespace {

std::vector<std::byte> write_image(const Raster& r, Encoding enc, const DeflateParams& params) {
    std::vector<Warning> ignore;
    Image img = encode(r, enc, nullptr, ignore, &params);
    Writer w;
    w.chunk("IHDR", img.ihdr);
    if (!img.plte.empty())
        w.chunk("PLTE", img.plte);
    if (!img.trns.empty())
        w.chunk("tRNS", img.trns);
    idat_chunks(w, img.stream, 65536);
    w.chunk("IEND", {});
    return w.finish();
}

// Visuals are report artifacts: level 6 keeps a 16-Mpixel heatmap fast to write.
std::vector<std::byte> write_visual(const Raster& r) {
    return write_image(r, {2, 8, 0}, {6, 15, 8, 3}); // opaque RGB, Z_RLE
}

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<PngFormat>();
}

} // namespace semsplinter::png
