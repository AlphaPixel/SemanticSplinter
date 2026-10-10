#include <semsplinter/merge.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <set>

namespace semsplinter {

std::string_view to_string(MergeRule r) noexcept {
    switch (r) {
    case MergeRule::unchanged:
        return "unchanged";
    case MergeRule::ours:
        return "ours";
    case MergeRule::theirs:
        return "theirs";
    case MergeRule::identical:
        return "identical";
    case MergeRule::recursed:
        return "recursed";
    case MergeRule::conflict:
        return "conflict";
    }
    return "unchanged";
}

// ---- GridMerge ----

namespace {

enum Cell : std::uint8_t { kSame, kOurs, kTheirs, kIdentical, kBoth }; // kBoth: changed differently

struct Geometry {
    std::array<std::uint64_t, 3> dim{1, 1, 1};
    std::size_t rank;
    std::uint64_t cells = 1;

    explicit Geometry(const GridShape& s) : rank(s.size()) {
        if (s.empty() || s.size() > 3)
            throw Error(ErrorCategory::internal, "grid_merge: rank must be 1 to 3");
        for (std::size_t i = 0; i < s.size(); ++i)
            dim[i] = s[i], cells *= s[i];
    }
    std::array<std::int64_t, 3> coord(std::uint64_t i) const {
        return {static_cast<std::int64_t>(i % dim[0]), static_cast<std::int64_t>(i / dim[0] % dim[1]),
                static_cast<std::int64_t>(i / dim[0] / dim[1])};
    }
    // Calls fn(cell, chebyshev distance) for every cell within radius r of i, i included.
    template <class Fn> void around(std::uint64_t i, std::int64_t r, Fn&& fn) const {
        auto c = coord(i);
        std::int64_t rz = rank > 2 ? r : 0, ry = rank > 1 ? r : 0;
        for (std::int64_t dz = -rz; dz <= rz; ++dz)
            for (std::int64_t dy = -ry; dy <= ry; ++dy)
                for (std::int64_t dx = -r; dx <= r; ++dx) {
                    std::int64_t x = c[0] + dx, y = c[1] + dy, z = c[2] + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= static_cast<std::int64_t>(dim[0]) ||
                        y >= static_cast<std::int64_t>(dim[1]) || z >= static_cast<std::int64_t>(dim[2]))
                        continue;
                    fn(static_cast<std::uint64_t>(x) +
                           dim[0] * (static_cast<std::uint64_t>(y) + dim[1] * static_cast<std::uint64_t>(z)),
                       std::max({std::abs(dx), std::abs(dy), std::abs(dz)}));
                }
    }
    Region full() const {
        if (rank == 1)
            return Range{0, dim[0]};
        if (rank == 2)
            return Rect{0, 0, dim[0], dim[1]};
        return Box{0, 0, 0, dim[0], dim[1], dim[2]};
    }
    Region bounds(const std::vector<std::uint64_t>& cells_in) const {
        std::array<std::int64_t, 3> lo{INT64_MAX, INT64_MAX, INT64_MAX}, hi{-1, -1, -1};
        for (auto i : cells_in) {
            auto c = coord(i);
            for (int k = 0; k < 3; ++k)
                lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
        }
        auto u = [](std::int64_t v) { return static_cast<std::uint64_t>(v); };
        auto ext = [&](int k) { return u(hi[k] - lo[k] + 1); };
        if (rank == 1)
            return Range{u(lo[0]), ext(0)};
        if (rank == 2)
            return Rect{u(lo[0]), u(lo[1]), ext(0), ext(1)};
        return Box{u(lo[0]), u(lo[1]), u(lo[2]), ext(0), ext(1), ext(2)};
    }
};

Region merge_bounds(const Region& a, const Region& b) {
    if (auto* r = std::get_if<Range>(&a)) {
        auto& s = std::get<Range>(b);
        auto lo = std::min(r->start, s.start), hi = std::max(r->start + r->length, s.start + s.length);
        return Range{lo, hi - lo};
    }
    if (auto* r = std::get_if<Rect>(&a)) {
        auto& s = std::get<Rect>(b);
        auto x = std::min(r->x, s.x), y = std::min(r->y, s.y);
        return Rect{x, y, std::max(r->x + r->w, s.x + s.w) - x, std::max(r->y + r->h, s.y + s.h) - y};
    }
    auto& r = std::get<Box>(a);
    auto& s = std::get<Box>(b);
    auto x = std::min(r.x, s.x), y = std::min(r.y, s.y), z = std::min(r.z, s.z);
    return Box{x,
               y,
               z,
               std::max(r.x + r.w, s.x + s.w) - x,
               std::max(r.y + r.h, s.y + s.h) - y,
               std::max(r.z + r.d, s.z + s.d) - z};
}

// Groups cells of one side into regions (radius 1 + gap); labels[cell] = region index or -1.
std::vector<GridRegion> label(const Geometry& g, const std::vector<std::uint8_t>& cls, bool ours_side,
                              unsigned gap, std::vector<std::int32_t>& labels) {
    auto member = [&](std::uint64_t i) {
        auto c = cls[i];
        return c == kIdentical || c == kBoth || c == (ours_side ? kOurs : kTheirs);
    };
    labels.assign(g.cells, -1);
    std::vector<GridRegion> regions;
    std::vector<std::uint64_t> stack;
    for (std::uint64_t i = 0; i < g.cells; ++i) {
        if (!member(i) || labels[i] >= 0)
            continue;
        auto id = static_cast<std::int32_t>(regions.size());
        GridRegion r;
        labels[i] = id;
        stack.push_back(i);
        while (!stack.empty()) {
            auto c = stack.back();
            stack.pop_back();
            r.cells.push_back(c);
            g.around(c, 1 + static_cast<std::int64_t>(gap), [&](std::uint64_t n, std::int64_t) {
                if (member(n) && labels[n] < 0)
                    labels[n] = id, stack.push_back(n);
            });
        }
        std::sort(r.cells.begin(), r.cells.end());
        r.bounds = g.bounds(r.cells);
        regions.push_back(std::move(r));
    }
    return regions;
}

} // namespace

