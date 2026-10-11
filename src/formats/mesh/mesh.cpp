// Simple meshes (docs/formats/mesh-simple.md, MVP): OBJ/MTL, PLY (ASCII, binary LE/BE), and STL (ASCII,
// binary) readers; geomcore metrics; OBJ objects merged by name (object-local layout) with face-index
// shifting; MTL merged per material and statement; PLY, STL, and shared-pool OBJ merged as one unit.
#include <semsplinter/emit.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/formats/mesh.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/text.hpp>

#include "../geomcore/geomcore.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace semsplinter::mesh {

namespace {

using geom::Mesh;
using geom::Vec3;

constexpr std::size_t npos = std::string_view::npos;
constexpr std::size_t kHead = 64 * 1024;
constexpr std::size_t kMaxGeomTris = 5'000'000; // Hausdorff is skipped above this (mesh-simple.md §5)
constexpr std::size_t kMapFacets = 100'000;     // binary STL facets mapped one by one up to this

[[noreturn]] void bad(const std::string& m) {
    throw Error(ErrorCategory::invalid_input, "mesh: " + m);
}

void charge(Limits* budget, std::uint64_t n) {
    if (budget)
        budget->charge(n);
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return o;
}

std::string extension(std::string_view name) {
    if (auto slash = name.find_last_of("/\\"); slash != npos)
        name.remove_prefix(slash + 1);
    const auto dot = name.rfind('.');
    return dot == npos ? std::string() : lower(name.substr(dot));
}

std::string_view chars(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::optional<double> num(std::string_view t) {
    if (!t.empty() && t[0] == '+')
        t.remove_prefix(1);
    double x = 0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), x);
    if (t.empty() || ec != std::errc{} || p != t.data() + t.size())
        return std::nullopt;
    return x;
}

std::optional<std::int64_t> integer(std::string_view t) {
    std::int64_t x = 0;
    auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), x);
    if (t.empty() || ec != std::errc{} || p != t.data() + t.size())
        return std::nullopt;
    return x;
}

std::string g6(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", x);
    return buf;
}

double r6(double x) {
    return std::isfinite(x) ? std::stod(g6(x)) : x;
}

// ---- Lines and tokens (OBJ, MTL) ----

struct TLine {
    std::size_t b = 0, e = 0, end = 0; // content [b, e) (continuations included), terminator [e, end)
};

// Lines ending in `\` continue on the next one (mesh-simple.md §2).
std::vector<TLine> split(std::string_view s) {
    std::vector<TLine> out;
    std::size_t p = 0;
    while (p < s.size()) {
        TLine l;
        l.b = p;
        for (;;) {
            const auto q = s.find_first_of("\r\n", p);
            if (q == npos) {
                l.e = l.end = s.size();
                break;
            }
            const std::size_t n = s[q] == '\r' && q + 1 < s.size() && s[q + 1] == '\n' ? 2 : 1;
            if (q > l.b && s[q - 1] == '\\') {
                p = q + n;
                continue;
            }
            l.e = q, l.end = q + n;
            break;
        }
        p = l.end;
        out.push_back(l);
    }
    return out;
}

bool space(std::string_view s, std::size_t i) {
    const char c = s[i];
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' ||
           (c == '\\' && i + 1 < s.size() && (s[i + 1] == '\r' || s[i + 1] == '\n'));
}

// Whitespace-separated tokens; a token starting with `#` begins a comment.
std::vector<std::string_view> tokens(std::string_view line, std::string_view* comment = nullptr) {
    std::vector<std::string_view> t;
    for (std::size_t i = 0; i < line.size();) {
        if (space(line, i)) {
            ++i;
            continue;
        }
        if (line[i] == '#') {
            if (comment)
                *comment = line.substr(i);
            break;
        }
        std::size_t j = i;
        while (j < line.size() && !space(line, j))
            ++j;
        t.push_back(line.substr(i, j - i));
        i = j;
    }
    return t;
}

std::string join(const std::vector<std::string_view>& t, std::size_t from = 0) {
    std::string o;
    for (std::size_t i = from; i < t.size(); ++i)
        o += (i > from ? " " : "") + std::string(t[i]);
    return o;
}

// Token lists equal, numbers within `tol` (number spelling is not a change, mesh-simple.md §4).
bool same_tokens(const std::vector<std::string_view>& a, const std::vector<std::string_view>& b, double tol) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i])
            continue;
        auto x = num(a[i]), y = num(b[i]);
        if (!x || !y || !(std::fabs(*x - *y) <= tol))
            return false;
    }
    return true;
}

// ---- Model ----

enum class Kind : std::uint8_t { obj, mtl, ply, stl };
enum class LT : std::uint8_t { trivia, v, vt, vn, vp, elem, other };

struct ObjUnit {
    std::string key, kw, name;               // key: name plus `~k` for a repeat; "" for the header
    std::size_t l0 = 0, l1 = 0;              // lines [l0, l1)
    std::array<std::uint32_t, 3> p0{}, p1{}; // v, vt, vn pool sizes at its start and end
    std::size_t f0 = 0, f1 = 0;              // faces [f0, f1)
    Hash128 canon;                           // its text with positive indices relative to p0
};

struct Stmt { // MTL: leading comments and blank lines, the statement line, its terminator
    std::string key;
    std::size_t b = 0, e = 0, end = 0;
    Hash128 h;
};

struct Material {
    std::string key; // "" for statements before the first `newmtl`
    std::vector<Stmt> st;
};

struct Type {
    std::uint8_t size = 0;
    bool real = false, sign = false;
    std::string_view name;
};

struct Prop {
    std::string name;
    Type type;
    std::optional<Type> count; // list properties
};

struct Element {
    std::string name;
    std::uint64_t count = 0;
    std::vector<Prop> props;
    std::size_t count_b = 0, count_e = 0; // the count digits in the header
    std::size_t body_b = 0, body_e = 0;
    std::vector<double> values; // every value but the geometry (x, y, z, vertex indices)
};

struct Doc final : Document {
    ByteSource src;
    Kind kind = Kind::obj;
    std::string storage; // "obj", "mtl", "ply ascii", "ply binary_little_endian", "stl binary", ...
    std::size_t bom = 0;
    std::string_view s; // text after the BOM (OBJ, MTL)
    std::vector<TLine> lines;
    std::vector<LT> lt;
    std::vector<ObjUnit> units;
    bool local = true; // OBJ: every face references only its own object's pool slices (§3)
    std::vector<std::uint32_t> fidx;
    std::vector<std::size_t> fstart{0};
    std::vector<Material> mats;
    Mesh mesh;
    std::vector<std::string> comments; // PLY comment and obj_info lines
    std::vector<Element> elements;
    std::string name;                 // ASCII STL solid name
    std::vector<std::uint16_t> attrs; // binary STL attribute words
    std::array<std::byte, 80> header{};
    EnvelopeMap map;
    bool has_map = false;

    std::string_view text(std::size_t line) const {
        return s.substr(lines[line].b, lines[line].e - lines[line].b);
    }
    std::string_view eol(std::size_t line) const {
        return s.substr(lines[line].e, lines[line].end - lines[line].e);
    }
};

void fan(Mesh& m, const std::vector<std::uint32_t>& poly) {
    for (std::size_t k = 1; k + 1 < poly.size(); ++k)
        m.tri.push_back({poly[0], poly[k], poly[k + 1]});
}

// ---- OBJ (§2, §3) ----

// `text` with every positive index component moved by `d` (v, vt, vn); the slash pattern and everything
// else stay as written. Negative (relative) indices never change.
std::string shift(std::string_view s, const std::array<std::int64_t, 3>& d) {
    std::string o;
    std::size_t i = 0;
    while (i < s.size() && space(s, i))
        o += s[i++];
    while (i < s.size() && !space(s, i))
        o += s[i++];
    int comp = 0;
    bool start = true;
    while (i < s.size()) {
        const char c = s[i];
        if (start && c == '#') {
            o.append(s.substr(i));
            break;
        }
        if (space(s, i)) {
            comp = 0, start = true, o += c, ++i;
        } else if (c == '/') {
            ++comp, start = true, o += c, ++i;
        } else if (start && c >= '0' && c <= '9' && comp < 3) {
            std::size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                ++j;
            o += std::to_string(*integer(s.substr(i, j - i)) + d[static_cast<std::size_t>(comp)]);
            i = j, start = false;
        } else {
            o += c, ++i, start = false;
        }
    }
    return o;
}

