#include "raster_util.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/bmp.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace rtest;

namespace {

void set32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (int k = 0; k < 4; ++k)
        b[at + static_cast<std::size_t>(k)] = std::byte((v >> (8 * k)) & 0xFF);
}

} // namespace

TEST_CASE("bmp: one image in every encoding diffs as encoding changes only") {
    const auto r = blocks(21, 13);
    std::vector<bmp::Encoding> encs = {
        {12, 24, 0, false}, {12, 8, 0, false},  {40, 1, 0, false},   {40, 2, 0, false},
        {40, 4, 0, false},  {40, 4, 2, false},  {40, 8, 0, true},    {40, 8, 1, false},
        {40, 16, 0, false}, {40, 16, 3, false}, {40, 24, 0, true},   {40, 32, 0, false},
        {40, 32, 3, false}, {56, 32, 3, false}, {108, 24, 0, false}, {124, 8, 0, false}};
    Fx fx("bmp");
    const auto ref = pixel_lines(fx.dump(bmp::write(r, encs[0])));
    for (auto& e : encs) {
        INFO("header " << e.header << " bpp " << e.bpp << " compression " << e.compression);
        auto b = bmp::write(r, e);
        CHECK(pixel_lines(fx.dump(b)) == ref);
        auto cs = fx.diff(bmp::write(r, encs[0]), b);
        CHECK(count_kind(cs, "pixels") + count_kind(cs, "shape") == 0);
    }
    // 1 bpp cannot hold four colors: promoted, still lossless.
    auto one = bmp::write(r, {40, 1, 0, false});
    CHECK(fx.dump(one).find("bpp=8") != std::string::npos);
}

TEST_CASE("bmp: alpha survives 32 bpp, and all-zero fourth bytes are padding (Q-RAST-8)") {
    Fx fx("bmp");
    auto r = blocks(8, 4);
    paint(r, 0, 0, 2, 2, rgba(10, 20, 30, 128));
    auto b = bmp::write(r, {40, 24, 0, false}); // promoted to 32 bpp for the alpha
    CHECK(fx.dump(b).find("bpp=32") != std::string::npos);
    CHECK(pixel_lines(fx.dump(b)) == pixel_lines(fx.dump(bmp::write(r, {40, 32, 0, false}))));
    // Opaque 32 bpp written with zero fourth bytes reads as opaque.
    auto opaque = bmp::write(blocks(8, 4), {40, 32, 0, false});
    CHECK(fx.dump(opaque).find("alpha=opaque") != std::string::npos);
    // Fully transparent: declared with an alpha mask, not lost as padding.
    auto clear = image(4, 4, [](std::uint32_t, std::uint32_t) { return rgba(1, 2, 3, 0); });
    CHECK(fx.dump(bmp::write(clear, {40, 32, 0, false})).find("alpha=binary") != std::string::npos);
}

TEST_CASE("bmp: disjoint edits merge, overlaps conflict, and passthrough is byte-exact") {
    Fx fx("bmp");
    const bmp::Encoding e{40, 24, 0, false};
    auto base = pattern();
    auto ours = base, theirs = base;
    paint(ours, 2, 2, 4, 4, rgba(255, 0, 0));
    paint(theirs, 30, 20, 5, 5, rgba(0, 0, 255));
    auto bb = bmp::write(base, e), ob = bmp::write(ours, e), tb = bmp::write(theirs, e);

    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto both = ours;
    paint(both, 30, 20, 5, 5, rgba(0, 0, 255));
    CHECK(fx.bytes(m) == bmp::write(both, e));

    CHECK(fx.bytes(fx.merge(bb, bb, tb)) == tb);
    CHECK(fx.bytes(fx.merge(bb, ob, bb)) == ob);

    auto clash = base;
    paint(clash, 3, 3, 2, 2, rgba(0, 255, 0));
    auto c = fx.merge(bb, ob, bmp::write(clash, e));
    REQUIRE(c.conflicts.size() == 1);
    CHECK(c.conflicts[0].at.path == "/pixels");
    CHECK(c.conflicts[0].reason == "both-modified");
    CHECK(fx.bytes(c) == ob); // policy ours
}

