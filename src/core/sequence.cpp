// SequenceMerge: identity matching, diff3 chunking against base, ID-matched moves (SPEC §5.1).
#include <semsplinter/error.hpp>
#include <semsplinter/sequence.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <unordered_map>

namespace semsplinter {

std::string_view to_string(SeqKind k) noexcept {
    switch (k) {
    case SeqKind::stable:
        return "stable";
    case SeqKind::ours:
        return "ours";
    case SeqKind::theirs:
        return "theirs";
    case SeqKind::identical:
        return "identical";
    case SeqKind::both_inserted:
        return "both-inserted";
    case SeqKind::recursed:
        return "recursed";
    case SeqKind::conflict:
        return "conflict";
    }
    return "stable";
}

std::vector<SeqPick> picks(const SeqChunk& c, std::string_view choice) {
    auto from = [](Side s, const std::vector<std::size_t>& idx) {
        std::vector<SeqPick> p;
        for (auto i : idx)
            p.push_back({s, i});
        return p;
    };
    switch (c.kind) {
    case SeqKind::stable:
        return from(Side::base, c.base);
    case SeqKind::theirs:
        return from(Side::theirs, c.theirs);
    case SeqKind::ours:
    case SeqKind::identical:
    case SeqKind::recursed:
        return from(Side::ours, c.ours);
    case SeqKind::both_inserted:
        break;
    case SeqKind::conflict:
        if (choice == "theirs")
            return from(Side::theirs, c.theirs);
        if (choice == "base")
            return from(Side::base, c.base);
        if (choice != "union")
            return from(Side::ours, c.ours);
        break;
    }
    auto p = from(Side::ours, c.ours);
    auto t = from(Side::theirs, c.theirs);
    p.insert(p.end(), t.begin(), t.end());
    return p;
}

namespace {

using Items = std::span<const SeqItem>;
constexpr std::size_t npos = static_cast<std::size_t>(-1);

// id -> position, sorted by id. A sorted vector: one allocation and cache-friendly lookups (#38).
using Positions = std::vector<std::pair<Hash128, std::size_t>>;

template <class IdAt> Positions id_positions(std::size_t n, IdAt id_at) {
    Positions p;
    for (std::size_t i = 0; i < n; ++i)
        if (const std::optional<Hash128>& id = id_at(i))
            p.emplace_back(*id, i);
    std::sort(p.begin(), p.end());
    if (std::adjacent_find(p.begin(), p.end(), [](auto& x, auto& y) { return x.first == y.first; }) !=
        p.end())
        throw Error(ErrorCategory::internal, "sequence_merge: duplicate element id");
    return p;
}

Positions id_positions(Items s) {
    return id_positions(s.size(), [&](std::size_t i) -> auto& { return s[i].id; });
}

std::optional<std::size_t> position(const Positions& p, const Hash128& id) {
    auto f = std::lower_bound(p.begin(), p.end(), id, [](auto& e, const Hash128& k) { return e.first < k; });
    return f != p.end() && f->first == id ? std::optional(f->second) : std::nullopt;
}

// Ids present in base and side whose relative order changed: those outside one longest increasing
// subsequence of side positions taken in base order.
// Returned sorted, for binary_search.
std::vector<Hash128> moved_ids(Items base, const Positions& side_pos) {
    std::vector<std::pair<std::size_t, Hash128>> seq; // (side position, id) in base order
    for (auto& it : base)
        if (it.id)
            if (auto p = position(side_pos, *it.id))
                seq.emplace_back(*p, *it.id);
    std::vector<std::size_t> tails, tail_idx, prev(seq.size(), npos);
    for (std::size_t i = 0; i < seq.size(); ++i) {
        auto pos = std::lower_bound(tails.begin(), tails.end(), seq[i].first) - tails.begin();
        if (static_cast<std::size_t>(pos) == tails.size())
            tails.push_back(seq[i].first), tail_idx.push_back(i);
        else
            tails[pos] = seq[i].first, tail_idx[pos] = i;
        prev[i] = pos ? tail_idx[pos - 1] : npos;
    }
    std::vector<bool> stays(seq.size());
    for (std::size_t i = tail_idx.empty() ? npos : tail_idx.back(); i != npos; i = prev[i])
        stays[i] = true;
    std::vector<Hash128> moved;
    for (std::size_t i = 0; i < seq.size(); ++i)
        if (!stays[i])
            moved.push_back(seq[i].second);
    std::sort(moved.begin(), moved.end());
    return moved;
}

// Content hash -> token. Hash128 is already a hash, so its low half is the bucket key.
struct LowHalf {
    std::size_t operator()(const Hash128& h) const noexcept { return static_cast<std::size_t>(h.lo); }
};
using Tokens = std::unordered_map<Hash128, Token, LowHalf>;

// Base -> side matching over the reduced sequences: stayed ids anchor, the gaps between anchors are
// diffed by content. Returns, per base element, the matched side position or npos.
std::vector<std::size_t> match(const std::vector<const SeqItem*>& b, const std::vector<const SeqItem*>& s,
                               DiffAlgorithm alg, Tokens& tokens, Token& unique) {
    const auto s_ids = id_positions(s.size(), [&](std::size_t i) -> auto& { return s[i]->id; });
    std::vector<std::size_t> m(b.size(), npos);
    auto token = [&](const SeqItem* it) {
        if (it->id)
            return unique++; // id'd elements match only by id
        return tokens.emplace(it->content, tokens.size()).first->second;
    };
    // Tokens compare only for equality, so skipping a gap with nothing to match changes no result.
    auto gap = [&](std::size_t b0, std::size_t b1, std::size_t s0, std::size_t s1) {
        if (b0 == b1 || s0 == s1)
            return;
        std::vector<Token> tb, ts;
        for (auto i = b0; i < b1; ++i)
            tb.push_back(token(b[i]));
        for (auto i = s0; i < s1; ++i)
            ts.push_back(token(s[i]));
        for (auto [x, y] : diff_match(tb, ts, alg))
            m[b0 + x] = s0 + y;
    };
    std::size_t pb = 0, ps = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (!b[i]->id)
            continue;
        auto j = position(s_ids, *b[i]->id);
        if (!j)
            continue;
        gap(pb, i, ps, *j);
        m[i] = *j;
        pb = i + 1, ps = *j + 1;
    }
    gap(pb, b.size(), ps, s.size());
    return m;
}

struct Merger {
    Items base, ours, theirs;
    const SeqMergeOptions& opt;
    std::vector<SeqChunk> out;

