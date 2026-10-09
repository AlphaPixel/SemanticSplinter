#include <semsplinter/error.hpp>
#include <semsplinter/formats/binary.hpp>
#include <semsplinter/report.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace semsplinter::binary {

namespace {

// Gear table: splitmix64 from a fixed seed, so chunk boundaries are identical on every platform.
const std::array<std::uint64_t, 256>& gear() {
    static const auto table = [] {
        std::array<std::uint64_t, 256> t{};
        std::uint64_t x = 0x5EED'C0DE'5EED'C0DEull;
        for (auto& v : t) {
            std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v = z ^ (z >> 31);
        }
        return t;
    }();
    return table;
}

// Top-bit masks: the cut test then depends on the last 64 bytes only, so boundaries resynchronize.
constexpr std::uint64_t top_bits(int n) {
    return ~std::uint64_t{0} << (64 - n);
}

std::size_t cut(std::span<const std::byte> d, std::size_t min, std::size_t avg, std::size_t max, int bits) {
    std::size_t n = std::min(d.size(), max);
    if (n <= min)
        return n;
    const std::uint64_t strict = top_bits(bits + 2), loose = top_bits(bits - 2);
    const auto& g = gear();
    std::uint64_t h = 0;
    std::size_t i = min, normal = std::min(avg, n);
    for (; i < normal; ++i) {
        h = (h << 1) + g[std::to_integer<std::uint8_t>(d[i])];
        if (!(h & strict))
            return i + 1;
    }
    for (; i < n; ++i) {
        h = (h << 1) + g[std::to_integer<std::uint8_t>(d[i])];
        if (!(h & loose))
            return i + 1;
    }
    return n;
}

std::uint64_t hash64(std::span<const std::byte> s) {
    return hash128(s).lo;
}

std::string hex(std::span<const std::byte> s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (auto b : s) {
        auto v = std::to_integer<unsigned>(b);
        out += digits[v >> 4];
        out += digits[v & 15];
    }
    return out;
}

void refine(std::span<const std::byte> a, std::span<const std::byte> b, std::size_t a0, std::size_t a1,
            std::size_t b0, std::size_t b1, std::vector<Edit>& out) {
    while (a0 < a1 && b0 < b1 && a[a0] == b[b0])
        ++a0, ++b0;
    while (a1 > a0 && b1 > b0 && a[a1 - 1] == b[b1 - 1])
        --a1, --b1;
    if (a0 == a1 && b0 == b1)
        return;
    constexpr std::size_t kExact = 64 * 1024;
    if (a0 == a1 || b0 == b1 || a1 - a0 > kExact || b1 - b0 > kExact) {
        out.push_back({a0, a1, b0, b1});
        return;
    }
    std::vector<Token> ta(a1 - a0), tb(b1 - b0);
    for (std::size_t i = 0; i < ta.size(); ++i)
        ta[i] = std::to_integer<std::uint8_t>(a[a0 + i]);
    for (std::size_t j = 0; j < tb.size(); ++j)
        tb[j] = std::to_integer<std::uint8_t>(b[b0 + j]);
    std::vector<char> ca(ta.size(), 1), cb(tb.size(), 1);
    for (auto [x, y] : diff_match(ta, tb, DiffAlgorithm::myers))
        ca[x] = cb[y] = 0;
    for (auto e : edits_from_flags(ca, cb))
        out.push_back({a0 + e.a0, a0 + e.a1, b0 + e.b0, b0 + e.b1});
}

} // namespace

std::vector<Chunk> chunk(std::span<const std::byte> s) {
    const bool large = s.size() > (std::size_t{256} << 20);
    const std::size_t min = large ? 16 << 10 : 2 << 10, avg = large ? 64 << 10 : 8 << 10,
                      max = large ? 256 << 10 : 64 << 10;
    const int bits = large ? 16 : 13;
    std::vector<Chunk> out;
    for (std::size_t pos = 0; pos < s.size();) {
        auto len = cut(s.subspan(pos), min, avg, max, bits);
        out.push_back({pos, len, hash64(s.subspan(pos, len))});
        pos += len;
    }
    return out;
}

