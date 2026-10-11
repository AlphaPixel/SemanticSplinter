// geomcore (mesh-simple.md §5): bounding boxes, area, volume, edge counts, Hausdorff/RMS distance over a
// uniform grid, and displacement regions.
#include "geomcore.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace semsplinter::geom {

namespace {

Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
double dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 lerp(const Vec3& a, const Vec3& b, double t) {
    return {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2])};
}

double seg2(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = sub(b, a);
    const double l = dot(ab, ab);
    const double t = l > 0 ? std::clamp(dot(sub(p, a), ab) / l, 0.0, 1.0) : 0.0;
    const Vec3 d = sub(p, lerp(a, b, t));
    return dot(d, d);
}

// Squared distance from p to triangle abc; degenerate triangles (and points) fall back to the edges.
double tri2(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 n = cross(sub(b, a), sub(c, a));
    const double nn = dot(n, n);
    if (nn > 0) {
        const double d = dot(sub(p, a), n) / nn;
        const Vec3 q{p[0] - d * n[0], p[1] - d * n[1], p[2] - d * n[2]};
        if (dot(cross(sub(b, q), sub(c, q)), n) >= 0 && dot(cross(sub(c, q), sub(a, q)), n) >= 0 &&
            dot(cross(sub(a, q), sub(b, q)), n) >= 0)
            return d * d * nn;
    }
    return std::min({seg2(p, a, b), seg2(p, b, c), seg2(p, c, a)});
}

// Uniform grid over a mesh's triangles (its points when it has none) for nearest-surface queries.
// ponytail: a triangle is listed in every cell its bbox overlaps; long slivers across a fine grid cost
// memory, a BVH is the upgrade if real meshes need it.
class Grid {
public:
    explicit Grid(const Mesh& m) : m_(m) {
        if (m.tri.empty())
            for (std::uint32_t i = 0; i < m.v.size(); ++i)
                own_.push_back({i, i, i});
        const auto& tris = m.tri.empty() ? own_ : m.tri;
        tris_ = &tris;
        const Box b = bbox(m);
        lo_ = b.lo;
        Vec3 ext = sub(b.hi, b.lo);
        const double big = std::max({ext[0], ext[1], ext[2]});
        double prod = 1;
        int k = 0;
        for (double e : ext)
            if (e > big * 1e-9)
                prod *= e, ++k;
        const double count = static_cast<double>(std::max<std::size_t>(tris.size(), 1));
        h_ = k ? std::pow(prod / count, 1.0 / k) : 1.0;
        if (!(h_ > 0))
            h_ = 1.0;
        for (;;) {
            double cells = 1;
            for (int a = 0; a < 3; ++a)
                n_[a] = std::max(1, static_cast<int>(std::ceil(ext[a] / h_))), cells *= n_[a];
            if (cells <= 4.0 * count + 8 && cells <= double(1 << 22))
                break;
            h_ *= 1.25;
        }
        std::vector<std::uint32_t> count_in(cells() + 1, 0);
        auto each_cell = [&](const Tri& t, auto&& f) {
            std::array<int, 3> c0{}, c1{};
            for (int a = 0; a < 3; ++a) {
                const double mn = std::min({m.v[t[0]][a], m.v[t[1]][a], m.v[t[2]][a]});
                const double mx = std::max({m.v[t[0]][a], m.v[t[1]][a], m.v[t[2]][a]});
                c0[a] = cell_of(mn, a), c1[a] = cell_of(mx, a);
            }
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x)
                        f(index(x, y, z));
        };
        for (auto& t : tris)
            each_cell(t, [&](std::size_t c) { ++count_in[c + 1]; });
        std::partial_sum(count_in.begin(), count_in.end(), count_in.begin());
        start_ = count_in;
        items_.resize(start_.back());
        for (std::uint32_t i = 0; i < tris.size(); ++i)
            each_cell(tris[i], [&](std::size_t c) { items_[count_in[c]++] = i; });
    }

    // Squared distance from p to the nearest triangle: rings of cells around p's cell until no unvisited
    // cell can be closer than the best found.
    double nearest2(const Vec3& p) const {
        std::array<int, 3> c{};
        for (int a = 0; a < 3; ++a)
            c[a] = cell_of(p[a], a);
        double best = std::numeric_limits<double>::infinity();
        for (int r = 0;; ++r) {
            for (int z = std::max(0, c[2] - r); z <= std::min(n_[2] - 1, c[2] + r); ++z)
                for (int y = std::max(0, c[1] - r); y <= std::min(n_[1] - 1, c[1] + r); ++y)
                    for (int x = std::max(0, c[0] - r); x <= std::min(n_[0] - 1, c[0] + r); ++x) {
                        if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != r)
                            continue;
                        const std::size_t cell = index(x, y, z);
                        for (auto k = start_[cell]; k < start_[cell + 1]; ++k) {
                            const Tri& t = (*tris_)[items_[k]];
                            best = std::min(best, tri2(p, m_.v[t[0]], m_.v[t[1]], m_.v[t[2]]));
                        }
                    }
            double lb = std::numeric_limits<double>::infinity();
            for (int a = 0; a < 3; ++a) {
                if (c[a] - r > 0)
                    lb = std::min(lb, p[a] - (lo_[a] + (c[a] - r) * h_));
                if (c[a] + r < n_[a] - 1)
                    lb = std::min(lb, lo_[a] + (c[a] + r + 1) * h_ - p[a]);
            }
            if (std::isinf(lb) || (lb > 0 && best <= lb * lb))
                return best;
        }
    }