    // Content three-way for one identity-matched element.
    SeqChunk element(std::optional<std::size_t> b, std::size_t o, std::size_t t) {
        SeqChunk c;
        if (b)
            c.base = {*b};
        c.ours = {o};
        c.theirs = {t};
        const Hash128* hb = b ? &base[*b].content : nullptr;
        const Hash128 &ho = ours[o].content, &ht = theirs[t].content;
        if (hb && ho == *hb && ht == *hb)
            c.kind = SeqKind::stable;
        else if (hb && ho == *hb)
            c.kind = SeqKind::theirs;
        else if (hb && ht == *hb)
            c.kind = SeqKind::ours;
        else if (ho == ht)
            c.kind = SeqKind::identical;
        else if (opt.recurse && opt.recurse(b, o, t))
            c.kind = SeqKind::recursed;
        else
            c.kind = SeqKind::conflict, c.reason = "both-modified";
        return c;
    }

    void push_stable(std::size_t b, std::size_t o, std::size_t t) {
        if (out.empty() || out.back().kind != SeqKind::stable || out.back().moved)
            out.push_back({});
        out.back().base.push_back(b);
        out.back().ours.push_back(o);
        out.back().theirs.push_back(t);
    }

    void unstable(std::vector<std::size_t> b, std::vector<std::size_t> o, std::vector<std::size_t> t) {
        auto same = [](Items x, const std::vector<std::size_t>& xi, Items y,
                       const std::vector<std::size_t>& yi) {
            return xi.size() == yi.size() &&
                   std::equal(xi.begin(), xi.end(), yi.begin(),
                              [&](auto i, auto j) { return x[i].content == y[j].content; });
        };
        bool oc = !same(base, b, ours, o), tc = !same(base, b, theirs, t);
        SeqChunk c{SeqKind::stable, std::move(b), std::move(o), std::move(t)};
        if (oc && !tc)
            c.kind = SeqKind::ours;
        else if (tc && !oc)
            c.kind = SeqKind::theirs;
        else if (oc && same(ours, c.ours, theirs, c.theirs))
            c.kind = SeqKind::identical;
        else if (oc && c.base.empty() && opt.insertion_order_insignificant)
            c.kind = SeqKind::both_inserted;
        else if (oc) {
            c.kind = SeqKind::conflict;
            c.reason = c.ours.empty() || c.theirs.empty() ? "modify-delete"
                       : c.base.empty()                   ? "add-add"
                                                          : "both-modified";
        }
        out.push_back(std::move(c));
    }

