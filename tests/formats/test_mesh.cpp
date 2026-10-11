#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>

#include "../../src/formats/geomcore/geomcore.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdio>
#include <sstream>

using namespace semsplinter;
using Catch::Approx;
using geom::Mesh;
using geom::Vec3;

namespace {

// ---- Generated meshes (mesh-simple.md §11) ----

// Side 2, centred, outward triangles: area 24, volume 8. Vertex i is at (x, y, z) = bits of i.
Mesh cube() {
    Mesh m;
    for (int i = 0; i < 8; ++i)
        m.v.push_back({i & 1 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 4 ? 1.0 : -1.0});
    m.tri = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
             {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    return m;
}

Mesh tetra() {
    Mesh m;
    m.v = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    m.tri = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
    return m;
}

Mesh displaced(Mesh m, std::initializer_list<std::uint32_t> vs, Vec3 d) {
    for (auto i : vs)
        for (int k = 0; k < 3; ++k)
            m.v[i][static_cast<std::size_t>(k)] += d[static_cast<std::size_t>(k)];
    return m;
}

std::string f6(double x) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", x);
    return b;
}

std::string obj(const std::vector<std::pair<std::string, Mesh>>& objects) {
    std::string o = "# generated\nmtllib scene.mtl\n";
    std::size_t base = 1;
    for (auto& [name, m] : objects) {
        o += "o " + name + "\n";
        for (auto& p : m.v)
            o += "v " + f6(p[0]) + " " + f6(p[1]) + " " + f6(p[2]) + "\n";
        o += "usemtl Wood\n";
        for (auto& t : m.tri)
            o += "f " + std::to_string(t[0] + base) + " " + std::to_string(t[1] + base) + " " +
                 std::to_string(t[2] + base) + "\n";
        base += m.v.size();
    }
    return o;
}

Vec3 normal(const Mesh& m, const geom::Tri& t) {
    const Vec3 &a = m.v[t[0]], &b = m.v[t[1]], &c = m.v[t[2]];
    const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    Vec3 n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (auto& x : n)
        x = l > 0 ? x / l : 0;
    return n;
}

void put(std::string& o, std::uint64_t v, int size, bool big) {
    for (int k = 0; k < size; ++k)
        o += static_cast<char>((v >> (8 * (big ? size - 1 - k : k))) & 0xFF);
}
void putf(std::string& o, double x, bool big = false) {
    put(o, std::bit_cast<std::uint32_t>(static_cast<float>(x)), 4, big);
}

std::string stl_ascii(const Mesh& m, bool normals = true) {
    std::string o = "solid part\n";
    for (auto& t : m.tri) {
        const Vec3 n = normals ? normal(m, t) : Vec3{0, 0, 0};
        o += "  facet normal " + f6(n[0]) + " " + f6(n[1]) + " " + f6(n[2]) + "\n    outer loop\n";
        for (auto i : t)
            o += "      vertex " + f6(m.v[i][0]) + " " + f6(m.v[i][1]) + " " + f6(m.v[i][2]) + "\n";
        o += "    endloop\n  endfacet\n";
    }
    return o + "endsolid part\n";
}

std::string stl_binary(const Mesh& m, std::string header = "splinter test", std::uint16_t attr = 0) {
    header.resize(80, ' ');
    std::string o = header;
    put(o, m.tri.size(), 4, false);
    for (auto& t : m.tri) {
        for (double x : normal(m, t))
            putf(o, x);
        for (auto i : t)
            for (double x : m.v[i])
                putf(o, x);
        put(o, attr, 2, false);
    }
    return o;
}

std::string ply(const Mesh& m, std::string_view format, std::string comment = "generated") {
    std::string o = "ply\nformat " + std::string(format) + " 1.0\ncomment " + comment + "\nelement vertex " +
                    std::to_string(m.v.size()) +
                    "\nproperty float x\nproperty float y\nproperty float z\nelement face " +
                    std::to_string(m.tri.size()) + "\nproperty list uchar int vertex_indices\nend_header\n";
    const bool big = format == "binary_big_endian";
    for (auto& p : m.v)
        if (format == "ascii")
            o += f6(p[0]) + " " + f6(p[1]) + " " + f6(p[2]) + "\n";
        else
            for (double x : p)
                putf(o, x, big);
    for (auto& t : m.tri)
        if (format == "ascii") {
            o += "3 " + std::to_string(t[0]) + " " + std::to_string(t[1]) + " " + std::to_string(t[2]) + "\n";
        } else {
            put(o, 3, 1, big);
            for (auto i : t)
                put(o, i, 4, big);
        }
    return o;
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& mesh = *reg.find("mesh");

    std::unique_ptr<Document> load(std::string_view s, std::string name = "x.obj") {
        auto b = as_bytes(s);
        return mesh.load(ByteSource::from_memory({b.begin(), b.end()}, std::move(name)), {ctx});
    }
    ChangeSet diff(std::string_view a, std::string_view b, std::string name = "x.obj") {
        return mesh.diff(*load(a, name), *load(b, name), {ctx});
    }
    MergeResult merge(std::string_view b, std::string_view o, std::string_view t, std::string name = "x.obj",
                      ConflictOptions c = {}) {
        auto db = load(b, name), dob = load(o, name), dt = load(t, name);
        return mesh.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::string text(const Document& d) {
        VectorSink s;
        mesh.save(d, s, {ctx});
        return {reinterpret_cast<const char*>(s.data.data()), s.data.size()};
    }
    std::string dump(std::string_view s, std::string name = "x.obj") {
        std::ostringstream os;
        mesh.dump(*load(s, name), os, {ctx});
        return os.str();
    }
    std::string detect(std::string_view s, std::string_view name) {
        return std::string(reg.detect(as_bytes(s), name).info().name);
    }
};

const Change* at(const ChangeSet& cs, std::string_view path) {
    for (auto& c : cs)
        if (c.at.to_string() == path)
            return &c;
    return nullptr;
}

// Two object-local objects; B also uses normals and negative indices.
constexpr std::string_view kBase = "# test\n"
                                   "mtllib m.mtl\n"
                                   "o A\n"
                                   "v 0 0 0\n"
                                   "v 1 0 0\n"
                                   "v 0 1 0\n"
                                   "f 1 2 3\n"
                                   "o B\n"
                                   "v 0 0 1\n"
                                   "v 1 0 1\n"
                                   "v 0 1 1\n"
                                   "vn 0 0 1\n"
                                   "f 4//1 5//1 6//1\n"
                                   "f -3//-1 -2//-1 -1//-1\n";

std::string replace(std::string s, std::string_view from, std::string_view to) {
    auto p = s.find(from);
    REQUIRE(p != std::string::npos);
    return s.replace(p, from.size(), to);
}

} // namespace

TEST_CASE("geomcore: area, volume, edges, distance, displacement") {
    const Mesh c = cube();
    CHECK(geom::area(c) == Approx(24));
    CHECK(geom::volume(c) == Approx(8));
    CHECK(geom::volume(tetra()) == Approx(1.0 / 6));
    auto b = geom::bbox(c);
    CHECK(b.lo == Vec3{-1, -1, -1});
    CHECK(b.hi == Vec3{1, 1, 1});
    auto e = geom::edges(c);
    CHECK(e.edges == 18);
    CHECK(e.boundary == 0);
    CHECK(e.nonmanifold == 0);
    Mesh open = c;
    open.tri.pop_back();
    CHECK(geom::edges(open).boundary == 3);

    // Hausdorff: zero for identical meshes, d for a translation by d, symmetric.
    auto same = geom::distance(c, c);
    REQUIRE(same);
    CHECK(same->hausdorff == 0);
    const Mesh moved = displaced(c, {0, 1, 2, 3, 4, 5, 6, 7}, {0.5, 0, 0});
    auto d = geom::distance(c, moved);
    REQUIRE(d);
    CHECK(d->hausdorff == Approx(0.5));
    CHECK(geom::distance(moved, c)->hausdorff == Approx(d->hausdorff));
    CHECK_FALSE(d->sampled);
    CHECK_FALSE(geom::distance(c, Mesh{}));
    Mesh pts;
    pts.v = {{0, 0, 0}, {3, 4, 0}};
    Mesh pt;
    pt.v = {{0, 0, 0}};
    CHECK(geom::distance(pts, pt)->hausdorff == Approx(5)); // point clouds: point to point

    // Displacement regions: connected components of moved vertices over the triangle edges.
    auto two = geom::displacement(c, displaced(c, {0, 7}, {0, 0, 0.25}), 0);
    REQUIRE(two);
    CHECK(two->moved == 2);
    CHECK(two->regions.size() == 2);
    CHECK(two->max == Approx(0.25));
    auto one = geom::displacement(c, displaced(c, {0, 3}, {0, 0, 0.25}), 0); // 0-3 is an edge
    REQUIRE(one);
    CHECK(one->regions.size() == 1);
    CHECK(one->regions[0].vertices == 2);
    CHECK(geom::displacement(c, displaced(c, {0}, {0, 0, 1e-9}), 1e-6)->moved == 0);
    CHECK_FALSE(geom::displacement(c, tetra(), 0));
}

TEST_CASE("mesh: detection, round trip, and identity merges for every encoding") {
    Fx fx;
    const Mesh c = cube();
    const std::vector<std::pair<std::string, std::string>> files = {
        {obj({{"Cube", c}, {"Tet", tetra()}}), "a.obj"},
        {"newmtl Wood\nKd 0.8 0.6 0.4\nNs 10\n", "a.mtl"},
        {ply(c, "ascii"), "a.ply"},
        {ply(c, "binary_little_endian"), "a.ply"},
        {ply(c, "binary_big_endian"), "a.ply"},
        {stl_ascii(c), "a.stl"},
        {stl_binary(c), "a.stl"},
        {std::string(kBase), "a.obj"}};
    for (auto& [s, name] : files) {
        INFO(name);
        CHECK(fx.detect(s, name) == "mesh");
        CHECK(fx.detect(s, "noext") == "mesh"); // content probe
        auto d = fx.load(s, name);
        CHECK(fx.text(*d) == s);
        auto r = fx.merge(s, s, s, name);
        CHECK(r.conflicts.empty());
        CHECK(fx.text(*r.document) == s); // a merge without changes is byte-identical
        CHECK(fx.diff(s, s, name).empty());
    }
    CHECK(fx.detect("Hello world.\nv is a letter.\n", "notes.txt") == "text");
    CHECK(fx.mesh.inspect(*fx.load(stl_binary(c), "a.stl"))["storage"] == "stl binary");
    CHECK(fx.mesh.inspect(*fx.load(ply(c, "binary_big_endian"), "a.ply"))["vertices"] == 8);
    CHECK(fx.mesh.inspect(*fx.load("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nf 1 2 \\\n4 3\n"))["triangles"] == 2);
    CHECK(fx.mesh.inspect(*fx.load(kBase))["layout"] == "object-local");
    CHECK(fx.mesh.inspect(*fx.load(kBase))["objects"] == nlohmann::json::array({"A", "B"}));
}

TEST_CASE("mesh: framing errors are invalid input, counts bounded before allocation") {
    Fx fx;
    const Mesh c = cube();
    auto le = ply(c, "binary_little_endian");
    CHECK_THROWS_AS(fx.load(le.substr(0, le.size() - 1), "a.ply"), Error);                    // truncated
    CHECK_THROWS_AS(fx.load(le + "x", "a.ply"), Error);                                       // extra bytes
    CHECK_THROWS_AS(fx.load(replace(le, "vertex 8", "vertex 999999999999"), "a.ply"), Error); // huge count
    CHECK_THROWS_AS(fx.load(replace(ply(c, "ascii"), "3 0 2 3", "3 0 2 9"), "a.ply"), Error); // index range
    CHECK_THROWS_AS(fx.load(replace(ply(c, "ascii"), "format ascii", "format ebcdic"), "a.ply"), Error);
    CHECK_THROWS_AS(fx.load("v 0 0 0\nf 1 2 3\n"), Error); // index range
    CHECK_THROWS_AS(fx.load("v 0 0 0\nf 0 1 1\n"), Error); // zero index
    CHECK_THROWS_AS(fx.load(replace(stl_ascii(c), "endloop", "endlop"), "a.stl"), Error);
    CHECK_THROWS_AS(fx.load(stl_binary(c).substr(0, 200), "a.stl"), Error); // the size equation fails
    // A binary STL whose header starts with "solid": the size equation decides (§2).
    CHECK(fx.mesh.inspect(*fx.load(stl_binary(c, "solid fooled you"), "a.stl"))["storage"] == "stl binary");
}

TEST_CASE("mesh: ASCII against binary, and endianness, are encoding changes only") {
    Fx fx;
    const Mesh c = cube();
    auto check_encoding_only = [&](const std::string& a, const std::string& b, const char* name) {
        auto cmp = fx.mesh.compare(*fx.load(a, name), *fx.load(b, name), {fx.ctx});
        CHECK_FALSE(cmp->differs());
        auto cs = cmp->changes();
        REQUIRE(cs.size() == 1);
        CHECK(cs[0].kind == "encoding");
    };
    check_encoding_only(ply(c, "ascii"), ply(c, "binary_little_endian"), "a.ply");
    check_encoding_only(ply(c, "binary_little_endian"), ply(c, "binary_big_endian"), "a.ply");
    check_encoding_only(stl_ascii(c), stl_binary(c), "a.stl");
    check_encoding_only(stl_ascii(c, false), stl_ascii(c, true), "a.stl"); // normals are derived

    // OBJ number spelling: compared as floats, within --tolerance.
    const std::string a = "o A\nv 1.000000 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\n";
    auto cs = fx.diff(a, replace(a, "1.000000", "1.0"));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].kind == "encoding");
    const std::string b = replace(a, "1.000000", "1.0000004");
    CHECK(fx.diff(a, b)[0].kind == "geometry");
    fx.opts["tolerance"] = "1e-6";
    CHECK(fx.diff(a, b)[0].kind == "encoding");
    CHECK(fx.diff(a, replace(a, "f 1 2 3", "f 1 2 3 # note"))[0].kind == "comment");
}