void parse_obj(Doc& d, Limits* budget) {
    d.lines = split(d.s);
    charge(budget, d.lines.size() * (sizeof(TLine) + 1));
    bool has_o = false;
    for (std::size_t i = 0; i < d.lines.size() && !has_o; ++i) {
        auto t = tokens(d.text(i));
        has_o = !t.empty() && t[0] == "o";
    }
    const std::string_view split_kw = has_o ? "o" : "g";
    std::array<std::uint32_t, 3> pool{};
    std::map<std::string, int> seen;
    d.units.push_back({});
    auto close = [&](std::size_t i) {
        auto& u = d.units.back();
        u.l1 = i, u.p1 = pool, u.f1 = d.fstart.size() - 1;
    };
    std::vector<std::uint32_t> poly;
    d.lt.resize(d.lines.size(), LT::trivia);
    for (std::size_t i = 0; i < d.lines.size(); ++i) {
        const auto t = tokens(d.text(i));
        if (t.empty())
            continue;
        const auto kw = t[0];
        const auto where = [&] { return "line " + std::to_string(i + 1) + ": "; };
        if (kw == split_kw) {
            close(i);
            ObjUnit u;
            u.kw = std::string(kw), u.name = join(t, 1);
            const int n = ++seen[u.name];
            u.key = u.name.empty() || n > 1 ? u.name + "~" + std::to_string(n) : u.name; // "" is the header
            u.l0 = i, u.p0 = pool, u.f0 = d.fstart.size() - 1;
            d.units.push_back(std::move(u));
            d.lt[i] = LT::other;
        } else if (kw == "v") {
            if (t.size() < 4)
                bad(where() + "v needs x y z");
            Vec3 p{};
            for (std::size_t k = 0; k < 3; ++k) {
                auto x = num(t[k + 1]);
                if (!x)
                    bad(where() + "bad number '" + std::string(t[k + 1]) + "'");
                p[k] = *x;
            }
            d.mesh.v.push_back(p);
            ++pool[0];
            d.lt[i] = LT::v;
        } else if (kw == "vt" || kw == "vn") {
            if (t.size() < 2)
                bad(where() + std::string(kw) + " needs values");
            ++pool[kw == "vt" ? 1 : 2];
            d.lt[i] = kw == "vt" ? LT::vt : LT::vn;
        } else if (kw == "vp") {
            d.lt[i] = LT::vp;
        } else if (kw == "f" || kw == "l" || kw == "p") {
            d.lt[i] = LT::elem;
            auto& u = d.units.back();
            poly.clear();
            for (std::size_t k = 1; k < t.size(); ++k) {
                std::size_t c = 0, from = 0;
                for (;;) {
                    const auto slash = t[k].find('/', from);
                    const auto part = t[k].substr(from, slash == npos ? npos : slash - from);
                    if (c >= 3 || (kw != "f" && c >= 2) || (kw == "p" && c >= 1))
                        bad(where() + "too many index components in '" + std::string(t[k]) + "'");
                    if (part.empty()) {
                        if (c == 0)
                            bad(where() + "missing vertex index in '" + std::string(t[k]) + "'");
                    } else {
                        auto x = integer(part);
                        const std::int64_t size = pool[c];
                        if (!x || *x == 0 || *x > size || *x < -size)
                            bad(where() + "index " + std::string(part) + " out of range");
                        const auto idx = static_cast<std::uint32_t>(*x > 0 ? *x - 1 : size + *x);
                        if (idx < u.p0[c])
                            d.local = false;
                        if (c == 0)
                            poly.push_back(idx);
                    }
                    if (slash == npos)
                        break;
                    from = slash + 1, ++c;
                }
            }
            if (kw == "f") {
                d.fidx.insert(d.fidx.end(), poly.begin(), poly.end());
                d.fstart.push_back(d.fidx.size());
                fan(d.mesh, poly);
            }
        } else {
            d.lt[i] = LT::other;
        }
    }
    close(d.lines.size());
    charge(budget, d.mesh.v.size() * sizeof(Vec3) + d.mesh.tri.size() * sizeof(geom::Tri) +
                       d.fidx.size() * 4 + d.fstart.size() * 8);
    for (auto& u : d.units) {
        Hasher h;
        std::array<std::int64_t, 3> rel{};
        for (std::size_t c = 0; c < 3; ++c)
            rel[c] = -static_cast<std::int64_t>(u.p0[c]);
        for (std::size_t i = u.l0; i < u.l1; ++i)
            h.add(d.lt[i] == LT::elem ? shift(d.text(i), rel) : std::string(d.text(i)));
        u.canon = h.digest();
    }
}

// One object's geometry: vertices in order of first reference, then its unreferenced pool vertices.
Mesh unit_mesh(const Doc& d, const ObjUnit& u) {
    Mesh m;
    std::unordered_map<std::uint32_t, std::uint32_t> local;
    auto id = [&](std::uint32_t g) {
        auto [it, fresh] = local.emplace(g, static_cast<std::uint32_t>(m.v.size()));
        if (fresh)
            m.v.push_back(d.mesh.v[g]);
        return it->second;
    };
    std::vector<std::uint32_t> poly;
    for (std::size_t f = u.f0; f < u.f1; ++f) {
        poly.clear();
        for (std::size_t k = d.fstart[f]; k < d.fstart[f + 1]; ++k)
            poly.push_back(id(d.fidx[k]));
        fan(m, poly);
    }
    for (std::uint32_t g = u.p0[0]; g < u.p1[0]; ++g)
        id(g);
    return m;
}

// ---- MTL (§2) ----

void parse_mtl(Doc& d, Limits* budget) {
    d.lines = split(d.s);
    charge(budget, d.lines.size() * sizeof(TLine));
    d.mats.push_back({});
    std::map<std::string, int> seen_mat, seen;
    std::size_t from = 0;
    for (std::size_t i = 0; i < d.lines.size(); ++i) {
        const auto t = tokens(d.text(i));
        if (t.empty())
            continue;
        std::string key(t[0]);
        if (key == "newmtl") {
            const std::string name = join(t, 1);
            const int n = ++seen_mat[name];
            d.mats.push_back({n > 1 ? name + "~" + std::to_string(n) : name, {}});
            seen.clear();
        }
        const int n = ++seen[key];
        if (n > 1)
            key += "~" + std::to_string(n);
        d.mats.back().st.push_back({key, from, d.lines[i].e, d.lines[i].end, {}});
        from = d.lines[i].end;
    }
    if (from < d.s.size()) { // trailing comments and blank lines
        if (d.mats.back().st.empty())
            d.mats.back().st.push_back({"#", from, d.s.size(), d.s.size(), {}});
        else
            d.mats.back().st.back().e = d.mats.back().st.back().end = d.s.size();
    }
    if (d.mats[0].st.empty())
        d.mats.erase(d.mats.begin());
    for (auto& m : d.mats)
        for (auto& st : m.st)
            st.h = hash128(d.s.substr(st.b, st.e - st.b));
}

Hash128 material_hash(const Material& m) {
    Hasher h;
    for (auto& st : m.st)
        h.add(st.key).add(st.h.hi).add(st.h.lo);
    return h.digest();
}

std::vector<std::string_view> stmt_tokens(const Doc& d, const Stmt& st, std::string_view* comment = nullptr) {
    // The statement is the last non-trivia line of its span.
    auto text = d.s.substr(st.b, st.e - st.b);
    std::vector<std::string_view> best;
    for (auto& l : split(text)) {
        auto t = tokens(text.substr(l.b, l.e - l.b), comment);
        if (!t.empty())
            best = std::move(t);
    }
    return best;
}

// ---- PLY (§2) ----

std::optional<Type> ply_type(std::string_view n) {
    static const std::pair<std::string_view, Type> table[] = {
        {"char", {1, false, true, "char"}},      {"int8", {1, false, true, "int8"}},
        {"uchar", {1, false, false, "uchar"}},   {"uint8", {1, false, false, "uint8"}},
        {"short", {2, false, true, "short"}},    {"int16", {2, false, true, "int16"}},
        {"ushort", {2, false, false, "ushort"}}, {"uint16", {2, false, false, "uint16"}},
        {"int", {4, false, true, "int"}},        {"int32", {4, false, true, "int32"}},
        {"uint", {4, false, false, "uint"}},     {"uint32", {4, false, false, "uint32"}},
        {"float", {4, true, true, "float"}},     {"float32", {4, true, true, "float32"}},
        {"double", {8, true, true, "double"}},   {"float64", {8, true, true, "float64"}}};
    for (auto& [k, t] : table)
        if (k == n)
            return t;
    return std::nullopt;
}

bool ply_magic(std::string_view s) {
    return s.starts_with("ply\n") || s.starts_with("ply\r\n");
}

