#include <semsplinter/error.hpp>
#include <semsplinter/formats/text.hpp>

#include <algorithm>
#include <array>

namespace semsplinter::text {

std::string_view TextDoc::line(std::size_t i) const {
    if (!decoded.empty())
        return decoded[i];
    auto b = src.bytes();
    return {reinterpret_cast<const char*>(b.data()) + lines[i].off, lines[i].len};
}

Eol TextDoc::dominant_eol() const noexcept {
    std::array<std::size_t, 4> n{};
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto e = lines[i].eol;
        if (e == Eol::lf && line(i).ends_with('\r')) // split on LF only: the CR stays in the content
            e = Eol::crlf;
        ++n[static_cast<std::size_t>(e)];
    }
    if (n[2] > n[1] && n[2] >= n[3])
        return Eol::crlf;
    if (n[3] > n[1] && n[3] > n[2])
        return Eol::cr;
    return Eol::lf;
}

std::unique_ptr<TextDoc> load(ByteSource src, const LoadOptions& opt) {
    auto doc = std::make_unique<TextDoc>();
    auto bytes = src.bytes();
    doc->enc = detect_encoding(bytes);
    if (opt.bom_is_content && doc->enc.encoding == Encoding::utf8)
        doc->enc.bom = 0;
    doc->lines = split_lines(bytes, doc->enc, opt.split);
    if (opt.strip_trailing_cr && doc->enc.encoding == Encoding::utf8)
        for (auto& l : doc->lines)
            if (l.eol == Eol::lf && l.len && bytes[l.off + l.len - 1] == std::byte{'\r'})
                --l.len;
    if (doc->enc.encoding != Encoding::utf8) {
        doc->decoded.reserve(doc->lines.size());
        for (auto& l : doc->lines) {
            doc->decoded.push_back(line_utf8(bytes, l, doc->enc.encoding));
            if (opt.strip_trailing_cr && l.eol == Eol::lf && !doc->decoded.back().empty() &&
                doc->decoded.back().back() == '\r')
                doc->decoded.back().pop_back();
        }
    }
    doc->src = std::move(src);
    return doc;
}

bool TextDiff::differs() const noexcept {
    for (std::size_t k = 0; k < edits.size(); ++k)
        if (ignorable.empty() || !ignorable[k])
            return true;
    return false;
}

TextDiff diff(const TextDoc& a, const TextDoc& b, const DiffOptions& opt) {
    LineClassifier cls(opt.normalize);
    cls.reserve(a.lines.size() + b.lines.size());
    auto classes = [&](const TextDoc& d) {
        std::vector<std::uint32_t> v(d.lines.size());
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = cls.classify(d.line(i), d.lines[i].eol != Eol::none || opt.ignore_missing_newline);
        return v;
    };
    auto ca = classes(a), cb = classes(b);
    TextDiff r;
    const auto& n = opt.normalize;
    if (n.ignore_case || n.ignore_tab_expansion || n.ignore_trailing_space || n.ignore_space_change ||
        n.ignore_all_space || opt.ignore_missing_newline) {
        // GNU finds the identical head and tail on raw bytes.
        LineClassifier raw;
        std::vector<std::uint32_t> xa(a.lines.size()), xb(b.lines.size());
        for (std::size_t i = 0; i < xa.size(); ++i)
            xa[i] = raw.classify(a.line(i), a.lines[i].eol != Eol::none);
        for (std::size_t i = 0; i < xb.size(); ++i)
            xb[i] = raw.classify(b.line(i), b.lines[i].eol != Eol::none);
        auto algo = opt.algorithm;
        algo.raw_a = xa, algo.raw_b = xb;
        r.edits = diff_lines(ca, cb, algo);
    } else {
        r.edits = diff_lines(ca, cb, opt.algorithm);
    }
    if (opt.ignore_blank_lines || !opt.ignore_re.empty()) {
        auto skip = [&](std::string_view s) {
            if (opt.ignore_blank_lines && cls.normalize(s).empty())
                return true;
            for (auto& re : opt.ignore_re)
                if (std::regex_search(s.begin(), s.end(), re))
                    return true;
            return false;
        };
        r.ignorable.resize(r.edits.size());
        for (std::size_t k = 0; k < r.edits.size(); ++k) {
            auto& e = r.edits[k];
            bool all = true;
            for (auto i = e.a0; all && i < e.a1; ++i)
                all = skip(a.line(i));
            for (auto j = e.b0; all && j < e.b1; ++j)
                all = skip(b.line(j));
            r.ignorable[k] = all;
        }
    }
    return r;
}

DiffSide diff_side(const TextDoc& d, std::string label) {
    DiffSide s;
    s.lines.reserve(d.lines.size());
    for (std::size_t i = 0; i < d.lines.size(); ++i)
        s.lines.push_back(d.line(i));
    s.missing_newline = d.missing_newline();
    s.label = std::move(label);
    return s;
}

namespace {

constexpr std::size_t kMaxValueText = 4096;

Value lines_value(const TextDoc& d, std::size_t s, std::size_t e) {
    std::string text;
    for (auto i = s; i < e; ++i) {
        text.append(d.line(i));
        text += '\n';
    }
    Value v;
    v.hash = hash128(text);
    if (text.size() <= kMaxValueText)
        v.text = std::move(text);
    return v;
}

Address lines_at(std::size_t s, std::size_t e) {
    return {"/lines", Range{s, e - s}};
}

} // namespace