TEST_CASE("mesh: geometry diff reports metrics and displacement regions") {
    Fx fx;
    const Mesh c = cube();
    for (auto [enc, name] : {std::pair{"stl", "a.stl"}, std::pair{"ply", "a.ply"}}) {
        INFO(enc);
        auto make = [&, enc = std::string(enc)](const Mesh& m) {
            return enc == "stl" ? stl_binary(m) : ply(m, "binary_little_endian");
        };
        auto cs = fx.diff(make(c), make(displaced(c, {0, 7}, {0, 0, -0.5})), name);
        const Change* g = at(cs, "/geometry");
        REQUIRE(g);
        CHECK(g->kind == "geometry");
        CHECK(g->metrics["moved"] == 2);
        CHECK(g->metrics["regions"] == 2);
        CHECK(g->metrics["max_displacement"] == 0.5);
        CHECK(g->metrics["hausdorff"].get<double>() > 0);
        CHECK(g->metrics["vertices"] == nlohmann::json::array({8, 8}));
    }
    // A different mesh: no displacement (topology differs), Hausdorff and counts still reported.
    auto cs = fx.diff(stl_binary(c), stl_binary(tetra()), "a.stl");
    const Change* g = at(cs, "/geometry");
    REQUIRE(g);
    CHECK_FALSE(g->metrics.contains("moved"));
    CHECK(g->metrics["triangles"] == nlohmann::json::array({12, 4}));
    // Converting formats is a storage change, plus geometry if it differs.
    cs = fx.diff(stl_binary(c), stl_binary(c, "other generator"), "a.stl");
    REQUIRE(at(cs, "/header"));
    CHECK(at(cs, "/header")->kind == "volatile");
    auto cross = fx.mesh.diff(*fx.load(obj({{"Cube", c}})), *fx.load(stl_binary(c), "a.stl"), {fx.ctx});
    REQUIRE(at(cross, "/storage"));
    CHECK(at(cross, "/storage")->kind == "storage");
}