struct BinReader {
    std::span<const std::byte> b;
    std::size_t p = 0;
    bool big = false;
    double read(const Type& t) {
        if (b.size() - p < t.size)
            bad("PLY body is shorter than its element counts");
        std::uint64_t u = 0;
        for (std::size_t k = 0; k < t.size; ++k) {
            const auto byte = std::to_integer<std::uint64_t>(b[p + (big ? k : t.size - 1 - k)]);
            u = u << 8 | byte;
        }
        p += t.size;
        if (t.real)
            return t.size == 4 ? double(std::bit_cast<float>(static_cast<std::uint32_t>(u)))
                               : std::bit_cast<double>(u);
        if (t.sign && t.size < 8 && (u >> (t.size * 8 - 1)) & 1)
            return static_cast<double>(static_cast<std::int64_t>(u | (~std::uint64_t{0} << (t.size * 8))));
        return static_cast<double>(u);
    }
};

struct AsciiReader {
    std::string_view s;
    std::size_t p = 0;
    double read(const Type&) {
        while (p < s.size() && space(s, p))
            ++p;
        std::size_t q = p;
        while (q < s.size() && !space(s, q))
            ++q;
        if (q == p)
            bad("PLY body is shorter than its element counts");
        auto x = num(s.substr(p, q - p));
        if (!x)
            bad("PLY: bad number '" + std::string(s.substr(p, q - p)) + "'");
        p = q;
        return *x;
    }
};

template <class R> void ply_body(Doc& d, R& r, std::size_t remaining, Limits* budget) {
    std::uint64_t nverts = 0;
    for (auto& e : d.elements)
        if (e.name == "vertex")
            nverts = e.count;
    std::vector<std::uint32_t> poly;
    for (auto& e : d.elements) {
        e.body_b = e.body_e = r.p;
        // Counts are bounded by the bytes left before anything is allocated (§11).
        std::uint64_t min_row = 0;
        for (auto& p : e.props)
            min_row += p.count ? p.count->size : p.type.size;
        if constexpr (std::is_same_v<R, AsciiReader>)
            min_row = e.props.size();
        if (min_row && e.count > remaining / min_row + 1)
            bad("PLY element '" + e.name + "' count " + std::to_string(e.count) + " exceeds the file size");
        if (e.props.empty())
            continue;
        const bool vtx = e.name == "vertex", face = e.name == "face";
        std::array<int, 3> xyz{-1, -1, -1};
        int list = -1;
        for (std::size_t k = 0; k < e.props.size(); ++k) {
            const auto& p = e.props[k];
            for (std::size_t a = 0; a < 3 && vtx; ++a)
                if (!p.count && p.name == std::string(1, static_cast<char>('x' + a)))
                    xyz[a] = static_cast<int>(k);
            if (face && p.count && (p.name == "vertex_indices" || p.name == "vertex_index"))
                list = static_cast<int>(k);
        }
        if (vtx && (xyz[0] < 0 || xyz[1] < 0 || xyz[2] < 0))
            bad("PLY vertex element without x, y, z");
        const std::size_t before = d.mesh.v.size() * sizeof(Vec3) + d.mesh.tri.size() * sizeof(geom::Tri);
        for (std::uint64_t row = 0; row < e.count; ++row) {
            Vec3 pos{};
            for (std::size_t k = 0; k < e.props.size(); ++k) {
                const auto& p = e.props[k];
                if (!p.count) {
                    const double x = r.read(p.type);
                    const auto a = std::find(xyz.begin(), xyz.end(), static_cast<int>(k));
                    if (vtx && a != xyz.end())
                        pos[static_cast<std::size_t>(a - xyz.begin())] = x;
                    else
                        e.values.push_back(x);
                    continue;
                }
                const double n = r.read(*p.count);
                if (!(n >= 0) || n != std::floor(n) || n > static_cast<double>(remaining))
                    bad("PLY: bad list length in element '" + e.name + "'");
                const auto len = static_cast<std::size_t>(n);
                if (static_cast<int>(k) == list) {
                    poly.clear();
                    for (std::size_t j = 0; j < len; ++j) {
                        const double v = r.read(p.type);
                        if (!(v >= 0) || v >= static_cast<double>(nverts) || v != std::floor(v))
                            bad("PLY: vertex index out of range in face " + std::to_string(row));
                        poly.push_back(static_cast<std::uint32_t>(v));
                    }
                    fan(d.mesh, poly);
                } else {
                    e.values.push_back(n);
                    for (std::size_t j = 0; j < len; ++j)
                        e.values.push_back(r.read(p.type));
                }
            }
            if (vtx)
                d.mesh.v.push_back(pos);
        }
        e.body_e = r.p;
        charge(budget, d.mesh.v.size() * sizeof(Vec3) + d.mesh.tri.size() * sizeof(geom::Tri) - before +
                           e.values.size() * sizeof(double));
    }
}

void parse_ply(Doc& d, Limits* budget) {
    const auto all = chars(d.src.bytes());
    std::size_t p = 0;
    std::string format;
    bool done = false;
    while (!done) {
        const auto q = all.find('\n', p);
        if (q == npos)
            bad("PLY header without end_header");
        auto line = all.substr(p, q - p);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        const auto t = tokens(line);
        const std::size_t at = p;
        p = q + 1;
        if (t.empty())
            continue;
        if (t[0] == "ply") {
        } else if (t[0] == "format") {
            if (t.size() != 3 || t[2] != "1.0" ||
                (t[1] != "ascii" && t[1] != "binary_little_endian" && t[1] != "binary_big_endian"))
                bad("PLY: unsupported format line '" + std::string(line) + "'");
            format = std::string(t[1]);
        } else if (t[0] == "comment" || t[0] == "obj_info") {
            d.comments.emplace_back(line);
        } else if (t[0] == "element") {
            auto n = t.size() == 3 ? integer(t[2]) : std::nullopt;
            if (!n || *n < 0)
                bad("PLY: bad element line '" + std::string(line) + "'");
            Element e;
            e.name = std::string(t[1]), e.count = static_cast<std::uint64_t>(*n);
            e.count_b = at + static_cast<std::size_t>(t[2].data() - line.data());
            e.count_e = e.count_b + t[2].size();
            d.elements.push_back(std::move(e));
        } else if (t[0] == "property") {
            if (d.elements.empty())
                bad("PLY: property before any element");
            Prop pr;
            if (t.size() == 5 && t[1] == "list") {
                auto c = ply_type(t[2]), v = ply_type(t[3]);
                if (!c || !v || c->real)
                    bad("PLY: bad list property '" + std::string(line) + "'");
                pr = {std::string(t[4]), *v, *c};
            } else if (t.size() == 3 && ply_type(t[1])) {
                pr = {std::string(t[2]), *ply_type(t[1]), std::nullopt};
            } else {
                bad("PLY: bad property line '" + std::string(line) + "'");
            }
            d.elements.back().props.push_back(std::move(pr));
        } else if (t[0] == "end_header") {
            done = true;
        } else {
            bad("PLY: unknown header line '" + std::string(line) + "'");
        }
    }
    if (format.empty())
        bad("PLY header without a format line");
    d.storage = "ply " + format;
    const std::size_t body = p;
    const std::size_t remaining = all.size() - body;
    if (format == "ascii") {
        AsciiReader r{all, body};
        ply_body(d, r, remaining, budget);
        for (std::size_t k = r.p; k < all.size(); ++k)
            if (!space(all, k))
                bad("PLY: data after the last element");
    } else {
        BinReader r{d.src.bytes(), body, format == "binary_big_endian"};
        ply_body(d, r, remaining, budget);
        if (r.p != all.size())
            bad("PLY: " + std::to_string(all.size() - r.p) + " bytes after the last element");
    }
    // Envelope map (§7.1): element counts are framing; each element's rows are content.
    auto& m = d.map.ranges;
    std::size_t at = 0;
    for (auto& e : d.elements) {
        if (e.count_b > at)
            m.push_back({at, e.count_b - at, ByteClass::content, "/header", ""});
        m.push_back({e.count_b, e.count_e - e.count_b, ByteClass::framing,
                     "/elements/" + pointer_escape(e.name), ""});
        at = e.count_e;
    }
    m.push_back({at, body - at, ByteClass::content, "/header", ""});
    at = body;
    for (std::size_t k = 0; k < d.elements.size(); ++k) {
        const std::size_t end = k + 1 == d.elements.size() ? all.size() : d.elements[k + 1].body_b;
        if (end > at)
            m.push_back(
                {at, end - at, ByteClass::content, "/elements/" + pointer_escape(d.elements[k].name), ""});
        at = std::max(at, end);
    }
    if (at < all.size())
        m.push_back({at, all.size() - at, ByteClass::content, "/elements", ""});
    d.has_map = true;
}

