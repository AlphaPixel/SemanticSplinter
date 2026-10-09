// Two-way sequence diff: Myers (linear space) and histogram. Clean-room implementations of the
// published algorithms (Myers 1986, "An O(ND) Difference Algorithm and Its Variations"; the histogram
// strategy as described for JGit/Git).
#include <semsplinter/sequence.hpp>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace semsplinter {

namespace {

using Span = std::span<const Token>;

// ponytail: exact Myers for both `myers` and `minimal`; GNU's cost cap for huge inputs comes with Phase 2
// goldens.
class Myers {
public:
    Myers(Span a, Span b, std::vector<Match>& out)
        : a_(a), b_(b), out_(out), off_(static_cast<long>(a.size() + b.size()) + 1),
          vf_(2 * (a.size() + b.size()) + 3), vb_(vf_.size()) {}

    void run(std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1) {
        while (a0 < a1 && b0 < b1 && a_[a0] == b_[b0])
            out_.emplace_back(a0++, b0++);
        std::size_t suffix = 0;
        while (a1 > a0 && b1 > b0 && a_[a1 - 1] == b_[b1 - 1])
            --a1, --b1, ++suffix;
        if (a0 < a1 && b0 < b1) {
            auto [x, y, u, v] = middle_snake(a0, a1, b0, b1);
            run(a0, x, b0, y);
            for (std::size_t i = x; i < u; ++i)
                out_.emplace_back(i, y + (i - x));
            run(u, a1, v, b1);
        }
        for (std::size_t i = 0; i < suffix; ++i)
            out_.emplace_back(a1 + i, b1 + i);
    }

private:
    struct Snake {
        std::size_t x, y, u, v;
    };

    long& f(long k) { return vf_[static_cast<std::size_t>(k + off_)]; }
    long& r(long k) { return vb_[static_cast<std::size_t>(k + off_)]; }

    // Returns the middle snake of an optimal path from (a0,b0) to (a1,b1), as absolute coordinates.
    Snake middle_snake(std::size_t a0, std::size_t a1, std::size_t b0, std::size_t b1) {
        const long n = static_cast<long>(a1 - a0), m = static_cast<long>(b1 - b0);
        const long delta = n - m;
        const bool odd = (delta & 1) != 0;
        auto A = [&](long i) { return a_[a0 + static_cast<std::size_t>(i)]; };
        auto B = [&](long j) { return b_[b0 + static_cast<std::size_t>(j)]; };
        auto snake = [&](long x0, long y0, long x1, long y1) {
            return Snake{a0 + static_cast<std::size_t>(x0), b0 + static_cast<std::size_t>(y0),
                         a0 + static_cast<std::size_t>(x1), b0 + static_cast<std::size_t>(y1)};
        };
        f(1) = 0;
        r(1) = 0;
        for (long d = 0; d <= (n + m + 1) / 2; ++d) {
            for (long k = -d; k <= d; k += 2) {
                long x = (k == -d || (k != d && f(k - 1) < f(k + 1))) ? f(k + 1) : f(k - 1) + 1;
                long y = x - k;
                const long xs = x, ys = y;
                while (x < n && y < m && A(x) == B(y))
                    ++x, ++y;
                f(k) = x;
                const long c = delta - k;
                if (odd && c >= -(d - 1) && c <= d - 1 && f(k) + r(c) >= n)
                    return snake(xs, ys, x, y);
            }
            for (long c = -d; c <= d; c += 2) {
                long x = (c == -d || (c != d && r(c - 1) < r(c + 1))) ? r(c + 1) : r(c - 1) + 1;
                long y = x - c;
                const long xs = x, ys = y;
                while (x < n && y < m && A(n - 1 - x) == B(m - 1 - y))
                    ++x, ++y;
                r(c) = x;
                const long k = delta - c;
                if (!odd && k >= -d && k <= d && f(k) + r(c) >= n)
                    return snake(n - x, m - y, n - xs, m - ys);
            }
        }
        throw std::logic_error("myers: no middle snake"); // unreachable
    }

    Span a_, b_;
    std::vector<Match>& out_;
    long off_;
    std::vector<long> vf_, vb_;
};

constexpr std::size_t kMaxChain = 64; // histogram: elements more frequent than this are not anchors

void histogram(Span a, Span b, std::vector<Match>& out) {
    struct Region {
        std::size_t a0, a1, b0, b1;
    };
    std::vector<Region> todo{{0, a.size(), 0, b.size()}};
    std::unordered_map<Token, std::vector<std::size_t>> occ;
    Myers fallback(a, b, out);
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
    std::sort(out.begin(), out.end());
}

} // namespace

std::vector<Match> diff_match(Span a, Span b, DiffAlgorithm alg) {
    std::vector<Match> out;
    if (alg == DiffAlgorithm::histogram) {
        histogram(a, b, out);
    } else {
        Myers m(a, b, out);
        m.run(0, a.size(), 0, b.size());
        std::sort(out.begin(), out.end());
    }
    return out;
}

} // namespace semsplinter
