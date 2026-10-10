#include <semsplinter/merge.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <random>

using namespace semsplinter;
using KV = std::pair<std::string, int>;
using List = std::vector<KV>;

namespace {
std::vector<KeyedRow<std::string>> km(const List& b, const List& o, const List& t) {
    return keyed_merge<std::string, int>(b, o, t);
}
} // namespace

TEST_CASE("KeyedMerge: every row of the SPEC 5.1 table") {
    struct Row {
        std::optional<int> b, o, t;
        MergeRule rule;
        std::optional<Side> take;
        const char* reason;
    };
    const auto n = std::nullopt;
    // base, ours, theirs -> rule, written side, conflict reason
    std::vector<Row> table{
        {1, 1, 1, MergeRule::unchanged, Side::ours, ""},
        {1, 2, 1, MergeRule::ours, Side::ours, ""},
        {1, 1, 3, MergeRule::theirs, Side::theirs, ""},
        {1, 2, 2, MergeRule::identical, Side::ours, ""},
        {1, 2, 3, MergeRule::conflict, Side::ours, "both-modified"},
        {1, n, 1, MergeRule::ours, n, ""},
        {1, n, 3, MergeRule::conflict, n, "modify-delete"},
        {1, 2, n, MergeRule::conflict, Side::ours, "modify-delete"},
        {1, 1, n, MergeRule::theirs, n, ""},
        {1, n, n, MergeRule::identical, n, ""},
        {n, 2, n, MergeRule::ours, Side::ours, ""},
        {n, n, 3, MergeRule::theirs, Side::theirs, ""},
        {n, 2, 2, MergeRule::identical, Side::ours, ""},
        {n, 2, 3, MergeRule::conflict, Side::ours, "add-add"},
    };
    for (auto& r : table) {
        auto list = [](std::optional<int> v) { return v ? List{{"k", *v}} : List{}; };
        auto rows = km(list(r.b), list(r.o), list(r.t));
        REQUIRE(rows.size() == 1);
        CAPTURE(r.b, r.o, r.t);
        REQUIRE(rows[0].outcome.rule == r.rule);
        REQUIRE(rows[0].outcome.take == r.take);
        REQUIRE(rows[0].outcome.reason == r.reason);
    }
}

TEST_CASE("KeyedMerge: composite values recurse") {
    List b{{"k", 1}}, o{{"k", 2}}, t{{"k", 3}};
    auto rows = keyed_merge<std::string, int>(
        b, o, t, {}, [](const std::string&, const int*, const int&, const int&) { return true; });
    REQUIRE(rows[0].outcome.rule == MergeRule::recursed);
}

TEST_CASE("KeyedMerge: output order is ours' order plus theirs' additions after their predecessor") {
    List b{{"a", 0}, {"b", 0}, {"c", 0}};
    List o{{"c", 0}, {"a", 0}, {"b", 0}, {"o1", 0}};
    List t{{"t0", 0}, {"a", 0}, {"t1", 0}, {"t2", 0}, {"b", 0}, {"c", 0}};
    std::vector<std::string> keys;
    for (auto& r : km(b, o, t))
        keys.push_back(r.key);
    REQUIRE(keys == std::vector<std::string>{"t0", "c", "a", "t1", "t2", "b", "o1"});
    // Keys deleted by both sides come last.
    auto rows = km({{"x", 0}, {"y", 0}}, {{"y", 0}}, {{"y", 0}});
    REQUIRE(rows.back().key == "x");
}

TEST_CASE("OpaqueLeaf compares by hash") {
    auto a = hash128("a"), b = hash128("b"), c = hash128("c");
    REQUIRE(opaque_leaf(&a, &b, &a).rule == MergeRule::ours);
    REQUIRE(opaque_leaf(&a, &b, &c).reason == "both-modified");
    REQUIRE(opaque_leaf(nullptr, &b, &c).reason == "add-add");
}

// ---- GridMerge ----

namespace {
struct Grids {
    std::vector<int> b, o, t;
    std::function<bool(Side, Side, std::uint64_t)> same() const {
        return [this](Side x, Side y, std::uint64_t i) { return at(x)[i] == at(y)[i]; };
    }
    const std::vector<int>& at(Side s) const { return s == Side::base ? b : s == Side::ours ? o : t; }
    std::vector<int> result(const GridResult& r) const {
        if (r.whole)
            return at(*r.whole);
        std::vector<int> v(b.size());
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = at(r.take[i])[i];
        return v;
    }
};

std::vector<int> random_grid(std::mt19937& rng, std::size_t n) {
    std::vector<int> v(n);
    for (auto& x : v)
        x = static_cast<int>(rng() % 3);
    return v;
}

std::vector<int> edit(std::mt19937& rng, std::vector<int> v) {
    for (int k = static_cast<int>(rng() % 6); k > 0; --k)
        v[rng() % v.size()] = 10 + static_cast<int>(rng() % 3);
    return v;
}
} // namespace