// ---- STL (§2) ----

bool stl_binary_size(std::span<const std::byte> b) {
    if (b.size() < 84)
        return false;
    std::uint64_t n = 0;
    for (int k = 3; k >= 0; --k)
        n = n << 8 | std::to_integer<std::uint64_t>(b[80 + static_cast<std::size_t>(k)]);
    return 84 + 50 * n == b.size();
}

bool starts_solid(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && space(s, i))
        ++i;
    return s.substr(i, 5) == "solid" && (i + 5 == s.size() || space(s, i + 5));
}

// STL is a triangle soup: identical positions are welded so topology and displacement can be compared.
struct Welder {
    Mesh& m;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> at;
    std::uint32_t operator()(const Vec3& p0) {
        const Vec3 p{p0[0] + 0.0, p0[1] + 0.0, p0[2] + 0.0}; // -0 welds with 0
        std::uint64_t h = 1469598103934665603ull;
        for (double x : p)
            h = (h ^ std::bit_cast<std::uint64_t>(x)) * 1099511628211ull;
        auto& v = at[h];
        for (auto i : v)
            if (m.v[i] == p)
                return i;
        v.push_back(static_cast<std::uint32_t>(m.v.size()));
        m.v.push_back(p);
        return v.back();
    }
};

void parse_stl_binary(Doc& d, Limits* budget) {
    const auto b = d.src.bytes();
    d.storage = "stl binary";
    std::copy(b.begin(), b.begin() + 80, d.header.begin());
    const std::size_t n = (b.size() - 84) / 50;
    charge(budget, n * (sizeof(geom::Tri) + 2 + 3 * sizeof(Vec3)));
    Welder weld{d.mesh, {}};
    BinReader r{b, 84, false};
    const Type f32{4, true, true, "float"}, u16{2, false, false, "ushort"};
    bool color = false;
    for (std::size_t i = 0; i < n; ++i) {
        r.p += 12; // the normal is derived (Q-MESH-4)
        std::array<std::uint32_t, 3> t{};
        for (auto& k : t) {
            Vec3 p{};
            for (auto& x : p)
                x = r.read(f32);
            k = weld(p);
        }
        d.mesh.tri.push_back(t);
        d.attrs.push_back(static_cast<std::uint16_t>(r.read(u16)));
        color = color || d.attrs.back() != 0;
    }
    // Envelope map (§7.1).
    auto& m = d.map.ranges;
    m.push_back({0, 80, ByteClass::volatile_, "/header", ""});
    m.push_back({80, 4, ByteClass::framing, "/geometry", ""});
    if (n > kMapFacets) {
        // ponytail: one content range above 100k facets (3 ranges per facet cost ~250 bytes each); a
        // range-free classifier is the upgrade if churn of large STLs matters.
        m.push_back({84, 50 * n, ByteClass::content, "/geometry", ""});
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t o = 84 + 50 * i;
            m.push_back({o, 12, ByteClass::derived, "/geometry", ""});
            m.push_back({o + 12, 36, ByteClass::content, "/geometry", ""});
            m.push_back({o + 48, 2, color ? ByteClass::content : ByteClass::framing, "/geometry", ""});
        }
    }
    d.has_map = true;
}