ChangeSet changes(const TextDoc& a, const TextDoc& b, const TextDiff& d) {
    ChangeSet cs;
    for (std::size_t k = 0; k < d.edits.size(); ++k) {
        if (!d.ignorable.empty() && d.ignorable[k])
            continue;
        auto& e = d.edits[k];
        Op op = e.a0 == e.a1 ? Op::insert : e.b0 == e.b1 ? Op::erase : Op::update;
        auto c = make_change(Side::theirs, op, op == Op::erase ? lines_at(e.a0, e.a1) : lines_at(e.b0, e.b1),
                             "lines");
        if (op != Op::insert)
            c.before = lines_value(a, e.a0, e.a1);
        if (op != Op::erase)
            c.after = lines_value(b, e.b0, e.b1);
        c.metrics["lines_deleted"] = e.a1 - e.a0;
        c.metrics["lines_inserted"] = e.b1 - e.b0;
        cs.push_back(std::move(c));
    }
    return cs;
}

nlohmann::ordered_json metrics(const TextDoc& a, const TextDoc& b, const TextDiff& d) {
    std::size_t del = 0, ins = 0, n = 0;
    for (std::size_t k = 0; k < d.edits.size(); ++k)
        if (d.ignorable.empty() || !d.ignorable[k])
            del += d.edits[k].a1 - d.edits[k].a0, ins += d.edits[k].b1 - d.edits[k].b0, ++n;
    std::size_t total = a.lines.size() + b.lines.size();
    nlohmann::ordered_json j;
    j["lines_a"] = a.lines.size();
    j["lines_b"] = b.lines.size();
    j["lines_deleted"] = del;
    j["lines_inserted"] = ins;
    j["changes"] = n;
    j["similarity"] = total ? 1.0 - static_cast<double>(del + ins) / static_cast<double>(total) : 1.0;
    j["encoding_a"] = to_string(a.enc.encoding);
    j["encoding_b"] = to_string(b.enc.encoding);
    return j;
}

namespace {

std::string_view algorithm_name(DiffAlgorithm a) {
    return a == DiffAlgorithm::myers      ? "myers"
           : a == DiffAlgorithm::minimal  ? "minimal"
           : a == DiffAlgorithm::patience ? "patience"
                                          : "histogram";
}

// GNU diff output (text.md §5): every style, exactly as GNU renders it.
class TextComparison final : public Comparison {
public:
    TextComparison(const TextDoc& a, const TextDoc& b, const DiffOptions& o) : a_(a), b_(b), o_(o) {
        auto x = a.src.bytes(), y = b.src.bytes();
        if (!std::equal(x.begin(), x.end(), y.begin(), y.end()))
            d_ = text::diff(a, b, o);
    }
    bool differs() const override { return d_.differs(); }
    ChangeSet changes() const override { return text::changes(a_, b_, d_); }
    nlohmann::ordered_json stats() const override { return metrics(a_, b_, d_); }
    nlohmann::ordered_json options() const override {
        return {{"algorithm", algorithm_name(o_.algorithm.algorithm)}};
    }
    std::vector<std::string> render(std::string& out, const RenderRequest& r) const override {
        if (d_.differs() || r.style == OutputStyle::side_by_side) // -y shows equal files too
            semsplinter::render(out, r.style, diff_side(a_, r.labels[0]), diff_side(b_, r.labels[1]),
                                d_.edits, d_.ignorable, r.options);
        // ed scripts cannot express a missing final newline: GNU compares as if it were there, then fails.
        std::vector<std::string> errors;
        if (r.style == OutputStyle::ed || r.style == OutputStyle::forward_ed)
            for (std::size_t k = 0; k < 2; ++k)
                if ((k ? b_ : a_).missing_newline())
                    errors.push_back(r.paths[k] + ": No newline at end of file");
        return errors;
    }

private:
    const TextDoc &a_, &b_;
    const DiffOptions& o_;
    TextDiff d_;
};

class TextFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{"text", "Plain text (GNU diff/diff3 compatible)", {}, {}, {}};
        return i;
    }
    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        return {looks_binary(head) ? 0 : 10};
    }
    std::unique_ptr<Document> load(ByteSource src, const semsplinter::LoadOptions& o) const override {
        o.ctx.budget.charge(src.size() / 2 + 16); // ~16 bytes per line on top of the input (text.md §5.5)
        return text::load(std::move(src), o.text);
    }
    ChangeSet diff(const Document& a, const Document& b, const semsplinter::DiffOptions& o) const override {
        auto &ta = cast(a), &tb = cast(b);
        return changes(ta, tb, text::diff(ta, tb, o.text));
    }
    std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                        const semsplinter::DiffOptions& o) const override {
        return std::make_unique<TextComparison>(cast(a), cast(b), o.text);
    }
    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const semsplinter::MergeOptions& o) const override {
        text::MergeOptions mo = o.text;
        mo.conflict = o.conflict;
        mo.labels = o.labels;
        if (mo.engine != Engine::gnu) // Git and native write CRLF markers into CRLF files
            mo.crlf_markers = cast(ours).dominant_eol() == Eol::crlf;
        auto m = text::merge(cast(base), cast(ours), cast(theirs), mo);
        m.result.marked = m.marked;
        m.result.document = text::load(ByteSource::from_memory(std::move(m.bytes)));
        return std::move(m.result);
    }
    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(cast(d).src.bytes());
    }
    void dump(const Document& d, std::ostream& os, const DumpOptions&) const override {
        auto& t = cast(d);
        for (std::size_t i = 0; i < t.lines.size(); ++i)
            os << t.line(i) << '\n';
    }
    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet&) const override {
        auto &ta = cast(a), &tb = cast(b);
        return metrics(ta, tb, text::diff(ta, tb));
    }

private:
    static const TextDoc& cast(const Document& d) {
        auto* t = dynamic_cast<const TextDoc*>(&d);
        if (!t)
            throw Error(ErrorCategory::internal, "text: foreign document");
        return *t;
    }
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<TextFormat>();
}

} // namespace semsplinter::text
