#include <semsplinter/sequence.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <chrono>
#include <random>

using namespace semsplinter;

namespace {

std::size_t lcs(const std::vector<Token>& a, const std::vector<Token>& b) {
    std::vector<std::vector<std::size_t>> t(a.size() + 1, std::vector<std::size_t>(b.size() + 1));
    for (std::size_t i = a.size(); i-- > 0;)
        for (std::size_t j = b.size(); j-- > 0;)
            t[i][j] = a[i] == b[j] ? t[i + 1][j + 1] + 1 : std::max(t[i + 1][j], t[i][j + 1]);
    return t[0][0];
}

std::vector<Token> random_tokens(std::mt19937& rng, std::size_t max_len, Token alphabet) {
    std::vector<Token> v(rng() % (max_len + 1));
    for (auto& t : v)
        t = rng() % alphabet;
    return v;
}

// A random edit of `v`: deletions, insertions, and replacements.
template <class T, class Gen> std::vector<T> mutate(std::mt19937& rng, std::vector<T> v, Gen fresh) {
    int edits = static_cast<int>(rng() % 4);
    for (int e = 0; e < edits; ++e) {
        auto pos = v.empty() ? 0 : rng() % (v.size() + 1);
        switch (rng() % 3) {
        case 0:
            if (pos < v.size())
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos));
            break;
        case 1:
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), fresh());
            break;
        default:
            if (pos < v.size())
                v[pos] = fresh();
        }
    }
    return v;
}

void check_matches(const std::vector<Token>& a, const std::vector<Token>& b, const std::vector<Match>& m) {
    for (std::size_t i = 0; i < m.size(); ++i) {
        REQUIRE(a[m[i].first] == b[m[i].second]);
        if (i)
            REQUIRE((m[i - 1].first < m[i].first && m[i - 1].second < m[i].second));
    }
}

std::vector<SeqItem> items(const std::vector<Token>& v) {
    std::vector<SeqItem> s;
    for (auto t : v)
        s.push_back({hash128(std::to_string(t)), std::nullopt});
    return s;
}

// Output content of a merge, conflicts resolved by `choice`.
std::vector<Hash128> output(const std::vector<SeqChunk>& chunks, const std::vector<SeqItem>& b,
                            const std::vector<SeqItem>& o, const std::vector<SeqItem>& t,
                            std::string_view choice = "ours") {
    std::vector<Hash128> out;
    for (auto& c : chunks)
        for (auto p : picks(c, choice))
            out.push_back((p.side == Side::base ? b : p.side == Side::ours ? o : t)[p.index].content);
    return out;
}

std::vector<Hash128> content(const std::vector<SeqItem>& s) {
    std::vector<Hash128> v;
    for (auto& i : s)
        v.push_back(i.content);
    return v;
}

bool has_conflict(const std::vector<SeqChunk>& chunks) {
    return std::any_of(chunks.begin(), chunks.end(), [](auto& c) { return c.kind == SeqKind::conflict; });
}

} // namespace

TEST_CASE("diff_match returns a valid, optimal matching") {
    auto seed = GENERATE(range(1u, 301u));
    std::mt19937 rng(seed);
    auto a = random_tokens(rng, 30, 5), b = random_tokens(rng, 30, 5);
    for (auto alg : {DiffAlgorithm::myers, DiffAlgorithm::minimal, DiffAlgorithm::histogram}) {
        auto m = diff_match(a, b, alg);
        check_matches(a, b, m);
        if (alg != DiffAlgorithm::histogram)
            REQUIRE(m.size() == lcs(a, b));
    }
}

TEST_CASE("histogram anchors on rare lines") {
    // The unique token 9 anchors even though matching the three 1s would be longer.
    std::vector<Token> a{9, 1, 1, 1}, b{1, 1, 1, 9};
    REQUIRE(diff_match(a, b, DiffAlgorithm::histogram) == std::vector<Match>{{0, 3}});
    REQUIRE(diff_match(a, b, DiffAlgorithm::minimal).size() == 3);
}

TEST_CASE("SequenceMerge identities hold on random sequences") {
    auto seed = GENERATE(range(1u, 201u));
    std::mt19937 rng(seed);
    Token next = 1000;
    auto fresh = [&] { return next++; };
    auto b = random_tokens(rng, 20, 6);
    auto x = mutate(rng, b, fresh), y = mutate(rng, b, fresh);
    auto B = items(b), X = items(x), Y = items(y);

    SECTION("merge(b,b,b) = b") {
        REQUIRE(output(sequence_merge(B, B, B), B, B, B) == content(B));
    }
    SECTION("merge(b,x,b) = x") {
        auto r = sequence_merge(B, X, B);
        REQUIRE_FALSE(has_conflict(r));
        REQUIRE(output(r, B, X, B) == content(X));
    }
    SECTION("merge(b,b,x) = x") {
        auto r = sequence_merge(B, B, X);
        REQUIRE_FALSE(has_conflict(r));
        REQUIRE(output(r, B, B, X) == content(X));
    }
    SECTION("merge(b,x,x) = x") {
        auto r = sequence_merge(B, X, X);
        REQUIRE_FALSE(has_conflict(r));
        REQUIRE(output(r, B, X, X) == content(X));
    }
    SECTION("ours/theirs symmetry modulo the policy") {
        REQUIRE(output(sequence_merge(B, X, Y), B, X, Y, "ours") ==
                output(sequence_merge(B, Y, X), B, Y, X, "theirs"));
    }
    SECTION("deterministic") {
        auto r1 = sequence_merge(B, X, Y), r2 = sequence_merge(B, X, Y);
        REQUIRE(r1.size() == r2.size());
        for (std::size_t i = 0; i < r1.size(); ++i) {
            REQUIRE(r1[i].kind == r2[i].kind);
            REQUIRE(r1[i].ours == r2[i].ours);
            REQUIRE(r1[i].theirs == r2[i].theirs);
        }
    }
}

