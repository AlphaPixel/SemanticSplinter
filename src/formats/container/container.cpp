#include "container.hpp"

#include <semsplinter/error.hpp>
#include <semsplinter/merge.hpp>

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>

namespace semsplinter::container {

namespace {

constexpr std::size_t kMaxDump = 1 << 20; // dump text per member (containers.md §5)

const Format& by_name(const Registry& reg, std::string_view name) {
    auto* f = reg.find(name);
    if (!f)
        throw Error(ErrorCategory::internal, "format '" + std::string(name) + "' is not registered");
    return *f;
}

// The format of a member: detection on every present side; disagreement or binary means binary (SPEC §11).
const Format& member_format(const Registry& reg, std::string_view path,
                            std::initializer_list<std::span<const std::byte>> sides) {
    const Format* first = nullptr;
    bool mixed = false;
    for (auto s : sides) {
        const Format& f = reg.detect(s, path);
        mixed |= f.info().name == "binary" || (first && first != &f);
        first = first ? first : &f;
    }
    return mixed ? by_name(reg, "binary") : *first;
}

// The context one level down: same registry, options, and budget (D-47).
Context child(const Context& c) {
    if (c.depth + 1 > c.budget.max_depth)
        throw Error(ErrorCategory::limit,
                    "containers nested deeper than --max-depth=" + std::to_string(c.budget.max_depth));
    return Context{c.registry, c.budget, c.depth + 1, c.format_options};
}

ByteSource source(std::span<const std::byte> b, std::string_view path) {
    return ByteSource::from_memory({b.begin(), b.end()}, std::string(path));
}

// Member-level failures (limits, bad or unsupported data) degrade to opaque handling (containers.md §6.2).
bool recoverable(const Error& e) {
    return e.category() != ErrorCategory::internal && e.category() != ErrorCategory::io;
}

std::string content_prefix(std::string_view path) {
    return entry_address(path) + "/content";
}

// Moves a member format's change below `prefix`, recording its new id.
void prefix_change(Change& c, const std::string& prefix, std::map<std::string, std::string>& ids) {
    const std::string old = c.id;
    c.at.path = prefix + c.at.path;
    if (c.from)
        c.from->path = prefix + c.from->path;
    for (auto& d : c.depends_on)
        d = prefix + d;
    c.id = make_change_id(c.side, c.op, c.at);
    ids[old] = c.id;
}

// A member's merge result re-addressed below `prefix`: changes, notes, conflicts (new ids), warnings.
void readdress(MergeResult& r, const std::string& prefix) {
    std::map<std::string, std::string> ids;
    for (auto* cs : {&r.changes_ours, &r.changes_theirs})
        for (auto& c : *cs)
            prefix_change(c, prefix, ids);
    auto mapped = [&](const std::string& id) {
        auto f = ids.find(id);
        return f == ids.end() ? id : f->second;
    };
    for (auto& a : r.auto_merged)
        a.change = mapped(a.change);
    for (auto& c : r.conflicts) {
        c.at.path = prefix + c.at.path;
        for (auto& id : c.changes)
            id = mapped(id);
        c.id = make_conflict_id(c.at, c.changes);
    }
    for (auto& w : r.warnings)
        w.at = prefix + w.at;
}

// Pairs members only in `a` with members only in `b` by equal content hash (containers.md §3).
std::vector<std::pair<std::size_t, std::size_t>> renames(const Archive& a, const Archive& b, Limits& l) {
    std::set<std::string> in_a, in_b;
    for (auto& e : a.entries)
        in_a.insert(e.path);
    for (auto& e : b.entries)
        in_b.insert(e.path);
    std::map<Hash128, std::vector<std::size_t>> gone;
    for (std::size_t i = 0; i < a.entries.size(); ++i)
        if (!in_b.count(a.entries[i].path))
            gone[a.hash(i, l)].push_back(i);
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (gone.empty())
        return out;
    for (std::size_t j = 0; j < b.entries.size(); ++j) {
        if (in_a.count(b.entries[j].path))
            continue;
        auto f = gone.find(b.hash(j, l));
        if (f != gone.end() && !f->second.empty()) {
            out.emplace_back(f->second.front(), j);
            f->second.erase(f->second.begin());
        }
    }
    return out;
}

const std::string* meta_value(const Archive::Entry& e, std::string_view field) {
    if (field == "path")
        return &e.path;
    for (auto& [k, v] : e.meta)
        if (k == field)
            return &v;
    return nullptr;
}

void meta_changes(const Archive::Entry& a, const Archive::Entry& b, const std::string& at, Side side,
                  ChangeSet& cs) {
    for (auto& [k, va] : a.meta) {
        auto* vb = meta_value(b, k);
        if (!vb || *vb == va)
            continue;
        Change c = make_change(side, Op::update, {at + "/meta/" + pointer_escape(k)}, "metadata");
        c.before.text = va, c.after.text = *vb;
        cs.push_back(std::move(c));
    }
}

// Content changes of a paired member: the member format's own diff, else one opaque change.
void content_changes(const Archive& a, std::size_t i, const Archive& b, std::size_t j, const DiffOptions& o,
                     Side side, ChangeSet& cs) {
    const std::string& path = b.entries[j].path;
    const std::string at = content_prefix(path);
    Limits& l = o.ctx.budget;
    try {
        if (!a.entries[i].opaque && !b.entries[j].opaque) {
            Context ctx = child(o.ctx);
            auto ca = a.content(i, l);
            auto cb = b.content(j, l);
            const Format& f = member_format(o.ctx.registry, path, {ca, cb});
            auto da = f.load(source(ca, path), {ctx});
            auto db = f.load(source(cb, path), {ctx});
            ChangeSet sub = f.diff(*da, *db, DiffOptions{ctx, o.text});
            std::map<std::string, std::string> ids;
            for (auto& c : sub) {
                c.side = side;
                prefix_change(c, at, ids);
                cs.push_back(std::move(c));
            }
            if (sub.empty()) // equal for the member's format: another encoding of the same content
                cs.push_back(make_change(side, Op::update, {at}, "encoding"));
            return;
        }
    } catch (const Error& e) {
        if (!recoverable(e))
            throw;
    }
    Change c = make_change(side, Op::update, {at}, "content");
    c.before.hash = a.hash(i, l), c.after.hash = b.hash(j, l);
    cs.push_back(std::move(c));
}

bool newer(const std::string& a, const std::string& b) {
    char* end_a = nullptr;
    char* end_b = nullptr;
    double x = std::strtod(a.c_str(), &end_a), y = std::strtod(b.c_str(), &end_b);
    if (!a.empty() && !b.empty() && *end_a == 0 && *end_b == 0)
        return x > y;
    return a > b; // ISO 8601 times order as text
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Ref {
    const Archive* a = nullptr;
    std::size_t i = 0;
    const Archive::Entry& entry() const { return a->entries[i]; }
};

class MemberMerge {
public:
    MemberMerge(const Archive& b, const Archive& o, const Archive& t, const MergeOptions& mo)
        : arch_{&b, &o, &t}, mo_(mo), l_(mo.ctx.budget) {}

    Merged run();

private:
    struct Field {
        std::string name;
        LeafOutcome out;
        std::optional<std::size_t> conflict;
    };
    struct Plan {
        std::array<std::optional<Ref>, 3> refs; // indexed by Side: base, ours, theirs
        LeafOutcome member;                     // the whole-member decision
        std::optional<std::size_t> conflict;
        bool fields = false; // both sides changed the member: decided per field
        LeafOutcome content;
        std::optional<std::size_t> content_conflict;
        std::optional<std::vector<std::byte>> merged;
        std::vector<Field> meta;
    };

    const Archive& arch(Side s) const { return *arch_[static_cast<int>(s)]; }
    std::vector<std::pair<std::string, Ref>> keyed(Side s) const;
    void plan_fields(Plan& p);
    std::optional<std::vector<std::byte>> sub_merge(const Ref* b, const Ref& o, const Ref& t,
                                                    std::string& failure);
    std::vector<std::string> ids(const Plan& p, int side, const std::string& suffix) const;
    std::size_t add_conflict(const Plan& p, const std::string& suffix, const std::string& reason);
    void notes(const Plan& p, const LeafOutcome& out, const std::string& suffix);
    std::optional<Side> decide(const Plan& p, std::optional<std::size_t> conflict,
                               std::optional<Side> take) const;
    void check_paths();

    std::array<const Archive*, 3> arch_;
    const MergeOptions& mo_;
    Limits& l_;
    Merged m_;
    std::vector<Conflict> own_, nested_;
    std::set<std::string> consumed_; // --resolve ids handed down to member merges
};

// Identity keys: a member's own path, or the base path it was renamed from.
std::vector<std::pair<std::string, Ref>> MemberMerge::keyed(Side s) const {
    const Archive& a = arch(s);
    std::map<std::size_t, std::size_t> from;
    if (s != Side::base)
        for (auto [i, j] : renames(arch(Side::base), a, l_))
            from[j] = i;
    std::vector<std::pair<std::string, Ref>> v;
    for (std::size_t k = 0; k < a.entries.size(); ++k) {
        auto f = from.find(k);
        v.emplace_back(f == from.end() ? a.entries[k].path : arch(Side::base).entries[f->second].path,
                       Ref{&a, k});
    }
    return v;
}

// Ids of `side`'s changes at the member (by that side's path or the base path) plus `suffix`.
std::vector<std::string> MemberMerge::ids(const Plan& p, int side, const std::string& suffix) const {
    const ChangeSet& cs = side == 1 ? m_.result.changes_ours : m_.result.changes_theirs;
    std::vector<std::string> out;
    for (auto& c : cs)
        for (int k : {side, 0})
            if (p.refs[k] && path_within(c.at.path, entry_address(p.refs[k]->entry().path) + suffix)) {
                out.push_back(c.id);
                break;
            }
    return out;
}

std::size_t MemberMerge::add_conflict(const Plan& p, const std::string& suffix, const std::string& reason) {
    auto v = ids(p, 1, suffix);
    auto t = ids(p, 2, suffix);
    v.insert(v.end(), t.begin(), t.end());
    const Ref& shown = p.refs[1] ? *p.refs[1] : p.refs[2] ? *p.refs[2] : *p.refs[0];
    own_.push_back(make_conflict({entry_address(shown.entry().path) + suffix}, std::move(v), reason));
    return own_.size() - 1;
}

void MemberMerge::notes(const Plan& p, const LeafOutcome& out, const std::string& suffix) {
    auto add = [&](int side, const char* rule) {
        for (auto& id : ids(p, side, suffix))
            m_.result.auto_merged.push_back({id, rule});
    };
    if (out.rule == MergeRule::ours)
        add(1, "independent");
    else if (out.rule == MergeRule::theirs)
        add(2, "independent");
    else if (out.rule == MergeRule::identical)
        add(1, "identical"), add(2, "identical");
}

std::optional<Side> MemberMerge::decide(const Plan& p, std::optional<std::size_t> conflict,
                                        std::optional<Side> take) const {
    if (!conflict)
        return take;
    auto s = take_for(own_[*conflict].resolved_as, p.refs[0].has_value(), p.refs[1].has_value(),
                      p.refs[2].has_value());
    return s && p.refs[static_cast<int>(*s)] ? s : std::nullopt;
}

std::optional<std::vector<std::byte>> MemberMerge::sub_merge(const Ref* b, const Ref& o, const Ref& t,
                                                             std::string& failure) {
    const std::string& path = o.entry().path;
    try {
        if ((b && b->entry().opaque) || o.entry().opaque || t.entry().opaque)
            return std::nullopt;
        Context ctx = child(mo_.ctx);
        auto co = o.a->content(o.i, l_);
        auto ct = t.a->content(t.i, l_);
        std::span<const std::byte> cb;
        if (b)
            cb = b->a->content(b->i, l_);
        const Format& f =
            b ? member_format(ctx.registry, path, {cb, co, ct}) : member_format(ctx.registry, path, {co, ct});
        if (!f.info().caps.merge)
            return std::nullopt;
        auto db = f.load(source(cb, path), {ctx});
        auto dob = f.load(source(co, path), {ctx});
        auto dt = f.load(source(ct, path), {ctx});
        const std::string prefix = content_prefix(path);
        auto run = [&](std::map<std::string, std::string> resolutions) {
            MergeOptions sub{ctx, mo_.conflict, mo_.labels, MergeOptions::Visual::none, mo_.text};
            sub.conflict.resolutions = std::move(resolutions);
            return f.merge(*db, *dob, *dt, sub);
        };
        MergeResult r = run({});
        // A --resolve file names nested conflicts by their ids in the archive: translate and merge again.
        if (!mo_.conflict.resolutions.empty() && !r.conflicts.empty()) {
            MergeResult tmp;
            tmp.changes_ours = r.changes_ours, tmp.changes_theirs = r.changes_theirs;
            tmp.conflicts = r.conflicts;
            readdress(tmp, prefix);
            std::map<std::string, std::string> res;
            for (std::size_t k = 0; k < r.conflicts.size(); ++k)
                if (auto res_it = mo_.conflict.resolutions.find(tmp.conflicts[k].id);
                    res_it != mo_.conflict.resolutions.end()) {
                    res[r.conflicts[k].id] = res_it->second;
                    consumed_.insert(res_it->first);
                }
            if (!res.empty())
                r = run(std::move(res));
        }
        readdress(r, prefix);
        VectorSink sink;
        f.save(*r.document, sink, {ctx});
        auto& acc = m_.result;
        // The member merge's own changes replace the diff's content changes of this member.
        std::erase_if(acc.changes_ours, [&](const Change& c) { return path_within(c.at.path, prefix); });
        std::erase_if(acc.changes_theirs, [&](const Change& c) {
            return path_within(c.at.path, content_prefix(t.entry().path));
        });
        acc.changes_ours.insert(acc.changes_ours.end(), r.changes_ours.begin(), r.changes_ours.end());
        acc.changes_theirs.insert(acc.changes_theirs.end(), r.changes_theirs.begin(), r.changes_theirs.end());
        acc.auto_merged.insert(acc.auto_merged.end(), r.auto_merged.begin(), r.auto_merged.end());
        acc.warnings.insert(acc.warnings.end(), r.warnings.begin(), r.warnings.end());
        nested_.insert(nested_.end(), r.conflicts.begin(), r.conflicts.end());
        if (r.marked)
            acc.marked = acc.marked.value_or(0) + *r.marked;
        return std::move(sink.data);
    } catch (const Error& e) {
        if (!recoverable(e))
            throw;
        if (e.category() == ErrorCategory::limit)
            failure = "limit";
        m_.result.warnings.push_back({"member-not-merged", entry_address(path), e.what()});
        return std::nullopt;
    }
}

void MemberMerge::plan_fields(Plan& p) {
    const Ref* rb = p.refs[0] ? &*p.refs[0] : nullptr;
    const Ref& ro = *p.refs[1];
    const Ref& rt = *p.refs[2];
    auto same = [&](const Ref& x, const Ref& y) { return x.a->same(x.i, *y.a, y.i, l_); };
    std::string failure;
    p.content = merge_leaf(rb, &ro, &rt, same, [&](const Ref* vb, const Ref& vo, const Ref& vt) {
        p.merged = sub_merge(vb, vo, vt, failure);
        return p.merged.has_value();
    });
    if (p.content.rule == MergeRule::conflict)
        p.content_conflict = add_conflict(p, "", failure.empty() ? p.content.reason : failure);
    else
        notes(p, p.content, "/content");

    std::vector<std::string> names{"path"};
    for (auto* r : {rb, &ro, &rt})
        if (r)
            for (auto& [k, v] : r->entry().meta)
                if (std::find(names.begin(), names.end(), k) == names.end())
                    names.push_back(k);
    for (auto& name : names) {
        Field f{name, {}, {}};
        const std::string* vb = rb ? meta_value(rb->entry(), name) : nullptr;
        const std::string* vo = meta_value(ro.entry(), name);
        const std::string* vt = meta_value(rt.entry(), name);
        f.out = merge_leaf(vb, vo, vt, std::equal_to<>{},
                           [](const std::string*, const std::string&, const std::string&) { return false; });
        const std::string suffix = name == "path" ? "" : "/meta/" + pointer_escape(name);
        if (f.out.rule == MergeRule::conflict && name == "mtime" && vo && vt) {
            // Times carry no content meaning: the later one wins (containers.md §6.1, Q-ZIP-6).
            f.out.rule = MergeRule::recursed;
            f.out.take = newer(*vo, *vt) ? Side::ours : Side::theirs;
            for (int s : {1, 2})
                for (auto& id : ids(p, s, suffix))
                    m_.result.auto_merged.push_back({id, "newest-mtime"});
        } else if (f.out.rule == MergeRule::conflict) {
            f.conflict = add_conflict(p, suffix, f.out.reason);
        } else if (name != "path") {
            notes(p, f.out, suffix);
        }
        p.meta.push_back(std::move(f));
    }
}

// Drops members whose output path repeats an earlier one, and warns about new case-only collisions.
void MemberMerge::check_paths() {
    auto path_of = [&](const OutEntry& e) -> const std::string& {
        auto f = e.from.find("path");
        const Pick& p = f == e.from.end() ? e.body : f->second;
        return arch(p.side).entries[p.index].path;
    };
    std::set<std::string> seen;
    std::map<std::string, std::string> folded;
    std::erase_if(m_.entries, [&](const OutEntry& e) {
        const std::string& path = path_of(e);
        if (!seen.insert(path).second) {
            m_.result.warnings.push_back(
                {"path-collision", entry_address(path), "two merged members have this path; kept the first"});
            return true;
        }
        auto [it, fresh] = folded.emplace(lower(path), path);
        if (!fresh) {
            bool had = false;
            for (auto* a : arch_) {
                bool x = false, y = false;
                for (auto& en : a->entries)
                    x |= en.path == it->second, y |= en.path == path;
                had |= x && y;
            }
            if (!had)
                m_.result.warnings.push_back(
                    {"case-collision", entry_address(path), "differs from " + it->second + " only in case"});
        }
        return false;
    });
}

Merged MemberMerge::run() {
    MergeResult& r = m_.result;
    const DiffOptions d{mo_.ctx, {}};
    r.changes_ours = diff(arch(Side::base), arch(Side::ours), d, Side::ours);
    r.changes_theirs = diff(arch(Side::base), arch(Side::theirs), d, Side::theirs);

    auto kb = keyed(Side::base), ko = keyed(Side::ours), kt = keyed(Side::theirs);
    auto eq = [&](const Ref& x, const Ref& y) {
        return x.entry().path == y.entry().path && x.entry().meta == y.entry().meta &&
               x.a->same(x.i, *y.a, y.i, l_);
    };
    auto rows = keyed_merge<std::string, Ref>(kb, ko, kt, eq,
                                              [](const std::string&, const Ref*, const Ref&, const Ref&) {
                                                  return true; // decided per field below
                                              });
    std::vector<Plan> plans;
    for (auto& row : rows) {
        Plan p;
        auto ref = [](const auto& v, std::optional<std::size_t> k) {
            return k ? std::optional<Ref>(v[*k].second) : std::nullopt;
        };
        p.refs = {ref(kb, row.base), ref(ko, row.ours), ref(kt, row.theirs)};
        p.member = row.outcome;
        if (row.outcome.rule == MergeRule::recursed)
            p.fields = true, plan_fields(p);
        else if (row.outcome.rule == MergeRule::conflict)
            p.conflict = add_conflict(p, "", row.outcome.reason);
        else
            notes(p, row.outcome, "");
        plans.push_back(std::move(p));
    }

    ConflictOptions co = mo_.conflict;
    for (auto& id : consumed_)
        co.resolutions.erase(id);
    apply_conflict_policy(own_, co, r.warnings);

    for (auto& p : plans) {
        if (!p.fields) {
            if (auto s = decide(p, p.conflict, p.member.take))
                m_.entries.push_back({Pick{*s, p.refs[static_cast<int>(*s)]->i}, {}, {}});
            continue;
        }
        auto s = decide(p, p.content_conflict, p.content.take);
        if (!s)
            continue;
        const Ref body = *p.refs[static_cast<int>(*s)];
        OutEntry e{Pick{*s, body.i}, {}, {}};
        if (p.content.rule == MergeRule::recursed && !p.content_conflict) {
            auto cur = body.a->content(body.i, l_);
            if (!std::equal(p.merged->begin(), p.merged->end(), cur.begin(), cur.end()))
                e.content = std::move(p.merged);
        }
        for (auto& f : p.meta) {
            auto fs = decide(p, f.conflict, f.out.take);
            if (!fs)
                continue;
            const Ref fr = *p.refs[static_cast<int>(*fs)];
            auto* v = meta_value(fr.entry(), f.name);
            auto* bv = meta_value(body.entry(), f.name);
            if (v && (!bv || *v != *bv))
                e.from[f.name] = Pick{*fs, fr.i};
        }
        m_.entries.push_back(std::move(e));
    }
    check_paths();
    r.conflicts = std::move(own_);
    r.conflicts.insert(r.conflicts.end(), nested_.begin(), nested_.end());
    return std::move(m_);
}

class TimesComparison final : public Comparison {
public:
    TimesComparison(std::unique_ptr<Comparison> inner, bool count_times)
        : inner_(std::move(inner)), count_(count_times) {}
    bool differs() const override {
        for (auto& c : inner_->changes())
            if (c.kind != "encoding" && (count_ || !c.at.path.ends_with("/meta/mtime")))
                return true;
        return false;
    }
    ChangeSet changes() const override { return inner_->changes(); }
    nlohmann::ordered_json stats() const override { return inner_->stats(); }
    nlohmann::ordered_json options() const override { return inner_->options(); }
    std::vector<std::string> render(std::string& out, const RenderRequest& r) const override {
        return inner_->render(out, r);
    }

private:
    std::unique_ptr<Comparison> inner_;
    bool count_;
};

Error corrupt(const char* codec) {
    return Error(ErrorCategory::invalid_input, std::string(codec) + ": corrupt or truncated stream");
}

} // namespace

std::string entry_address(std::string_view path) {
    return "/entries/" + pointer_escape(path);
}

bool suspicious_path(std::string_view p) {
    return p.starts_with('/') || p.find('\\') != std::string_view::npos ||
           p.find('\0') != std::string_view::npos || (p.size() > 1 && p[1] == ':') || p == ".." ||
           p.starts_with("../") || p.find("/../") != std::string_view::npos || p.ends_with("/..");
}

ChangeSet diff(const Archive& a, const Archive& b, const DiffOptions& o, Side side) {
    Limits& l = o.ctx.budget;
    ChangeSet cs;
    std::map<std::string, std::size_t> ia, ib;
    for (std::size_t i = 0; i < a.entries.size(); ++i)
        ia[a.entries[i].path] = i;
    for (std::size_t j = 0; j < b.entries.size(); ++j)
        ib[b.entries[j].path] = j;
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (auto& [path, i] : ia)
        if (auto f = ib.find(path); f != ib.end())
            pairs.emplace_back(i, f->second);
    auto moves = renames(a, b, l);
    std::set<std::size_t> moved_a, moved_b;
    for (auto [i, j] : moves) {
        moved_a.insert(i), moved_b.insert(j);
        Change c = make_change(side, Op::move, {entry_address(b.entries[j].path)}, "rename");
        c.from = Address{entry_address(a.entries[i].path)};
        c.before.hash = c.after.hash = b.hash(j, l);
        cs.push_back(std::move(c));
    }
    pairs.insert(pairs.end(), moves.begin(), moves.end());
    for (auto [i, j] : pairs) {
        meta_changes(a.entries[i], b.entries[j], entry_address(b.entries[j].path), side, cs);
        if (!a.same(i, b, j, l))
            content_changes(a, i, b, j, o, side, cs);
    }
    for (std::size_t i = 0; i < a.entries.size(); ++i)
        if (!ib.count(a.entries[i].path) && !moved_a.count(i)) {
            Change c = make_change(side, Op::erase, {entry_address(a.entries[i].path)}, "member");
            c.before.hash = a.hash(i, l);
            cs.push_back(std::move(c));
        }
    for (std::size_t j = 0; j < b.entries.size(); ++j)
        if (!ia.count(b.entries[j].path) && !moved_b.count(j)) {
            Change c = make_change(side, Op::insert, {entry_address(b.entries[j].path)}, "member");
            c.after.hash = b.hash(j, l);
            cs.push_back(std::move(c));
        }
    sort_canonical(cs);
    return cs;
}

Merged merge(const Archive& base, const Archive& ours, const Archive& theirs, const MergeOptions& mo) {
    return MemberMerge(base, ours, theirs, mo).run();
}

bool is_ours(const Merged& m, const Archive& ours) {
    if (m.entries.size() != ours.entries.size())
        return false;
    for (std::size_t k = 0; k < m.entries.size(); ++k)
        if (!m.entries[k].raw() || m.entries[k].body != Pick{Side::ours, k})
            return false;
    return true;
}

void dump_members(const Archive& a, std::ostream& os, const DumpOptions& o) {
    std::vector<std::size_t> order(a.entries.size());
    for (std::size_t k = 0; k < order.size(); ++k)
        order[k] = k;
    std::sort(order.begin(), order.end(),
              [&](std::size_t x, std::size_t y) { return a.entries[x].path < a.entries[y].path; });
    for (auto i : order) {
        const std::string& path = a.entries[i].path;
        if (a.entries[i].opaque)
            continue;
        try {
            Context ctx = child(o.ctx);
            auto c = a.content(i, o.ctx.budget);
            if (c.empty()) // directories, empty files: the entry line says it all
                continue;
            const Format& f = o.ctx.registry.detect(c, path);
            if (f.info().name == "binary") // the entry line already has its size and CRC
                continue;
            auto d = f.load(source(c, path), {ctx});
            std::ostringstream ms;
            f.dump(*d, ms, {ctx});
            auto s = ms.str();
            if (s.size() > kMaxDump)
                continue;
            os << "=== " << path << " (" << f.info().name << ")\n" << s;
            if (!s.empty() && s.back() != '\n')
                os << '\n';
        } catch (const Error& e) {
            if (!recoverable(e))
                throw;
        }
    }
}

namespace {

std::optional<std::size_t> index_of(const Archive& a, std::string_view path) {
    for (std::size_t i = 0; i < a.entries.size(); ++i)
        if (a.entries[i].path == path)
            return i;
    return std::nullopt;
}

Error not_member(const Address& at) {
    return Error(ErrorCategory::unsupported, "patch: " + at.to_string() + " is not a member unit");
}

const Archive& archive(const Document& d) {
    auto* a = dynamic_cast<const Archive*>(&d);
    if (!a)
        throw Error(ErrorCategory::internal, "container: not an archive");
    return *a;
}

} // namespace

std::optional<std::vector<std::byte>> payload(const Archive& a, const Address& at) {
    const auto segs = pointer_split(at.path);
    if (segs.size() < 2 || segs[0] != "entries")
        throw not_member(at);
    const auto i = index_of(a, segs[1]);
    if (!i)
        return std::nullopt;
    Limits l = a.limits.fresh();
    std::vector<Warning> ignored;
    if (segs.size() == 2 || segs[2] == "meta") {
        OutEntry e{{Side::ours, *i}, {}, {}};
        if (segs.size() > 2)
            e.content.emplace(); // the record alone
        return a.write({nullptr, &a, nullptr}, {e}, true, l, ignored);
    }
    if (segs[2] != "content")
        throw not_member(at);
    const std::string& path = a.entries[*i].path;
    auto c = a.content(*i, l);
    const std::string prefix = content_prefix(path);
    if (at.path.size() > prefix.size() && a.registry) {
        const Format& f = a.registry->detect(c, path);
        if (f.info().caps.patch) {
            Context ctx = child(Context{*a.registry, l, a.depth, a.options});
            auto d = f.load(source(c, path), {ctx});
            return f.payload(*d, {at.path.substr(prefix.size()), at.region});
        }
    }
    return std::vector<std::byte>(c.begin(), c.end());
}

std::vector<std::byte> implant(const Format& self, const Archive& t, std::span<const UnitValue> units,
                               const LoadOptions& o, std::vector<Warning>& warnings) {
    struct Member {
        bool set = false;                              // the member itself: `whole`, or removed
        const std::vector<std::byte>* whole = nullptr; // a payload archive
        const std::vector<std::byte>* meta = nullptr;  // a payload archive with the fields below
        std::vector<std::string> fields;
        const std::vector<std::byte>* content = nullptr;
        std::vector<UnitValue> nested;
        std::size_t aux = 0; // its payload's index in the aux archive
    };
    std::map<std::string, Member> members;
    for (auto& u : units) {
        const auto segs = pointer_split(u.at.path);
        if (segs.size() < 2 || segs[0] != "entries")
            throw not_member(u.at);
        Member& m = members[segs[1]];
        const std::string prefix = content_prefix(segs[1]);
        if (segs.size() == 2)
            m.set = true, m.whole = u.value;
        else if (segs[2] == "meta" && segs.size() == 4 && u.value)
            m.meta = u.value, m.fields.push_back(segs[3]);
        else if (segs[2] == "content" && u.at.path.size() == prefix.size())
            m.content = u.value ? u.value : m.content;
        else if (segs[2] == "content")
            m.nested.push_back({{u.at.path.substr(prefix.size()), u.at.region}, u.value, u.op});
        else if (segs[2] != "meta")
            throw not_member(u.at);
    }

    // Every payload member in one archive (the base side of the final write).
    // ponytail: one rewrite per payload, quadratic in the payloads' size; fine for the members a patch adds.
    Limits& l = o.ctx.budget;
    std::unique_ptr<Document> aux;
    std::size_t parts = 0;
    for (auto& [path, m] : members) {
        const auto* v = m.set ? m.whole : m.meta;
        if (!v)
            continue;
        Limits pl = l.fresh();
        Context pc{o.ctx.registry, pl, o.ctx.depth, o.ctx.format_options};
        auto part = self.load(ByteSource::from_memory(*v, path), {pc});
        if (archive(*part).entries.size() != 1)
            throw Error(ErrorCategory::invalid_input,
                        "patch: the payload of " + entry_address(path) + " is not a one-member archive");
        m.aux = parts++;
        if (!aux) {
            aux = std::move(part);
            continue;
        }
        const Archive& a = archive(*aux);
        std::vector<OutEntry> es;
        for (std::size_t k = 0; k < a.entries.size(); ++k)
            es.push_back({{Side::ours, k}, {}, {}});
        es.push_back({{Side::theirs, 0}, {}, {}});
        std::vector<Warning> ignored;
        auto bytes = a.write({nullptr, &a, &archive(*part)}, es, true, pl, ignored);
        Limits al = l.fresh();
        Context ac{o.ctx.registry, al, o.ctx.depth, o.ctx.format_options};
        aux = self.load(ByteSource::from_memory(std::move(bytes), "payload"), {ac});
    }
    const Archive* ax = aux ? &archive(*aux) : nullptr;

    std::vector<OutEntry> out;
    for (std::size_t i = 0; i < t.entries.size(); ++i) {
        auto f = members.find(t.entries[i].path);
        if (f == members.end()) {
            out.push_back({{Side::ours, i}, {}, {}});
            continue;
        }
        Member& m = f->second;
        if (m.set) {
            if (m.whole)
                out.push_back({{Side::base, m.aux}, {}, {}});
            m.whole = nullptr; // placed
            continue;
        }
        OutEntry e{{Side::ours, i}, {}, {}};
        if (m.meta)
            for (auto& field : m.fields)
                e.from[field] = {Side::base, m.aux};
        if (m.content) {
            e.content = *m.content;
        } else if (!m.nested.empty()) {
            const std::string& path = t.entries[i].path;
            auto c = t.content(i, l);
            const Format& mf = o.ctx.registry.detect(c, path);
            if (mf.info().caps.patch) {
                Context ctx = child(o.ctx);
                auto d = mf.load(source(c, path), {ctx});
                auto nd = mf.implant(*d, m.nested, {ctx});
                VectorSink s;
                mf.save(*nd, s, {ctx});
                e.content = std::move(s.data);
            } else { // the payloads are the member's bytes
                for (auto& u : m.nested)
                    if (u.value)
                        e.content = *u.value;
            }
        }
        out.push_back(std::move(e));
    }
    for (auto& [path, m] : members)
        if (m.set && m.whole)
            out.push_back({{Side::base, m.aux}, {}, {}});
    return t.write({ax, &t, nullptr}, out, false, l, warnings);
}

std::unique_ptr<Comparison> compare(std::unique_ptr<Comparison> inner, bool count_times) {
    return std::make_unique<TimesComparison>(std::move(inner), count_times);
}

// ---- Codecs ----

std::vector<std::byte> inflate(std::span<const std::byte> in, std::uint64_t max_out, int window_bits,
                               bool partial) {
    z_stream z{};
    if (inflateInit2(&z, window_bits) != Z_OK)
        throw Error(ErrorCategory::internal, "zlib: init failed");
    std::unique_ptr<z_stream, int (*)(z_streamp)> guard(&z, inflateEnd);
    std::vector<std::byte> out;
    std::array<Bytef, 65536> buf;
    std::size_t fed = 0;
    auto feed = [&] {
        std::size_t n = std::min<std::size_t>(in.size() - fed, std::size_t{1} << 30);
        z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data() + fed));
        z.avail_in = static_cast<uInt>(n);
        fed += n;
    };
    feed();
    for (;;) {
        if (z.avail_in == 0 && fed < in.size())
            feed();
        z.next_out = buf.data();
        z.avail_out = static_cast<uInt>(buf.size());
        int rc = ::inflate(&z, Z_NO_FLUSH);
        auto* b = reinterpret_cast<const std::byte*>(buf.data());
        out.insert(out.end(), b, b + (buf.size() - z.avail_out));
        if (out.size() > max_out) {
            out.resize(max_out + 1);
            return out;
        }
        if (rc == Z_STREAM_END) {
            // gzip: another member may follow (RFC 1952 §2.2).
            if (window_bits > 15 && z.avail_in >= 2 && z.next_in[0] == 0x1f && z.next_in[1] == 0x8b) {
                inflateReset(&z);
                continue;
            }
            return out;
        }
        if (rc == Z_BUF_ERROR && z.avail_in == 0 && fed == in.size()) {
            if (partial)
                return out;
            throw corrupt(window_bits > 15 ? "gzip" : "deflate");
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            if (partial)
                return out;
            throw corrupt(window_bits > 15 ? "gzip" : "deflate");
        }
    }
}

