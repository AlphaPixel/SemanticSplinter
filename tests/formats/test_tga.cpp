#include "raster_util.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/tga.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace rtest;

namespace {

// Appends a TGA 2.0 extension area with an author and a timestamp, and points the footer at it.
std::vector<std::byte> with_ext(std::vector<std::byte> b, std::string_view author, int year,
                                int attribute = 0) {
    const std::size_t footer = b.size() - 26;
    std::vector<std::byte> ext(495);
    ext[0] = std::byte{0xEF}, ext[1] = std::byte{0x01};
    std::memcpy(ext.data() + 2, author.data(), author.size());
    ext[367] = std::byte{5}, ext[369] = std::byte{1}; // month 5, day 1
    ext[371] = std::byte(year & 0xFF), ext[372] = std::byte(year >> 8);
    ext[494] = std::byte(attribute);
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(footer), ext.begin(), ext.end());
    for (int k = 0; k < 4; ++k)
        b[footer + 495 + static_cast<std::size_t>(k)] = std::byte((footer >> (8 * k)) & 0xFF);
    return b;
}

} // namespace

TEST_CASE("tga: one image in every encoding diffs as encoding changes only") {
    Fx fx("tga");
    const auto r = blocks(19, 11);
    std::vector<tga::Encoding> encs;
    for (bool rle : {false, true})
        for (bool top : {false, true})
            for (bool right : {false, true}) {
                encs.push_back({2, 24, rle, top, right, true});
                encs.push_back({1, 8, rle, top, right, false});
            }
    encs.push_back({2, 32, false, false, false, true});
    encs.push_back({2, 16, true, false, false, true});
    encs.push_back({2, 15, false, true, false, true});
    const auto ref = pixel_lines(fx.dump(tga::write(r, encs[0])));
    for (auto& e : encs) {
        INFO("type " << int(e.type) << " bpp " << int(e.bpp) << " rle " << e.rle << " top " << e.top
                     << " right " << e.right);
        auto b = tga::write(r, e);
        CHECK(pixel_lines(fx.dump(b)) == ref);
        CHECK(only_encoding(fx.diff(tga::write(r, encs[0]), b)));
    }
    // Gray, and 16-bit gray with alpha.
    auto g = image(9, 7, [](std::uint32_t x, std::uint32_t y) {
        return rgba(int(x * 20), int(x * 20), int(x * 20), y ? 255 : 7);
    });
    auto gb = tga::write(g, {3, 16, true, false, false, true});
    CHECK(fx.dump(gb).find("type=gray") != std::string::npos);
    std::ostringstream ref2;
    raster::dump_tiles(g, ref2);
    CHECK(pixel_lines(fx.dump(gb)) == ref2.str());
    // 15-bit cannot hold the pattern's 8-bit values: promoted to 24.
    CHECK(fx.dump(tga::write(pattern(), {2, 15, false, false, false, true})).find("bpp=24") !=
          std::string::npos);
}

TEST_CASE("tga: pixels and extension fields merge; timestamps take the newest") {
    Fx fx("tga");
    auto base = pattern();
    auto bb = with_ext(tga::write(base, {2, 24, true, false, false, true}), "base", 2020);
    auto painted = base;
    paint(painted, 3, 3, 4, 4, rgba(255, 255, 0));
    auto ob = with_ext(tga::write(painted, {2, 24, true, false, false, true}), "base", 2024);
    auto tb = with_ext(tga::write(base, {2, 24, true, false, false, true}), "Ann", 2022);
    auto m = fx.merge(bb, ob, tb);
    CHECK(m.conflicts.empty());
    auto d = fx.dump(fx.bytes(m));
    CHECK(d.find("key /tga/ext/author \"Ann\"") != std::string::npos);
    CHECK(d.find("key /tga/ext/datetime 2024-05-01") != std::string::npos);
    CHECK(pixel_lines(d) == pixel_lines(fx.dump(ob)));
    CHECK(std::any_of(m.auto_merged.begin(), m.auto_merged.end(),
                      [](const AutoMerge& a) { return a.rule == "newest"; }));
    CHECK(fx.bytes(fx.merge(bb, bb, tb)) == tb);
}

TEST_CASE("tga: alpha with attribute type 0 is ignored, and a merge that adds alpha says so") {
    Fx fx("tga");
    auto r = blocks(8, 8);
    paint(r, 0, 0, 2, 2, rgba(1, 2, 3, 40));
    auto plain = tga::write(r, {2, 32, false, false, false, true});
    CHECK(fx.dump(plain).find("alpha=varies") != std::string::npos);
    auto no_alpha = with_ext(plain, "", 0, 0);
    CHECK(fx.dump(no_alpha).find("alpha=opaque") != std::string::npos);
    // The attribute type reinterprets the pixels: changing it against paint on the other side conflicts.
    auto opaque = blocks(8, 8);
    auto bb = with_ext(tga::write(opaque, {2, 32, false, false, false, true}), "x", 2020, 0);
    auto ob = with_ext(plain, "x", 2020, 3);
    auto t = opaque;
    paint(t, 6, 6, 2, 2, rgba(9, 9, 9));
    auto tb = with_ext(tga::write(t, {2, 32, false, false, false, true}), "x", 2020, 0);
    auto m = fx.merge(bb, ob, tb);
    REQUIRE(m.conflicts.size() == 1);
    CHECK(m.conflicts[0].reason == "dependency");
    // Alone, the alpha edit passes through with attribute 3.
    auto d = fx.dump(fx.bytes(fx.merge(bb, ob, bb)));
    CHECK(d.find("key /tga/ext/attribute 3") != std::string::npos);
    CHECK(d.find("alpha=varies") != std::string::npos);
}

TEST_CASE("tga: probe and framing") {
    Fx fx("tga");
    auto b = tga::write(pattern(), {});
    CHECK(fx.f.probe(b, "").confidence == 100);
    CHECK(fx.f.probe(tga::write(pattern(), {2, 24, false, false, false, false}), "").confidence == 30);
    auto cut = b;
    cut.resize(100);
    CHECK_THROWS_AS(fx.load(cut), Error);
    auto d = fx.load(b);
    CHECK(fx.f.envelope(*d)->ranges.back().unit == "/tga/footer");
}