TEST_CASE("GridMerge identities hold on random grids") {
    auto seed = GENERATE(range(1u, 101u));
    std::mt19937 rng(seed);
    GridShape shape{static_cast<std::uint64_t>(1 + rng() % 7), static_cast<std::uint64_t>(1 + rng() % 7)};
    if (seed % 3 == 0)
        shape = {shape[0] * shape[1]}; // 1-D
    auto n = static_cast<std::size_t>(shape.size() == 1 ? shape[0] : shape[0] * shape[1]);
    auto b = random_grid(rng, n), x = edit(rng, b), y = edit(rng, b);
    auto run = [&](Grids g) { return std::pair(g, grid_merge(shape, shape, shape, g.same())); };

    {
        auto [g, r] = run({b, b, b});
        REQUIRE(g.result(r) == b);
        REQUIRE(r.conflicts.empty());
    }
    {
        auto [g, r] = run({b, x, b});
        REQUIRE(g.result(r) == x);
        REQUIRE(r.conflicts.empty());
    }
    {
        auto [g, r] = run({b, b, x});
        REQUIRE(g.result(r) == x);
        REQUIRE(r.conflicts.empty());
    }
    {
        auto [g, r] = run({b, x, x});
        REQUIRE(g.result(r) == x);
        REQUIRE(r.conflicts.empty());
    }
    {
        auto [g1, r1] = run({b, x, y});
        auto [g2, r2] = run({b, y, x});
        for (std::size_t i = 0; i < r2.conflicts.size(); ++i)
            grid_resolve(r2, i, Side::theirs);
        REQUIRE(g1.result(r1) == g2.result(r2)); // symmetry modulo the policy
        auto [g3, r3] = run({b, x, y});
        REQUIRE(r3.take == r1.take); // deterministic
    }
}

TEST_CASE("GridMerge: regions, guard band, adjacency") {
    // 6x1 row: ours edits cell 1, theirs edits cell 3.
    GridShape s{6};
    Grids g{{0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0}, {0, 0, 0, 1, 0, 0}};
    auto r = grid_merge(s, s, s, g.same());
    REQUIRE(r.conflicts.empty());
    REQUIRE(g.result(r) == std::vector<int>{0, 1, 0, 1, 0, 0});
    REQUIRE(r.ours.size() == 1);
    REQUIRE(std::get<Range>(r.ours[0].bounds) == Range{1, 1});

    GridOptions guard;
    guard.guard = 2;
    auto rg = grid_merge(s, s, s, g.same(), guard);
    REQUIRE(rg.conflicts.size() == 1);
    REQUIRE(rg.conflicts[0].reason == "guard-band");
    REQUIRE(std::get<Range>(rg.conflicts[0].bounds) == Range{1, 3});
    REQUIRE(g.result(rg) == std::vector<int>{0, 1, 0, 0, 0, 0}); // ours wins the conflicted regions

    Grids adj{{0, 0, 0}, {1, 0, 0}, {0, 2, 0}};
    auto ra = grid_merge({3}, {3}, {3}, adj.same());
    REQUIRE(ra.conflicts.empty());
    REQUIRE(ra.adjacent.size() == 1);
}

TEST_CASE("GridMerge: 2-D regions are 8-connected and --gap joins them") {
    // 4x4, ours changes (0,0) and (1,1) (diagonal) and (3,3).
    GridShape s{4, 4};
    std::vector<int> b(16, 0), o = b;
    o[0] = o[5] = o[15] = 1;
    Grids g{b, o, b};
    auto r = grid_merge(s, s, s, g.same());
    REQUIRE(r.ours.size() == 2);
    REQUIRE(std::get<Rect>(r.ours[0].bounds) == Rect{0, 0, 2, 2});
    GridOptions gap;
    gap.gap = 1;
    REQUIRE(grid_merge(s, s, s, g.same(), gap).ours.size() == 1);
}