TEST_CASE("SequenceMerge classifies chunks") {
    auto B = items({1, 2, 3}), O = items({1, 20, 3}), T = items({1, 21, 3});
    auto r = sequence_merge(B, O, T);
    REQUIRE(r.size() == 3);
    REQUIRE(r[1].kind == SeqKind::conflict);
    REQUIRE(r[1].reason == "both-modified");
    REQUIRE(output(r, B, O, T, "union").size() == 4);

    auto add = sequence_merge(items({1}), items({1, 5}), items({1, 6}));
    REQUIRE(add.back().reason == "add-add");
    SeqMergeOptions opt;
    opt.insertion_order_insignificant = true;
    auto both = sequence_merge(items({1}), items({1, 5}), items({1, 6}), opt);
    REQUIRE(both.back().kind == SeqKind::both_inserted);

    auto del = sequence_merge(items({1, 2}), items({1}), items({1, 7}));
    REQUIRE(del.back().reason == "modify-delete");
}

namespace {
// Items with ids: token t has id t; content may differ per side.
std::vector<SeqItem> idd(std::initializer_list<std::pair<Token, int>> v) {
    std::vector<SeqItem> s;
    for (auto [id, c] : v)
        s.push_back({hash128(std::to_string(id) + ":" + std::to_string(c)), hash128(std::to_string(id))});
    return s;
}
} // namespace

TEST_CASE("SequenceMerge: ID-matched moves") {
    auto B = idd({{1, 0}, {2, 0}, {3, 0}, {4, 0}});
    SECTION("move on ours plus edit on theirs merges") {
        auto O = idd({{2, 0}, {3, 0}, {1, 0}, {4, 0}}); // 1 moved after 3
        auto T = idd({{1, 9}, {2, 0}, {3, 0}, {4, 0}}); // 1 edited
        auto r = sequence_merge(B, O, T);
        REQUIRE_FALSE(has_conflict(r));
        REQUIRE(output(r, B, O, T) == content(idd({{2, 0}, {3, 0}, {1, 9}, {4, 0}})));
    }
    SECTION("same move on both sides is identical") {
        auto O = idd({{2, 0}, {3, 0}, {1, 0}, {4, 0}});
        auto r = sequence_merge(B, O, O);
        REQUIRE_FALSE(has_conflict(r));
        REQUIRE(output(r, B, O, O) == content(O));
    }
    SECTION("moves to different places conflict") {
        auto O = idd({{2, 0}, {3, 0}, {1, 0}, {4, 0}});
        auto T = idd({{2, 0}, {3, 0}, {4, 0}, {1, 0}});
        auto r = sequence_merge(B, O, T);
        REQUIRE(std::any_of(r.begin(), r.end(), [](auto& c) { return c.reason == "move-move"; }));
    }
    SECTION("move against delete conflicts") {
        auto O = idd({{2, 0}, {3, 0}, {1, 0}, {4, 0}});
        auto T = idd({{2, 0}, {3, 0}, {4, 0}});
        auto r = sequence_merge(B, O, T);
        REQUIRE(std::any_of(r.begin(), r.end(), [](auto& c) { return c.reason == "modify-delete"; }));
    }
    SECTION("both edit one element: recurse or conflict") {
        auto O = idd({{1, 0}, {2, 5}, {3, 0}, {4, 0}});
        auto T = idd({{1, 0}, {2, 6}, {3, 0}, {4, 0}});
        auto r = sequence_merge(B, O, T);
        REQUIRE(r.size() == 3);
        REQUIRE(r[1].kind == SeqKind::conflict);
        SeqMergeOptions opt;
        opt.recurse = [](std::optional<std::size_t> b, std::size_t o, std::size_t t) {
            return b && o == 1 && t == 1;
        };
        REQUIRE(sequence_merge(B, O, T, opt)[1].kind == SeqKind::recursed);
    }
}