void parse_stl_ascii(Doc& d, Limits* budget) {
    const auto s = chars(d.src.bytes());
    d.storage = "stl ascii";
    std::size_t p = 0;
    auto next = [&]() -> std::string_view {
        while (p < s.size() && space(s, p))
            ++p;
        std::size_t q = p;
        while (q < s.size() && !space(s, q))
            ++q;
        auto t = s.substr(p, q - p);
        p = q;
        return t;
    };
    auto rest_of_line = [&] {
        const auto q = std::min(s.find_first_of("\r\n", p), s.size());
        auto t = s.substr(p, q - p);
        p = q;
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.remove_prefix(1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
            t.remove_suffix(1);
        return t;
    };
    auto expect = [&](std::string_view w) {
        if (next() != w)
            bad("ASCII STL: expected '" + std::string(w) + "' near byte " + std::to_string(p));
    };
    auto number = [&] {
        auto t = next();
        auto x = num(t);
        if (!x)
            bad("ASCII STL: bad number '" + std::string(t) + "'");
        return *x;
    };
    expect("solid");
    d.name = std::string(rest_of_line());
    Welder weld{d.mesh, {}};
    for (;;) {
        auto t = next();
        if (t == "facet") {
            expect("normal");
            for (int k = 0; k < 3; ++k)
                number();
            expect("outer"), expect("loop");
            std::array<std::uint32_t, 3> tri{};
            for (auto& k : tri) {
                expect("vertex");
                Vec3 v{};
                for (auto& x : v)
                    x = number();
                k = weld(v);
            }
            expect("endloop"), expect("endfacet");
            d.mesh.tri.push_back(tri);
        } else if (t == "endsolid") {
            rest_of_line();
            auto n = next();
            if (n.empty())
                break;
            if (n != "solid")
                bad("ASCII STL: data after endsolid");
            rest_of_line(); // further solids join the same soup
        } else {
            bad("ASCII STL: expected 'facet' or 'endsolid' near byte " + std::to_string(p));
        }
    }
    charge(budget, d.mesh.v.size() * sizeof(Vec3) + d.mesh.tri.size() * sizeof(geom::Tri));
}

// ---- Loading ----

bool mtl_content(std::string_view s) {
    bool newmtl = false;
    for (auto& l : split(s.substr(0, kHead))) {
        auto t = tokens(s.substr(l.b, l.e - l.b));
        if (t.empty())
            continue;
        if (t[0] == "v" || t[0] == "f" || t[0] == "o")
            return false;
        newmtl = newmtl || t[0] == "newmtl";
    }
    return newmtl;
}

std::unique_ptr<Doc> parse(ByteSource src, Limits* budget) {
    auto d = std::make_unique<Doc>();
    d->src = std::move(src);
    const auto b = d->src.bytes();
    const auto all = chars(b);
    const auto ext = extension(d->src.name());
    if (ply_magic(all)) {
        d->kind = Kind::ply;
        parse_ply(*d, budget);
    } else if (stl_binary_size(b)) {
        d->kind = Kind::stl;
        parse_stl_binary(*d, budget);
    } else if (starts_solid(all)) {
        d->kind = Kind::stl;
        parse_stl_ascii(*d, budget);
    } else if (ext == ".stl" || ext == ".ply") {
        bad("not a" + std::string(ext == ".stl" ? "n STL" : " PLY") + " file");
    } else {
        if (looks_binary(b))
            bad("binary data is not OBJ or MTL");
        d->bom = all.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        d->s = all.substr(d->bom);
        d->kind = ext == ".mtl" || (ext != ".obj" && mtl_content(d->s)) ? Kind::mtl : Kind::obj;
        d->storage = d->kind == Kind::mtl ? "mtl" : "obj";
        if (d->kind == Kind::mtl)
            parse_mtl(*d, budget);
        else
            parse_obj(*d, budget);
    }
    return d;
}

const Doc& cast(const Document& d) {
    auto* c = dynamic_cast<const Doc*>(&d);
    if (!c)
        throw Error(ErrorCategory::internal, "mesh: foreign document");
    return *c;
}

double tolerance(const OptionValues& o) {
    auto it = o.find("tolerance");
    if (it == o.end())
        return 0;
    auto x = num(it->second);
    if (!x || !(*x >= 0))
        throw Error(ErrorCategory::unsupported, "mesh: --tolerance=" + it->second + " (expected a distance)");
    return *x;
}

// ---- Metrics (§5) ----

nlohmann::ordered_json vec(const Vec3& v) {
    return {r6(v[0]), r6(v[1]), r6(v[2])};
}

std::string counts(const Mesh& m) {
    return std::to_string(m.v.size()) + " vertices, " + std::to_string(m.tri.size()) + " triangles";
}

Hash128 positions_hash(const Mesh& m) {
    Hasher h;
    for (auto& p : m.v)
        for (double x : p)
            h.add(g6(x));
    return h.digest();
}

Hash128 topology_hash(const Mesh& m) {
    Hasher h;
    for (auto& t : m.tri)
        for (auto i : t)
            h.add(std::uint64_t{i});
    return h.digest();
}

Hash128 geometry_hash(const Mesh& m) {
    auto a = positions_hash(m), b = topology_hash(m);
    Hasher h;
    h.add(a.hi).add(a.lo).add(b.hi).add(b.lo);
    return h.digest();
}

std::string summary(const Mesh& m) {
    const auto b = geom::bbox(m);
    std::string o = "vertices=" + std::to_string(m.v.size()) + " triangles=" + std::to_string(m.tri.size());
    if (!b.empty)
        o += " bbox=" + g6(b.lo[0]) + "," + g6(b.lo[1]) + "," + g6(b.lo[2]) + ".." + g6(b.hi[0]) + "," +
             g6(b.hi[1]) + "," + g6(b.hi[2]);
    o += " area=" + g6(geom::area(m)) + " volume=" + g6(geom::volume(m));
    o += " positions=xxh3:" + positions_hash(m).hex().substr(0, 16);
    o += " topology=xxh3:" + topology_hash(m).hex().substr(0, 16);
    return o;
}

nlohmann::ordered_json metrics(const Mesh& a, const Mesh& b, double tol) {
    nlohmann::ordered_json m;
    m["vertices"] = {a.v.size(), b.v.size()};
    m["triangles"] = {a.tri.size(), b.tri.size()};
    const auto ba = geom::bbox(a), bb = geom::bbox(b);
    if (!ba.empty && !bb.empty) {
        m["bbox"] = {{vec(ba.lo), vec(ba.hi)}, {vec(bb.lo), vec(bb.hi)}};
        Vec3 ca{}, cb{};
        for (auto& p : a.v)
            for (std::size_t k = 0; k < 3; ++k)
                ca[k] += p[k] / static_cast<double>(a.v.size());
        for (auto& p : b.v)
            for (std::size_t k = 0; k < 3; ++k)
                cb[k] += p[k] / static_cast<double>(b.v.size());
        m["centroid_shift"] =
            r6(std::sqrt((cb[0] - ca[0]) * (cb[0] - ca[0]) + (cb[1] - ca[1]) * (cb[1] - ca[1]) +
                         (cb[2] - ca[2]) * (cb[2] - ca[2])));
    }
    m["area"] = {r6(geom::area(a)), r6(geom::area(b))};
    m["volume"] = {r6(geom::volume(a)), r6(geom::volume(b))};
    const auto ea = geom::edges(a), eb = geom::edges(b);
    if (ea.boundary != eb.boundary)
        m["boundary_edges"] = {ea.boundary, eb.boundary};
    if (ea.nonmanifold != eb.nonmanifold)
        m["nonmanifold_edges"] = {ea.nonmanifold, eb.nonmanifold};
    if (std::max(a.tri.size(), b.tri.size()) > kMaxGeomTris) {
        m["hausdorff_skipped"] = true;
    } else if (auto d = geom::distance(a, b)) {
        m["hausdorff"] = r6(d->hausdorff), m["rms"] = r6(d->rms);
        if (d->sampled)
            m["sampled"] = true;
    }
    if (auto dp = geom::displacement(a, b, tol)) {
        m["moved"] = dp->moved;
        m["max_displacement"] = r6(dp->max);
        m["mean_displacement"] = r6(dp->mean);
        m["regions"] = dp->regions.size();
        if (!dp->regions.empty() && dp->regions.size() <= 16) {
            auto& l = m["region_list"] = nlohmann::ordered_json::array();
            for (auto& g : dp->regions)
                l.push_back(
                    {{"bbox", {vec(g.box.lo), vec(g.box.hi)}}, {"max", r6(g.max)}, {"vertices", g.vertices}});
        }
    }
    return m;
}

bool same_geometry(const Mesh& a, const Mesh& b, double tol) {
    auto dp = geom::displacement(a, b, tol);
    return dp && dp->moved == 0;
}

std::optional<Change> geometry_change(const Mesh& a, const Mesh& b, double tol, Side side, std::string path) {
    if (same_geometry(a, b, tol))
        return std::nullopt;
    Change c = make_change(side, Op::update, {std::move(path)}, "geometry");
    c.before = {counts(a), geometry_hash(a)}, c.after = {counts(b), geometry_hash(b)};
    c.metrics = metrics(a, b, tol);
    return c;
}

// ---- Diff (§5) ----

std::string unit_path(const std::string& key) {
    return key.empty() ? "/header" : "/objects/name:" + pointer_escape(key);
}

std::string material_path(const std::string& key) {
    return key.empty() ? "/header" : "/materials/name:" + pointer_escape(key);
}

const ObjUnit* find_unit(const Doc& d, const std::string& key) {
    for (auto& u : d.units)
        if (u.key == key)
            return &u;
    return nullptr;
}

const Material* find_material(const Doc& d, const std::string& key) {
    for (auto& m : d.mats)
        if (m.key == key)
            return &m;
    return nullptr;
}

const Stmt* find_stmt(const Material& m, const std::string& key) {
    for (auto& s : m.st)
        if (s.key == key)
            return &s;
    return nullptr;
}

// Statements equal line by line (comments and blank lines aside), numbers within `tol`, face indices
// relative to each object's pools. `comments` tells whether the comments match too.
bool same_unit(const Doc& a, const ObjUnit& ua, const Doc& b, const ObjUnit& ub, double tol, bool& comments) {
    auto lines = [](const Doc& d, const ObjUnit& u) {
        std::vector<std::pair<std::vector<std::string>, std::string>> v; // tokens, comment
        std::array<std::int64_t, 3> rel{};
        for (std::size_t c = 0; c < 3; ++c)
            rel[c] = -static_cast<std::int64_t>(u.p0[c]);
        for (std::size_t i = u.l0; i < u.l1; ++i) {
            const std::string text = d.lt[i] == LT::elem ? shift(d.text(i), rel) : std::string(d.text(i));
            std::string_view comment;
            auto t = tokens(text, &comment);
            v.emplace_back(std::vector<std::string>(t.begin(), t.end()), std::string(comment));
        }
        return v;
    };
    const auto la = lines(a, ua), lb = lines(b, ub);
    std::vector<std::string_view> ca, cb;
    std::vector<std::vector<std::string_view>> ta, tb;
    for (auto& [t, c] : la) {
        if (!c.empty())
            ca.push_back(c);
        if (!t.empty())
            ta.emplace_back(t.begin(), t.end());
    }
    for (auto& [t, c] : lb) {
        if (!c.empty())
            cb.push_back(c);
        if (!t.empty())
            tb.emplace_back(t.begin(), t.end());
    }
    comments = ca == cb;
    if (ta.size() != tb.size())
        return false;
    for (std::size_t i = 0; i < ta.size(); ++i)
        if (!same_tokens(ta[i], tb[i], tol))
            return false;
    return true;
}

// Non-geometry statements of an object (o, g, usemtl, s, mtllib, ...), one line each.
std::vector<std::string> statements(const Doc& d, const ObjUnit& u) {
    std::vector<std::string> v;
    for (std::size_t i = u.l0; i < u.l1; ++i)
        if (d.lt[i] == LT::other && (i != u.l0 || u.key.empty())) // the object's own `o` line is its address
            v.push_back(join(tokens(d.text(i))));
    return v;
}

std::string clip(std::string t) {
    return t.size() > 120 ? t.substr(0, 117) + "..." : t;
}

std::string joined(const std::vector<std::string>& v) {
    std::string o;
    for (auto& x : v)
        o += (o.empty() ? "" : "; ") + x;
    return clip(o);
}

void diff_obj(const Doc& a, const Doc& b, Side side, double tol, ChangeSet& out) {
    for (auto& ua : a.units) {
        const std::string path = unit_path(ua.key);
        const ObjUnit* ub = find_unit(b, ua.key);
        if (!ub) {
            if (ua.l0 == ua.l1)
                continue;
            Change c = make_change(side, Op::erase, {path}, "object");
            c.before = {counts(unit_mesh(a, ua)), ua.canon};
            out.push_back(std::move(c));
            continue;
        }
        if (ua.canon == ub->canon)
            continue;
        bool comments = true;
        if (same_unit(a, ua, b, *ub, tol, comments)) {
            Change c = make_change(side, Op::update, {path}, comments ? "encoding" : "comment");
            c.before.hash = ua.canon, c.after.hash = ub->canon;
            out.push_back(std::move(c));
            continue;
        }
        const Mesh ma = unit_mesh(a, ua), mb = unit_mesh(b, *ub);
        if (auto g = geometry_change(ma, mb, tol, side, path)) {
            out.push_back(std::move(*g));
            continue;
        }
        Change c = make_change(side, Op::update, {path}, "statements");
        c.before = {joined(statements(a, ua)), ua.canon}, c.after = {joined(statements(b, *ub)), ub->canon};
        out.push_back(std::move(c));
    }
    for (auto& ub : b.units)
        if (!find_unit(a, ub.key) && ub.l0 != ub.l1) {
            Change c = make_change(side, Op::insert, {unit_path(ub.key)}, "object");
            c.after = {counts(unit_mesh(b, ub)), ub.canon};
            out.push_back(std::move(c));
        }
}

std::string stmt_text(const Doc& d, const Stmt& st) {
    return clip(join(stmt_tokens(d, st)));
}

void diff_mtl(const Doc& a, const Doc& b, Side side, double tol, ChangeSet& out) {
    for (auto& ma : a.mats) {
        const std::string path = material_path(ma.key);
        const Material* mb = find_material(b, ma.key);
        if (!mb) {
            out.push_back(make_change(side, Op::erase, {path}, "material"));
            continue;
        }
        for (auto& sa : ma.st) {
            const std::string sp = path + "/" + pointer_escape(sa.key);
            const Stmt* sb = find_stmt(*mb, sa.key);
            if (!sb) {
                Change c = make_change(side, Op::erase, {sp}, "statement");
                c.before = {stmt_text(a, sa), sa.h};
                out.push_back(std::move(c));
                continue;
            }
            if (sa.h == sb->h)
                continue;
            std::string_view ca, cb;
            const auto ta = stmt_tokens(a, sa, &ca), tb = stmt_tokens(b, *sb, &cb);
            const char* kind = !same_tokens(ta, tb, tol) ? "statement" : ca != cb ? "comment" : "encoding";
            Change c = make_change(side, Op::update, {sp}, kind);
            c.before = {clip(join(ta)), sa.h}, c.after = {clip(join(tb)), sb->h};
            out.push_back(std::move(c));
        }
        for (auto& sb : mb->st)
            if (!find_stmt(ma, sb.key)) {
                Change c = make_change(side, Op::insert, {path + "/" + pointer_escape(sb.key)}, "statement");
                c.after = {stmt_text(b, sb), sb.h};
                out.push_back(std::move(c));
            }
    }
    for (auto& mb : b.mats)
        if (!find_material(a, mb.key))
            out.push_back(make_change(side, Op::insert, {material_path(mb.key)}, "material"));
}

std::string element_decl(const Element& e) {
    std::string o = std::to_string(e.count) + ":";
    for (auto& p : e.props)
        o += " " + p.name + ":" + (p.count ? "list " + std::string(p.count->name) + " " : "") +
             std::string(p.type.name);
    return o;
}

std::string values_hash(const Element& e) {
    Hasher h;
    for (double x : e.values)
        h.add(g6(x));
    return "xxh3:" + h.digest().hex().substr(0, 16);
}

bool same_values(const std::vector<double>& a, const std::vector<double>& b, double tol) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!(std::fabs(a[i] - b[i]) <= tol) && !(std::isnan(a[i]) && std::isnan(b[i])))
            return false;
    return true;
}