TEST_CASE("GridMerge: conflicting cells resolve per conflict") {
    GridShape s{3};
    Grids g{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    auto r = grid_merge(s, s, s, g.same());
    REQUIRE(r.conflicts.size() == 1);
    REQUIRE(r.conflicts[0].reason == "both-modified");
    REQUIRE(g.result(r)[0] == 1);
    grid_resolve(r, 0, Side::theirs);
    REQUIRE(g.result(r)[0] == 2);
    grid_resolve(r, 0, Side::base);
    REQUIRE(g.result(r)[0] == 0);
}

// #28: the per-cell state and the region cell lists are charged to --max-memory.
TEST_CASE("GridMerge: working memory is charged to the budget") {
    GridShape s{3};
    Grids g{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}; // cell 0 is in an ours and a theirs region
    Limits l;
    GridOptions opt;
    opt.budget = &l;
    grid_merge(s, s, s, g.same(), opt);
    CHECK(l.memory_used == 3 * 10 + 2 * 8);
    Limits tight;
    tight.max_memory = 3 * 10 + 2 * 8 - 1;
    opt.budget = &tight;
    try {
        grid_merge(s, s, s, g.same(), opt);
        FAIL("no limit error");
    } catch (const Error& e) {
        CHECK(e.category() == ErrorCategory::limit);
    }
}

TEST_CASE("GridMerge: shape changes") {
    auto same_shape_eq = [](const Grids& g) { return g.same(); };
    Grids g{{0, 0}, {0, 0, 0}, {0, 0}}; // ours grew, theirs untouched
    auto r = grid_merge({2}, {3}, {2}, same_shape_eq(g));
    REQUIRE(r.whole == Side::ours);

    Grids h{{0, 0}, {0, 0, 0}, {0, 5}}; // ours grew, theirs edited
    auto c = grid_merge({2}, {3}, {2}, same_shape_eq(h));
    REQUIRE(c.shape_conflict);
    REQUIRE(c.conflicts.at(0).reason == "shape-mismatch");
    grid_resolve(c, 0, Side::theirs);
    REQUIRE(c.whole == Side::theirs);
}

// ---- Policy and dependencies ----

TEST_CASE("conflict policy, resolutions, and mark fallback") {
    std::vector<Conflict> cs{make_conflict({"/a"}, {"c-1", "c-2"}, "both-modified"),
                             make_conflict({"/b"}, {"c-3", "c-4"}, "both-modified")};
    std::vector<Warning> w;
    ConflictOptions opt;
    opt.policy = Policy::mark;
    opt.resolutions = parse_resolutions(R"({")" + cs[1].id + R"(": "theirs", "x-000000000000": "base"})");
    apply_conflict_policy(cs, opt, w);
    REQUIRE(cs[0].resolved_as == "ours");
    REQUIRE_FALSE(cs[0].resolved);
    REQUIRE(cs[1].resolved_as == "theirs");
    REQUIRE(cs[1].resolved);
    REQUIRE(w.size() == 2);
    REQUIRE(w[0].code == "mark-unsupported");
    REQUIRE(w[1].code == "resolve-unmatched");

    opt = {};
    opt.resolve_all = ResolveAll::union_;
    REQUIRE_THROWS_AS(apply_conflict_policy(cs, opt, w), Error);
    opt.resolve_all = ResolveAll::theirs;
    apply_conflict_policy(cs, opt, w);
    REQUIRE(cs[0].resolved);

    opt = {};
    opt.resolutions = {{cs[0].id, "union"}};
    REQUIRE_THROWS_AS(apply_conflict_policy(cs, opt, w), Error);
    REQUIRE_THROWS_AS(parse_resolutions("[1]"), Error);
    REQUIRE_THROWS_AS(parse_resolutions("{"), Error);
}

TEST_CASE("dependency checking") {
    auto use = make_change(Side::ours, Op::insert, {"/svg/use[@id=u1]"});
    use.depends_on = {"/defs/grad[@id=g1]"};
    auto del = make_change(Side::theirs, Op::erase, {"/defs/grad[@id=g1]"});
    auto mod = make_change(Side::theirs, Op::update, {"/defs/grad[@id=g1]/stop"});
    auto mod_parent = make_change(Side::theirs, Op::update, {"/defs/grad[@id=g1]"});

    std::vector<AutoMerge> am;
    std::vector<Conflict> cs;
    check_dependencies({use}, {del}, am, cs);
    REQUIRE(cs.size() == 1);
    REQUIRE(cs[0].reason == "dependency");

    am.clear(), cs.clear();
    check_dependencies({use}, {mod_parent, mod}, am, cs);
    REQUIRE(cs.empty());
    REQUIRE(am == std::vector<AutoMerge>{{use.id, "dependency-touched"}});

    am.clear(), cs.clear();
    check_dependencies({use}, {mod_parent}, am, cs, [](const Change&, const Change&) { return true; });
    REQUIRE(cs.size() == 1);

    // A dependency on a parent is touched by a change below it (pixels depend on /color, gAMA changed).
    auto paint = make_change(Side::ours, Op::update, {"/pixels"});
    paint.depends_on = {"/color"};
    auto gama = make_change(Side::theirs, Op::insert, {"/color/gAMA"});
    am.clear(), cs.clear();
    check_dependencies({paint}, {gama}, am, cs);
    REQUIRE(am == std::vector<AutoMerge>{{paint.id, "dependency-touched"}});
}