std::vector<std::byte> deflate(std::span<const std::byte> in, int level, int window_bits) {
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw Error(ErrorCategory::internal, "zlib: init failed");
    std::unique_ptr<z_stream, int (*)(z_streamp)> guard(&z, deflateEnd);
    std::vector<std::byte> out;
    std::array<Bytef, 65536> buf;
    std::size_t fed = 0;
    int flush = Z_NO_FLUSH;
    do {
        std::size_t n = std::min<std::size_t>(in.size() - fed, std::size_t{1} << 30);
        z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data() + fed));
        z.avail_in = static_cast<uInt>(n);
        fed += n;
        flush = fed == in.size() ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = buf.data();
            z.avail_out = static_cast<uInt>(buf.size());
            ::deflate(&z, flush);
            auto* b = reinterpret_cast<const std::byte*>(buf.data());
            out.insert(out.end(), b, b + (buf.size() - z.avail_out));
        } while (z.avail_out == 0);
    } while (flush != Z_FINISH);
    return out;
}

std::vector<std::byte> unzstd(std::span<const std::byte> in, std::uint64_t max_out, bool partial) {
    std::unique_ptr<ZSTD_DStream, std::size_t (*)(ZSTD_DStream*)> d(ZSTD_createDStream(), ZSTD_freeDStream);
    std::vector<std::byte> out;
    std::array<std::byte, 65536> buf;
    ZSTD_inBuffer zin{in.data(), in.size(), 0};
    for (;;) {
        ZSTD_outBuffer zout{buf.data(), buf.size(), 0};
        std::size_t rc = ZSTD_decompressStream(d.get(), &zout, &zin);
        if (ZSTD_isError(rc)) {
            if (partial)
                return out;
            throw corrupt("zstd");
        }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(zout.pos));
        if (out.size() > max_out) {
            out.resize(max_out + 1);
            return out;
        }
        if (rc == 0 && zin.pos == zin.size)
            return out;
        if (zin.pos == zin.size && zout.pos < zout.size) {
            if (partial)
                return out;
            throw corrupt("zstd");
        }
    }
}

