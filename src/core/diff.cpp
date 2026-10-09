// Two-way sequence diff: Myers (linear space) and histogram. Clean-room implementations of the
// published algorithms (Myers 1986, "An O(ND) Difference Algorithm and Its Variations"; the histogram
// strategy as described for JGit/Git; patience diff as described by Bram Cohen).
#include <semsplinter/sequence.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace semsplinter {

namespace {

using Span = std::span<const Token>;

class Myers {
public:
    // `cost_limit` bounds the edit cost explored per recursion step (0 = unlimited, i.e. minimal). Past it
    // the search splits at the furthest-reaching diagonal instead of the optimal midpoint (text.md §5.1).
    Myers(Span a, Span b, std::vector<Match>& out, std::size_t cost_limit = 0)
        : a_(a), b_(b), out_(out), vf_(a.size() + b.size() + 3), vb_(vf_.size()),
          limit_(static_cast<long>(cost_limit)) {}

    // Divide and conquer on the midpoint of an optimal path. Matches inside the midpoint's snake are found by
    // the prefix/suffix trimming of the halves; this choice of split decides which of several equal-cost
    // scripts comes out, and it reproduces GNU diff's on the black-box corpus.
    void run(std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1) {
        while (a0 < a1 && b0 < b1 && a_[a0] == b_[b0])
            out_.emplace_back(a0++, b0++);
        std::size_t suffix = 0;
        while (a1 > a0 && b1 > b0 && a_[a1 - 1] == b_[b1 - 1])
            --a1, --b1, ++suffix;
        if (a0 < a1 && b0 < b1) {
            auto [x, y] = midpoint(a0, a1, b0, b1);
            run(a0, x, b0, y);
            run(x, a1, y, b1);
        }
        for (std::size_t i = 0; i < suffix; ++i)
            out_.emplace_back(a1 + i, b1 + i);
    }

private:
    // Furthest-reaching x per diagonal k = x - y (relative), forward from (0,0) and backward from (n,m).
    // Diagonals stay inside the grid; the slots just outside the live range hold sentinels.
    long& f(long k) { return vf_[static_cast<std::size_t>(k + m_ + 1)]; }
    long& r(long k) { return vb_[static_cast<std::size_t>(k + m_ + 1)]; }

    std::pair<std::size_t, std::size_t> midpoint(std::size_t a0, std::size_t a1, std::size_t b0,
                                                 std::size_t b1) {
        const long n = static_cast<long>(a1 - a0), m = static_cast<long>(b1 - b0);
        m_ = m;
        const long delta = n - m;
        const bool odd = (delta & 1) != 0;
        auto A = [&](long i) { return a_[a0 + static_cast<std::size_t>(i)]; };
        auto B = [&](long j) { return b_[b0 + static_cast<std::size_t>(j)]; };
        auto at = [&](long x, long y) {
            return std::pair{a0 + static_cast<std::size_t>(x), b0 + static_cast<std::size_t>(y)};
        };
        constexpr long kNone = -1, kFar = LONG_MAX;
        long fmin = 0, fmax = 0, bmin = delta, bmax = delta;
        f(0) = 0;
        r(delta) = n;
        for (long d = 1;; ++d) {
            // Extend the forward diagonal range by one on each side while it stays in the grid.
            if (fmin > -m)
                f(--fmin - 1) = kNone;
            else
                ++fmin;
            if (fmax < n)
                f(++fmax + 1) = kNone;
            else
                --fmax;
            for (long k = fmax; k >= fmin; k -= 2) {
                long lo = f(k - 1), hi = f(k + 1);
                long x = lo >= hi ? lo + 1 : hi, y = x - k;
                while (x < n && y < m && A(x) == B(y))
                    ++x, ++y;
                f(k) = x;
                if (odd && bmin <= k && k <= bmax && r(k) <= x)
                    return at(x, y);
            }
            if (bmin > -m)
                r(--bmin - 1) = kFar;
            else
                ++bmin;
            if (bmax < n)
                r(++bmax + 1) = kFar;
            else
                --bmax;
            for (long k = bmax; k >= bmin; k -= 2) {
                long lo = r(k - 1), hi = r(k + 1);
                long x = lo < hi ? lo : hi - 1, y = x - k;
                while (x > 0 && y > 0 && A(x - 1) == B(y - 1))
                    --x, --y;
                r(k) = x;
                if (!odd && fmin <= k && k <= fmax && x <= f(k))
                    return at(x, y);
            }
            if (limit_ && d >= limit_) {
                // Too expensive: split where the forward or backward search got furthest from its corner.
                long best = -1, bx = 0, by = 0;
                for (long k = fmax; k >= fmin; k -= 2) {
                    long x = std::min(f(k), n), y = x - k;
                    if (y >= 0 && y <= m && x + y > best && x + y < n + m)
                        best = x + y, bx = x, by = y;
                }
                for (long k = bmax; k >= bmin; k -= 2) {
                    long x = std::max(r(k), 0L), y = x - k;
                    if (y >= 0 && y <= m && (n - x) + (m - y) > best && x + y > 0)
                        best = (n - x) + (m - y), bx = x, by = y;
                }
                if (best > 0)
                    return at(bx, by);
            }
        }
    }

