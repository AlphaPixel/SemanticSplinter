#include "common.hpp"

#include <semsplinter/formats/raster_png.hpp>
#include <semsplinter/sequence.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace semsplinter::rastermisc {

// ---- Helpers ----

void bad(std::string_view format, const std::string& what) {
    throw Error(ErrorCategory::invalid_input, std::string(format) + ": " + what);
}

std::string hex16(const Hash128& h) {
    return h.hex().substr(0, 16);
}

std::string bytes_text(std::span<const std::byte> d) {
    return "bytes=" + std::to_string(d.size()) + " xxh3=" + hex16(hash128(d));
}

Raster new_raster(std::uint32_t w, std::uint32_t h, bool wide, Limits& budget) {
    const std::uint64_t cells = std::uint64_t{w} * h;
    if (cells > budget.max_pixels)
        throw Error(ErrorCategory::limit,
                    "image of " + std::to_string(cells) + " pixels exceeds --max-pixels");
    budget.charge(cells * (wide ? 8 : 4));
    return Raster(w, h, wide);
}

std::uint64_t Reader::u(std::uint64_t at, unsigned width, bool big) const {
    if (at > d.size() || d.size() - at < width)
        bad(format, "truncated file (read at " + std::to_string(at) + ")");
    std::uint64_t v = 0;
    for (unsigned k = 0; k < width; ++k)
        v |= std::uint64_t(std::to_integer<unsigned>(d[at + (big ? width - 1 - k : k)])) << (8 * k);
    return v;
}

std::span<const std::byte> Reader::sub(std::uint64_t at, std::uint64_t len) const {
    if (at > d.size() || d.size() - at < len)
        bad(format, "truncated file (" + std::to_string(len) + " bytes at " + std::to_string(at) + ")");
    return d.subspan(at, len);
}

void put_le(std::vector<std::byte>& o, std::uint64_t v, unsigned width) {
    for (unsigned k = 0; k < width; ++k)
        o.push_back(std::byte((v >> (8 * k)) & 0xFF));
}

void put_be(std::vector<std::byte>& o, std::uint64_t v, unsigned width) {
    for (unsigned k = width; k-- > 0;)
        o.push_back(std::byte((v >> (8 * k)) & 0xFF));
}

Rgba8 rgba8(const raster::Cell& c) noexcept {
    return {static_cast<std::uint8_t>(c[0] >> 8), static_cast<std::uint8_t>(c[1] >> 8),
            static_cast<std::uint8_t>(c[2] >> 8), static_cast<std::uint8_t>(c[3] >> 8)};
}

raster::Cell cell(const Rgba8& c) noexcept {
    return {static_cast<std::uint16_t>(c[0] * 257), static_cast<std::uint16_t>(c[1] * 257),
            static_cast<std::uint16_t>(c[2] * 257), static_cast<std::uint16_t>(c[3] * 257)};
}

bool narrow_ok(const Raster& r) noexcept {
    if (!r.wide)
        return true;
    for (std::uint64_t i = 0; i < r.cells(); ++i)
        for (auto v : r.get(i))
            if (v % 257)
                return false;
    return true;
}

const Key* find_key(std::span<const Key> keys, std::string_view path) noexcept {
    for (auto& k : keys)
        if (k.key == path)
            return &k;
    return nullptr;
}

std::optional<std::vector<Rgba8>> palettize(const Raster& r, std::vector<Rgba8> pal, std::size_t max,
                                            std::vector<std::uint8_t>& index) {
    std::map<Rgba8, std::uint8_t> at;
    for (std::size_t k = pal.size(); k-- > 0;) // the first of duplicate entries wins
        at[pal[k]] = static_cast<std::uint8_t>(k);
    index.assign(r.cells(), 0);
    for (std::uint64_t i = 0; i < r.cells(); ++i) {
        auto c = rgba8(r.get(i));
        auto f = at.find(c);
        if (f == at.end()) {
            if (pal.size() >= max)
                return std::nullopt;
            f = at.emplace(c, static_cast<std::uint8_t>(pal.size())).first;
            pal.push_back(c);
        }
        index[i] = f->second;
    }
    return pal;
}