TEST_CASE("mesh: OBJ objects merge with face indices shifted") {
    Fx fx;
    // Ours grows A by one vertex and face (so B's positive indices shift by one there, its negative ones
    // stay); theirs moves a vertex of B. The merge writes theirs' B at ours' indices.
    const std::string ours = replace(replace(std::string(kBase), "f 1 2 3\n", "v 1 1 0\nf 1 2 3\nf 2 4 3\n"),
                                     "f 4//1 5//1 6//1", "f 5//1 6//1 7//1");
    const std::string theirs = replace(std::string(kBase), "v 0 1 1", "v 0 2 1");
    auto r = fx.merge(kBase, ours, theirs);
    CHECK(r.conflicts.empty());
    CHECK(fx.text(*r.document) == replace(ours, "v 0 1 1", "v 0 2 1"));
    // Theirs grows A instead: ours' edit to B is shifted on the way out.
    // CRLF without a final newline: an object appended after it gets ours' terminator first.
    const std::string crlf = "o A\r\nv 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3";
    auto m =
        fx.merge(crlf, replace(crlf, "v 1 0 0", "v 2 0 0"), crlf + "\r\no B\r\nv 0 0 1\r\nf -1 -1 -1\r\n");
    CHECK(m.conflicts.empty());
    CHECK(fx.text(*m.document) ==
          "o A\r\nv 0 0 0\r\nv 2 0 0\r\nv 0 1 0\r\nf 1 2 3\r\no B\r\nv 0 0 1\r\nf -1 -1 -1\r\n");
    auto swapped = fx.merge(kBase, theirs, ours);
    CHECK(swapped.conflicts.empty());
    CHECK(fx.text(*swapped.document) == replace(ours, "v 0 1 1", "v 0 2 1"));
    REQUIRE(at(r.changes_ours, "/objects/name:A"));
    REQUIRE(at(r.changes_theirs, "/objects/name:B"));
    CHECK(at(r.changes_theirs, "/objects/name:B")->kind == "geometry");
    // The shift alone is not a change of B.
    CHECK_FALSE(at(fx.diff(kBase, ours), "/objects/name:B"));

    // Objects added on both sides, and deleted.
    const std::string add_c = std::string(kBase) + "o C\nv 5 5 5\nv 6 5 5\nv 5 6 5\nf 7 8 9\n";
    const std::string del_a = "# test\nmtllib m.mtl\no B\nv 0 0 1\nv 1 0 1\nv 0 1 1\nvn 0 0 1\n"
                              "f 1//1 2//1 3//1\nf -3//-1 -2//-1 -1//-1\n";
    r = fx.merge(kBase, add_c, del_a);
    CHECK(r.conflicts.empty());
    CHECK(fx.text(*r.document) ==
          "# test\nmtllib m.mtl\no B\nv 0 0 1\nv 1 0 1\nv 0 1 1\nvn 0 0 1\n"
          "f 1//1 2//1 3//1\nf -3//-1 -2//-1 -1//-1\no C\nv 5 5 5\nv 6 5 5\nv 5 6 5\nf 4 5 6\n");
    CHECK(fx.mesh.inspect(*r.document)["objects"] == nlohmann::json::array({"B", "C"}));
}