std::vector<std::byte> zstd(std::span<const std::byte> in, int level, bool checksum) {
    std::unique_ptr<ZSTD_CCtx, std::size_t (*)(ZSTD_CCtx*)> c(ZSTD_createCCtx(), ZSTD_freeCCtx);
    ZSTD_CCtx_setParameter(c.get(), ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(c.get(), ZSTD_c_checksumFlag, checksum ? 1 : 0);
    std::vector<std::byte> out(ZSTD_compressBound(in.size()));
    std::size_t n = ZSTD_compress2(c.get(), out.data(), out.size(), in.data(), in.size());
    if (ZSTD_isError(n))
        throw Error(ErrorCategory::internal, "zstd: compression failed");
    out.resize(n);
    return out;
}

std::uint64_t decode_limit(const Limits& l, std::uint64_t compressed) {
    auto left = [](std::uint64_t max, std::uint64_t used) { return used < max ? max - used : 0; };
    std::uint64_t n = std::min(left(l.max_zip_total, l.zip_used), left(l.max_memory, l.memory_used));
    std::uint64_t ratio = l.max_zip_ratio && compressed > UINT64_MAX / l.max_zip_ratio
                              ? UINT64_MAX
                              : compressed * l.max_zip_ratio;
    return std::min(n, std::max<std::uint64_t>(ratio, 1 << 20));
}

void charge(Limits& l, std::uint64_t n) {
    if (n > l.max_zip_total - std::min(l.zip_used, l.max_zip_total))
        throw Error(ErrorCategory::limit,
                    "decompressed container data exceeds --max-zip-size=" + std::to_string(l.max_zip_total));
    l.charge(n);
    l.zip_used += n;
}

} // namespace semsplinter::container