private:
    int cell_of(double x, int a) const {
        const double f = std::floor((x - lo_[a]) / h_);
        return f < 0 ? 0 : f >= n_[a] ? n_[a] - 1 : static_cast<int>(f);
    }
    std::size_t index(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(n_[1]) + static_cast<std::size_t>(y)) *
                   static_cast<std::size_t>(n_[0]) +
               static_cast<std::size_t>(x);
    }
    std::size_t cells() const {
        return static_cast<std::size_t>(n_[0]) * static_cast<std::size_t>(n_[1]) *
               static_cast<std::size_t>(n_[2]);
    }

    const Mesh& m_;
    std::vector<Tri> own_;
    const std::vector<Tri>* tris_ = nullptr;
    Vec3 lo_{};
    double h_ = 1;
    std::array<int, 3> n_{1, 1, 1};
    std::vector<std::uint32_t> start_, items_;
};

constexpr std::size_t kExactTris = 200000, kSamples = 16384;

// Vertices plus surface samples (Q-MESH-2). The R2 low-discrepancy sequence is fixed, so reports are
// reproducible.
std::vector<Vec3> sample_points(const Mesh& m, bool& sampled) {
    std::vector<Vec3> pts = m.v;
    if (m.tri.size() <= kExactTris) {
        for (auto& t : m.tri) {
            const Vec3 &a = m.v[t[0]], &b = m.v[t[1]], &c = m.v[t[2]];
            pts.push_back(lerp(a, b, 0.5));
            pts.push_back(lerp(b, c, 0.5));
            pts.push_back(lerp(c, a, 0.5));
            pts.push_back({(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3});
        }
        return pts;
    }
    sampled = true;
    std::vector<double> cum;
    double total = 0;
    for (auto& t : m.tri) {
        const Vec3 n = cross(sub(m.v[t[1]], m.v[t[0]]), sub(m.v[t[2]], m.v[t[0]]));
        cum.push_back(total += std::sqrt(dot(n, n)));
    }
    constexpr double g = 1.32471795724474602596; // plastic number
    for (std::size_t i = 0; i < kSamples; ++i) {
        const double k = static_cast<double>(i) + 0.5;
        const double u = std::fmod(k / static_cast<double>(kSamples), 1.0);
        double s = std::fmod(0.5 + k / g, 1.0), t = std::fmod(0.5 + k / (g * g), 1.0);
        const auto ti =
            static_cast<std::size_t>(std::lower_bound(cum.begin(), cum.end(), u * total) - cum.begin());
        const Tri& tr = m.tri[std::min(ti, m.tri.size() - 1)];
        if (s + t > 1)
            s = 1 - s, t = 1 - t;
        const Vec3 &a = m.v[tr[0]], &b = m.v[tr[1]], &c = m.v[tr[2]];
        pts.push_back({a[0] + s * (b[0] - a[0]) + t * (c[0] - a[0]),
                       a[1] + s * (b[1] - a[1]) + t * (c[1] - a[1]),
                       a[2] + s * (b[2] - a[2]) + t * (c[2] - a[2])});
    }
    return pts;
}

struct UnionFind {
    std::vector<std::uint32_t> p;
    explicit UnionFind(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    std::uint32_t find(std::uint32_t x) {
        while (p[x] != x)
            x = p[x] = p[p[x]];
        return x;
    }
    void unite(std::uint32_t a, std::uint32_t b) {
        a = find(a), b = find(b);
        if (a != b)
            p[std::max(a, b)] = std::min(a, b);
    }
};

} // namespace

void Box::add(const Vec3& p) {
    if (empty) {
        lo = hi = p, empty = false;
        return;
    }
    for (int a = 0; a < 3; ++a)
        lo[a] = std::min(lo[a], p[a]), hi[a] = std::max(hi[a], p[a]);
}

Box bbox(const Mesh& m) {
    Box b;
    for (auto& p : m.v)
        b.add(p);
    return b;
}

double area(const Mesh& m) {
    double s = 0;
    for (auto& t : m.tri) {
        const Vec3 n = cross(sub(m.v[t[1]], m.v[t[0]]), sub(m.v[t[2]], m.v[t[0]]));
        s += std::sqrt(dot(n, n));
    }
    return s / 2;
}

double volume(const Mesh& m) {
    double s = 0;
    for (auto& t : m.tri)
        s += dot(m.v[t[0]], cross(m.v[t[1]], m.v[t[2]]));
    return s / 6;
}

Edges edges(const Mesh& m) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> e;
    e.reserve(m.tri.size() * 3);
    for (auto& t : m.tri)
        for (int k = 0; k < 3; ++k) {
            const auto a = t[k], b = t[(k + 1) % 3];
            if (a != b)
                e.emplace_back(std::min(a, b), std::max(a, b));
        }
    std::sort(e.begin(), e.end());
    Edges r;
    for (std::size_t i = 0; i < e.size();) {
        std::size_t j = i;
        while (j < e.size() && e[j] == e[i])
            ++j;
        ++r.edges;
        r.boundary += j - i == 1;
        r.nonmanifold += j - i > 2;
        i = j;
    }
    return r;
}