TEST_CASE("SequenceMerge: keyed chunks with moves, inserts and deletes are pinned") {
    // A digest of the exact chunk lists over random keyed sequences, both sides moving, inserting,
    // deleting and editing, with some id-less elements mixed in. Taken before the #38 speedup; any
    // change to the chunks changes it. Only mt19937 raw output is used, so it is portable.
    std::string all;
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng(seed);
        auto r = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        std::vector<std::pair<int, int>> base; // (id or -1 for none, content)
        for (int i = 0; i < 60; ++i) {
            // Draws sequenced explicitly (argument evaluation order differs between compilers).
            auto content = static_cast<int>(r(3));
            base.emplace_back(r(5) ? i : -1, content);
        }
        int next = 1000;
        auto edit = [&](std::vector<std::pair<int, int>> v) {
            for (int e = static_cast<int>(r(8)); e > 0; --e) {
                if (v.empty())
                    break;
                auto p = r(v.size());
                switch (r(4)) {
                case 0: { // move
                    auto x = v[p];
                    v.erase(v.begin() + static_cast<std::ptrdiff_t>(p));
                    v.insert(v.begin() + static_cast<std::ptrdiff_t>(r(v.size() + 1)), x);
                    break;
                }
                case 1:
                    v.erase(v.begin() + static_cast<std::ptrdiff_t>(p));
                    break;
                case 2:
                    v.insert(v.begin() + static_cast<std::ptrdiff_t>(p),
                             {r(2) ? next++ : -1, static_cast<int>(r(3))});
                    break;
                default:
                    v[p].second += 1 + static_cast<int>(r(2));
                }
            }
            return v;
        };
        auto make = [](const std::vector<std::pair<int, int>>& v) {
            std::vector<SeqItem> s;
            for (auto [id, c] : v)
                s.push_back({hash128(std::to_string(id < 0 ? 0 : id) + ":" + std::to_string(c)),
                             id < 0 ? std::nullopt : std::optional(hash128(std::to_string(id)))});
            return s;
        };
        auto B = make(base), O = make(edit(base)), T = make(edit(base));
        SeqMergeOptions opt;
        opt.insertion_order_insignificant = seed % 2;
        for (auto& c : sequence_merge(B, O, T, opt)) {
            all += std::string(to_string(c.kind)) + (c.moved ? "m" : "") + c.reason;
            for (auto* l : {&c.base, &c.ours, &c.theirs}) {
                all += '|';
                for (auto i : *l)
                    all += std::to_string(i) + ',';
            }
            all += ';';
        }
        all += '\n';
    }
    REQUIRE(hash128(all).hex() == "daa261ede12e44e408dde90bbc64eecc");
}

// Benchmark for #38, hidden from CI: run `semsplinter-tests "[.perf]"` in Release.
TEST_CASE("SequenceMerge: 100k keyed elements", "[.perf]") {
    const int n = 100000;
    std::vector<std::pair<int, int>> b;
    for (int i = 0; i < n; ++i)
        b.emplace_back(i, 0);
    auto o = b, t = b;
    std::rotate(o.begin() + 100, o.begin() + 101, o.begin() + 5000); // ours moves one element
    o.erase(o.begin() + 20100, o.begin() + 20110);                   // and deletes ten
    t.insert(t.begin() + 70000, {n + 1, 0});                         // theirs inserts one
    for (auto& [id, c] : o)
        c = id % 1000 == 3; // each side edits one element in a thousand
    for (auto& [id, c] : t)
        c = id % 1000 == 7;
    auto make = [](const std::vector<std::pair<int, int>>& v) {
        std::vector<SeqItem> s;
        for (auto [id, c] : v)
            s.push_back({hash128(std::to_string(id) + ":" + std::to_string(c)), hash128(std::to_string(id))});
        return s;
    };
    auto B = make(b), O = make(o), T = make(t);
    auto t0 = std::chrono::steady_clock::now();
    auto r = sequence_merge(B, O, T);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    CHECK_FALSE(has_conflict(r));
    WARN("sequence_merge " << ms.count() << " ms");
}

TEST_CASE("SequenceMerge with ids reproduces one-sided rearrangements") {
    auto seed = GENERATE(range(1u, 101u));
    std::mt19937 rng(seed);
    std::vector<std::pair<Token, int>> base;
    for (Token i = 0; i < 8; ++i)
        base.emplace_back(i, 0);
    auto x = base;
    std::shuffle(x.begin(), x.end(), rng);
    x.resize(rng() % 9);
    for (auto& e : x)
        e.second = static_cast<int>(rng() % 2);
    x.emplace_back(100 + seed, 0); // an insertion
    auto make = [](const std::vector<std::pair<Token, int>>& v) {
        std::vector<SeqItem> s;
        for (auto [id, c] : v)
            s.push_back({hash128(std::to_string(id) + ":" + std::to_string(c)), hash128(std::to_string(id))});
        return s;
    };
    auto B = make(base), X = make(x);
    auto r1 = sequence_merge(B, X, B);
    REQUIRE_FALSE(has_conflict(r1));
    REQUIRE(output(r1, B, X, B) == content(X));
    auto r2 = sequence_merge(B, B, X);
    REQUIRE_FALSE(has_conflict(r2));
    REQUIRE(output(r2, B, B, X) == content(X));
}