TEST_CASE("mesh: OBJ conflicts, marking, and shared pools") {
    Fx fx;
    const std::string ours = replace(std::string(kBase), "v 1 0 0", "v 2 0 0");
    const std::string theirs = replace(std::string(kBase), "v 1 0 0", "v 3 0 0");
    auto r = fx.merge(kBase, ours, theirs);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.to_string() == "/objects/name:A");
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].changes.size() == 2);
    CHECK(fx.text(*r.document) == ours);

    ConflictOptions take_theirs;
    take_theirs.policy = Policy::theirs;
    CHECK(fx.text(*fx.merge(kBase, ours, theirs, "x.obj", take_theirs).document) == theirs);

    ConflictOptions mark;
    mark.policy = Policy::mark;
    r = fx.merge(kBase, ours, theirs, "x.obj", mark);
    const std::string marked = fx.text(*r.document);
    CHECK(marked.find("o A.splinter_theirs\nv 0 0 0\nv 3 0 0\nv 0 1 0\nf 4 5 6\n") != std::string::npos);
    CHECK(marked.find("# splinter-conflict " + r.conflicts[0].id) != std::string::npos);
    CHECK(marked.find("o B\nv 0 0 1\nv 1 0 1\nv 0 1 1\nvn 0 0 1\nf 7//1 8//1 9//1\n") != std::string::npos);
    CHECK(fx.mesh.inspect(*r.document)["objects"] == nlohmann::json::array({"A", "A.splinter_theirs", "B"}));

    // Faces reaching into another object's vertices: the geometry is one unit (Q-MESH-1).
    const std::string shared = "o A\nv 0 0 0\nv 1 0 0\nv 0 1 0\no B\nv 0 0 1\nf 1 2 4\n";
    CHECK(fx.mesh.inspect(*fx.load(shared))["layout"] == "shared-pool");
    const std::string s1 = replace(shared, "v 1 0 0", "v 2 0 0"), s2 = replace(shared, "v 0 0 1", "v 0 0 2");
    r = fx.merge(shared, s1, s2);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.to_string() == "/");
    CHECK(r.warnings.size() == 1);
    CHECK(fx.text(*fx.merge(shared, s1, shared).document) == s1);
}

