#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/text.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace semsplinter {

namespace {

// "1.2 MiB", "830 B".
std::string byte_size(std::uint64_t n) {
    char buf[32];
    if (n < 1024)
        return std::to_string(n) + " B";
    const bool kib = n < (std::uint64_t{1} << 20);
    std::snprintf(buf, sizeof buf, kib ? "%.1f KiB" : "%.1f MiB",
                  static_cast<double>(n) / (kib ? 1024 : 1024 * 1024));
    return buf;
}

// The normal report of a structured format (SPEC §7.2): one line per change.
std::string render_changes(const ChangeSet& cs) {
    std::string o;
    for (auto& c : cs) {
        o += c.op == Op::insert ? '+' : c.op == Op::erase ? '-' : c.op == Op::move ? '>' : '~';
        o += " " + c.at.to_string();
        if (c.before.text && c.after.text)
            o += " " + *c.before.text + " -> " + *c.after.text;
        else if (c.after.text || c.before.text)
            o += " " + *(c.after.text ? c.after.text : c.before.text);
        for (auto& [k, v] : c.metrics.items()) {
            if (k == "bbox" || k == "depth")
                continue;
            std::string val;
            if (v.is_array())
                for (auto& x : v)
                    val += (val.empty() ? "" : ",") + (x.is_string() ? x.get<std::string>() : x.dump());
            else
                val = v.is_string() ? v.get<std::string>() : v.dump();
            o += " " + k + "=" + val;
        }
        o += "\n";
    }
    return o;
}

// "# bytes changed 1.2 MiB: 99.8% encoding, <0.1% integrity; content: 212 px in 1 region" (SPEC §12.6).
std::string churn_line(const Churn& ch, const nlohmann::ordered_json& stats) {
    std::vector<std::pair<std::uint64_t, std::size_t>> parts;
    for (std::size_t k = 0; k < kByteClasses; ++k)
        if (ch.by_class[k])
            parts.emplace_back(ch.by_class[k], k);
    std::stable_sort(parts.begin(), parts.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::string o = "# bytes changed " + byte_size(ch.total()) + ":";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        double pct = 100.0 * static_cast<double>(parts[i].first) / static_cast<double>(ch.total());
        char buf[16];
        std::snprintf(buf, sizeof buf, pct < 0.1 ? "<0.1%%" : "%.1f%%", pct);
        o += std::string(i ? ", " : " ") + buf + " " +
             std::string(to_string(static_cast<ByteClass>(parts[i].second)));
    }
    if (stats.value("regions", std::uint64_t{0})) {
        auto regions = stats.value("regions", std::uint64_t{0});
        o += "; content: " + std::to_string(stats.value("pixels_changed", std::uint64_t{0})) + " px in " +
             std::to_string(regions) + (regions == 1 ? " region" : " regions");
    }
    return o + "\n";
}

// Regions of different kinds count as overlapping.
bool overlaps(const Region& x, const Region& y) {
    if (auto *a = std::get_if<Rect>(&x), *b = std::get_if<Rect>(&y); a && b)
        return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
    if (auto *a = std::get_if<Range>(&x), *b = std::get_if<Range>(&y); a && b)
        return a->start < b->start + b->length && b->start < a->start + a->length;
    return true;
}

// A change belongs to a dump line when either address lies within the other; a change with a region
// needs a line with an overlapping region.
bool touches(const Change& c, const Address& line) {
    if (!path_within(c.at.path, line.path) && !path_within(line.path, c.at.path))
        return false;
    return !c.at.region || (line.region && overlaps(*c.at.region, *line.region));
}

// The hunk's ADDRESS (CLI.md §9): the lines' common path, plus the union of their rect or range regions
// when every line has that path. Empty when no line has an address.
std::string common_address(const std::vector<const Address*>& ls) {
    if (ls.empty())
        return {};
    std::string p = ls[0]->path;
    for (auto* l : ls)
        while (!path_within(l->path, p)) {
            auto k = p.rfind('/');
            p.resize(k == std::string::npos ? 0 : k);
        }
    std::optional<Region> r;
    for (auto* l : ls) {
        if (l->path != p || (r && l->region && l->region->index() != r->index())) {
            r.reset();
            break;
        }
        if (!l->region)
            continue; // the whole unit's summary line, such as PNG's "pixels xxh3=..."
        if (!r)
            r = l->region;
        else if (auto* a = std::get_if<Rect>(&*r)) {
            const Rect& b = std::get<Rect>(*l->region);
            auto x1 = std::max(a->x + a->w, b.x + b.w), y1 = std::max(a->y + a->h, b.y + b.h);
            a->x = std::min(a->x, b.x), a->y = std::min(a->y, b.y), a->w = x1 - a->x, a->h = y1 - a->y;
        } else if (auto* c = std::get_if<Range>(&*r)) {
            const Range& d = std::get<Range>(*l->region);
            auto e = std::max(c->start + c->length, d.start + d.length);
            c->start = std::min(c->start, d.start), c->length = e - c->start;
        } else {
            r.reset();
            break;
        }
    }
    return p.empty() ? "/" : Address{p, r}.to_string();
}

// One --annotate comment line (Q-TEXT-9): "# 212 px changed, bbox 10,20 32x8, max ΔE00 14.2" for pixel
// regions, else the change's normal-report line.
std::string note_line(const Change& c) {
    auto& m = c.metrics;
    auto bbox = m.find("bbox");
    if (c.kind != "pixels" || !m.contains("pixels") || bbox == m.end() || bbox->size() != 4)
        return "# " + render_changes({c});
    auto n = [&](std::size_t i) { return std::to_string((*bbox)[i].get<std::uint64_t>()); };
    std::string o = "# " + std::to_string(m.at("pixels").get<std::uint64_t>()) + " px changed, bbox " + n(0) +
                    "," + n(1) + " " + n(2) + "x" + n(3);
    if (auto de = m.find("delta_e00_max"); de != m.end() && de->is_number()) {
        char buf[32];
        std::snprintf(buf, sizeof buf,
                      ", max \xCE\x94"
                      "E00 %.1f",
                      de->get<double>());
        o += buf;
    }
    return o + "\n";
}

class GenericComparison final : public Comparison {
public:
    GenericComparison(const Format& f, const Document& a, const Document& b, const DiffOptions& o)
        : f_(f), a_(a), b_(b), o_(o), cs_(f.diff(a, b, o)) {}

