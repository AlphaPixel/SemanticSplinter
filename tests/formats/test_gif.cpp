#include "raster_util.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/gif.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace rtest;

namespace {

Raster sprite(int shift, std::uint32_t w = 24, std::uint32_t h = 17) {
    return image(w, h, [shift](std::uint32_t x, std::uint32_t y) {
        static const Cell c[] = {rgba(0, 0, 0), rgba(255, 255, 255), rgba(200, 30, 30), rgba(30, 200, 30),
                                 rgba(0, 0, 0, 0)};
        return c[(x / 3 + y / 3 + static_cast<std::uint32_t>(shift)) % 5];
    });
}

std::vector<gif::FrameSpec> anim() {
    return {
        {sprite(0), 0, 0, 10, 0, false}, {sprite(1), 0, 0, 10, 2, false}, {sprite(2), 0, 0, 10, 2, false}};
}

bool contains(const std::vector<std::byte>& hay, std::span<const std::byte> needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

} // namespace

TEST_CASE("gif: still and interlaced frames decode to the same canonical pixels") {
    Fx fx("gif");
    auto r = sprite(0);
    auto plain = gif::write({{r}});
    auto il = gif::write({{r, 0, 0, 0, 0, true}});
    std::ostringstream ref;
    raster::dump_tiles(r, ref);
    CHECK(pixel_lines(fx.dump(plain)) == ref.str());
    CHECK(pixel_lines(fx.dump(il)) == ref.str());
    auto cs = fx.diff(plain, il);
    CHECK(only_encoding(cs));
    CHECK(fx.dump(plain).starts_with("gif width=24 height=17 version=89a"));
    CHECK(fx.f.probe(plain, "").confidence == 100);
}

TEST_CASE("gif: frames merge positionally; unchanged frames keep their LZW bytes") {
    Fx fx("gif");
    auto base = anim();
    auto ours = base, theirs = base;
    paint(ours[1].pixels, 2, 2, 3, 3, rgba(0, 0, 0));
    theirs[2].delay = 50;
    auto bb = gif::write(base, 0), ob = gif::write(ours, 0), tb = gif::write(theirs, 0);
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto out = fx.bytes(m);
    auto want = ours;
    want[2].delay = 50;
    CHECK(pixel_lines(fx.dump(out)) == pixel_lines(fx.dump(gif::write(want, 0))));
    CHECK(fx.dump(out).find("key /delay 50cs") != std::string::npos);
    // Frame 0 is copied from ours: its LZW data is in the output verbatim.
    auto d0 = gif::write({base[0]});
    std::span<const std::byte> lzw0(d0.data() + 13 + 3 * 8 + 8 + 10, 40);
    CHECK(contains(out, lzw0));
    CHECK(fx.bytes(fx.merge(bb, bb, tb)) == tb);
    // Same pixels painted differently in one frame: a region conflict at that frame.
    auto clash = base;
    paint(clash[1].pixels, 3, 3, 2, 2, rgba(200, 30, 30));
    auto c = fx.merge(bb, ob, gif::write(clash, 0));
    REQUIRE(c.conflicts.size() == 1);
    CHECK(c.conflicts[0].at.path == "/frames/1/pixels");
}

TEST_CASE("gif: inserted frames merge as a sequence with a loop-count change") {
    Fx fx("gif");
    auto base = anim();
    auto theirs = base;
    theirs.push_back({sprite(3), 0, 0, 10, 2, false});
    auto bb = gif::write(base, 0), ob = gif::write(base, 3), tb = gif::write(theirs, 0);
    auto diff = fx.diff(bb, tb);
    REQUIRE(diff.size() == 1);
    CHECK(diff[0].op == Op::insert);
    CHECK(diff[0].at.path == "/frames/3");
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto d = fx.dump(fx.bytes(m));
    CHECK(d.find("frames=4") != std::string::npos);
    CHECK(d.find("key /animation/loop 3") != std::string::npos);
}

TEST_CASE("gif: a merged frame past 256 colors is refused, never dithered") {
    Fx fx("gif");
    auto base = image(40, 20, [](std::uint32_t, std::uint32_t) { return rgba(0, 0, 0); });
    auto ours = base, theirs = base;
    for (std::uint32_t i = 0; i < 200; ++i) {
        ours.set(i, rgba(static_cast<int>(i), 1, 1));
        theirs.set(400 + i, rgba(static_cast<int>(i), 2, 2));
    }
    CHECK_THROWS_AS(fx.merge(gif::write({{base}}), gif::write({{ours}}), gif::write({{theirs}})), Error);
}

TEST_CASE("gif: comments are keys, framing errors are rejected") {
    Fx fx("gif");
    auto b = gif::write({{sprite(0)}});
    std::vector<std::byte> comment{std::byte{0x21}, std::byte{0xFE}, std::byte{2},
                                   std::byte{'h'},  std::byte{'i'},  std::byte{0}};
    auto c = b;
    c.insert(c.end() - 1, comment.begin(), comment.end());
    auto cs = fx.diff(b, c);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/comments/0");
    CHECK(*cs[0].after.text == "\"hi\"");
    auto cut = b;
    cut.resize(cut.size() - 10);
    CHECK_THROWS_AS(fx.load(cut), Error);
    auto d = fx.load(c);
    CHECK(fx.f.envelope(*d)->ranges.back().unit == "/end");
}