TEST_CASE("mesh: MTL merges per material and statement") {
    Fx fx;
    const std::string base =
        "# materials\nnewmtl Wood\nKd 0.8 0.6 0.4\nNs 10\n\nnewmtl Glass\nKd 1 1 1\nd 0.2\n";
    const std::string ours = replace(base, "Kd 0.8 0.6 0.4", "Kd 0.7 0.5 0.3");
    const std::string theirs = replace(base, "Ns 10", "Ns 20") + "\nnewmtl Metal\nKs 1 1 1\n";
    auto r = fx.merge(base, ours, theirs, "m.mtl");
    CHECK(r.conflicts.empty());
    CHECK(fx.text(*r.document) == replace(ours, "Ns 10", "Ns 20") + "\nnewmtl Metal\nKs 1 1 1\n");
    const Change* kd = at(r.changes_ours, "/materials/name:Wood/Kd");
    REQUIRE(kd);
    CHECK(*kd->after.text == "Kd 0.7 0.5 0.3");
    CHECK(at(r.changes_theirs, "/materials/name:Metal"));

    r = fx.merge(base, ours, replace(base, "Kd 0.8 0.6 0.4", "Kd 0 0 0"), "m.mtl");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.to_string() == "/materials/name:Wood/Kd");
    CHECK(fx.text(*r.document) == ours);

    CHECK(fx.dump(base, "m.mtl") ==
          "# mesh mtl\n"
          "[/materials/name:Wood] newmtl Wood\n[/materials/name:Wood] Kd 0.8 0.6 0.4\n"
          "[/materials/name:Wood] Ns 10\n[/materials/name:Glass] newmtl Glass\n"
          "[/materials/name:Glass] Kd 1 1 1\n[/materials/name:Glass] d 0.2\n");
}