std::optional<Distance> distance(const Mesh& a, const Mesh& b) {
    if (a.v.empty() || b.v.empty()) {
        if (a.v.empty() && b.v.empty())
            return Distance{};
        return std::nullopt;
    }
    Distance d;
    double sum = 0;
    std::size_t n = 0;
    auto one_side = [&](const Mesh& from, const Mesh& to) {
        const Grid g(to);
        for (auto& p : sample_points(from, d.sampled)) {
            const double q = g.nearest2(p);
            d.hausdorff = std::max(d.hausdorff, std::sqrt(q));
            sum += q, ++n;
        }
    };
    one_side(a, b);
    one_side(b, a);
    d.rms = std::sqrt(sum / static_cast<double>(n));
    return d;
}

std::optional<Displacement> displacement(const Mesh& a, const Mesh& b, double tol) {
    if (a.v.size() != b.v.size() || a.tri != b.tri)
        return std::nullopt;
    Displacement r;
    std::vector<double> d(a.v.size());
    double sum = 0;
    for (std::size_t i = 0; i < a.v.size(); ++i) {
        const Vec3 x = sub(b.v[i], a.v[i]);
        d[i] = std::sqrt(dot(x, x));
        sum += d[i];
        r.max = std::max(r.max, d[i]);
        r.moved += d[i] > tol;
    }
    r.mean = a.v.empty() ? 0 : sum / static_cast<double>(a.v.size());
    UnionFind uf(a.v.size());
    for (auto& t : a.tri)
        for (int k = 0; k < 3; ++k)
            if (d[t[k]] > tol && d[t[(k + 1) % 3]] > tol)
                uf.unite(t[k], t[(k + 1) % 3]);
    std::vector<std::size_t> slot(a.v.size(), SIZE_MAX);
    for (std::uint32_t i = 0; i < a.v.size(); ++i) {
        if (!(d[i] > tol))
            continue;
        const auto root = uf.find(i);
        if (slot[root] == SIZE_MAX)
            slot[root] = r.regions.size(), r.regions.emplace_back();
        auto& g = r.regions[slot[root]];
        g.box.add(b.v[i]);
        g.max = std::max(g.max, d[i]);
        ++g.vertices;
    }
    return r;
}

} // namespace semsplinter::geom
