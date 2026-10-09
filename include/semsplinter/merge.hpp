#pragma once

#include <semsplinter/error.hpp>
#include <semsplinter/hash.hpp>
#include <semsplinter/model.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace semsplinter {

// ---- KeyedMerge and OpaqueLeaf (SPEC §5.1) ----

enum class MergeRule : std::uint8_t {
    unchanged, // base == ours == theirs
    ours,      // only ours changed (edit, add, or delete)
    theirs,    // only theirs changed
    identical, // both made the same change (including both deleting)
    recursed,  // composite value merged by the format's callback
    conflict,
};

std::string_view to_string(MergeRule) noexcept;

struct LeafOutcome {
    MergeRule rule = MergeRule::unchanged;
    std::optional<Side>
        take;           // side whose value is written; nullopt: absent. For conflicts: ours (policy default)
    std::string reason; // both-modified, modify-delete, add-add
};

// Three-way decision for one unit. Absent sides are nullptr. `eq` compares two present values;
// `recurse(base_or_null, ours, theirs)` may merge a composite value that both sides changed.
template <class V, class Eq, class Recurse>
LeafOutcome merge_leaf(const V* b, const V* o, const V* t, Eq&& eq, Recurse&& recurse) {
    LeafOutcome r;
    auto conflict = [&](const char* why) {
        r.rule = MergeRule::conflict, r.reason = why;
        r.take = o ? std::optional(Side::ours) : std::nullopt;
        return r;
    };
    if (b && o && t) {
        bool bo = eq(*b, *o), bt = eq(*b, *t);
        if (bo && bt)
            r.rule = MergeRule::unchanged, r.take = Side::ours;
        else if (bo)
            r.rule = MergeRule::theirs, r.take = Side::theirs;
        else if (bt)
            r.rule = MergeRule::ours, r.take = Side::ours;
        else if (eq(*o, *t))
            r.rule = MergeRule::identical, r.take = Side::ours;
        else if (recurse(b, *o, *t))
            r.rule = MergeRule::recursed, r.take = Side::ours;
        else
            return conflict("both-modified");
    } else if (b && o) { // theirs deleted
        if (!eq(*b, *o))
            return conflict("modify-delete");
        r.rule = MergeRule::theirs;
    } else if (b && t) { // ours deleted
        if (!eq(*b, *t))
            return conflict("modify-delete");
        r.rule = MergeRule::ours;
    } else if (b) {
        r.rule = MergeRule::identical; // both deleted
    } else if (o && t) {
        if (eq(*o, *t))
            r.rule = MergeRule::identical, r.take = Side::ours;
        else if (recurse(nullptr, *o, *t))
            r.rule = MergeRule::recursed, r.take = Side::ours;
        else
            return conflict("add-add");
    } else if (o) {
        r.rule = MergeRule::ours, r.take = Side::ours;
    } else if (t) {
        r.rule = MergeRule::theirs, r.take = Side::theirs;
    }
    return r;
}

// OpaqueLeaf: an indivisible value compared by hash.
inline LeafOutcome opaque_leaf(const Hash128* b, const Hash128* o, const Hash128* t) {
    return merge_leaf(b, o, t, std::equal_to<>{},
                      [](const Hash128*, const Hash128&, const Hash128&) { return false; });
}

// What a conflicted unit holds under a choice ("ours", "theirs", "base").
inline std::optional<Side> take_for(std::string_view choice, bool in_base, bool in_ours, bool in_theirs) {
    if (choice == "theirs")
        return in_theirs ? std::optional(Side::theirs) : std::nullopt;
    if (choice == "base")
        return in_base ? std::optional(Side::base) : std::nullopt;
    return in_ours ? std::optional(Side::ours) : std::nullopt;
}

template <class K> struct KeyedRow {
    K key;
    std::optional<std::size_t> base, ours, theirs; // indices into the inputs
    LeafOutcome outcome;
};