std::unique_ptr<BinDoc> load(ByteSource src) {
    auto d = std::make_unique<BinDoc>();
    d->hash = hash128(src.bytes());
    d->chunks = chunk(src.bytes());
    d->src = std::move(src);
    return d;
}

BinDiff diff(const BinDoc& da, const BinDoc& db, std::size_t gap) {
    auto a = da.src.bytes(), b = db.src.bytes();
    BinDiff r;
    if (a.size() == b.size() && da.hash == db.hash)
        return r;
    std::vector<Edit> raw;
    std::uint64_t anchored = 0;
    constexpr std::size_t kSmall = 64 * 1024;
    if (a.size() <= kSmall && b.size() <= kSmall) {
        refine(a, b, 0, a.size(), 0, b.size(), raw);
        // Coverage for small inputs: bytes of B outside the reported ranges.
        anchored = b.size();
        for (auto& e : raw)
            anchored -= e.b1 - e.b0;
    } else {
        // Anchor diff over chunk classes; equal hashes are confirmed by memcmp.
        std::unordered_map<std::uint64_t, std::vector<std::pair<std::span<const std::byte>, std::uint64_t>>>
            reps;
        std::uint64_t next = 0;
        auto classify = [&](std::span<const std::byte> s, const Chunk& c) {
            auto bytes = s.subspan(c.off, c.len);
            auto& bucket = reps[c.hash];
            for (auto& [rep, id] : bucket)
                if (rep.size() == bytes.size() && std::memcmp(rep.data(), bytes.data(), bytes.size()) == 0)
                    return id;
            bucket.emplace_back(bytes, next);
            return next++;
        };
        std::vector<Token> ta, tb;
        for (auto& c : da.chunks)
            ta.push_back(classify(a, c));
        for (auto& c : db.chunks)
            tb.push_back(classify(b, c));
        std::size_t pa = 0, pb = 0;
        for (auto [i, j] : diff_match(ta, tb, DiffAlgorithm::histogram)) {
            auto &ca = da.chunks[i], &cb = db.chunks[j];
            refine(a, b, pa, ca.off, pb, cb.off, raw);
            anchored += cb.len;
            pa = ca.off + ca.len, pb = cb.off + cb.len;
        }
        refine(a, b, pa, a.size(), pb, b.size(), raw);
    }
    for (auto& e : raw) {
        if (!r.ranges.empty() && e.a0 - r.ranges.back().a1 < gap) {
            r.ranges.back().a1 = e.a1, r.ranges.back().b1 = e.b1;
            continue;
        }
        r.ranges.push_back(e);
    }
    for (auto& e : r.ranges) {
        auto la = e.a1 - e.a0, lb = e.b1 - e.b0, common = std::min(la, lb);
        r.replaced += common, r.inserted += lb - common, r.deleted += la - common;
    }
    r.anchor_coverage = b.empty() ? 1.0 : static_cast<double>(anchored) / static_cast<double>(b.size());
    return r;
}

namespace {
Value bytes_value(std::span<const std::byte> s) {
    Value v;
    v.hash = hash128(s);
    if (s.size() <= 64)
        v.text = hex(s);
    return v;
}
} // namespace

ChangeSet changes(const BinDoc& da, const BinDoc& db, const BinDiff& d) {
    auto a = da.src.bytes(), b = db.src.bytes();
    ChangeSet cs;
    for (auto& e : d.ranges) {
        Op op = e.a0 == e.a1 ? Op::insert : e.b0 == e.b1 ? Op::erase : Op::update;
        Address at = op == Op::erase ? Address{"/bytes", Range{e.a0, e.a1 - e.a0}}
                                     : Address{"/bytes", Range{e.b0, e.b1 - e.b0}};
        auto c = make_change(Side::theirs, op, std::move(at), "bytes");
        if (op != Op::insert)
            c.before = bytes_value(a.subspan(e.a0, e.a1 - e.a0));
        if (op != Op::erase)
            c.after = bytes_value(b.subspan(e.b0, e.b1 - e.b0));
        cs.push_back(std::move(c));
    }
    return cs;
}

