#include "raster_util.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/webp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace rtest;

namespace {

// Appends a chunk to a WebP and fixes the RIFF size (a simple file becomes invalid, so only for extended
// ones).
std::vector<std::byte> with_chunk(std::vector<std::byte> b, std::string_view fourcc, std::string_view data) {
    for (char c : fourcc)
        b.push_back(std::byte(c));
    for (int k = 0; k < 4; ++k)
        b.push_back(std::byte((data.size() >> (8 * k)) & 0xFF));
    for (char c : data)
        b.push_back(std::byte(c));
    if (data.size() % 2)
        b.push_back(std::byte{0});
    const auto riff = b.size() - 8;
    for (int k = 0; k < 4; ++k)
        b[4 + static_cast<std::size_t>(k)] = std::byte((riff >> (8 * k)) & 0xFF);
    return b;
}

} // namespace

TEST_CASE("webp: lossless round trip is exact, lossless re-encode is deterministic") {
    Fx fx("webp");
    auto r = pattern(30, 20);
    paint(r, 0, 0, 3, 3, rgba(10, 20, 30, 0)); // color under alpha 0 kept (exact)
    auto b = webp::write({{r}});
    std::ostringstream ref;
    raster::dump_tiles(r, ref);
    CHECK(pixel_lines(fx.dump(b)) == ref.str());
    CHECK(webp::write({{r}}) == b);
    CHECK(fx.dump(b).starts_with("webp width=30 height=20 container=simple"));
    CHECK(fx.f.probe(b, "").confidence == 100);
}

TEST_CASE("webp: lossless pixels merge; lossy stills merge whole or conflict") {
    Fx fx("webp");
    auto base = pattern(32, 24);
    auto ours = base, theirs = base;
    paint(ours, 1, 1, 3, 3, rgba(255, 0, 0));
    paint(theirs, 25, 18, 3, 3, rgba(0, 0, 255));
    auto bb = webp::write({{base}}), ob = webp::write({{ours}}), tb = webp::write({{theirs}});
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    paint(ours, 25, 18, 3, 3, rgba(0, 0, 255));
    CHECK(fx.bytes(m) == webp::write({{ours}}));

    auto lb = webp::write({{base, 0, 0, 0, false}});
    auto lo = webp::write({{ours, 0, 0, 0, false}});
    auto lt = webp::write({{theirs, 0, 0, 0, false}});
    CHECK(fx.dump(lb).find("lossy") == std::string::npos); // still: encoding per frame is not in the header
    auto one = fx.merge(lb, lb, lt);
    CHECK(fx.bytes(one) == lt);
    auto both = fx.merge(lb, lo, lt);
    REQUIRE(both.conflicts.size() == 1);
    CHECK(both.conflicts[0].reason == "encoding-mismatch");
    CHECK(fx.bytes(both) == lo);
    // --reencode merges in pixel space and warns.
    fx.opts["reencode"] = "";
    fx.opts["tolerance"] = "60"; // lossy coding noise around the edits
    auto re = fx.merge(lb, lo, lt);
    CHECK(re.conflicts.empty());
    CHECK(std::any_of(re.warnings.begin(), re.warnings.end(),
                      [](const Warning& w) { return w.code == "lossy-reencode"; }));
}

TEST_CASE("webp: animated frames merge; unchanged frames keep their bitstreams") {
    Fx fx("webp");
    std::vector<webp::FrameSpec> base{{pattern(16, 16), 0, 0, 100, true},
                                      {blocks(16, 16), 0, 0, 100, false},
                                      {pattern(8, 8), 4, 4, 50, true}};
    auto ours = base, theirs = base;
    paint(ours[0].pixels, 1, 1, 2, 2, rgba(1, 2, 3));
    theirs[1].duration = 300;
    auto bb = webp::write(base, 0), ob = webp::write(ours, 0), tb = webp::write(theirs, 0);
    auto d = fx.dump(bb);
    CHECK(d.find("frames=3") != std::string::npos);
    CHECK(d.find("encoding=lossy") != std::string::npos);
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto want = ours;
    want[1].duration = 300;
    CHECK(fx.bytes(m) == webp::write(want, 0));
    // The lossy frame changed on both sides: a frame conflict.
    auto o2 = base, t2 = base;
    paint(o2[1].pixels, 0, 0, 4, 4, rgba(255, 0, 0));
    paint(t2[1].pixels, 10, 10, 4, 4, rgba(255, 255, 255));
    auto c = fx.merge(bb, webp::write(o2, 0), webp::write(t2, 0));
    REQUIRE(c.conflicts.size() == 1);
    CHECK(c.conflicts[0].at.path == "/frames/1/pixels");
}

TEST_CASE("webp: metadata chunks are keys; VP8X flags follow the chunks") {
    Fx fx("webp");
    auto base = webp::write({{pattern(), 0, 0, 0, true}}, 0); // a one-frame animation: extended container
    auto ob = with_chunk(base, "EXIF", "exif-ours");
    auto tb = with_chunk(base, "XMP ", "<x/>");
    auto m = fx.merge(base, ob, tb);
    CHECK(m.conflicts.empty());
    auto out = fx.bytes(m);
    auto d = fx.dump(out);
    CHECK(d.find("key /exif") != std::string::npos);
    CHECK(d.find("key /xmp") != std::string::npos);
    CHECK((std::to_integer<int>(out[20]) & 0x0C) == 0x0C); // EXIF and XMP flags
    auto off = base;
    off[4] = std::byte{1};
    auto doc = fx.load(off);
    CHECK(fx.f.inspect(*doc)["warnings"].size() == 1);
}