// Merges three ordered key/value lists. Rows come back in output order: ours' order, with keys ours
// lacks inserted after their nearest preceding key in theirs that is already placed (ties keep theirs'
// order). Keys only in base (deleted by both) follow at the end. `recurse(key, base_or_null, ours, theirs)`.
template <class K, class V, class Eq = std::equal_to<>>
std::vector<KeyedRow<K>>
keyed_merge(std::span<const std::pair<K, V>> base, std::span<const std::pair<K, V>> ours,
            std::span<const std::pair<K, V>> theirs, Eq eq = {},
            const std::function<bool(const K&, const V*, const V&, const V&)>& recurse = {}) {
    auto index = [](std::span<const std::pair<K, V>> s) {
        std::map<K, std::size_t> m;
        for (std::size_t i = 0; i < s.size(); ++i)
            if (!m.emplace(s[i].first, i).second)
                throw Error(ErrorCategory::internal, "keyed_merge: duplicate key");
        return m;
    };
    auto ib = index(base), io = index(ours), it = index(theirs);
    auto at = [](const std::map<K, std::size_t>& m, const K& k) -> std::optional<std::size_t> {
        auto f = m.find(k);
        return f == m.end() ? std::nullopt : std::optional(f->second);
    };
    auto row = [&](const K& k) {
        KeyedRow<K> r{k, at(ib, k), at(io, k), at(it, k), {}};
        const V* b = r.base ? &base[*r.base].second : nullptr;
        const V* o = r.ours ? &ours[*r.ours].second : nullptr;
        const V* t = r.theirs ? &theirs[*r.theirs].second : nullptr;
        r.outcome = merge_leaf(b, o, t, eq, [&](const V* vb, const V& vo, const V& vt) {
            return recurse && recurse(k, vb, vo, vt);
        });
        return r;
    };
    // after[i + 1]: theirs-only keys that follow ours[i]; after[0]: those before every ours key.
    std::vector<std::vector<const K*>> after(ours.size() + 1);
    std::size_t anchor = 0;
    for (auto& [k, v] : theirs) {
        if (auto o = at(io, k))
            anchor = *o + 1;
        else
            after[anchor].push_back(&k);
    }
    std::vector<KeyedRow<K>> out;
    for (std::size_t i = 0; i <= ours.size(); ++i) {
        if (i > 0)
            out.push_back(row(ours[i - 1].first));
        for (auto* k : after[i])
            out.push_back(row(*k));
    }
    for (auto& [k, v] : base)
        if (!io.count(k) && !it.count(k))
            out.push_back(row(k));
    return out;
}

// ---- GridMerge (SPEC §5.1) ----

// Dense grid of 1 to 3 dimensions; dims[0] varies fastest (x, then y, then z).
using GridShape = std::vector<std::uint64_t>;

struct GridOptions {
    unsigned gap = 0;   // --gap: same-side changed cells closer than this join one region
    unsigned guard = 0; // --guard: ours/theirs regions within this many cells conflict as one (D-10)
};

struct GridRegion {
    Region bounds;                    // bounding rect/range/box
    std::vector<std::uint64_t> cells; // changed cell indices, ascending
};

struct GridConflict {
    std::vector<std::size_t> ours, theirs; // region indices
    Region bounds;
    std::string reason; // both-modified, guard-band, shape-mismatch
};

struct GridResult {
    // Shape: set when one side's whole grid is the result (it reshaped and the other left the grid
    // untouched, or both reshaped identically). Cell data below is then empty.
    std::optional<Side> whole;
    bool shape_conflict = false; // whole grid conflicts (shape-mismatch); `take` the chosen side wholesale

    std::vector<Side> take;               // per cell: which side's cell value is written
    std::vector<GridRegion> ours, theirs; // changed regions per side
    std::vector<GridConflict> conflicts;  // cells start out as ours (policy default)
    std::vector<std::pair<std::size_t, std::size_t>>
        adjacent; // (ours, theirs) regions 1 cell apart: warning `adjacent-edits`
};

// `same(a, b, cell)` compares one cell of two sides (a tolerance lives here). It is called only with
// equal shapes, except while checking whether a side touched the grid at all.
// ponytail: dense per-cell labels, ~9 bytes per cell; tile the grid if 2^30-cell merges matter.
GridResult grid_merge(const GridShape& base, const GridShape& ours, const GridShape& theirs,
                      const std::function<bool(Side, Side, std::uint64_t)>& same, const GridOptions& = {});

// Rewrites a conflict's cells to `side`.
void grid_resolve(GridResult&, std::size_t conflict, Side side);

// ---- Conflict policy (SPEC §5.4) ----

enum class Policy : std::uint8_t { ours, theirs, base, mark };       // --conflict=
enum class ResolveAll : std::uint8_t { none, ours, theirs, union_ }; // --ours / --theirs / --union

struct ConflictOptions {
    Policy policy = Policy::ours;
    ResolveAll resolve_all = ResolveAll::none;
    std::map<std::string, std::string> resolutions; // --resolve=FILE: conflict id -> choice
};

// Parses a --resolve file: { "x-1a2b...": "theirs", ... }. Throws Error{invalid_input}.
std::map<std::string, std::string> parse_resolutions(std::string_view json);

// Sets resolved_as/resolved on every conflict. A resolution naming an unoffered choice, or --union
// where a conflict offers no union, is an error. Unmatched resolution ids warn `resolve-unmatched`;
// `mark` without a marking falls back to ours with `mark-unsupported`.
void apply_conflict_policy(std::vector<Conflict>&, const ConflictOptions&, std::vector<Warning>&);

// ---- Dependencies (SPEC §5.3) ----

// Checks each auto-merged change's depends_on against the other side's changes at, above, or below the
// dependency's address. A dependency the other side erased is a `dependency` conflict; one it otherwise
// changed is noted `dependency-touched`, unless `upgrade(dependent, other)` makes it a conflict.
void check_dependencies(const ChangeSet& ours, const ChangeSet& theirs, std::vector<AutoMerge>&,
                        std::vector<Conflict>&,
                        const std::function<bool(const Change&, const Change&)>& upgrade = {});

} // namespace semsplinter