void diff_ply(const Doc& a, const Doc& b, Side side, double tol, ChangeSet& out) {
    if (a.comments != b.comments) {
        Change c = make_change(side, Op::update, {"/header/comments"}, "comment");
        auto all = [](const std::vector<std::string>& v) {
            std::string o;
            for (auto& x : v)
                o += (o.empty() ? "" : "; ") + x;
            return clip(o);
        };
        c.before.text = all(a.comments), c.after.text = all(b.comments);
        out.push_back(std::move(c));
    }
    auto find = [](const Doc& d, const std::string& n) -> const Element* {
        for (auto& e : d.elements)
            if (e.name == n)
                return &e;
        return nullptr;
    };
    for (auto& ea : a.elements) {
        const std::string path = "/elements/" + pointer_escape(ea.name);
        const Element* eb = find(b, ea.name);
        if (!eb) {
            Change c = make_change(side, Op::erase, {path}, "element");
            c.before.text = element_decl(ea);
            out.push_back(std::move(c));
        } else if (element_decl(ea) != element_decl(*eb) || !same_values(ea.values, eb->values, tol)) {
            Change c = make_change(side, Op::update, {path}, "element");
            c.before.text = element_decl(ea), c.after.text = element_decl(*eb);
            if (c.before.text == c.after.text)
                c.before.text = "values " + values_hash(ea), c.after.text = "values " + values_hash(*eb);
            out.push_back(std::move(c));
        }
    }
    for (auto& eb : b.elements)
        if (!find(a, eb.name)) {
            Change c = make_change(side, Op::insert, {"/elements/" + pointer_escape(eb.name)}, "element");
            c.after.text = element_decl(eb);
            out.push_back(std::move(c));
        }
}

std::string header_text(const Doc& d) {
    std::string o;
    for (auto byte : d.header) {
        const auto c = std::to_integer<unsigned char>(byte);
        o += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
    }
    while (!o.empty() && (o.back() == '.' || o.back() == ' '))
        o.pop_back();
    return o;
}

void diff_stl(const Doc& a, const Doc& b, Side side, ChangeSet& out) {
    if (a.name != b.name && a.storage == "stl ascii" && b.storage == "stl ascii") {
        Change c = make_change(side, Op::update, {"/solid/name"}, "name");
        c.before.text = a.name, c.after.text = b.name;
        out.push_back(std::move(c));
    }
    if (a.storage == "stl binary" && b.storage == "stl binary") {
        if (a.header != b.header) {
            Change c = make_change(side, Op::update, {"/header"}, "volatile");
            c.before.text = header_text(a), c.after.text = header_text(b);
            out.push_back(std::move(c));
        }
        if (a.attrs != b.attrs) {
            Change c = make_change(side, Op::update, {"/solid/attributes"}, "attributes");
            auto colored = [](const Doc& d) {
                return std::to_string(
                           std::count_if(d.attrs.begin(), d.attrs.end(), [](auto x) { return x != 0; })) +
                       " facets with attribute words";
            };
            c.before.text = colored(a), c.after.text = colored(b);
            out.push_back(std::move(c));
        }
    }
}

std::string family(const Doc& d) {
    return d.storage.substr(0, 3);
}

ChangeSet diff_docs(const Doc& a, const Doc& b, Side side, double tol) {
    ChangeSet cs;
    if (a.kind == Kind::obj && b.kind == Kind::obj) {
        diff_obj(a, b, side, tol, cs);
    } else if (a.kind == Kind::mtl && b.kind == Kind::mtl) {
        diff_mtl(a, b, side, tol, cs);
    } else {
        if (a.storage != b.storage) {
            // ASCII against binary of one format is a change of encoding only (§4).
            Change c =
                make_change(side, Op::update, {"/storage"}, family(a) == family(b) ? "encoding" : "storage");
            c.before.text = a.storage, c.after.text = b.storage;
            cs.push_back(std::move(c));
        }
        if (a.kind != Kind::mtl && b.kind != Kind::mtl)
            if (auto g = geometry_change(a.mesh, b.mesh, tol, side, "/geometry"))
                cs.push_back(std::move(*g));
        if (a.kind == Kind::ply && b.kind == Kind::ply)
            diff_ply(a, b, side, tol, cs);
        if (a.kind == Kind::stl && b.kind == Kind::stl)
            diff_stl(a, b, side, cs);
    }
    if (cs.empty() && !std::ranges::equal(a.src.bytes(), b.src.bytes())) {
        Change c = make_change(side, Op::update, {"/storage"}, "encoding"); // spelling, whitespace, normals
        c.before.text = a.storage, c.after.text = b.storage;
        cs.push_back(std::move(c));
    }
    sort_canonical(cs);
    return cs;
}

// ---- Writer (§7): ours' bytes, spans copied raw through the Emitter, shifted face lines rewritten ----

struct TextOut {
    std::vector<emit::Seg> segs;
    std::string eol = "\n";
    bool open = false; // the last piece has no terminator
    std::array<std::uint32_t, 3> pool{};

    void close() {
        if (open)
            segs.push_back(emit::bytes(eol)), open = false;
    }
    void raw(const Doc& d, std::size_t b, std::size_t e, bool terminated) {
        if (b == e)
            return;
        close();
        segs.push_back(emit::raw(d.src, d.bom + b, e - b));
        open = !terminated;
    }
    void text(const std::string& t, bool terminated) {
        if (t.empty())
            return;
        close();
        segs.push_back(emit::bytes(t));
        open = !terminated;
    }
    std::vector<std::byte> finish(const Doc& ours) {
        std::vector<emit::Seg> all;
        if (ours.bom)
            all.push_back(emit::raw(ours.src, 0, ours.bom));
        for (auto& s : segs)
            all.push_back(std::move(s));
        return emit::emit(emit::group(1, std::move(all)));
    }
};