    // diff3 chunking over reduced sequences (rb/ro/rt map reduced -> original indices).
    void chunk(const std::vector<std::size_t>& rb, const std::vector<std::size_t>& ro,
               const std::vector<std::size_t>& rt, const std::vector<std::size_t>& mo,
               const std::vector<std::size_t>& mt) {
        std::size_t ib = 0, io = 0, it = 0;
        const std::size_t nb = rb.size(), no = ro.size(), nt = rt.size();
        for (;;) {
            while (ib < nb && mo[ib] == io && mt[ib] == it) {
                auto& hb = base[rb[ib]].content;
                if (ours[ro[io]].content == hb && theirs[rt[it]].content == hb) // element()'s stable case
                    push_stable(rb[ib], ro[io], rt[it]);
                else
                    out.push_back(element(rb[ib], ro[io], rt[it]));
                ++ib, ++io, ++it;
            }
            if (ib == nb && io == no && it == nt)
                return;
            std::size_t sb = ib;
            while (sb < nb && (mo[sb] == npos || mt[sb] == npos))
                ++sb;
            std::size_t so = sb < nb ? mo[sb] : no, st = sb < nb ? mt[sb] : nt;
            auto pick = [](const std::vector<std::size_t>& r, std::size_t from, std::size_t to) {
                return std::vector<std::size_t>(r.begin() + static_cast<std::ptrdiff_t>(from),
                                                r.begin() + static_cast<std::ptrdiff_t>(to));
            };
            unstable(pick(rb, ib, sb), pick(ro, io, so), pick(rt, it, st));
            ib = sb, io = so, it = st;
        }
    }

    // Inserts `c` after the chunk holding `side` element `after` (npos: at the start). An aligned stable
    // chunk is split so the element lands exactly after `after`.
    // ponytail: a linear scan of the output per moved element, O(moves x n); index the chunks if
    // sequences with thousands of moves get slow.
    void place(SeqChunk c, Side side, std::size_t after) {
        std::size_t at = 0;
        if (after != npos) {
            for (std::size_t k = 0; k < out.size(); ++k) {
                auto& list = side == Side::ours ? out[k].ours : out[k].theirs;
                auto f = std::find(list.begin(), list.end(), after);
                if (f == list.end())
                    continue;
                auto q = static_cast<std::size_t>(f - list.begin()) + 1;
                at = k + 1;
                if (out[k].kind == SeqKind::stable && q < list.size()) {
                    SeqChunk tail{SeqKind::stable};
                    for (auto v : {&SeqChunk::base, &SeqChunk::ours, &SeqChunk::theirs}) {
                        auto& src = out[k].*v;
                        (tail.*v).assign(src.begin() + static_cast<std::ptrdiff_t>(q), src.end());
                        src.resize(q);
                    }
                    out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), std::move(tail));
                }
                break;
            }
        }
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), std::move(c));
    }
};

