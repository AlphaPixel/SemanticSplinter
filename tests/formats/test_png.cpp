#include <semsplinter/envelope.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/formats/png.hpp>
#include <semsplinter/formats/raster.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <functional>
#include <sstream>

using namespace semsplinter;
using raster::Cell;
using raster::Raster;

namespace {

Cell rgba(int r, int g, int b, int a = 255) {
    return {static_cast<std::uint16_t>(r * 257), static_cast<std::uint16_t>(g * 257),
            static_cast<std::uint16_t>(b * 257), static_cast<std::uint16_t>(a * 257)};
}

Raster image(std::uint32_t w, std::uint32_t h, const std::function<Cell(std::uint32_t, std::uint32_t)>& fn) {
    Raster r(w, h, false);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            r.set(std::uint64_t{y} * w + x, fn(x, y));
    return r;
}

Raster pattern(std::uint32_t w = 64, std::uint32_t h = 48) {
    return image(w, h, [](std::uint32_t x, std::uint32_t y) {
        return rgba((x * 4) & 0xFF, (y * 5) & 0xFF, ((x + y) * 3) & 0xFF);
    });
}

void paint(Raster& r, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, Cell c) {
    for (auto y = y0; y < y0 + h; ++y)
        for (auto x = x0; x < x0 + w; ++x)
            r.set(std::uint64_t{y} * r.width + x, c);
}

std::uint32_t crc_of(std::string_view type, const std::vector<std::byte>& data) {
    return crc32(data, crc32(as_bytes(type)));
}

// Inserts a chunk right after IHDR (offset 33).
std::vector<std::byte> with_chunk(std::vector<std::byte> png, std::string_view type, std::string_view text) {
    std::vector<std::byte> data(reinterpret_cast<const std::byte*>(text.data()),
                                reinterpret_cast<const std::byte*>(text.data()) + text.size());
    std::vector<std::byte> c;
    auto put32 = [&](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            c.push_back(std::byte((v >> s) & 0xFF));
    };
    put32(static_cast<std::uint32_t>(data.size()));
    for (char ch : type)
        c.push_back(std::byte(ch));
    c.insert(c.end(), data.begin(), data.end());
    put32(crc_of(type, data));
    png.insert(png.begin() + 33, c.begin(), c.end());
    return png;
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& png = *reg.find("png");

    std::unique_ptr<Document> load(const std::vector<std::byte>& b) {
        return png.load(ByteSource::from_memory(b, "x.png"), {ctx});
    }
    ChangeSet diff(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
        return png.diff(*load(a), *load(b), {ctx});
    }
    MergeResult merge(const std::vector<std::byte>& b, const std::vector<std::byte>& o,
                      const std::vector<std::byte>& t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return png.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::vector<std::byte> bytes(const MergeResult& r) {
        VectorSink s;
        png.save(*r.document, s, {ctx});
        return s.data;
    }
    std::string dump(const std::vector<std::byte>& b) {
        std::ostringstream os;
        png.dump(*load(b), os, {ctx});
        return os.str();
    }
};

std::size_t count_kind(const ChangeSet& cs, std::string_view kind) {
    return static_cast<std::size_t>(
        std::count_if(cs.begin(), cs.end(), [&](const Change& c) { return c.kind == kind; }));
}

std::string pixel_lines(const std::string& dump) { // dump minus its first (encoding) line
    return dump.substr(dump.find('\n') + 1);
}

} // namespace

TEST_CASE("raster: CIEDE2000 matches Sharma's test data") {
    CHECK(raster::delta_e00_lab({50, 2.6772, -79.7751}, {50, 0, -82.7485}) ==
          Catch::Approx(2.0425).margin(1e-4));
    CHECK(raster::delta_e00_lab({50, -1.3802, -84.2814}, {50, 0, -82.7485}) ==
          Catch::Approx(1.0000).margin(1e-4));
    CHECK(raster::delta_e00_lab({50, 2.5, 0}, {73, 25, -18}) == Catch::Approx(27.1492).margin(1e-4));
    CHECK(raster::delta_e00(rgba(0, 0, 0), rgba(255, 255, 255)) == Catch::Approx(100).margin(1e-3));
}

TEST_CASE("png: one image in every encoding diffs as encoding changes only") {
    // Two gray levels, opaque: representable as gray1..16, rgb, index1..8, graya, rgba, interlaced or not.
    auto r = image(13, 9, [](std::uint32_t x, std::uint32_t y) {
        return (x * y) % 3 ? rgba(255, 255, 255) : rgba(0, 0, 0);
    });
    using png::Encoding;
    std::vector<Encoding> encs;
    using u8 = std::uint8_t;
    for (u8 il : {u8{0}, u8{1}}) {
        for (u8 d : {u8{1}, u8{2}, u8{4}, u8{8}, u8{16}})
            encs.push_back({0, d, il});
        for (u8 d : {u8{1}, u8{2}, u8{4}, u8{8}})
            encs.push_back({3, d, il});
        for (u8 ct : {u8{2}, u8{4}, u8{6}})
            for (u8 d : {u8{8}, u8{16}})
                encs.push_back({ct, d, il});
    }
    Fx fx;
    auto first = png::write(r, encs[0]);
    const auto ref = pixel_lines(fx.dump(first));
    for (auto& e : encs) {
        INFO(e.name() << (e.interlace ? " interlaced" : ""));
        auto b = png::write(r, e);
        CHECK(std::to_integer<int>(b[24]) == e.bit_depth);
        CHECK(std::to_integer<int>(b[25]) == e.color_type);
        auto cs = fx.diff(first, b);
        CHECK(count_kind(cs, "encoding") == cs.size());
        CHECK(pixel_lines(fx.dump(b)) == ref);
    }
}

TEST_CASE("png: 16-bit, alpha, and tRNS keys decode to canonical values") {
    Raster wide(3, 1, true);
    wide.set(0, {0x1234, 0x5678, 0x9ABC, 0xFFFF});
    wide.set(1, {0, 0, 0, 0});
    wide.set(2, {0xFFFF, 0xFFFF, 0xFFFF, 0x8000});
    Fx fx;
    auto b = png::write(wide, {6, 16, 0});
    auto d = fx.load(b);
    CHECK(count_kind(fx.diff(b, png::write(wide, {6, 16, 1})), "pixels") == 0);
    // A transparent key color: RGB8 with tRNS, read back as alpha 0 with the color kept.
    auto keyed =
        image(4, 1, [](std::uint32_t x, std::uint32_t) { return x == 2 ? rgba(9, 8, 7, 0) : rgba(x, 0, 0); });
    auto kb = png::write(keyed, {2, 8, 0});
    CHECK(std::to_integer<int>(kb[25]) == 2); // stayed RGB: tRNS key, not promoted to RGBA
    CHECK(count_kind(fx.diff(kb, png::write(keyed, {6, 8, 0})), "pixels") == 0);
    // Not representable: color in a gray target is promoted.
    auto colored = png::write(pattern(), {0, 8, 0});
    CHECK(std::to_integer<int>(colored[25]) == 2);
}

TEST_CASE("png: diff reports regions with metrics") {
    auto a = pattern(), b = a;
    paint(b, 2, 3, 4, 2, rgba(255, 0, 0));
    paint(b, 40, 30, 1, 1, rgba(0, 0, 0, 0));
    Fx fx;
    auto cs = fx.diff(png::write(a), png::write(b));
    REQUIRE(count_kind(cs, "pixels") == 2);
    auto& c = cs[0];
    CHECK(c.at.to_string() == "/pixels#rect=2,3,4,2");
    CHECK(c.metrics["pixels"] == 8);
    CHECK(c.metrics["depth"] == 8);
    CHECK(c.depends_on == std::vector<std::string>{"/color"});
    CHECK(cs[1].metrics["alpha_changed"] == 1);
    CHECK(cs[1].metrics["delta_e00_max"].get<double>() > 0);
    CHECK(fx.diff(png::write(a), png::write(a)).empty());
}

TEST_CASE("png: disjoint edits merge; conflicts are regions; passthrough is byte-exact") {
    Fx fx;
    auto base = pattern(), ours = base, theirs = base;
    paint(ours, 1, 1, 3, 3, rgba(255, 0, 0));
    paint(theirs, 50, 40, 2, 2, rgba(0, 0, 255));
    auto pb = png::write(base), po = png::write(ours), pt = png::write(theirs);
    auto r = fx.merge(pb, po, pt);
    CHECK(r.conflicts.empty());
    CHECK(r.auto_merged.size() == 2);
    auto expect = ours;
    paint(expect, 50, 40, 2, 2, rgba(0, 0, 255));
    auto out = fx.bytes(r);
    CHECK(fx.diff(out, png::write(expect)).empty());

    // merge(b, b, x) == x and merge(b, x, b) == x, byte for byte.
    CHECK(fx.bytes(fx.merge(pb, pb, pt)) == pt);
    CHECK(fx.bytes(fx.merge(pb, po, pb)) == po);
    CHECK(fx.bytes(fx.merge(pb, po, po)) == po);

    // Same pixel, different values: one both-modified conflict; ours by default, theirs on request.
    auto t2 = base;
    paint(t2, 2, 2, 1, 1, rgba(0, 255, 0));
    auto pt2 = png::write(t2);
    r = fx.merge(pb, po, pt2);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].at.to_string() == "/pixels#rect=1,1,3,3");
    CHECK(fx.diff(fx.bytes(r), po).empty());
    r = fx.merge(pb, po, pt2, {Policy::theirs});
    CHECK(r.conflicts[0].resolved_as == "theirs");
    CHECK(fx.diff(fx.bytes(r), pt2).empty());
}