GridResult grid_merge(const GridShape& base, const GridShape& ours, const GridShape& theirs,
                      const std::function<bool(Side, Side, std::uint64_t)>& same, const GridOptions& opt) {
    GridResult res;
    auto untouched = [&](Side s) {
        Geometry g(base);
        for (std::uint64_t i = 0; i < g.cells; ++i)
            if (!same(Side::base, s, i))
                return false;
        return true;
    };
    if (ours != base || theirs != base) {
        if (ours == theirs) { // both reshaped the same way
            Geometry g(ours);
            bool eq = true;
            for (std::uint64_t i = 0; i < g.cells && eq; ++i)
                eq = same(Side::ours, Side::theirs, i);
            if (eq)
                res.whole = Side::ours;
            else
                res.shape_conflict = true;
        } else if (theirs == base && untouched(Side::theirs)) {
            res.whole = Side::ours;
        } else if (ours == base && untouched(Side::ours)) {
            res.whole = Side::theirs;
        } else {
            res.shape_conflict = true;
        }
        if (res.shape_conflict)
            res.conflicts.push_back({{}, {}, Geometry(base).full(), "shape-mismatch"});
        return res;
    }

    Geometry g(base);
    // Working memory (#28): class, take, and two labels per cell, then 8 bytes per region cell.
    constexpr std::uint64_t kPerCell = sizeof(std::uint8_t) + sizeof(Side) + 2 * sizeof(std::int32_t);
    auto charge = [&](std::uint64_t n, std::uint64_t each) {
        if (opt.budget)
            opt.budget->charge(n > UINT64_MAX / each ? UINT64_MAX : n * each);
    };
    charge(g.cells, kPerCell);
    std::vector<std::uint8_t> cls(g.cells);
    res.take.assign(g.cells, Side::base);
    std::uint64_t region_cells = 0; // ours' plus theirs'
    for (std::uint64_t i = 0; i < g.cells; ++i) {
        bool ob = same(Side::base, Side::ours, i), tb = same(Side::base, Side::theirs, i);
        cls[i] = ob && tb                            ? kSame
                 : tb                                ? kOurs
                 : ob                                ? kTheirs
                 : same(Side::ours, Side::theirs, i) ? kIdentical
                                                     : kBoth;
        res.take[i] = cls[i] == kSame ? Side::base : cls[i] == kTheirs ? Side::theirs : Side::ours;
        region_cells += cls[i] == kSame ? 0 : cls[i] == kOurs || cls[i] == kTheirs ? 1 : 2;
    }
    charge(region_cells, sizeof(std::uint64_t));
    std::vector<std::int32_t> lo, lt;
    res.ours = label(g, cls, true, opt.gap, lo);
    res.theirs = label(g, cls, false, opt.gap, lt);

    // Union-find over regions: ours are 0..no-1, theirs no.. . Edge when an ours-changed cell and a
    // theirs-changed cell are within `guard` (distance 0 means a cell both changed differently).
    const std::size_t no = res.ours.size();
    std::vector<std::size_t> parent(no + res.theirs.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    auto find = [&](std::size_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    std::set<std::pair<std::size_t, std::size_t>> edges, near;
    const std::int64_t radius = std::max<std::int64_t>(opt.guard, 1);
    for (std::size_t r = 0; r < no; ++r)
        for (auto c : res.ours[r].cells) {
            if (cls[c] != kOurs && cls[c] != kBoth)
                continue;
            g.around(c, radius, [&](std::uint64_t n, std::int64_t d) {
                if ((cls[n] != kTheirs && cls[n] != kBoth) || lt[n] < 0)
                    return;
                auto t = static_cast<std::size_t>(lt[n]);
                if (d <= static_cast<std::int64_t>(opt.guard))
                    edges.emplace(r, t);
                else if (d == 1)
                    near.emplace(r, t);
            });
        }
    for (auto [r, t] : edges)
        parent[find(r)] = find(no + t);

    std::map<std::size_t, std::size_t> group; // root -> conflict index
    for (auto [r, t] : edges) {
        auto root = find(r);
        if (group.emplace(root, res.conflicts.size()).second)
            res.conflicts.push_back({{}, {}, res.ours[r].bounds, opt.guard ? "guard-band" : "both-modified"});
    }
    for (std::size_t i = 0; i < parent.size(); ++i) {
        auto f = group.find(find(i));
        if (f == group.end())
            continue;
        auto& c = res.conflicts[f->second];
        auto& reg = i < no ? res.ours[i] : res.theirs[i - no];
        (i < no ? c.ours : c.theirs).push_back(i < no ? i : i - no);
        c.bounds = merge_bounds(c.bounds, reg.bounds);
        for (auto cell : reg.cells)
            res.take[cell] = Side::ours;
    }
    // A guard-band conflict whose regions share a cell both changed differently is plain both-modified.
    for (auto& c : res.conflicts)
        for (auto r : c.ours)
            for (auto cell : res.ours[r].cells)
                if (cls[cell] == kBoth)
                    c.reason = "both-modified";
    for (auto [r, t] : near)
        if (find(r) != find(no + t))
            res.adjacent.emplace_back(r, t);
    return res;
}

void grid_resolve(GridResult& res, std::size_t conflict, Side side) {
    auto& c = res.conflicts.at(conflict);
    if (c.reason == "shape-mismatch") {
        res.whole = side;
        return;
    }
    for (auto r : c.ours)
        for (auto cell : res.ours[r].cells)
            res.take[cell] = side;
    for (auto r : c.theirs)
        for (auto cell : res.theirs[r].cells)
            res.take[cell] = side;
}

// ---- Conflict policy ----

std::map<std::string, std::string> parse_resolutions(std::string_view text) {
    std::map<std::string, std::string> out;
    try {
        auto j = nlohmann::json::parse(text);
        if (!j.is_object())
            throw Error(ErrorCategory::invalid_input, "resolve file: expected a JSON object");
        for (auto& [k, v] : j.items()) {
            if (!v.is_string())
                throw Error(ErrorCategory::invalid_input,
                            "resolve file: choice for " + k + " is not a string");
            out[k] = v.get<std::string>();
        }
    } catch (const nlohmann::json::exception& e) {
        throw Error(ErrorCategory::invalid_input, std::string("resolve file: ") + e.what());
    }
    return out;
}

void apply_conflict_policy(std::vector<Conflict>& conflicts, const ConflictOptions& opt,
                           std::vector<Warning>& warnings) {
    auto offered = [](const Conflict& c, const std::string& choice) {
        return std::find(c.choices.begin(), c.choices.end(), choice) != c.choices.end();
    };
    std::set<std::string> used;
    for (auto& c : conflicts) {
        c.resolved = false;
        if (auto r = opt.resolutions.find(c.id); r != opt.resolutions.end()) {
            if (!offered(c, r->second))
                throw Error(ErrorCategory::invalid_input,
                            "resolve file: conflict " + c.id + " does not offer '" + r->second + "'");
            c.resolved_as = r->second, c.resolved = true;
            used.insert(c.id);
            continue;
        }
        if (opt.resolve_all != ResolveAll::none) {
            std::string choice = opt.resolve_all == ResolveAll::ours     ? "ours"
                                 : opt.resolve_all == ResolveAll::theirs ? "theirs"
                                                                         : "union";
            if (!offered(c, choice))
                throw Error(ErrorCategory::unsupported, "--" + choice + " is not supported for conflict " +
                                                            c.id + " at " + c.at.to_string());
            c.resolved_as = choice, c.resolved = true;
            continue;
        }
        switch (opt.policy) {
        case Policy::ours:
            c.resolved_as = "ours";
            break;
        case Policy::theirs:
            c.resolved_as = "theirs";
            break;
        case Policy::base:
            c.resolved_as = "base";
            break;
        case Policy::mark:
            if (offered(c, "mark")) {
                c.resolved_as = "mark";
            } else {
                c.resolved_as = "ours";
                warnings.push_back({"mark-unsupported", c.at.to_string(), "no conflict marking; wrote ours"});
            }
            break;
        }
    }
    for (auto& [id, choice] : opt.resolutions)
        if (!used.count(id))
            warnings.push_back({"resolve-unmatched", {}, "no conflict with id " + id});
}

// ---- Dependencies ----

void check_dependencies(const ChangeSet& ours, const ChangeSet& theirs, std::vector<AutoMerge>& auto_merged,
                        std::vector<Conflict>& conflicts,
                        const std::function<bool(const Change&, const Change&)>& upgrade) {
    std::set<std::string> in_conflict;
    for (auto& c : conflicts)
        in_conflict.insert(c.changes.begin(), c.changes.end());
    auto check = [&](const ChangeSet& mine, const ChangeSet& other) {
        for (auto& c : mine) {
            if (in_conflict.count(c.id))
                continue;
            for (auto& dep : c.depends_on) {
                for (auto& o : other) {
                    if (!path_within(dep, o.at.path) && !path_within(o.at.path, dep))
                        continue;
                    if (o.op == Op::erase || (upgrade && upgrade(c, o))) {
                        auto x = make_conflict(c.at, {c.id, o.id}, "dependency");
                        if (std::none_of(conflicts.begin(), conflicts.end(),
                                         [&](const Conflict& e) { return e.id == x.id; }))
                            conflicts.push_back(std::move(x));
                    } else {
                        AutoMerge note{c.id, "dependency-touched"};
                        if (std::find(auto_merged.begin(), auto_merged.end(), note) == auto_merged.end())
                            auto_merged.push_back(note);
                    }
                }
            }
        }
    };
    check(ours, theirs);
    check(theirs, ours);
}

} // namespace semsplinter