Hash128 identity(const SeqItem& it) {
    return it.id ? *it.id : it.content;
}

} // namespace

std::vector<SeqChunk> sequence_merge(Items base, Items ours, Items theirs, const SeqMergeOptions& opt) {
    const auto bpos = id_positions(base), opos = id_positions(ours), tpos = id_positions(theirs);
    const auto mv_o = moved_ids(base, opos), mv_t = moved_ids(base, tpos);
    std::vector<Hash128> moved;
    std::set_union(mv_o.begin(), mv_o.end(), mv_t.begin(), mv_t.end(), std::back_inserter(moved));
    auto in = [](const std::vector<Hash128>& v, const Hash128& id) {
        return std::binary_search(v.begin(), v.end(), id);
    };

    auto reduce = [&](Items s, std::vector<std::size_t>& idx, std::vector<const SeqItem*>& items) {
        for (std::size_t i = 0; i < s.size(); ++i)
            if (!s[i].id || !in(moved, *s[i].id))
                idx.push_back(i), items.push_back(&s[i]);
    };
    std::vector<std::size_t> rb, ro, rt;
    std::vector<const SeqItem*> ib, io, it;
    reduce(base, rb, ib);
    reduce(ours, ro, io);
    reduce(theirs, rt, it);

    Tokens tokens;
    Token unique = Token{1} << 63;
    auto mo = match(ib, io, opt.algorithm, tokens, unique);
    auto mt = match(ib, it, opt.algorithm, tokens, unique);

    Merger m{base, ours, theirs, opt, {}};
    m.chunk(rb, ro, rt, mo, mt);

    // Moved elements, placed after their nearest placed predecessor in the mover's sequence.
    std::vector<bool> placed_o(ours.size()), placed_t(theirs.size()); // side elements already in m.out
    auto mark = [&](const SeqChunk& c) {
        for (auto i : c.ours)
            placed_o[i] = true;
        for (auto i : c.theirs)
            placed_t[i] = true;
    };
    for (auto& c : m.out)
        mark(c);
    auto predecessor = [&](Side side, std::size_t i) {
        auto& placed = side == Side::ours ? placed_o : placed_t;
        while (i-- > 0)
            if (placed[i])
                return i;
        return npos;
    };
    std::vector<std::pair<Side, std::size_t>> order; // mover side and its index, in mover order
    for (std::size_t i = 0; i < ours.size(); ++i)
        if (ours[i].id && in(mv_o, *ours[i].id))
            order.emplace_back(Side::ours, i);
    for (std::size_t i = 0; i < theirs.size(); ++i)
        if (theirs[i].id && in(mv_t, *theirs[i].id) && !in(mv_o, *theirs[i].id))
            order.emplace_back(Side::theirs, i);

    for (auto [side, i] : order) {
        const Hash128 id = *(side == Side::ours ? ours : theirs)[i].id;
        auto b = position(bpos, id), o = position(opos, id), t = position(tpos, id);
        SeqChunk c;
        if (!o || !t) { // the other side deleted what this side moved
            c.kind = SeqKind::conflict;
            c.reason = "modify-delete";
            c.base = {*b};
            if (o)
                c.ours = {*o};
            if (t)
                c.theirs = {*t};
        } else {
            c = m.element(b, *o, *t);
            if (in(mv_o, id) && in(mv_t, id)) {
                // Same place: the same immediate predecessor (or none) on both sides.
                bool same_place = (*o == 0 && *t == 0) ||
                                  (*o > 0 && *t > 0 && identity(ours[*o - 1]) == identity(theirs[*t - 1]));
                if (!same_place) {
                    // ponytail: a move-move conflict sits at ours' position whatever the resolution.
                    c.kind = SeqKind::conflict;
                    c.reason = "move-move";
                }
            }
        }
        c.moved = true;
        const auto after = predecessor(side, i);
        mark(c);
        m.place(std::move(c), side, after);
    }
    return std::move(m.out);
}

} // namespace semsplinter