TEST_CASE("png: adjacent edits warn; --guard=1 makes them conflict") {
    Fx fx;
    auto base = pattern(), ours = base, theirs = base;
    paint(ours, 10, 10, 2, 2, rgba(255, 0, 0));
    paint(theirs, 12, 10, 2, 2, rgba(0, 0, 255)); // distance 1
    auto pb = png::write(base), po = png::write(ours), pt = png::write(theirs);
    auto r = fx.merge(pb, po, pt);
    CHECK(r.conflicts.empty());
    REQUIRE(r.warnings.size() >= 1);
    CHECK(std::any_of(r.warnings.begin(), r.warnings.end(),
                      [](const Warning& w) { return w.code == "adjacent-edits"; }));
    fx.opts["guard"] = "1";
    r = fx.merge(pb, po, pt);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "guard-band");
    // Distance 2 stays clean at guard 1.
    auto far = base;
    paint(far, 13, 10, 2, 2, rgba(0, 0, 255));
    CHECK(fx.merge(pb, po, png::write(far)).conflicts.empty());
}

TEST_CASE("png: metadata merges with pixels; untouched chunks keep their bytes") {
    Fx fx;
    auto base = pattern(), ours = base;
    paint(ours, 5, 5, 2, 2, rgba(1, 2, 3));
    auto pb = png::write(base), po = png::write(ours);
    auto pt = with_chunk(pb, "tEXt", std::string("Author\0Alice", 12));
    auto r = fx.merge(pb, po, pt);
    CHECK(r.conflicts.empty());
    auto out = fx.bytes(r);
    auto cs = fx.diff(po, out);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/chunks/tEXt:Author");
    CHECK(cs[0].op == Op::insert);
    CHECK(*cs[0].after.text == "\"Alice\"");
    // Pixels came from ours untouched: its IDAT is copied, so out = ours plus one chunk.
    CHECK(out.size() == po.size() + 12 + 12);

    // Both edit the same key differently: a keyed conflict.
    auto po2 = with_chunk(po, "tEXt", std::string("Author\0Bob", 10));
    r = fx.merge(pb, po2, pt);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/chunks/tEXt:Author");
    CHECK(r.conflicts[0].reason == "add-add");

    // Save timestamps merge newest-wins instead of conflicting.
    auto stamp = [&](const std::vector<std::byte>& p, const char* t) {
        return with_chunk(p, "tEXt", std::string("date:modify\0", 12) + t);
    };
    auto sb = stamp(pb, "2026-10-01T00:00:00+00:00"), so = stamp(po, "2026-10-02T00:00:00+00:00"),
         st = stamp(pb, "2026-10-03T00:00:00+00:00");
    r = fx.merge(sb, so, st);
    CHECK(r.conflicts.empty());
    auto merged = fx.dump(fx.bytes(r));
    CHECK(merged.find("2026-10-03") != std::string::npos);

    // tIME renders as a date; --png-ignore-chunks hides it from the diff.
    auto z = with_chunk(pb, "tIME", std::string("\x07\xEA\x0A\x09\x0C\x00\x00", 7));
    auto cs2 = fx.diff(pb, z);
    REQUIRE(cs2.size() == 1);
    CHECK(*cs2[0].after.text == "2026-10-09 12:00:00");
    fx.opts["png-ignore-chunks"] = "tIME";
    CHECK(fx.diff(pb, z).empty());
}

