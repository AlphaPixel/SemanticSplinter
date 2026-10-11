#include "raster_util.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/qoi.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace rtest;

TEST_CASE("qoi: round trip through every op, 3 and 4 channels") {
    Fx fx("qoi");
    auto r = image(37, 23, [](std::uint32_t x, std::uint32_t y) {
        // runs, small diffs, luma steps, big jumps, and repeated colors (index)
        if (y % 5 == 0)
            return rgba(10, 20, 30);
        if (y % 5 == 1)
            return rgba(static_cast<int>(x % 3), static_cast<int>(x % 2), 7);
        if (y % 5 == 2)
            return rgba(static_cast<int>(x * 7 % 256), static_cast<int>(x * 5 % 256),
                        static_cast<int>(x * 6 % 256));
        if (y % 5 == 3)
            return rgba(static_cast<int>(x * 61 % 256), static_cast<int>(y * 37 % 256), 3);
        return rgba(static_cast<int>(x % 4 * 60), 0, 0, static_cast<int>(x * 9 % 256));
    });
    auto b4 = qoi::write(r, 4);
    auto d = fx.load(b4);
    auto opaque = r;
    for (std::uint64_t i = 0; i < opaque.cells(); ++i) {
        auto c = opaque.get(i);
        c[3] = 0xFFFF;
        opaque.set(i, c);
    }
    std::ostringstream ref;
    raster::dump_tiles(r, ref);
    CHECK(pixel_lines(fx.dump(b4)) == ref.str());
    // 3 channels, opaque image: same pixels; with alpha the writer promotes to 4.
    auto b3 = qoi::write(opaque, 3);
    CHECK(std::to_integer<int>(b3[12]) == 3);
    CHECK(only_encoding(fx.diff(qoi::write(opaque, 4), b3)));
    CHECK(std::to_integer<int>(qoi::write(r, 3)[12]) == 4);
}

TEST_CASE("qoi: merge, passthrough, colorspace dependency") {
    Fx fx("qoi");
    auto base = pattern();
    auto ours = base, theirs = base;
    paint(ours, 1, 1, 3, 3, rgba(255, 0, 0));
    paint(theirs, 30, 20, 3, 3, rgba(0, 0, 255));
    auto bb = qoi::write(base), ob = qoi::write(ours), tb = qoi::write(theirs);
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    paint(ours, 30, 20, 3, 3, rgba(0, 0, 255));
    CHECK(fx.bytes(m) == qoi::write(ours));
    CHECK(fx.bytes(fx.merge(bb, bb, tb)) == tb);

    auto linear = qoi::write(base, 4, 1);
    auto cs = fx.diff(bb, linear);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/qoi/colorspace");
    auto dep = fx.merge(bb, linear, tb);
    REQUIRE(dep.conflicts.size() == 1);
    CHECK(dep.conflicts[0].reason == "dependency");
}

TEST_CASE("qoi: framing errors and the envelope map") {
    Fx fx("qoi");
    auto b = qoi::write(pattern());
    CHECK(fx.f.probe(b, "").confidence == 100);
    auto cut = b;
    cut.resize(cut.size() - 3);
    CHECK_THROWS_AS(fx.load(cut), Error);
    auto d = fx.load(b);
    auto& map = *fx.f.envelope(*d);
    CHECK(map.ranges.back().unit == "/end");
    CHECK(map.ranges[4].layer == "qoi");
}