std::optional<std::pair<std::vector<std::uint8_t>, std::vector<Rgba8>>> merged_indices(const Plan& p,
                                                                                       const OutFrame& f) {
    auto indexed = [&](Side s) -> const Frame* {
        if (!f.src[static_cast<std::size_t>(s)])
            return nullptr;
        auto& fr = p.frame(f, s);
        return fr.indices.empty() || fr.pixels.width != f.pixels.width || fr.pixels.height != f.pixels.height
                   ? nullptr
                   : &fr;
    };
    if (f.same)
        if (auto* fr = indexed(*f.same))
            return std::pair{fr->indices, fr->palette};
    if (f.take.empty())
        return std::nullopt;
    std::array<const Frame*, 3> fs{indexed(Side::base), indexed(Side::ours), indexed(Side::theirs)};
    std::array<bool, 3> used{};
    for (auto s : f.take)
        used[static_cast<std::size_t>(s)] = true;
    const std::vector<Rgba8>* pal = nullptr;
    for (std::size_t k = 0; k < 3; ++k)
        if (used[k]) {
            if (!fs[k] || (pal && fs[k]->palette != *pal))
                return std::nullopt;
            pal = &fs[k]->palette;
        }
    std::vector<std::uint8_t> idx(f.take.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = fs[static_cast<std::size_t>(f.take[i])]->indices[i];
    return std::pair{std::move(idx), *pal};
}

namespace {

using raster::Cell;

// ---- Options (png.md §4; raster-misc.md §6) ----

struct Opts {
    raster::CellCompare cmp;
    unsigned gap = 0, guard = 0;
    bool reencode = false;
    std::vector<std::string> ignore; // keys hidden from diff
};

unsigned number(const OptionValues& o, const char* name) {
    auto it = o.find(name);
    if (it == o.end())
        return 0;
    unsigned v = 0;
    auto [p, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    if (ec != std::errc{} || p != it->second.data() + it->second.size())
        throw Error(ErrorCategory::unsupported, std::string("--") + name + "=" + it->second +
                                                    " (a whole number of 8-bit units is supported)");
    return v;
}

Opts options(const OptionValues& o, std::string_view ignore_option) {
    Opts r;
    r.cmp.tolerance = static_cast<std::uint16_t>(std::min(65535u, number(o, "tolerance") * 257));
    r.cmp.alpha_zero_equal = o.count("alpha-zero-equal") > 0;
    r.gap = number(o, "gap"), r.guard = number(o, "guard");
    r.reencode = o.count("reencode") > 0;
    if (auto it = o.find(ignore_option); !ignore_option.empty() && it != o.end()) {
        std::stringstream ss(it->second);
        for (std::string item; std::getline(ss, item, ',');)
            if (!item.empty())
                r.ignore.push_back(item);
    }
    return r;
}

// Equal-storage views of up to three rasters (16-bit if any is). Widened copies are charged to `budget`.
class Views {
public:
    Views(std::initializer_list<const Raster*> in, Limits* budget = nullptr) {
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

bool decodable(const Frame& f) {
    return !f.opaque;
}

bool same_shape(const Frame& a, const Frame& b) {
    return a.pixels.width == b.pixels.width && a.pixels.height == b.pixels.height;
}

bool equal_cells(const Raster& a, const Raster& b) {
    return a.width == b.width && a.height == b.height && raster::hash_cells(a) == raster::hash_cells(b);
}

// Pixel identity of a frame: canonical cells and shape, or the coded bytes of an opaque frame.
Hash128 pixel_id(const Frame& f) {
    Hasher h;
    if (f.opaque)
        h.add("opaque").add(f.coded.hi).add(f.coded.lo);
    else
        h.add(f.pixels.width)
            .add(f.pixels.height)
            .add(raster::hash_cells(f.pixels).hi)
            .add(raster::hash_cells(f.pixels).lo);
    return h.digest();
}

bool same_pixels(const Frame& a, const Frame& b) {
    if (a.opaque != b.opaque)
        return false;
    if (a.opaque)
        return a.coded == b.coded;
    return same_shape(a, b) && raster::hash_cells(a.pixels) == raster::hash_cells(b.pixels);
}

using KeyList = std::vector<std::pair<std::string, Hash128>>;

KeyList sorted(KeyList v) {
    std::sort(v.begin(), v.end());
    return v;
}

// Identity of a whole frame for SequenceMerge (raster-misc.md §2): frame control plus raster.
Hash128 frame_id(const Frame& f) {
    Hasher h;
    auto p = pixel_id(f);
    h.add(p.hi).add(p.lo);
    KeyList ks;
    for (auto& k : f.keys)
        ks.emplace_back(k.key, k.value);
    for (auto& [k, v] : sorted(ks))
        h.add(k).add(v.hi).add(v.lo);
    return h.digest();
}

class Engine final : public Format {
public:
    explicit Engine(std::unique_ptr<Codec> c) : c_(std::move(c)) {}

    const FormatInfo& info() const override { return c_->info(); }
    Probe probe(std::span<const std::byte> head, std::string_view name) const override {
        return {c_->probe(head, name)};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return c_->load(std::move(src), o.ctx.budget, o.ctx.format_options);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        const Opts opt = opts(o.ctx.format_options);
        ChangeSet out;
        two_way(doc(a), doc(b), Side::theirs, opt, o.ctx.budget, out, true, true);
        sort_canonical(out);
        return out;
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions&) const override;

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions&) const override {
        auto& p = doc(d);
        const Frame* f0 = p.frames.empty() ? nullptr : &p.frames[0];
        os << info().name;
        if (f0 && !f0->opaque)
            os << " width=" << f0->pixels.width << " height=" << f0->pixels.height;
        for (auto& [k, v] : p.encoding)
            os << " " << k << "=" << v;
        if (p.sequence)
            os << " " << c_->frame_word() << "=" << p.frames.size();
        os << "\n";
        auto keys = [&](const std::vector<Key>& ks, const std::string& indent) {
            std::vector<const Key*> v;
            for (auto& k : ks)
                v.push_back(&k);
            std::sort(v.begin(), v.end(), [](const Key* x, const Key* y) { return x->key < y->key; });
            for (auto* k : v)
                os << indent << "key " << k->key << " " << k->text << "\n";
        };
        keys(p.keys, "");
        for (std::size_t i = 0; i < p.frames.size(); ++i) {
            auto& f = p.frames[i];
            std::string indent;
            if (p.sequence) {
                os << c_->frame_word().substr(0, c_->frame_word().size() - 1) << " " << i;
                if (!f.opaque)
                    os << " " << f.pixels.width << "x" << f.pixels.height;
                if (!f.encoding.empty())
                    os << " encoding=" << f.encoding;
                os << "\n";
                indent = "  ";
            }
            keys(f.keys, indent);
            if (f.opaque) {
                os << indent << "coded xxh3=" << hex16(f.coded) << "\n";
                continue;
            }
            std::ostringstream tiles;
            raster::dump_tiles(f.pixels, tiles);
            std::istringstream lines(tiles.str());
            for (std::string line; std::getline(lines, line);)
                os << indent << line << "\n";
        }
    }

    const EnvelopeMap* envelope(const Document& d) const override { return &doc(d).map; }

    std::vector<std::byte> visual(const Document& a, const Document& b, const DiffOptions& o) const override {
        auto& da = doc(a);
        auto& db = doc(b);
        const Opts opt = opts(o.ctx.format_options);
        // The first frame that differs, or frame 0.
        std::size_t k = 0;
        for (std::size_t i = 0; i < std::min(da.frames.size(), db.frames.size()); ++i)
            if (!same_pixels(da.frames[i], db.frames[i])) {
                k = i;
                break;
            }
        if (k >= da.frames.size() || k >= db.frames.size() || da.frames[k].opaque || db.frames[k].opaque)
            throw Error(ErrorCategory::unsupported, "--visual: no decodable frame to compare");
        Views v({&da.frames[k].pixels, &db.frames[k].pixels}, &o.ctx.budget);
        if (!same_shape(da.frames[k], db.frames[k]))
            return raster::encode_png(raster::heatmap(v[1], v[1], {}));
        return raster::encode_png(
            raster::heatmap(v[0], v[1], raster::diff_regions(v[0], v[1], opt.cmp, opt.gap, &o.ctx.budget)));
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& p = doc(d);
        nlohmann::ordered_json j;
        if (!p.frames.empty() && !p.frames[0].opaque)
            j["width"] = p.frames[0].pixels.width, j["height"] = p.frames[0].pixels.height;
        for (auto& [k, v] : p.encoding)
            j[k] = v;
        j[std::string(c_->frame_word())] = p.frames.size();
        j["integrity"] = "ok"; // these formats carry no integrity fields (raster-misc.md §7.1)
        for (auto& [k, v] : p.structure.items())
            j[k] = v;
        auto& units = j["units"] = nlohmann::ordered_json::array();
        for (auto& k : p.keys)
            units.push_back({{"at", k.key}, {"kind", k.kind}, {"value", k.text}});
        for (std::size_t i = 0; i < p.frames.size(); ++i)
            for (auto& k : p.frames[i].keys)
                units.push_back({{"at", prefix(p.sequence, i) + k.key}, {"kind", k.kind}, {"value", k.text}});
        auto& warn = j["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : p.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return j;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json j;
        std::uint64_t regions = 0, pixels = 0;
        for (auto& c : cs)
            if (c.kind == "pixels" && c.at.region)
                ++regions, pixels += c.metrics.value("pixels", std::uint64_t{0});
        j[std::string(c_->frame_word())] = {doc(a).frames.size(), doc(b).frames.size()};
        j["regions"] = regions;
        j["pixels_changed"] = pixels;
        return j;
    }

private:
    std::unique_ptr<Codec> c_;

    const Doc& doc(const Document& d) const {
        auto* p = dynamic_cast<const Doc*>(&d);
        if (!p)
            throw Error(ErrorCategory::internal,
                        std::string(info().name) + ": not a document of this format");
        return *p;
    }

    Opts opts(const OptionValues& o) const { return options(o, c_->ignore_option()); }

    std::string prefix(bool seq, std::size_t i) const {
        return seq ? "/" + std::string(c_->frame_word()) + "/" + std::to_string(i) : std::string{};
    }

    static bool ignored(const std::string& key, const Opts& o) {
        for (auto& i : o.ignore)
            if (key.ends_with("/" + i))
                return true;
        return false;
    }

    // Color keys the pixels depend on (Q-PNG-5 analog: /icc, /qoi/colorspace).
    static std::vector<std::string> color_keys(std::initializer_list<const Doc*> ds) {
        std::set<std::string> s;
        for (auto* d : ds)
            for (auto& k : d->keys)
                if (k.kind == "color")
                    s.insert(k.key);
        return {s.begin(), s.end()};
    }

    static void key_changes(std::span<const Key> a, std::span<const Key> b, const std::string& pre, Side side,
                            const Opts& opt, ChangeSet& out) {
        std::map<std::string, const Key*> ma, mb;
        for (auto& k : a)
            ma[k.key] = &k;
        for (auto& k : b)
            mb[k.key] = &k;
        auto value = [](const Key& k) { return Value{k.text, k.value}; };
        for (auto& [k, e] : ma) {
            if (ignored(k, opt))
                continue;
            auto f = mb.find(k);
            if (f == mb.end()) {
                auto c = make_change(side, Op::erase, {pre + k}, e->kind);
                c.before = value(*e);
                out.push_back(std::move(c));
            } else if (f->second->value != e->value) {
                auto c = make_change(side, Op::update, {pre + k}, e->kind);
                c.before = value(*e), c.after = value(*f->second);
                out.push_back(std::move(c));
            }
        }
        for (auto& [k, e] : mb)
            if (!ma.count(k) && !ignored(k, opt)) {
                auto c = make_change(side, Op::insert, {pre + k}, e->kind);
                c.after = value(*e);
                out.push_back(std::move(c));
            }
    }

    static Change region_change(const Raster& a, const Raster& b, const GridRegion& r, Side side,
                                const std::string& pre, const std::vector<std::string>& deps) {
        auto c = make_change(side, Op::update, {pre + "/pixels", r.bounds}, "pixels");
        c.before.hash = raster::hash_cells(a, r.cells);
        c.after.hash = raster::hash_cells(b, r.cells);
        c.depends_on = deps;
        c.metrics = raster::region_metrics(a, b, r);
        return c;
    }

    static Change whole_change(const Frame& a, const Frame& b, Side side, const std::string& pre,
                               const std::vector<std::string>& deps) {
        auto c = make_change(side, Op::update, {pre + "/pixels"}, "pixels");
        c.before.hash = pixel_id(a), c.after.hash = pixel_id(b);
        c.depends_on = deps;
        return c;
    }

    static void encoding_change(const Frame& a, const Frame& b, const std::string& pre, Side side,
                                ChangeSet& out) {
        if (a.encoding != b.encoding) {
            auto c = make_change(side, Op::update, {pre + "/encoding"}, "encoding");
            c.before.text = a.encoding, c.after.text = b.encoding;
            out.push_back(std::move(c));
        }
    }

    static void shape_change(const Frame& a, const Frame& b, const std::string& pre, Side side,
                             ChangeSet& out) {
        if (decodable(a) && decodable(b) && !same_shape(a, b)) {
            auto c = make_change(side, Op::update, {pre + "/shape"}, "shape");
            c.before.text = std::to_string(a.pixels.width) + "x" + std::to_string(a.pixels.height);
            c.after.text = std::to_string(b.pixels.width) + "x" + std::to_string(b.pixels.height);
            out.push_back(std::move(c));
        }
    }

    // Shape and pixel changes of frame b against frame a.
    void pixel_changes(const Frame& a, const Frame& b, const std::string& pre, Side side, const Opts& opt,
                       Limits& budget, const std::vector<std::string>& deps, ChangeSet& out) const {
        if (decodable(a) && decodable(b) && !same_shape(a, b)) {
            shape_change(a, b, pre, side, out);
        } else if (decodable(a) && decodable(b)) {
            Views v({&a.pixels, &b.pixels}, &budget);
            for (auto& r : raster::diff_regions(v[0], v[1], opt.cmp, opt.gap, &budget))
                out.push_back(region_change(v[0], v[1], r, side, pre, deps));
        } else if (!same_pixels(a, b)) {
            out.push_back(whole_change(a, b, side, pre, deps));
        }
    }

    // Changes of b against a. Frames pair by position when the counts agree, else by identity (inserted
    // and erased frames).
    void two_way(const Doc& a, const Doc& b, Side side, const Opts& opt, Limits& budget, ChangeSet& out,
                 bool meta, bool frames) const {
        if (meta) {
            std::map<std::string, std::string> ea(a.encoding.begin(), a.encoding.end()),
                eb(b.encoding.begin(), b.encoding.end());
            for (auto& [k, v] : eb)
                if (ea[k] != v) {
                    auto c = make_change(side, Op::update, {"/encoding/" + k}, "encoding");
                    c.before.text = ea[k], c.after.text = v;
                    out.push_back(std::move(c));
                }
            key_changes(a.keys, b.keys, "", side, opt, out);
        }
        if (!frames)
            return;
        const bool seq = a.sequence || b.sequence;
        const auto deps = color_keys({&a, &b});
        if (a.frames.size() == b.frames.size()) {
            for (std::size_t i = 0; i < a.frames.size(); ++i) {
                auto pre = prefix(seq, i);
                key_changes(a.frames[i].keys, b.frames[i].keys, pre, side, opt, out);
                encoding_change(a.frames[i], b.frames[i], pre, side, out);
                pixel_changes(a.frames[i], b.frames[i], pre, side, opt, budget, deps, out);
            }
            return;
        }
        std::vector<Token> ta, tb;
        for (auto& f : a.frames)
            ta.push_back(frame_id(f).lo);
        for (auto& f : b.frames)
            tb.push_back(frame_id(f).lo);
        auto m = diff_match(ta, tb, DiffAlgorithm::histogram);
        std::vector<bool> ma(a.frames.size()), mb(b.frames.size());
        for (auto [i, j] : m)
            ma[i] = mb[j] = true;
        for (std::size_t i = 0; i < ma.size(); ++i)
            if (!ma[i]) {
                auto c = make_change(side, Op::erase, {prefix(true, i)}, "frame");
                c.before.hash = frame_id(a.frames[i]);
                out.push_back(std::move(c));
            }
        for (std::size_t j = 0; j < mb.size(); ++j)
            if (!mb[j]) {
                auto c = make_change(side, Op::insert, {prefix(true, j)}, "frame");
                c.after.hash = frame_id(b.frames[j]);
                out.push_back(std::move(c));
            }
    }

    KeyList key_list(const Doc& d, bool with_frames, std::map<std::string, std::size_t>* frame_of) const {
        KeyList v;
        for (auto& k : d.keys)
            v.emplace_back(k.key, k.value);
        if (with_frames)
            for (std::size_t i = 0; i < d.frames.size(); ++i)
                for (auto& k : d.frames[i].keys) {
                    v.emplace_back(prefix(d.sequence, i) + k.key, k.value);
                    if (frame_of)
                        (*frame_of)[v.back().first] = i;
                }
        return v;
    }

    const Key* key_of(const Doc& d, const std::string& path, bool seq) const {
        if (auto* k = find_key(d.keys, path))
            return k;
        for (std::size_t i = 0; i < d.frames.size(); ++i) {
            auto pre = prefix(seq, i);
            if (path.starts_with(pre))
                if (auto* k = find_key(d.frames[i].keys, std::string_view(path).substr(pre.size())))
                    return k;
        }
        return nullptr;
    }
};

Side side_of(const std::string& choice) {
    return choice == "theirs" ? Side::theirs : choice == "base" ? Side::base : Side::ours;
}

MergeResult Engine::merge(const Document& base_d, const Document& ours_d, const Document& theirs_d,
                          const MergeOptions& mo) const {
    const Doc& b = doc(base_d);
    const Doc& o = doc(ours_d);
    const Doc& t = doc(theirs_d);
    const std::array<const Doc*, 3> docs{&b, &o, &t};
    const Opts opt = [&] {
        Opts x = opts(mo.ctx.format_options);
        x.ignore.clear(); // merge carries ignored keys
        return x;
    }();
    Limits& budget = mo.ctx.budget;
    MergeResult res;
    for (auto [d, name] : {std::pair{&b, "base"}, std::pair{&o, "ours"}, std::pair{&t, "theirs"}})
        for (auto w : d->warnings) {
            w.message = std::string(name) + ": " + w.message;
            res.warnings.push_back(std::move(w));
        }
    const bool seq = b.sequence || o.sequence || t.sequence;
    const bool positional = b.frames.size() == o.frames.size() && o.frames.size() == t.frames.size();
    const auto deps = color_keys({&b, &o, &t});
    auto& co = res.changes_ours;
    auto& ct = res.changes_theirs;
    auto D = [&](Side s) -> const Doc& { return *docs[static_cast<std::size_t>(s)]; };

    // Metadata and encoding changes; frame changes are added per mode below.
    two_way(b, o, Side::ours, opt, budget, co, true, false);
    two_way(b, t, Side::theirs, opt, budget, ct, true, false);
    auto change_at = [](const ChangeSet& cs, std::string_view path) -> const Change* {
        for (auto& c : cs)
            if (c.at.path == path && !c.at.region)
                return &c;
        return nullptr;
    };
    struct Pending {
        enum Kind { grid, whole, key, chunk, dependency } kind;
        std::size_t frame = 0, index = 0;
    };
    std::vector<Pending> pend;
    auto add_conflict = [&](Address at, std::vector<std::string> ids, std::string reason, Pending p) {
        ids.erase(std::remove(ids.begin(), ids.end(), std::string{}), ids.end());
        res.conflicts.push_back(make_conflict(std::move(at), std::move(ids), std::move(reason)));
        pend.push_back(p);
    };
    auto id_of = [](const Change* c) { return c ? c->id : std::string{}; };
    auto note = [&](const std::vector<std::string>& ids, const char* rule) {
        for (auto& id : ids)
            res.auto_merged.push_back({id, rule});
    };

    // ---- Frames ----
    struct FrameState {
        enum Mode { grid, whole } mode = grid;
        GridResult gr;
        Side take = Side::ours; // whole mode
        std::unique_ptr<Views> v;
    };
    std::vector<FrameState> fs;
    std::map<std::string, std::tuple<std::size_t, Side, std::size_t>>
        region_of; // change -> frame, side, region
    std::vector<SeqChunk> chunks;
    if (positional) {
        fs.resize(o.frames.size());
        for (std::size_t i = 0; i < fs.size(); ++i) {
            const Frame& fb = b.frames[i];
            const Frame& fo = o.frames[i];
            const Frame& ft = t.frames[i];
            const std::string pre = prefix(seq, i);
            auto& st = fs[i];
            for (auto [s, f] : {std::pair{Side::ours, &fo}, std::pair{Side::theirs, &ft}}) {
                auto& cs = s == Side::ours ? co : ct;
                encoding_change(fb, *f, pre, s, cs);
                if (change_at(cs, pre + "/encoding"))
                    note({change_at(cs, pre + "/encoding")->id}, "independent");
            }
            const bool any_opaque = fb.opaque || fo.opaque || ft.opaque;
            const bool lossy = fb.lossy || fo.lossy || ft.lossy;
            if (any_opaque || (lossy && !opt.reencode)) {
                // Whole-frame leaf (raster-misc.md §6: lossy frames merge only as whole frames).
                st.mode = FrameState::whole;
                std::vector<std::string> ids_o, ids_t;
                for (auto [s, f, ids] :
                     {std::tuple{Side::ours, &fo, &ids_o}, std::tuple{Side::theirs, &ft, &ids_t}}) {
                    auto& cs = s == Side::ours ? co : ct;
                    const auto n = cs.size();
                    pixel_changes(fb, *f, pre, s, opt, budget, deps, cs);
                    for (auto k = n; k < cs.size(); ++k)
                        ids->push_back(cs[k].id);
                }
                auto hb = pixel_id(fb), ho = pixel_id(fo), ht = pixel_id(ft);
                auto out = opaque_leaf(&hb, &ho, &ht);
                st.take = out.take.value_or(Side::ours);
                if (out.rule == MergeRule::conflict) {
                    std::vector<std::string> ids = ids_o;
                    ids.insert(ids.end(), ids_t.begin(), ids_t.end());
                    add_conflict({pre + "/pixels"}, ids, any_opaque ? "both-modified" : "encoding-mismatch",
                                 {Pending::whole, i, 0});
                } else {
                    note(ids_o, out.rule == MergeRule::identical ? "identical" : "independent");
                    note(ids_t, out.rule == MergeRule::identical ? "identical" : "independent");
                }
                continue;
            }
            if (lossy)
                res.warnings.push_back({"lossy-reencode", pre + "/pixels",
                                        "lossy frame merged in pixel space and re-encoded (--reencode)"});
            st.v = std::make_unique<Views>(
                std::initializer_list<const Raster*>{&fb.pixels, &fo.pixels, &ft.pixels}, &budget);
            auto& v = *st.v;
            auto R = [&](Side s) -> const Raster& { return v[static_cast<std::size_t>(s)]; };
            const GridShape sb{fb.pixels.width, fb.pixels.height}, so{fo.pixels.width, fo.pixels.height},
                sht{ft.pixels.width, ft.pixels.height};
            st.gr = grid_merge(sb, so, sht,
                               [&](Side x, Side y, std::uint64_t c) { return opt.cmp.same(R(x), R(y), c); },
                               {opt.gap, opt.guard, &budget});
            auto& gr = st.gr;
            shape_change(fb, fo, pre, Side::ours, co);
            shape_change(fb, ft, pre, Side::theirs, ct);
            if (gr.whole) {
                st.mode = FrameState::whole, st.take = *gr.whole;
                for (auto* cs : {&co, &ct})
                    if (auto* sc = change_at(*cs, pre + "/shape"))
                        note({sc->id}, change_at(co, pre + "/shape") && change_at(ct, pre + "/shape")
                                           ? "identical"
                                           : "independent");
                // The reshaped side's pixels come along (no per-region changes on a reshape).
            } else if (gr.shape_conflict) {
                st.mode = FrameState::whole;
                add_conflict({pre + "/shape"},
                             {id_of(change_at(co, pre + "/shape")), id_of(change_at(ct, pre + "/shape"))},
                             "shape-mismatch", {Pending::whole, i, 0});
            } else {
                std::vector<std::string> ids_o, ids_t;
                for (std::size_t k = 0; k < gr.ours.size(); ++k) {
                    co.push_back(
                        region_change(R(Side::base), R(Side::ours), gr.ours[k], Side::ours, pre, deps));
                    ids_o.push_back(co.back().id), region_of[co.back().id] = {i, Side::ours, k};
                }
                for (std::size_t k = 0; k < gr.theirs.size(); ++k) {
                    ct.push_back(
                        region_change(R(Side::base), R(Side::theirs), gr.theirs[k], Side::theirs, pre, deps));
                    ids_t.push_back(ct.back().id), region_of[ct.back().id] = {i, Side::theirs, k};
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
                    add_conflict({pre + "/pixels", g.bounds}, ids, g.reason, {Pending::grid, i, k});
                }
                auto mark = [&](const std::vector<GridRegion>& regs, const std::vector<std::string>& ids) {
                    for (std::size_t k = 0; k < regs.size(); ++k)
                        if (!conflicted.count(ids[k])) {
                            bool same =
                                std::all_of(regs[k].cells.begin(), regs[k].cells.end(), [&](std::uint64_t c) {
                                    return opt.cmp.same(R(Side::ours), R(Side::theirs), c);
                                });
                            res.auto_merged.push_back({ids[k], same ? "identical" : "independent"});
                        }
                };
                mark(gr.ours, ids_o), mark(gr.theirs, ids_t);
                for (auto [r, k] : gr.adjacent)
                    res.warnings.push_back(
                        {"adjacent-edits", Address{pre + "/pixels", gr.ours[r].bounds}.to_string(),
                         "ours and theirs edit adjacent pixels (" + to_string(gr.ours[r].bounds) + ", " +
                             to_string(gr.theirs[k].bounds) + "); --guard=1 makes this a conflict"});
            }
        }
    } else {
        // Frame counts differ: SequenceMerge of whole frames by identity (raster-misc.md §2).
        auto items = [&](const Doc& d) {
            std::vector<SeqItem> v;
            for (auto& f : d.frames)
                v.push_back({frame_id(f), std::nullopt});
            return v;
        };
        auto ib = items(b), io = items(o), it = items(t);
        chunks = sequence_merge(ib, io, it);
        const std::size_t no = co.size(), nt = ct.size();
        two_way(b, o, Side::ours, opt, budget, co, false, true);
        two_way(b, t, Side::theirs, opt, budget, ct, false, true);
        std::set<std::string> in_conflict;
        for (std::size_t k = 0; k < chunks.size(); ++k) {
            auto& ch = chunks[k];
            if (ch.kind != SeqKind::conflict)
                continue;
            std::vector<std::string> ids;
            auto collect = [&](const ChangeSet& cs, const std::vector<std::size_t>& idx, Op op) {
                for (auto i : idx)
                    for (auto& c : cs)
                        if (c.kind == "frame" && c.op == op && c.at.path == prefix(true, i))
                            ids.push_back(c.id);
            };
            collect(co, ch.ours, Op::insert), collect(ct, ch.theirs, Op::insert);
            collect(co, ch.base, Op::erase), collect(ct, ch.base, Op::erase);
            in_conflict.insert(ids.begin(), ids.end());
            const auto at = !ch.ours.empty() ? ch.ours[0] : !ch.base.empty() ? ch.base[0] : 0;
            add_conflict({prefix(true, at)}, ids, ch.reason.empty() ? "both-modified" : ch.reason,
                         {Pending::chunk, 0, k});
        }
        for (auto [cs, n] : {std::pair{&co, no}, std::pair{&ct, nt}})
            for (auto k = n; k < cs->size(); ++k)
                if (!in_conflict.count((*cs)[k].id))
                    res.auto_merged.push_back({(*cs)[k].id, "independent"});
    }

    // ---- Keys (KeyedMerge) and encoding ----
    std::map<std::string, std::size_t> frame_of;
    auto lb = key_list(b, positional, nullptr), lo = key_list(o, positional, &frame_of),
         lt = key_list(t, positional, &frame_of);
    key_list(b, positional, &frame_of);
    if (positional)
        for (std::size_t i = 0; i < o.frames.size(); ++i) {
            const std::string pre = prefix(seq, i);
            key_changes(b.frames[i].keys, o.frames[i].keys, pre, Side::ours, opt, co);
            key_changes(b.frames[i].keys, t.frames[i].keys, pre, Side::theirs, opt, ct);
        }
    auto rows = keyed_merge<std::string, Hash128>(lb, lo, lt);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        auto& row = rows[k];
        auto* xo = change_at(co, row.key);
        auto* xt = change_at(ct, row.key);
        const Key* ko = row.ours ? key_of(o, row.key, seq) : nullptr;
        const Key* kt = row.theirs ? key_of(t, row.key, seq) : nullptr;
        if (row.outcome.rule == MergeRule::conflict && ko && kt && ko->time) {
            row.outcome.take = kt->text > ko->text ? Side::theirs : Side::ours; // newest (SPEC §12.1)
            for (auto* x : {xo, xt})
                if (x)
                    res.auto_merged.push_back({x->id, "newest"});
        } else if (row.outcome.rule == MergeRule::conflict) {
            add_conflict({row.key}, {id_of(xo), id_of(xt)}, row.outcome.reason, {Pending::key, 0, k});
        } else if (row.outcome.rule != MergeRule::unchanged) {
            for (auto* x : {xo, xt})
                if (x)
                    res.auto_merged.push_back(
                        {x->id, row.outcome.rule == MergeRule::identical ? "identical" : "independent"});
        }
    }
    Side enc = Side::ours;
    if (o.encoding == b.encoding && t.encoding != b.encoding)
        enc = Side::theirs;
    for (auto& c : co)
        if (c.at.path.starts_with("/encoding/"))
            res.auto_merged.push_back({c.id, change_at(ct, c.at.path) ? "encoding-ours" : "independent"});
    for (auto& c : ct)
        if (c.at.path.starts_with("/encoding/") && !change_at(co, c.at.path))
            res.auto_merged.push_back({c.id, "independent"});

    // Color dependency (png.md §6.2): pixels painted under one interpretation, profile changed on the other.
    const std::size_t before_deps = res.conflicts.size();
    check_dependencies(co, ct, res.auto_merged, res.conflicts, [](const Change& c, const Change& other) {
        return c.kind == "pixels" && other.kind == "color";
    });
    for (std::size_t k = before_deps; k < res.conflicts.size(); ++k)
        pend.push_back({Pending::dependency, 0, 0});
    {
        std::set<std::string> in_conflict;
        for (auto& c : res.conflicts)
            in_conflict.insert(c.changes.begin(), c.changes.end());
        std::erase_if(res.auto_merged, [&](const AutoMerge& a) {
            return in_conflict.count(a.change) && a.rule != "dependency-touched";
        });
    }

    apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);

    // ---- Apply the decisions ----
    std::vector<std::optional<Side>> key_take(rows.size());
    for (std::size_t k = 0; k < rows.size(); ++k)
        key_take[k] = rows[k].outcome.take;
    std::vector<std::string> chunk_choice(chunks.size(), "ours");
    for (std::size_t k = 0; k < res.conflicts.size(); ++k) {
        auto& c = res.conflicts[k];
        const Side s = side_of(c.resolved_as);
        auto& p = pend[k];
        switch (p.kind) {
        case Pending::grid:
            grid_resolve(fs[p.frame].gr, p.index, s);
            break;
        case Pending::whole:
            fs[p.frame].take = s;
            break;
        case Pending::key:
            key_take[p.index] = take_for(c.resolved_as, rows[p.index].base.has_value(),
                                         rows[p.index].ours.has_value(), rows[p.index].theirs.has_value());
            break;
        case Pending::chunk:
            chunk_choice[p.index] = c.resolved_as;
            break;
        case Pending::dependency:
            for (auto& id : c.changes) {
                if (auto f = region_of.find(id); f != region_of.end() && std::get<1>(f->second) != s) {
                    auto [fi, side, r] = f->second;
                    auto& gr = fs[fi].gr;
                    for (auto cell : (side == Side::ours ? gr.ours : gr.theirs)[r].cells)
                        gr.take[cell] = s;
                }
                for (auto* cs : {&co, &ct})
                    for (auto& ch : *cs)
                        if (ch.id == id && ch.kind == "color" && ch.side != s)
                            for (std::size_t r = 0; r < rows.size(); ++r)
                                if (rows[r].key == ch.at.path)
                                    key_take[r] =
                                        take_for(c.resolved_as, rows[r].base.has_value(),
                                                 rows[r].ours.has_value(), rows[r].theirs.has_value());
            }
            break;
        }
    }

    Plan plan;
    plan.docs = docs;
    plan.enc = enc;
    if (positional) {
        for (std::size_t i = 0; i < fs.size(); ++i) {
            OutFrame of;
            of.src = {i, i, i};
            auto& st = fs[i];
            if (st.mode == FrameState::whole || st.gr.whole || st.gr.shape_conflict) {
                Side s = st.mode == FrameState::whole ? st.take : st.gr.whole.value_or(Side::ours);
                of.pixels = D(s).frames[i].pixels;
                of.same = s;
            } else {
                auto& v = *st.v;
                const Raster& any = v[0];
                budget.charge(any.cells() * any.cell_bytes());
                of.pixels = Raster(any.width, any.height, any.wide);
                const auto cb = any.cell_bytes();
                for (std::uint64_t c = 0; c < any.cells(); ++c)
                    std::memcpy(of.pixels.data.data() + c * cb,
                                v[static_cast<std::size_t>(st.gr.take[c])].at(c), cb);
                of.take = st.gr.take;
            }
            plan.frames.push_back(std::move(of));
        }
    } else {
        for (std::size_t k = 0; k < chunks.size(); ++k)
            for (auto [s, idx] : picks(chunks[k], chunk_choice[k])) {
                OutFrame of;
                of.src[static_cast<std::size_t>(s)] = idx;
                of.pixels = D(s).frames[idx].pixels;
                of.same = s;
                for (auto& key : D(s).frames[idx].keys)
                    of.keys.emplace_back(key.key, s);
                plan.frames.push_back(std::move(of));
            }
    }
    for (std::size_t k = 0; k < rows.size(); ++k) {
        if (!key_take[k])
            continue;
        auto f = frame_of.find(rows[k].key);
        if (f == frame_of.end())
            plan.keys.emplace_back(rows[k].key, *key_take[k]);
        else
            plan.frames[f->second].keys.emplace_back(rows[k].key.substr(prefix(seq, f->second).size()),
                                                     *key_take[k]);
    }
    // A side whose source frame has exactly the merged pixels (ours first).
    for (auto& of : plan.frames)
        if (!of.same || of.take.size())
            for (Side s : {Side::ours, Side::theirs, Side::base})
                if (of.src[static_cast<std::size_t>(s)]) {
                    auto& f = plan.frame(of, s);
                    if (!f.opaque && equal_cells(f.pixels, of.pixels)) {
                        of.same = s;
                        break;
                    }
                }

    // The merged document's keys, for passthrough and validation.
    auto expected = [&](const Plan& p) {
        KeyList v;
        for (auto& [k, s] : p.keys)
            v.emplace_back(k, find_key(p.doc(s).keys, k)->value);
        for (std::size_t i = 0; i < p.frames.size(); ++i)
            for (auto& [k, s] : p.frames[i].keys) {
                auto& f = p.frame(p.frames[i], s);
                v.emplace_back(prefix(true, i) + k, find_key(f.keys, k)->value);
            }
        return sorted(v);
    };
    // Frames whose output side is not the source of their keys: look the key up in that side's frame.
    for (auto& of : plan.frames)
        for (auto& [k, s] : of.keys)
            if (!of.src[static_cast<std::size_t>(s)])
                throw Error(ErrorCategory::internal, "frame key from a side without that frame");
    const KeyList want = expected(plan);

    std::vector<std::byte> bytes;
    for (Side s : {Side::ours, Side::theirs, Side::base}) {
        const Doc& d = D(s);
        if (d.frames.size() != plan.frames.size() || d.encoding != D(enc).encoding)
            continue;
        bool same = true;
        KeyList dk;
        for (auto& k : d.keys)
            dk.emplace_back(k.key, k.value);
        for (std::size_t i = 0; i < d.frames.size() && same; ++i) {
            auto& of = plan.frames[i];
            auto& f = d.frames[i];
            same = of.src[static_cast<std::size_t>(s)] == i &&
                   (f.opaque ? of.same == s : equal_cells(f.pixels, of.pixels));
            if (same && of.src[static_cast<std::size_t>(enc)])
                same = plan.frame(of, enc).encoding == f.encoding;
            for (auto& k : f.keys)
                dk.emplace_back(prefix(true, i) + k.key, k.value);
        }
        if (same && sorted(dk) == want) {
            auto src = d.src.bytes();
            bytes.assign(src.begin(), src.end());
            break;
        }
    }
    const bool passthrough = !bytes.empty();
    if (!passthrough)
        bytes = c_->write(plan, res.warnings);

    // Validation (raster-misc.md §8): the written file must load to the merged document.
    Limits out_budget = budget.fresh();
    std::unique_ptr<Doc> out;
    try {
        out =
            c_->load(ByteSource::from_memory(std::move(bytes), "merged"), out_budget, mo.ctx.format_options);
    } catch (const Error& e) {
        throw Error(ErrorCategory::internal,
                    std::string(info().name) + " writer produced an unreadable file: " + e.what());
    }
    if (!passthrough) {
        bool ok = out->frames.size() == plan.frames.size();
        for (std::size_t i = 0; ok && i < plan.frames.size(); ++i) {
            auto& f = out->frames[i];
            auto& of = plan.frames[i];
            if (f.opaque)
                ok = of.same && plan.frame(of, *of.same).opaque && plan.frame(of, *of.same).coded == f.coded;
            else if (std::find(plan.approximate.begin(), plan.approximate.end(), i) != plan.approximate.end())
                ok = f.pixels.width == of.pixels.width &&
                     f.pixels.height == of.pixels.height; // lossy: as written
            else
                ok = equal_cells(f.pixels, of.pixels);
        }
        KeyList got;
        for (auto& k : out->keys)
            got.emplace_back(k.key, k.value);
        for (std::size_t i = 0; i < out->frames.size(); ++i)
            for (auto& k : out->frames[i].keys)
                got.emplace_back(prefix(true, i) + k.key, k.value);
        // Keys the writer had to drop or rewrite (warned) are not compared.
        auto is_dropped = [&](const auto& kv) {
            return std::any_of(plan.dropped.begin(), plan.dropped.end(),
                               [&](const std::string& d) { return kv.first == d; });
        };
        KeyList w = want;
        std::erase_if(w, is_dropped);
        std::erase_if(got, is_dropped);
        if (!ok || sorted(got) != w)
            throw Error(ErrorCategory::internal, std::string(info().name) +
                                                     " writer validation failed: output does not load to the "
                                                     "merged document");
    }
    if (mo.visual == MergeOptions::Visual::always ||
        (mo.visual == MergeOptions::Visual::on_conflict && res.unresolved_conflicts())) {
        for (std::size_t i = 0; i < fs.size(); ++i)
            if (fs[i].v && !b.frames[i].opaque) {
                res.visual = raster::encode_png(raster::merge_visual((*fs[i].v)[0], fs[i].gr));
                break;
            }
        if (res.visual.empty() && !b.frames.empty() && !b.frames[0].opaque)
            res.visual = raster::encode_png(raster::merge_visual(b.frames[0].pixels, GridResult{}));
    }
    sort_canonical(co), sort_canonical(ct);
    res.document = std::move(out);
    return res;
}

} // namespace

std::unique_ptr<Format> make_format(std::unique_ptr<Codec> c) {
    return std::make_unique<Engine>(std::move(c));
}

} // namespace semsplinter::rastermisc