nlohmann::ordered_json metrics(const BinDoc& a, const BinDoc& b, const BinDiff& d) {
    nlohmann::ordered_json j;
    j["size_a"] = a.src.size();
    j["size_b"] = b.src.size();
    j["bytes_inserted"] = d.inserted;
    j["bytes_deleted"] = d.deleted;
    j["bytes_replaced"] = d.replaced;
    j["ranges"] = d.ranges.size();
    j["anchor_coverage"] = d.anchor_coverage;
    bool updates_only = std::all_of(d.ranges.begin(), d.ranges.end(),
                                    [](const Edit& e) { return e.a1 - e.a0 == e.b1 - e.b0; });
    j["in_place"] = a.src.size() == b.src.size() && updates_only;
    return j;
}

void dump(const BinDoc& d, std::ostream& os, const DumpOptions& o) {
    auto s = d.src.bytes();
    os << "# binary " << s.size() << " bytes " << hash_ref(d.hash) << '\n';
    const bool rows = s.size() <= o.max_dump;
    char buf[16];
    for (std::size_t k = 0; k < d.chunks.size(); ++k) {
        auto& c = d.chunks[k];
        auto bytes = s.subspan(c.off, c.len);
        os << "## chunk " << k << ' ' << c.len << " bytes " << hash_ref(hash128(bytes)) << '\n';
        if (!rows)
            continue;
        for (std::size_t r = 0; r < c.len; r += 16) {
            auto row = bytes.subspan(r, std::min<std::size_t>(16, c.len - r));
            std::snprintf(buf, sizeof buf, "%08zx", o.absolute_offsets ? c.off + r : r);
            std::string h = hex(row);
            h.resize(32, ' ');
            os << buf << "  " << h << "  |";
            for (auto b : row) {
                auto v = std::to_integer<unsigned char>(b);
                os << (v >= 0x20 && v < 0x7F ? static_cast<char>(v) : '.');
            }
            os << "|\n";
        }
    }
}