TEST_CASE("png: a color change against a pixel change is a dependency conflict") {
    Fx fx;
    auto base = pattern(), ours = base;
    paint(ours, 5, 5, 2, 2, rgba(1, 2, 3));
    auto pb = png::write(base), po = png::write(ours);
    auto pt = with_chunk(pb, "gAMA", std::string("\x00\x00\xB1\x8F", 4));
    auto r = fx.merge(pb, po, pt);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "dependency");
    CHECK(r.conflicts[0].at.path == "/pixels");
    // Resolved as ours: ours' pixels, ours' (absent) gAMA.
    CHECK(fx.diff(fx.bytes(r), po).empty());
    fx.opts["png-color-dependency"] = "warn";
    r = fx.merge(pb, po, pt);
    CHECK(r.conflicts.empty());
    CHECK(std::any_of(r.auto_merged.begin(), r.auto_merged.end(),
                      [](const AutoMerge& a) { return a.rule == "dependency-touched"; }));
    auto cs = fx.diff(po, fx.bytes(r));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/color/gAMA");
}

TEST_CASE("png: a pure crop on one side merges with paint on the other") {
    Fx fx;
    auto base = pattern(64, 48);
    auto crop =
        image(32, 20, [&](std::uint32_t x, std::uint32_t y) { return base.get((y + 10) * 64ull + x + 8); });
    auto theirs = base;
    paint(theirs, 12, 14, 3, 3, rgba(255, 255, 0));
    auto pb = png::write(base), po = png::write(crop), pt = png::write(theirs);
    auto r = fx.merge(pb, po, pt);
    CHECK(r.conflicts.empty());
    auto expect = crop;
    paint(expect, 4, 4, 3, 3, rgba(255, 255, 0));
    CHECK(fx.diff(fx.bytes(r), png::write(expect)).empty());
    auto cs = fx.diff(pb, po);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].metrics["reshape"] == "crop");
    CHECK(cs[0].metrics["offset"] == nlohmann::json::array({8, 10}));

    // Paint outside the cropped area: cropped-away.
    auto outside = base;
    paint(outside, 60, 2, 2, 2, rgba(0, 0, 0));
    r = fx.merge(pb, po, png::write(outside));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "cropped-away");
    // A non-pure reshape against paint is shape-mismatch.
    r = fx.merge(pb, png::write(image(10, 10, [](std::uint32_t, std::uint32_t) { return rgba(7, 7, 7); })),
                 pt);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "shape-mismatch");
}