    // Encoding-only changes leave the files semantically identical (SPEC §12.2).
    bool differs() const override {
        return std::any_of(cs_.begin(), cs_.end(), [](const Change& c) { return c.kind != "encoding"; });
    }
    ChangeSet changes() const override { return cs_; }
    nlohmann::ordered_json stats() const override { return f_.stats(a_, b_, cs_); }

    std::vector<std::string> render(std::string& out, const RenderRequest& r) const override {
        if (r.style == OutputStyle::ed || r.style == OutputStyle::forward_ed || r.style == OutputStyle::rcs)
            throw Error(ErrorCategory::unsupported, std::string("option '") +
                                                        (r.style == OutputStyle::ed    ? "-e"
                                                         : r.style == OutputStyle::rcs ? "-n"
                                                                                       : "-f") +
                                                        "' not supported for format '" +
                                                        std::string(f_.info().name) + "'");
        if (cs_.empty() && r.style != OutputStyle::side_by_side)
            return {};
        if (r.style == OutputStyle::normal) {
            out += render_changes(cs_);
            if (r.churn && r.churn->total())
                out += churn_line(*r.churn, stats());
            return {};
        }
        // Unified, context, side by side: the dump lines, diffed as text (SPEC §7.2).
        const Format* text = o_.ctx.registry.find("text");
        if (!text)
            throw Error(ErrorCategory::internal, "the text format is not registered");
        std::vector<Address> la, lb; // per dump line, when the format provides them
        auto dump = [&](const Document& d, std::vector<Address>& at) {
            std::ostringstream os;
            f_.dump(d, os, {o_.ctx, &at});
            auto s = os.str();
            if (at.size() != static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n')) +
                                 (!s.empty() && s.back() != '\n'))
                at.clear();
            auto* p = reinterpret_cast<const std::byte*>(s.data());
            return text->load(ByteSource::from_memory({p, p + s.size()}), {o_.ctx});
        };
        auto ta = dump(a_, la);
        auto tb = dump(b_, lb);
        RenderRequest dumps = r;
        dumps.churn = nullptr;
        // The addressed lines a hunk changes, on both sides.
        auto lines = [&](const Edit& first, const Edit& last) {
            std::vector<const Address*> v;
            auto add = [&](const std::vector<Address>& at, std::size_t s, std::size_t e) {
                for (auto i = s; i < e && i < at.size(); ++i)
                    if (!at[i].path.empty())
                        v.push_back(&at[i]);
            };
            for (const Edit* e = &first; e <= &last; ++e) {
                add(la, e->a0, e->a1);
                add(lb, e->b0, e->b1);
            }
            return v;
        };
        const bool addressed = !la.empty() || !lb.empty(); // else the dump diff stays as it was
        if (addressed && !dumps.options.hunk_address)
            dumps.options.hunk_address = [&](const Edit& first, const Edit& last) {
                return common_address(lines(first, last));
            };
        if (addressed && dumps.options.annotate)
            // ponytail: changes x lines per hunk; index changes by path if huge change sets get slow.
            dumps.options.hunk_note = [&](const Edit& first, const Edit& last) {
                auto ls = lines(first, last);
                std::string o;
                for (auto& c : cs_)
                    if (std::any_of(ls.begin(), ls.end(), [&](const Address* l) { return touches(c, *l); }))
                        o += note_line(c);
                return o;
            };
        return text->compare(*ta, *tb, o_)->render(out, dumps);
    }

private:
    const Format& f_;
    const Document &a_, &b_;
    const DiffOptions& o_;
    ChangeSet cs_;
};

} // namespace