TEST_CASE("bmp: indexed merges keep palette indices, even duplicated colors") {
    Fx fx("bmp");
    auto base = blocks(16, 8);
    auto b = bmp::write(base, {40, 8, 0, false});
    // Duplicate palette entry 4 = entry 0 (black), and paint index 4 into ours.
    auto dup = b;
    const std::size_t table = 14 + 40;
    set32(dup, 46, 5); // biClrUsed
    // Shift pixels to make room for one more table entry.
    dup.insert(dup.begin() + static_cast<std::ptrdiff_t>(table + 16),
               {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    set32(dup, 10, static_cast<std::uint32_t>(table + 20));
    set32(dup, 2, static_cast<std::uint32_t>(dup.size()));
    auto ours = dup, theirs = dup;
    const std::size_t px = table + 20;
    ours[px] = std::byte{4}; // bottom-left pixel: black via the duplicate entry
    theirs[px + 15] = std::byte{1};
    auto m = fx.merge(dup, ours, theirs);
    REQUIRE(m.conflicts.empty());
    auto out = fx.bytes(m);
    REQUIRE(out.size() == dup.size());
    CHECK(out[px] == std::byte{4});
    CHECK(out[px + 15] == std::byte{1});
}

TEST_CASE("bmp: resolution and color space are keys that merge with pixels") {
    Fx fx("bmp");
    auto base = pattern(16, 16);
    auto bb = bmp::write(base, {124, 24, 0, false});
    auto ours = bb;
    set32(ours, 38, 3780), set32(ours, 42, 3780); // 96 dpi
    auto painted = base;
    paint(painted, 0, 0, 3, 3, rgba(9, 9, 9));
    auto tb = bmp::write(painted, {124, 24, 0, false});
    auto m = fx.merge(bb, ours, tb);
    CHECK(m.conflicts.empty());
    auto out = fx.bytes(m);
    auto d = fx.dump(out);
    CHECK(d.find("key /bmp/ppm 3780x3780 per-metre") != std::string::npos);
    CHECK(d.find("key /bmp/colorspace") != std::string::npos);
    CHECK(pixel_lines(d) == pixel_lines(fx.dump(tb)));

    // A color-space change against paint is a dependency conflict.
    auto cs = bb;
    set32(cs, 70, 0x57696E20); // 'Win '
    auto dep = fx.merge(bb, cs, tb);
    REQUIRE(dep.conflicts.size() == 1);
    CHECK(dep.conflicts[0].reason == "dependency");
}

TEST_CASE("bmp: wrong bfSize warns, a pixel edit churns content only") {
    Fx fx("bmp");
    auto b = bmp::write(pattern(), {40, 24, 0, false});
    auto off = b;
    set32(off, 2, 7);
    auto d = fx.load(off);
    auto j = fx.f.inspect(*d);
    REQUIRE(j["warnings"].size() == 1);
    CHECK(j["warnings"][0]["code"] == "framing-inconsistent");

    auto r = pattern();
    paint(r, 5, 5, 1, 1, rgba(1, 2, 3));
    auto e = bmp::write(r, {40, 24, 0, false});
    auto da = fx.load(b), de = fx.load(e);
    auto churn = attribute_churn(b, *fx.f.envelope(*da), e, *fx.f.envelope(*de));
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::content)] == 3);
    CHECK(churn.total() == 3);
}

TEST_CASE("bmp: probe and framing errors") {
    Fx fx("bmp");
    auto b = bmp::write(pattern(), {});
    CHECK(fx.f.probe(b, "").confidence == 90);
    std::vector<std::byte> junk(b.begin(), b.begin() + 40);
    CHECK_THROWS_AS(fx.load(junk), Error);
    auto bad = b;
    set32(bad, 14, 99);
    CHECK(fx.f.probe(bad, "").confidence == 0);
    CHECK_THROWS_AS(fx.load(bad), Error);
}