    Span a_, b_;
    std::vector<Match>& out_;
    std::vector<long> vf_, vb_;
    long limit_;
    long m_ = 0;
};

// GNU-style cost limit: grows with the square root of the input, never below 4096.
// ponytail: the growth factor is a guess tuned to keep 1 M-line inputs near-linear; refine against GNU
// timings.
std::size_t auto_cost_limit(std::size_t n, std::size_t m) {
    return std::max<std::size_t>(4096, static_cast<std::size_t>(std::sqrt(static_cast<double>(n + m)) * 64));
}

constexpr std::size_t kMaxChain = 64; // histogram: elements more frequent than this are not anchors

void histogram(Span a, Span b, std::vector<Match>& out, std::size_t cost_limit) {
    struct Region {
        std::size_t a0, a1, b0, b1;
    };
    std::vector<Region> todo{{0, a.size(), 0, b.size()}};
    std::unordered_map<Token, std::vector<std::size_t>> occ;
    Myers fallback(a, b, out, cost_limit);
    while (!todo.empty()) {
        auto [a0, a1, b0, b1] = todo.back();
        todo.pop_back();
        while (a0 < a1 && b0 < b1 && a[a0] == b[b0])
            out.emplace_back(a0++, b0++);
        while (a1 > a0 && b1 > b0 && a[a1 - 1] == b[b1 - 1])
            out.emplace_back(--a1, --b1);
        if (a0 == a1 || b0 == b1)
            continue;

        occ.clear();
        for (std::size_t i = a0; i < a1; ++i)
            occ[a[i]].push_back(i);

        // The common run whose rarest element is least frequent in a; ties go to the longer run.
        // ponytail: rescans each candidate run, O(n * kMaxChain * run) worst case; JGit's skip-ahead if slow.
        std::size_t best_count = std::numeric_limits<std::size_t>::max(), best_len = 0;
        std::size_t sa = 0, sb = 0;
        for (std::size_t j = b0; j < b1; ++j) {
            auto it = occ.find(b[j]);
            if (it == occ.end() || it->second.size() > kMaxChain)
                continue;
            for (std::size_t i : it->second) {
                std::size_t s_a = i, s_b = j, e_a = i + 1, e_b = j + 1;
                while (s_a > a0 && s_b > b0 && a[s_a - 1] == b[s_b - 1])
                    --s_a, --s_b;
                while (e_a < a1 && e_b < b1 && a[e_a] == b[e_b])
                    ++e_a, ++e_b;
                std::size_t mc = std::numeric_limits<std::size_t>::max();
                for (std::size_t t = s_a; t < e_a; ++t)
                    mc = std::min(mc, occ[a[t]].size());
                if (mc < best_count || (mc == best_count && e_a - s_a > best_len))
                    best_count = mc, best_len = e_a - s_a, sa = s_a, sb = s_b;
            }
        }
        if (best_len == 0) {
            fallback.run(a0, a1, b0, b1);
            continue;
        }
        for (std::size_t t = 0; t < best_len; ++t)
            out.emplace_back(sa + t, sb + t);
        todo.push_back({sa + best_len, a1, sb + best_len, b1});
        todo.push_back({a0, sa, b0, sb});
    }
}

// Patience: elements unique in both regions are anchors; their longest increasing subsequence splits the
// region; recurse between anchors; Myers where no unique common element exists.
void patience(Span a, Span b, std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1,
              std::vector<Match>& out, Myers& fallback) {
    while (a0 < a1 && b0 < b1 && a[a0] == b[b0])
        out.emplace_back(a0++, b0++);
    std::size_t suffix = 0;
    while (a1 > a0 && b1 > b0 && a[a1 - 1] == b[b1 - 1])
        --a1, --b1, ++suffix;
    auto tail = [&] {
        for (std::size_t i = 0; i < suffix; ++i)
            out.emplace_back(a1 + i, b1 + i);
    };
    if (a0 == a1 || b0 == b1)
        return tail();

    struct Count {
        std::size_t na = 0, nb = 0, ia = 0, ib = 0;
    };
    std::unordered_map<Token, Count> counts;
    for (std::size_t i = a0; i < a1; ++i) {
        auto& c = counts[a[i]];
        ++c.na, c.ia = i;
    }
    for (std::size_t j = b0; j < b1; ++j)
        if (auto it = counts.find(b[j]); it != counts.end())
            ++it->second.nb, it->second.ib = j;
    std::vector<Match> uniq;
    for (std::size_t i = a0; i < a1; ++i)
        if (auto& c = counts[a[i]]; c.na == 1 && c.nb == 1)
            uniq.emplace_back(i, c.ib);
    if (uniq.empty()) {
        fallback.run(a0, a1, b0, b1);
        return tail();
    }
    // Patience sorting LIS on the b indices.
    std::vector<std::size_t> piles, prev(uniq.size(), SIZE_MAX);
    for (std::size_t k = 0; k < uniq.size(); ++k) {
        auto it = std::lower_bound(piles.begin(), piles.end(), uniq[k].second,
                                   [&](std::size_t p, std::size_t v) { return uniq[p].second < v; });
        if (it != piles.begin())
            prev[k] = *(it - 1);
        if (it == piles.end())
            piles.push_back(k);
        else
            *it = k;
    }
    std::vector<Match> anchors;
    for (std::size_t k = piles.back(); k != SIZE_MAX; k = prev[k])
        anchors.push_back(uniq[k]);
    std::reverse(anchors.begin(), anchors.end());
    for (auto [i, j] : anchors) {
        patience(a, b, a0, i, b0, j, out, fallback);
        out.emplace_back(i, j);
        a0 = i + 1, b0 = j + 1;
    }
    patience(a, b, a0, a1, b0, b1, out, fallback);
    tail();
}

} // namespace

std::vector<Match> diff_match(Span a, Span b, DiffAlgorithm alg) {
    std::vector<Match> out;
    const std::size_t limit = alg == DiffAlgorithm::minimal ? 0 : auto_cost_limit(a.size(), b.size());
    if (alg == DiffAlgorithm::histogram) {
        histogram(a, b, out, limit);
    } else if (alg == DiffAlgorithm::patience) {
        Myers m(a, b, out, limit);
        patience(a, b, 0, a.size(), 0, b.size(), out, m);
    } else {
        Myers m(a, b, out, limit);
        m.run(0, a.size(), 0, b.size());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace semsplinter
