#include "../cli/console.hpp"
#include "../scratch.hpp"
#include "raster_util.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/ifd.hpp>
#include <semsplinter/formats/tiff.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <fstream>

using namespace rtest;

namespace {

struct Scratch {
    std::filesystem::path dir;
    explicit Scratch(const std::string& name) : dir(scratch_path(name)) {
        std::filesystem::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string path(const std::string& name) const { return splinter::cli::from_path(dir / name); }
    std::string file(const std::string& name, const std::vector<std::byte>& b) const {
        std::ofstream(dir / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return path(name);
    }
    std::vector<std::byte> bytes(const std::string& name) const {
        std::ifstream f(dir / name, std::ios::binary);
        std::string s{std::istreambuf_iterator<char>(f), {}};
        return {reinterpret_cast<const std::byte*>(s.data()),
                reinterpret_cast<const std::byte*>(s.data()) + s.size()};
    }
};

tiff::Encoding::Tag ascii(std::uint16_t tag, std::string_view s) {
    std::vector<std::byte> v(reinterpret_cast<const std::byte*>(s.data()),
                             reinterpret_cast<const std::byte*>(s.data()) + s.size());
    v.push_back(std::byte{0}); // NUL-terminated
    return {tag, 2, v.size(), v};
}

tiff::Encoding::Tag shorts(std::uint16_t tag, std::vector<std::uint16_t> s) {
    std::vector<std::byte> v;
    for (auto x : s)
        v.push_back(std::byte(x & 0xFF)), v.push_back(std::byte(x >> 8));
    return {tag, 3, s.size(), v};
}

tiff::Encoding::Tag doubles(std::uint16_t tag, std::vector<double> d) {
    std::vector<std::byte> v(d.size() * 8);
    std::memcpy(v.data(), d.data(), v.size());
    return {tag, 12, d.size(), v};
}

Raster wide_pattern() {
    Raster r(20, 10, true);
    for (std::uint64_t i = 0; i < r.cells(); ++i)
        r.set(i, {static_cast<std::uint16_t>(i * 331), static_cast<std::uint16_t>(i * 17), 7, 0xFFFF});
    return r;
}

} // namespace

TEST_CASE("tiff: one image in every compression, predictor and layout decodes the same") {
    Fx fx("tiff");
    const auto r = pattern(33, 21);
    std::vector<tiff::Encoding> encs;
    for (std::uint16_t c : std::initializer_list<std::uint16_t>{1, 5, 8, 32773})
        for (std::uint16_t pred : std::initializer_list<std::uint16_t>{1, 2})
            for (bool big : {false, true})
                encs.push_back({c, pred, 8, 2, 5, big, big});
    const auto ref = pixel_lines(fx.dump(tiff::write({r}, encs[0])));
    for (auto& e : encs) {
        INFO("compression " << e.compression << " predictor " << e.predictor << " big " << e.big_endian);
        auto b = tiff::write({r}, e);
        CHECK(pixel_lines(fx.dump(b)) == ref);
        CHECK(only_encoding(fx.diff(tiff::write({r}, encs[0]), b)));
    }
    // Gray at 1..8 bits, palette, alpha, and 16-bit samples.
    auto g = image(
        13, 9, [](std::uint32_t x, std::uint32_t) { return x % 2 ? rgba(255, 255, 255) : rgba(0, 0, 0); });
    const auto gref = pixel_lines(fx.dump(tiff::write({g}, {1, 1, 8, 1})));
    for (std::uint16_t bits : std::initializer_list<std::uint16_t>{1, 2, 4})
        CHECK(pixel_lines(fx.dump(tiff::write({g}, {5, 1, bits, 1}))) == gref);
    CHECK(pixel_lines(fx.dump(tiff::write({blocks()}, {5, 1, 4, 3}))) ==
          pixel_lines(fx.dump(tiff::write({blocks()}, {}))));
    auto a = pattern();
    paint(a, 0, 0, 3, 3, rgba(1, 2, 3, 99));
    CHECK(fx.dump(tiff::write({a}, {8, 2, 8, 2})).find("alpha=varies") != std::string::npos);
    // One uncompressed strip larger than 8 KiB: libtiff must not chop it into virtual strips.
    auto large = pattern(100, 40);
    CHECK(pixel_lines(fx.dump(tiff::write({large}, {1, 1, 8, 2, 40}))) ==
          pixel_lines(fx.dump(tiff::write({large}, {5, 2, 8, 2}))));
    const auto w = wide_pattern();
    std::ostringstream wref;
    raster::dump_tiles(w, wref);
    for (bool big : {false, true})
        CHECK(pixel_lines(fx.dump(tiff::write({w}, {5, 2, 16, 2, 0, big}))) == wref.str());
}

TEST_CASE("tiff: stable layout keeps ours' bytes and unchanged strips in place") {
    Fx fx("tiff");
    tiff::Encoding e{5, 2, 8, 2, 4};
    e.tags = {ascii(305, "ed 1.0"), ascii(306, "2024:01:01 00:00:00")};
    auto base = pattern(32, 32);
    auto ours = base, theirs = base;
    paint(ours, 1, 1, 2, 2, rgba(255, 0, 0));     // strip 0
    paint(theirs, 20, 29, 2, 2, rgba(0, 0, 255)); // strip 7
    auto bb = tiff::write({base}, e), ob = tiff::write({ours}, e);
    auto te = e;
    te.tags = {ascii(305, "ed 2.0"), ascii(306, "2025:06:01 00:00:00")};
    auto tb = tiff::write({theirs}, te);
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto out = fx.bytes(m);
    // Ours' bytes are a prefix, apart from the IFD0 pointer.
    REQUIRE(out.size() > ob.size());
    CHECK(std::equal(ob.begin() + 8, ob.end(), out.begin() + 8));
    auto to = ifd::parse(ob), tm = ifd::parse(out);
    auto so = to.numbers(*to.chain[0].find(273)), sm = tm.numbers(*tm.chain[0].find(273));
    REQUIRE(sm.size() == 8);
    for (std::size_t s = 0; s < 7; ++s)
        CHECK(sm[s] == so[s]); // only strip 7 (theirs' edit) is new
    CHECK(sm[7] >= ob.size());
    auto d = fx.dump(out);
    CHECK(d.find("key /tags/0x0131 \"ed 2.0\"") != std::string::npos);
    CHECK(d.find("key /tags/0x0132 \"2025:06:01 00:00:00\"") != std::string::npos);
    auto both = ours;
    paint(both, 20, 29, 2, 2, rgba(0, 0, 255));
    CHECK(pixel_lines(d) == pixel_lines(fx.dump(tiff::write({both}, e))));
    CHECK(fx.bytes(fx.merge(bb, bb, tb)) == tb);
}

TEST_CASE("tiff: pages merge positionally; inserted pages merge as a sequence") {
    Fx fx("tiff");
    tiff::Encoding e{8, 2, 8, 2};
    std::vector<Raster> base{pattern(), blocks(), pattern(30, 20)};
    auto ours = base, theirs = base;
    paint(ours[0], 0, 0, 4, 4, rgba(9, 9, 9));
    paint(theirs[2], 10, 10, 4, 4, rgba(8, 8, 8));
    auto bb = tiff::write(base, e);
    auto m = fx.merge(bb, tiff::write(ours, e), tiff::write(theirs, e));
    CHECK(m.conflicts.empty());
    auto want = ours;
    want[2] = theirs[2];
    CHECK(pixel_lines(fx.dump(fx.bytes(m))) == pixel_lines(fx.dump(tiff::write(want, e))));
    auto more = base;
    more.push_back(blocks(10, 10));
    auto diff = fx.diff(bb, tiff::write(more, e));
    REQUIRE(diff.size() == 1);
    CHECK(diff[0].at.path == "/pages/3");
    auto m2 = fx.merge(bb, tiff::write(ours, e), tiff::write(more, e));
    CHECK(m2.conflicts.empty());
    CHECK(fx.dump(fx.bytes(m2)).find("pages=4") != std::string::npos);
}

TEST_CASE("tiff: GeoTIFF keys merge as keys") {
    Fx fx("tiff");
    auto geo = [](std::uint16_t epsg, double scale) {
        tiff::Encoding e{5, 1, 8, 2};
        // GTModelType=1 (projected), ProjectedCSType=epsg, ProjLinearUnits in the double table.
        e.tags = {shorts(34735, {1, 1, 0, 3, 1024, 0, 1, 1, 3072, 0, 1, epsg, 3076, 34736, 1, 0}),
                  doubles(34736, {scale}), doubles(33550, {1.0, 1.0, 0.0})};
        return e;
    };
    auto base = pattern();
    auto bb = tiff::write({base}, geo(32633, 1.0));
    auto ob = tiff::write({base}, geo(32634, 1.0));
    auto painted = base;
    paint(painted, 5, 5, 2, 2, rgba(1, 1, 1));
    auto tb = tiff::write({painted}, geo(32633, 0.3048));
    auto cs = fx.diff(bb, ob);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/pages/0/geo/3072");
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto d = fx.dump(fx.bytes(m));
    CHECK(d.find("key /geo/3072 32634") != std::string::npos);
    CHECK(d.find("key /geo/3076 0.30480000000000002") != std::string::npos);
}

TEST_CASE("tiff: undecodable pages merge whole; framing errors are rejected") {
    Fx fx("tiff");
    tiff::Encoding e{1, 1, 8, 1};
    e.tags = {shorts(339, {3})}; // SampleFormat IEEE float on 8-bit samples: not canonical, opaque
    auto g = image(8, 8, [](std::uint32_t x, std::uint32_t) { return rgba(int(x), int(x), int(x)); });
    auto g2 = g;
    paint(g2, 0, 0, 1, 1, rgba(200, 200, 200));
    auto bb = tiff::write({g}, e), ob = tiff::write({g2}, e);
    CHECK(fx.dump(bb).find("coded xxh3=") != std::string::npos);
    auto m = fx.merge(bb, ob, bb);
    CHECK(fx.bytes(m) == ob);
    auto c = fx.merge(
        bb, ob, tiff::write({image(8, 8, [](std::uint32_t, std::uint32_t) { return rgba(5, 5, 5); })}, e));
    REQUIRE(c.conflicts.size() == 1);
    CHECK(c.conflicts[0].at.path == "/pages/0/pixels");

    auto b = tiff::write({pattern()});
    CHECK(fx.f.probe(b, "").confidence == 100);
    auto cut = b;
    cut.resize(50);
    CHECK_THROWS_AS(fx.load(cut), Error);
}

TEST_CASE("tiff: --tiff-ignore-tags hides tags from the CLI diff; --reencode is a merge option") {
    Scratch s("tiff-cli");
    tiff::Encoding a{5, 1, 8, 2}, b = a;
    a.tags = {ascii(305, "one")}, b.tags = {ascii(305, "two")};
    auto pa = s.file("a.tif", tiff::write({pattern()}, a)), pb = s.file("b.tif", tiff::write({pattern()}, b));
    CaptureConsole c1, c2, c3;
    CHECK(run_cli(c1, {"diff", pa, pb}) == 1);
    CHECK(c1.out_text.find("/pages/0/tags/0x0131") != std::string::npos);
    CHECK(run_cli(c2, {"diff", "--tiff-ignore-tags=0x0131", pa, pb}) == 0);
    CHECK(run_cli(c3, {"merge", "--reencode", "-o", s.path("m.tif"), pa, pa, pb}) == 0);
    CHECK(s.bytes("m.tif") == tiff::write({pattern()}, b));
}