TEST_CASE("png: one changed pixel churns the deflate stream, attributed to encoding") {
    Fx fx;
    auto a = image(512, 512, [](std::uint32_t x, std::uint32_t y) {
        return rgba((x / 7 + y / 5) & 0xFF, (x * y / 97) & 0xFF, (x ^ y) & 0x3F);
    });
    auto b = a;
    b.set(3 * 512 + 3, rgba(255, 0, 255));
    auto pa = png::write(a), pb = png::write(b);
    auto da = fx.load(pa), db = fx.load(pb);
    auto churn = attribute_churn(pa, *fx.png.envelope(*da), pb, *fx.png.envelope(*db));
    std::uint64_t enc = churn.by_class[static_cast<std::size_t>(ByteClass::encoding)];
    INFO("total " << churn.total() << " encoding " << enc << " file " << pb.size());
    CHECK(churn.total() > pb.size() * 9 / 10);
    CHECK(enc >= churn.total() * 99 / 100);
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::unexplained)] == 0);
    CHECK(count_kind(fx.png.diff(*da, *db, {fx.ctx}), "pixels") == 1);
}

TEST_CASE("png: re-encode reuses inferred zlib settings and unchanged filters") {
    Fx fx;
    auto base = pattern(200, 150), ours = base, theirs = base;
    paint(ours, 1, 1, 2, 2, rgba(255, 0, 0));
    paint(theirs, 190, 140, 2, 2, rgba(0, 255, 0));
    auto r = fx.merge(png::write(base), png::write(ours), png::write(theirs));
    auto w = std::find_if(r.warnings.begin(), r.warnings.end(),
                          [](const Warning& x) { return x.code == "encoding-reencoded"; });
    REQUIRE(w != r.warnings.end());
    CHECK(w->message.find("inferred") != std::string::npos);
}