MergeResult merge(const BinDoc& base, const BinDoc& ours, const BinDoc& theirs, const ConflictOptions& co,
                  const ByteMergeOptions* bm, std::vector<std::byte>& output) {
    MergeResult r;
    auto pick = [&](const BinDoc& d) {
        auto s = d.src.bytes();
        output.assign(s.begin(), s.end());
    };
    auto whole_change = [&](Side side) {
        auto c = make_change(side, Op::update, {"/"}, "bytes");
        (side == Side::ours ? r.changes_ours : r.changes_theirs).push_back(c);
        return c.id;
    };
    const bool om = ours.hash != base.hash, ot = theirs.hash != base.hash;
    if (!om || !ot || ours.hash == theirs.hash || !bm) {
        if (!om && !ot) {
            pick(ours);
            return r;
        }
        if (om && ot && ours.hash != theirs.hash) { // OpaqueLeaf conflict (SPEC §5.1)
            auto c =
                make_conflict({"/"}, {whole_change(Side::ours), whole_change(Side::theirs)}, "both-modified");
            r.conflicts.push_back(c);
            apply_conflict_policy(r.conflicts, co, r.warnings);
            auto& how = r.conflicts.back().resolved_as;
            pick(how == "theirs" ? theirs : how == "base" ? base : ours);
            return r;
        }
        if (om && ot) {
            auto a = whole_change(Side::ours), b = whole_change(Side::theirs);
            r.auto_merged.push_back({a, "identical"});
            r.auto_merged.push_back({b, "identical"});
        } else {
            r.auto_merged.push_back({whole_change(om ? Side::ours : Side::theirs), "independent"});
        }
        pick(om ? ours : theirs);
        return r;
    }

    // Byte-level diff3 (--binary-merge).
    auto ro = diff(base, ours, 1), rt = diff(base, theirs, 1);
    auto sb = base.src.bytes(), so = ours.src.bytes(), st = theirs.src.bytes();
    auto record = [&](Side side, const BinDiff& d) {
        ChangeSet cs = changes(base, side == Side::ours ? ours : theirs, d);
        for (auto& c : cs) {
            c.side = side;
            c.id = make_change_id(side, c.op, c.at);
        }
        return cs;
    };
    r.changes_ours = record(Side::ours, ro);
    r.changes_theirs = record(Side::theirs, rt);
    r.warnings.push_back({"binary-merge-unverified", "/",
                          "byte-level merge of an unrecognized format. Internal offsets, checksums, and "
                          "compression are not understood. Verify the result."});

    // Size rule: a size change followed by the other side's later edit likely shifted offsets.
    auto first_resize = [](const BinDiff& d) -> std::optional<std::size_t> {
        for (auto& e : d.ranges)
            if (e.a1 - e.a0 != e.b1 - e.b0)
                return e.a0;
        return std::nullopt;
    };
    auto edits_after = [](const BinDiff& d, std::size_t pos) {
        return std::any_of(d.ranges.begin(), d.ranges.end(), [&](const Edit& e) { return e.a1 > pos; });
    };
    auto fo = first_resize(ro), ft = first_resize(rt);
    if (!bm->force && ((fo && edits_after(rt, *fo)) || (ft && edits_after(ro, *ft)))) {
        std::vector<std::string> ids;
        for (auto& c : r.changes_ours)
            ids.push_back(c.id);
        for (auto& c : r.changes_theirs)
            ids.push_back(c.id);
        r.conflicts.push_back(make_conflict({"/"}, ids, "dependency"));
        apply_conflict_policy(r.conflicts, co, r.warnings);
        auto& how = r.conflicts.back().resolved_as;
        pick(how == "theirs" ? theirs : how == "base" ? base : ours);
        return r;
    }

    struct Pending {
        Diff3Block b;
        std::size_t conflict = SIZE_MAX;
    };
    std::vector<Pending> plan;
    for (auto& b : diff3_blocks(ro.ranges, rt.ranges, bm->guard)) {
        Pending p{b};
        if (b.ours() && b.theirs()) {
            bool same = b.m1 - b.m0 == b.y1 - b.y0 &&
                        std::memcmp(so.data() + b.m0, st.data() + b.y0, b.m1 - b.m0) == 0;
            std::vector<std::string> ids;
            for (auto e = b.em0; e < b.em1; ++e)
                ids.push_back(r.changes_ours[e].id);
            for (auto e = b.ey0; e < b.ey1; ++e)
                ids.push_back(r.changes_theirs[e].id);
            if (same) {
                for (auto& id : ids)
                    r.auto_merged.push_back({id, "identical"});
            } else {
                p.conflict = r.conflicts.size();
                r.conflicts.push_back(make_conflict({"/bytes", Range{b.o0, b.o1 - b.o0}}, ids,
                                                    b.guard_joined ? "guard-band"
                                                    : b.o0 == b.o1 ? "add-add"
                                                                   : "both-modified"));
            }
        } else {
            for (auto e = b.em0; e < b.em1; ++e)
                r.auto_merged.push_back({r.changes_ours[e].id, "independent"});
            for (auto e = b.ey0; e < b.ey1; ++e)
                r.auto_merged.push_back({r.changes_theirs[e].id, "independent"});
        }
        plan.push_back(p);
    }
    apply_conflict_policy(r.conflicts, co, r.warnings);

    // Nearby but separate edits by both sides are often one logical edit (binary.md §6).
    for (std::size_t k = 0; k + 1 < plan.size(); ++k) {
        auto &x = plan[k].b, &y = plan[k + 1].b;
        if (((x.ours() && y.theirs()) || (x.theirs() && y.ours())) && y.o0 - x.o1 < 16)
            r.warnings.push_back({"adjacent-edits", Address{"/bytes", Range{x.o0, y.o1 - x.o0}}.to_string(),
                                  "edits by both sides within 16 bytes"});
    }

    output.clear();
    std::size_t at = 0;
    for (auto& p : plan) {
        auto& b = p.b;
        output.insert(output.end(), sb.begin() + static_cast<long>(at), sb.begin() + static_cast<long>(b.o0));
        std::string_view how = p.conflict == SIZE_MAX ? (b.ours() ? "ours" : "theirs")
                                                      : std::string_view(r.conflicts[p.conflict].resolved_as);
        if (how == "theirs")
            output.insert(output.end(), st.begin() + static_cast<long>(b.y0),
                          st.begin() + static_cast<long>(b.y1));
        else if (how == "base")
            output.insert(output.end(), sb.begin() + static_cast<long>(b.o0),
                          sb.begin() + static_cast<long>(b.o1));
        else
            output.insert(output.end(), so.begin() + static_cast<long>(b.m0),
                          so.begin() + static_cast<long>(b.m1));
        at = b.o1;
    }
    output.insert(output.end(), sb.begin() + static_cast<long>(at), sb.end());
    return r;
}