TEST_CASE("mesh: PLY and STL merge as one unit") {
    Fx fx;
    const Mesh c = cube();
    const auto base = stl_binary(c), ours = stl_binary(displaced(c, {0}, {0, 0, -1})),
               theirs = stl_binary(displaced(c, {7}, {0, 0, 1}));
    CHECK(fx.text(*fx.merge(base, base, theirs, "a.stl").document) == theirs);
    auto r = fx.merge(base, ours, theirs, "a.stl");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.to_string() == "/");
    CHECK(fx.text(*r.document) == ours);
    const auto p = ply(c, "ascii"), pc = ply(c, "ascii", "edited");
    r = fx.merge(p, pc, p, "a.ply");
    CHECK(r.conflicts.empty());
    CHECK(fx.text(*r.document) == pc);
    CHECK(at(r.changes_ours, "/header/comments"));
}

TEST_CASE("mesh: envelope map and churn of binary STL") {
    Fx fx;
    const Mesh c = cube();
    const auto a = stl_binary(c), b = stl_binary(displaced(c, {0}, {0, 0, -0.5}));
    auto da = fx.load(a, "a.stl"), db = fx.load(b, "b.stl");
    const EnvelopeMap* ma = fx.mesh.envelope(*da);
    REQUIRE(ma);
    ma->validate(a.size());
    CHECK(ma->ranges[0].cls == ByteClass::volatile_);
    CHECK(ma->ranges[1].cls == ByteClass::framing);
    auto churn = attribute_churn(as_bytes(a), *ma, as_bytes(b), *fx.mesh.envelope(*db));
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::content)] > 0);
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::derived)] > 0); // the normals moved
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::framing)] == 0);
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::unexplained)] == 0);
    const auto p = ply(c, "binary_big_endian");
    auto dp = fx.load(p, "a.ply");
    fx.mesh.envelope(*dp)->validate(p.size());
}

TEST_CASE("mesh: dump is a stable per-object summary") {
    Fx fx;
    const std::string d = fx.dump(obj({{"Cube", cube()}}));
    CHECK(
        d.starts_with("# mesh obj object-local\n[/header] mtllib scene.mtl\n"
                      "[/objects/name:Cube] vertices=8 triangles=12 bbox=-1,-1,-1..1,1,1 area=24 volume=8 "));
    CHECK(d.find("[/objects/name:Cube] usemtl Wood\n") != std::string::npos);
    // The same triangles give the same geometry line in every encoding.
    auto geometry = [](const std::string& dump) {
        auto p = dump.find("[/geometry]");
        return dump.substr(p, dump.find('\n', p) - p);
    };
    const auto g = geometry(fx.dump(stl_binary(cube()), "a.stl"));
    CHECK(geometry(fx.dump(stl_ascii(cube()), "a.stl")) == g);
}