TEST_CASE("png: dump is stable and line-oriented") {
    Fx fx;
    auto p = with_chunk(png::write(pattern(130, 70)), "tEXt", std::string("Title\0Hi", 8));
    auto d = fx.dump(p);
    CHECK(d.starts_with(
        "png width=130 height=70 encoding=rgba8 interlace=none\nchunk tEXt:Title \"Hi\"\npixels xxh3="));
    CHECK(d.find("alpha=opaque") != std::string::npos);
    CHECK(d.find("\ntile 128,64,2,6 xxh3=") != std::string::npos);
    CHECK(std::count(d.begin(), d.end(), '\n') == 3 + 6);
}

TEST_CASE("png: APNG animation chunks are one opaque unit") {
    Fx fx;
    auto base = png::write(pattern());
    auto anim = with_chunk(base, "acTL", std::string("\0\0\0\1\0\0\0\0", 8));
    auto cs = fx.diff(base, anim);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/animation");
    CHECK(cs[0].kind == "animation");
    auto ours = pattern();
    paint(ours, 0, 0, 1, 1, rgba(0, 0, 0));
    auto r = fx.merge(base, png::write(ours), anim);
    CHECK(r.conflicts.empty());
    auto out = fx.bytes(r);
    auto d = fx.dump(out);
    CHECK(d.find("animation frames=1 xxh3=") != std::string::npos);
    auto anim2 = with_chunk(base, "acTL", std::string("\0\0\0\2\0\0\0\0", 8));
    r = fx.merge(base, anim2, anim);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "unsupported");
}

TEST_CASE("png: probe, framing errors, and CRC mismatches") {
    Fx fx;
    auto p = png::write(pattern());
    CHECK(fx.png.probe(p, "a.bin").confidence == 100);
    CHECK(fx.png.probe(as_bytes("GIF89a......"), "a.png").confidence == 0);
    auto truncated = std::vector<std::byte>(p.begin(), p.begin() + 40);
    CHECK_THROWS_AS(fx.load(truncated), Error);
    auto bad = p;
    bad[29] ^= std::byte{1}; // IHDR CRC
    auto d = fx.load(bad);
    auto j = fx.png.inspect(*d);
    CHECK(j["integrity"]["status"] == "mismatch");
    fx.opts["integrity"] = "strict";
    CHECK_THROWS_AS(fx.load(bad), Error);
}