std::string dominant_eol(const Doc& d) {
    for (std::size_t i = 0; i < d.lines.size(); ++i)
        if (d.lines[i].end > d.lines[i].e)
            return std::string(d.eol(i));
    return "\n";
}

// An object's lines with its face indices moved to the output's pools; `rename` writes theirs' copy of a
// conflicting object as `o name.splinter_theirs` (--conflict=mark, §6).
void put_unit(TextOut& out, const Doc& d, const ObjUnit& u, bool rename = false) {
    std::array<std::int64_t, 3> delta{};
    bool moved = false;
    for (std::size_t c = 0; c < 3; ++c) {
        delta[c] = static_cast<std::int64_t>(out.pool[c]) - static_cast<std::int64_t>(u.p0[c]);
        moved = moved || delta[c] != 0;
    }
    std::size_t run = npos;
    auto flush = [&](std::size_t upto_line) {
        if (run == npos)
            return;
        const auto& last = d.lines[upto_line - 1];
        out.raw(d, d.lines[run].b, last.end, last.end > last.e);
        run = npos;
    };
    for (std::size_t i = u.l0; i < u.l1; ++i) {
        const bool rn = rename && i == u.l0;
        if (!rn && !(moved && d.lt[i] == LT::elem)) {
            if (run == npos)
                run = i;
            continue;
        }
        flush(i);
        const std::string t = rn ? u.kw + " " + u.name + ".splinter_theirs" : shift(d.text(i), delta);
        out.text(t + std::string(d.eol(i)), d.lines[i].end > d.lines[i].e);
    }
    flush(u.l1);
    for (std::size_t c = 0; c < 3; ++c)
        out.pool[c] += u.p1[c] - u.p0[c];
}

// ---- Merge (§6) ----

struct Pending {
    Address at;
    std::string reason;
    bool mark = false;
};

std::size_t ix(Side s) {
    return static_cast<std::size_t>(s);
}

class MeshFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "mesh",
            "3-D meshes: OBJ/MTL, PLY, STL (geometry metrics; OBJ objects and MTL materials merged)",
            {".obj", ".mtl", ".ply", ".stl"},
            {true, true, false, true, false},
            {}};
        return i;
    }

    // PLY magic: 100. Binary STL whose size equation holds within the head: 90. ASCII STL (`solid` and a
    // facet): 80. OBJ (`v` lines with faces or objects) or MTL (`newmtl`): 60. Extension alone: 60.
    Probe probe(std::span<const std::byte> head, std::string_view name) const override {
        const auto s = chars(head);
        const auto ext = extension(name);
        if (ply_magic(s))
            return {100};
        if (head.size() < kHead && stl_binary_size(head))
            return {90};
        if (starts_solid(s) && (s.find("facet") != npos || s.find("endsolid") != npos))
            return {80};
        if (ext == ".stl")
            return {60}; // a binary STL larger than the head
        if (looks_binary(head) || ext == ".ply")
            return {0};
        std::size_t v = 0, f = 0, grp = 0, newmtl = 0;
        auto lines = split(s);
        if (head.size() >= kHead && !lines.empty())
            lines.pop_back();
        for (auto& l : lines) {
            auto t = tokens(s.substr(l.b, l.e - l.b));
            if (t.empty())
                continue;
            v += t[0] == "v" && t.size() >= 4 && num(t[1]) && num(t[2]) && num(t[3]);
            f += t[0] == "f";
            grp += t[0] == "o" || t[0] == "g" || t[0] == "mtllib" || t[0] == "usemtl";
            newmtl += t[0] == "newmtl";
        }
        int c = newmtl || (v >= 3 && (f || grp)) || (v && f) ? 60 : 0;
        if (ext == ".obj" || ext == ".mtl")
            c = std::max(c, 60);
        return {c};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return parse(std::move(src), &o.ctx.budget);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        return diff_docs(cast(a), cast(b), Side::theirs, tolerance(o.ctx.format_options));
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override {
        const std::array<const Doc*, 3> d{&cast(base), &cast(ours), &cast(theirs)};
        const double tol = tolerance(mo.ctx.format_options);
        MergeResult res;
        res.changes_ours = diff_docs(*d[0], *d[1], Side::ours, tol);
        res.changes_theirs = diff_docs(*d[0], *d[2], Side::theirs, tol);
        auto bytes = [&](std::size_t k) { return d[k]->src.bytes(); };

        std::vector<Pending> pending;
        std::function<std::vector<std::byte>(const std::vector<Conflict>&)> write;
        const bool all_obj =
            std::all_of(d.begin(), d.end(), [](const Doc* x) { return x->kind == Kind::obj; });
        const bool all_mtl =
            std::all_of(d.begin(), d.end(), [](const Doc* x) { return x->kind == Kind::mtl; });
        const bool local = std::all_of(d.begin(), d.end(), [](const Doc* x) { return x->local; });
        std::optional<Side> whole; // an unchanged side yields the other side's file as is
        if (std::ranges::equal(bytes(0), bytes(1)))
            whole = Side::theirs;
        else if (std::ranges::equal(bytes(0), bytes(2)) || std::ranges::equal(bytes(1), bytes(2)))
            whole = Side::ours;

        if (whole) {
            write = [&, whole](const std::vector<Conflict>&) {
                auto b = bytes(ix(*whole));
                return std::vector<std::byte>(b.begin(), b.end());
            };
        } else if (all_obj && local) {
            write = merge_obj(d, pending);
        } else if (all_mtl) {
            write = merge_mtl(d, pending);
        } else {
            if (all_obj)
                res.warnings.push_back(
                    {"shared-pool", "/",
                     "faces reference vertices across objects; the geometry merges as one unit"});
            pending.push_back({{"/"}, "both-modified", false});
            write = [&](const std::vector<Conflict>& cs) {
                const auto take = take_for(cs.at(0).resolved_as, true, true, true).value_or(Side::ours);
                auto b = bytes(ix(take));
                return std::vector<std::byte>(b.begin(), b.end());
            };
        }

        std::set<std::string> in_conflict;
        for (auto& p : pending) {
            std::vector<std::string> ids;
            for (auto* cs : {&res.changes_ours, &res.changes_theirs})
                for (auto& c : *cs)
                    if (path_within(c.at.path, p.at.path))
                        ids.push_back(c.id), in_conflict.insert(c.id);
            auto c = make_conflict(p.at, std::move(ids), p.reason);
            if (p.mark)
                c.choices.push_back("mark");
            res.conflicts.push_back(std::move(c));
        }
        auto sig = [](const Change& c) {
            return std::string(to_string(c.op)) + " " + c.at.to_string() + " " +
                   (c.after.hash ? c.after.hash->hex() : c.after.text.value_or(""));
        };
        std::array<std::set<std::string>, 2> made;
        for (auto& c : res.changes_ours)
            made[0].insert(sig(c));
        for (auto& c : res.changes_theirs)
            made[1].insert(sig(c));
        for (auto [cs, other] :
             {std::pair{&res.changes_ours, &made[1]}, std::pair{&res.changes_theirs, &made[0]}})
            for (auto& c : *cs)
                if (!in_conflict.count(c.id))
                    res.auto_merged.push_back({c.id, other->count(sig(c)) ? "identical" : "independent"});
        apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);

        auto out = write(res.conflicts);
        try {
            res.document = parse(ByteSource::from_memory(std::move(out), d[1]->src.name()), nullptr);
        } catch (const Error& e) {
            throw Error(ErrorCategory::internal,
                        std::string("mesh writer produced an unloadable file: ") + e.what());
        }
        return res;
    }

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(cast(d).src.bytes());
    }

    void dump(const Document& doc, std::ostream& os, const DumpOptions&) const override {
        const Doc& d = cast(doc);
        os << "# mesh " << d.storage;
        if (d.kind == Kind::obj)
            os << (d.local ? " object-local" : " shared-pool");
        os << '\n';
        if (d.kind == Kind::obj) {
            for (auto& u : d.units) {
                const std::string path = unit_path(u.key);
                const Mesh m = unit_mesh(d, u);
                if (!m.v.empty())
                    os << '[' << path << "] " << summary(m) << '\n';
                for (auto& st : statements(d, u))
                    os << '[' << path << "] " << st << '\n';
            }
        } else if (d.kind == Kind::mtl) {
            for (auto& m : d.mats)
                for (auto& st : m.st)
                    if (auto t = stmt_tokens(d, st); !t.empty())
                        os << '[' << material_path(m.key) << "] " << join(t) << '\n';
        } else {
            for (auto& c : d.comments)
                os << c << '\n';
            for (auto& e : d.elements) {
                os << "element " << e.name << ' ' << element_decl(e) << '\n';
                if (!e.values.empty())
                    os << "[/elements/" << pointer_escape(e.name) << "] values=" << values_hash(e) << '\n';
            }
            if (d.kind == Kind::stl && d.storage == "stl ascii")
                os << "solid " << d.name << '\n';
            os << "[/geometry] " << summary(d.mesh) << '\n';
            if (std::any_of(d.attrs.begin(), d.attrs.end(), [](auto x) { return x != 0; })) {
                Hasher h;
                for (auto x : d.attrs)
                    h.add(std::uint64_t{x});
                os << "[/solid/attributes] xxh3:" << h.digest().hex().substr(0, 16) << '\n';
            }
        }
    }

    const EnvelopeMap* envelope(const Document& d) const override {
        return cast(d).has_map ? &cast(d).map : nullptr;
    }

    nlohmann::ordered_json inspect(const Document& doc) const override {
        const Doc& d = cast(doc);
        nlohmann::ordered_json o{{"storage", d.storage}};
        if (d.kind != Kind::mtl)
            o["vertices"] = d.mesh.v.size(), o["triangles"] = d.mesh.tri.size();
        if (d.kind == Kind::obj) {
            o["layout"] = d.local ? "object-local" : "shared-pool";
            auto& v = o["objects"] = nlohmann::ordered_json::array();
            for (auto& u : d.units)
                if (!u.key.empty())
                    v.push_back(u.key);
        } else if (d.kind == Kind::mtl) {
            auto& v = o["materials"] = nlohmann::ordered_json::array();
            for (auto& m : d.mats)
                if (!m.key.empty())
                    v.push_back(m.key);
        } else if (d.kind == Kind::ply) {
            o["comments"] = d.comments;
            auto& v = o["elements"] = nlohmann::ordered_json::array();
            for (auto& e : d.elements)
                v.push_back({{"name", e.name}, {"declaration", element_decl(e)}});
        } else {
            if (d.storage == "stl ascii")
                o["name"] = d.name;
            else
                o["header"] = header_text(d);
            o["color"] = std::any_of(d.attrs.begin(), d.attrs.end(), [](auto x) { return x != 0; });
        }
        return o;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json o;
        for (auto op : {Op::insert, Op::erase, Op::update, Op::move})
            o[std::string(to_string(op))] =
                std::count_if(cs.begin(), cs.end(), [&](const Change& c) { return c.op == op; });
        const Doc &da = cast(a), &db = cast(b);
        if (da.kind != Kind::mtl && db.kind != Kind::mtl) { // counts only: the distances are on the changes
            o["vertices"] = {da.mesh.v.size(), db.mesh.v.size()};
            o["triangles"] = {da.mesh.tri.size(), db.mesh.tri.size()};
        }
        return o;
    }

