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
        auto dump = [&](const Document& d) {
            std::ostringstream os;
            f_.dump(d, os, {o_.ctx});
            auto s = os.str();
            auto* p = reinterpret_cast<const std::byte*>(s.data());
            return text->load(ByteSource::from_memory({p, p + s.size()}), {o_.ctx});
        };
        auto ta = dump(a_), tb = dump(b_);
        RenderRequest dumps = r;
        dumps.churn = nullptr;
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