std::unique_ptr<Comparison> Format::compare(const Document& a, const Document& b,
                                            const DiffOptions& o) const {
    return std::make_unique<GenericComparison>(*this, a, b, o);
}

std::vector<std::byte> Format::visual(const Document&, const Document&, const DiffOptions&) const {
    throw Error(ErrorCategory::unsupported,
                "--visual: format '" + std::string(info().name) + "' has no visual diff");
}

std::optional<std::vector<std::byte>> Format::payload(const Document&, const Address&) const {
    throw Error(ErrorCategory::unsupported,
                "patches for format '" + std::string(info().name) + "' are not supported");
}

std::unique_ptr<Document> Format::implant(const Document&, std::span<const UnitValue>,
                                          const LoadOptions&) const {
    throw Error(ErrorCategory::unsupported,
                "patches for format '" + std::string(info().name) + "' are not supported");
}

void Registry::add(std::unique_ptr<Format> f) {
    formats_.push_back(std::move(f));
}

const Format* Registry::find(std::string_view name) const noexcept {
    for (auto& f : formats_)
        if (f->info().name == name)
            return f.get();
    return nullptr;
}

namespace {
std::string lower_extension(std::string_view name) {
    auto slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    std::string ext(name.substr(dot));
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}
} // namespace

const Format& Registry::detect(std::span<const std::byte> content, std::string_view logical_name,
                               std::string_view explicit_name) const {
    if (!explicit_name.empty()) {
        if (auto* f = find(explicit_name))
            return *f;
        throw Error(ErrorCategory::unsupported, "unknown format '" + std::string(explicit_name) + "'");
    }
    auto head = content.first(std::min<std::size_t>(content.size(), 64 * 1024));
    if (auto ext = lower_extension(logical_name); !ext.empty())
        for (auto& f : formats_)
            if (std::find(f->info().extensions.begin(), f->info().extensions.end(), ext) !=
                    f->info().extensions.end() &&
                f->probe(head, logical_name).confidence > 0)
                return *f;
    const Format* best = nullptr;
    int conf = 0;
    for (auto& f : formats_) {
        auto name = f->info().name;
        if (name == "text" || name == "binary")
            continue; // fallbacks, step 4
        if (int c = f->probe(head, logical_name).confidence; c > conf)
            best = f.get(), conf = c;
    }
    if (best)
        return *best;
    auto* fallback = find(looks_binary(head) ? "binary" : "text");
    if (!fallback)
        throw Error(ErrorCategory::internal, "text and binary formats are not registered");
    return *fallback;
}

} // namespace semsplinter