namespace {

// --gap (Q-BIN-4): ranges closer than this many bytes are reported as one; 8 by default.
std::size_t gap_of(const OptionValues& o) {
    auto it = o.find("gap");
    return it == o.end() ? 8 : static_cast<std::size_t>(std::stoull(it->second));
}

class BinaryComparison final : public Comparison {
public:
    BinaryComparison(const BinDoc& a, const BinDoc& b, std::size_t gap)
        : a_(a), b_(b), d_(binary::diff(a, b, gap)) {}
    bool differs() const override { return !d_.identical(); }
    ChangeSet changes() const override { return binary::changes(a_, b_, d_); }
    nlohmann::ordered_json stats() const override { return metrics(a_, b_, d_); }
    std::vector<std::string> render(std::string& out, const RenderRequest& r) const override {
        if (differs()) // GNU's message, whatever the output style
            out += "Binary files " + r.names[0] + " and " + r.names[1] + " differ\n";
        return {};
    }

private:
    const BinDoc &a_, &b_;
    BinDiff d_;
};

class BinaryFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "binary",
            "Any file (byte ranges, hex dump)",
            {},
            {true, true, false, true, false},
            {{"binary-merge", OptionSpec::Arg::optional, "byte-level merge of unrecognized files (=force)",
              true},
             {"dump-offsets", OptionSpec::Arg::none, "absolute offsets in the hex dump"},
             {"max-dump", OptionSpec::Arg::required, "input size above which the dump lists chunks only"}}};
        return i;
    }
    Probe probe(std::span<const std::byte>, std::string_view) const override { return {1}; }
    std::unique_ptr<Document> load(ByteSource src, const semsplinter::LoadOptions& o) const override {
        o.ctx.budget.charge(src.size() / 512 + 64); // chunk table
        return binary::load(std::move(src));
    }
    ChangeSet diff(const Document& a, const Document& b, const semsplinter::DiffOptions& o) const override {
        auto &x = cast(a), &y = cast(b);
        return changes(x, y, binary::diff(x, y, gap_of(o.ctx.format_options)));
    }
    std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                        const semsplinter::DiffOptions& o) const override {
        return std::make_unique<BinaryComparison>(cast(a), cast(b), gap_of(o.ctx.format_options));
    }
    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const semsplinter::MergeOptions& o) const override {
        const auto& fo = o.ctx.format_options;
        std::optional<ByteMergeOptions> bm;
        if (auto it = fo.find("binary-merge"); it != fo.end()) {
            bm.emplace();
            bm->force = it->second == "force";
            if (auto g = fo.find("guard"); g != fo.end())
                bm->guard = static_cast<std::size_t>(std::stoull(g->second));
        }
        std::vector<std::byte> out;
        auto r = binary::merge(cast(base), cast(ours), cast(theirs), o.conflict, bm ? &*bm : nullptr, out);
        r.document = binary::load(ByteSource::from_memory(std::move(out)));
        return r;
    }
    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(cast(d).src.bytes());
    }
    void dump(const Document& d, std::ostream& os, const semsplinter::DumpOptions& o) const override {
        binary::DumpOptions bo;
        const auto& fo = o.ctx.format_options;
        bo.absolute_offsets = fo.count("dump-offsets") > 0;
        if (auto it = fo.find("max-dump"); it != fo.end())
            bo.max_dump = std::stoull(it->second);
        binary::dump(cast(d), os, bo);
    }
    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet&) const override {
        auto &x = cast(a), &y = cast(b);
        return metrics(x, y, binary::diff(x, y));
    }

private:
    static const BinDoc& cast(const Document& d) {
        auto* b = dynamic_cast<const BinDoc*>(&d);
        if (!b)
            throw Error(ErrorCategory::internal, "binary: foreign document");
        return *b;
    }
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<BinaryFormat>();
}

} // namespace semsplinter::binary