private:
    // OBJ, object-local on every side (§3, §6): KeyedMerge of objects by name; both sides changing one
    // object conflicts (MVP); every object is written with its face indices shifted to the new pools.
    static std::function<std::vector<std::byte>(const std::vector<Conflict>&)>
    merge_obj(const std::array<const Doc*, 3>& d, std::vector<Pending>& pending) {
        using KV = std::pair<std::string, Hash128>;
        std::array<std::vector<KV>, 3> l;
        for (std::size_t k = 0; k < 3; ++k)
            for (auto& u : d[k]->units)
                l[k].emplace_back(u.key, u.canon);
        struct Pick {
            std::array<std::optional<std::size_t>, 3> at;
            std::optional<Side> take;
            std::optional<std::size_t> conflict;
        };
        std::vector<Pick> picks;
        for (auto& r : keyed_merge<std::string, Hash128>(l[0], l[1], l[2])) {
            Pick p{{r.base, r.ours, r.theirs}, r.outcome.take, std::nullopt};
            if (r.outcome.rule == MergeRule::conflict) {
                p.conflict = pending.size();
                pending.push_back({{unit_path(r.key)}, r.outcome.reason, r.theirs && !r.key.empty()});
            }
            picks.push_back(p);
        }
        return [d, picks](const std::vector<Conflict>& cs) {
            TextOut out;
            out.eol = dominant_eol(*d[1]);
            auto put = [&](Side s, const Pick& p, bool rename = false) {
                if (auto i = p.at[ix(s)])
                    put_unit(out, *d[ix(s)], d[ix(s)]->units[*i], rename);
            };
            for (auto& p : picks) {
                if (!p.conflict) {
                    if (p.take)
                        put(*p.take, p);
                    continue;
                }
                const Conflict& c = cs[*p.conflict];
                if (c.resolved_as == "mark") {
                    put(Side::ours, p);
                    const auto& u = d[2]->units[*p.at[2]];
                    out.text("# splinter-conflict " + c.id + ": theirs' version of " + u.name + out.eol,
                             true);
                    put(Side::theirs, p, true);
                } else if (auto s = take_for(c.resolved_as, p.at[0].has_value(), p.at[1].has_value(),
                                             p.at[2].has_value())) {
                    put(*s, p);
                }
            }
            return out.finish(*d[1]);
        };
    }

    // MTL (§6): KeyedMerge per material, then per statement key inside materials both sides changed.
    static std::function<std::vector<std::byte>(const std::vector<Conflict>&)>
    merge_mtl(const std::array<const Doc*, 3>& d, std::vector<Pending>& pending) {
        using KV = std::pair<std::string, Hash128>;
        struct Pick {
            std::array<std::optional<std::size_t>, 3> at; // materials, or statements of `stmts`' material
            std::optional<Side> take;
            std::optional<std::size_t> conflict;
            std::vector<Pick> stmts; // a material merged per statement
            bool recursed = false;
        };
        std::array<std::vector<KV>, 3> l;
        for (std::size_t k = 0; k < 3; ++k)
            for (auto& m : d[k]->mats)
                l[k].emplace_back(m.key, material_hash(m));
        std::vector<Pick> picks;
        for (auto& r : keyed_merge<std::string, Hash128>(
                 l[0], l[1], l[2], std::equal_to<>{},
                 [](const std::string&, const Hash128*, const Hash128&, const Hash128&) { return true; })) {
            Pick p{{r.base, r.ours, r.theirs}, r.outcome.take, std::nullopt, {}, false};
            if (r.outcome.rule == MergeRule::conflict) {
                p.conflict = pending.size();
                pending.push_back({{material_path(r.key)}, r.outcome.reason, false});
            } else if (r.outcome.rule == MergeRule::recursed) {
                p.recursed = true;
                std::array<std::vector<KV>, 3> sl;
                for (std::size_t k = 0; k < 3; ++k)
                    if (p.at[k])
                        for (auto& st : d[k]->mats[*p.at[k]].st)
                            sl[k].emplace_back(st.key, st.h);
                for (auto& sr : keyed_merge<std::string, Hash128>(sl[0], sl[1], sl[2])) {
                    Pick sp{{sr.base, sr.ours, sr.theirs}, sr.outcome.take, std::nullopt, {}, false};
                    if (sr.outcome.rule == MergeRule::conflict) {
                        sp.conflict = pending.size();
                        pending.push_back({{material_path(r.key) + "/" + pointer_escape(sr.key)},
                                           sr.outcome.reason,
                                           false});
                    }
                    p.stmts.push_back(sp);
                }
            }
            picks.push_back(std::move(p));
        }
        return [d, picks](const std::vector<Conflict>& cs) {
            TextOut out;
            out.eol = dominant_eol(*d[1]);
            auto side_of = [&](const Pick& p) -> std::optional<Side> {
                if (!p.conflict)
                    return p.take;
                return take_for(cs[*p.conflict].resolved_as, p.at[0].has_value(), p.at[1].has_value(),
                                p.at[2].has_value());
            };
            auto put_stmt = [&](const Doc& x, const Stmt& st) { out.raw(x, st.b, st.end, st.end > st.e); };
            for (auto& p : picks) {
                if (!p.recursed) {
                    if (auto s = side_of(p))
                        for (auto& st : d[ix(*s)]->mats[*p.at[ix(*s)]].st)
                            put_stmt(*d[ix(*s)], st);
                    continue;
                }
                for (auto& sp : p.stmts)
                    if (auto s = side_of(sp)) {
                        const Doc& x = *d[ix(*s)];
                        put_stmt(x, x.mats[*p.at[ix(*s)]].st[*sp.at[ix(*s)]]);
                    }
            }
            return out.finish(*d[1]);
        };
    }
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<MeshFormat>();
}

} // namespace semsplinter::mesh
