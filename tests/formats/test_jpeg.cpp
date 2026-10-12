#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/formats/jpeg.hpp>
#include <semsplinter/formats/raster.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <sstream>

using namespace semsplinter;
using raster::Raster;

namespace {

Raster pattern(std::uint32_t w = 64, std::uint32_t h = 48) {
    Raster r(w, h, false);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            r.set(std::uint64_t{y} * w + x,
                  {static_cast<std::uint16_t>(((x * 4) & 0xFF) * 257),
                   static_cast<std::uint16_t>(((y * 5) & 0xFF) * 257),
                   static_cast<std::uint16_t>((((x ^ y) * 3) & 0xFF) * 257), 0xFFFF});
    return r;
}

std::vector<std::byte> bytes(std::string_view s) {
    auto b = as_bytes(s);
    return {b.begin(), b.end()};
}

std::vector<std::byte> make(const jpeg::EncodeOptions& o = {}, std::uint32_t w = 64, std::uint32_t h = 48) {
    return jpeg::encode(pattern(w, h), o);
}

using Mcus = std::vector<std::array<std::uint32_t, 2>>;

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& jpg = *reg.find("jpeg");

    std::unique_ptr<Document> load(const std::vector<std::byte>& b) {
        return jpg.load(ByteSource::from_memory(b, "x.jpg"), {ctx});
    }
    ChangeSet diff(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
        return jpg.diff(*load(a), *load(b), {ctx});
    }
    MergeResult merge(const std::vector<std::byte>& b, const std::vector<std::byte>& o,
                      const std::vector<std::byte>& t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return jpg.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::vector<std::byte> out(const MergeResult& r) {
        VectorSink s;
        jpg.save(*r.document, s, {ctx});
        return s.data;
    }
    std::string dump(const std::vector<std::byte>& b) {
        std::ostringstream os;
        jpg.dump(*load(b), os, {ctx});
        return os.str();
    }
};

std::size_t count_kind(const ChangeSet& cs, std::string_view kind) {
    return static_cast<std::size_t>(
        std::count_if(cs.begin(), cs.end(), [&](const Change& c) { return c.kind == kind; }));
}

// The bytes from the first DQT through EOI: tables, frame header, and scans.
std::vector<std::byte> image_part(const std::vector<std::byte>& f) {
    for (std::size_t i = 2; i + 1 < f.size(); ++i)
        if (f[i] == std::byte{0xFF} && f[i + 1] == std::byte{0xDB})
            return {f.begin() + static_cast<long>(i), f.end()};
    return {};
}

} // namespace

TEST_CASE("jpeg: probe, segments, and framing errors") {
    Fx fx;
    auto f = make();
    CHECK(fx.jpg.probe(f, "x").confidence == 100);
    CHECK(fx.jpg.probe(bytes("\xFF\xD8\xFF"), "x").confidence == 60);
    CHECK(fx.jpg.probe(bytes("\x89PNG"), "x").confidence == 0);
    CHECK(&fx.reg.detect(f, "photo.bin") == &fx.jpg);
    auto truncated = f;
    truncated.resize(f.size() - 2); // no EOI
    CHECK_THROWS_AS(fx.load(truncated), Error);
    auto d = fx.load(f);
    const auto* map = fx.jpg.envelope(*d);
    REQUIRE(map);
    CHECK_NOTHROW(map->validate(f.size()));
}

TEST_CASE("jpeg: re-encoding without requantizing diffs as encoding changes only") {
    Fx fx;
    auto base = make({});
    for (auto o : {jpeg::EncodeOptions{.progressive = true}, jpeg::EncodeOptions{.restart = 2},
                   jpeg::EncodeOptions{.arithmetic = true}}) {
        auto other = make(o);
        auto cs = fx.diff(base, other);
        CHECK(count_kind(cs, "pixels") == 0);
        CHECK(count_kind(cs, "encoding") >= 1);
        CHECK_FALSE(fx.jpg.compare(*fx.load(base), *fx.load(other), {fx.ctx})->differs());
    }
}

TEST_CASE("jpeg: diff reports MCU-aligned regions with coefficient and pixel metrics") {
    Fx fx;
    auto a = make({}, 61, 45); // odd size: edge MCUs carry padding
    auto b = jpeg::edit_mcus(a, Mcus{{1, 1}, {2, 1}}, 8);
    auto cs = fx.diff(a, b);
    REQUIRE(count_kind(cs, "pixels") == 1);
    auto& c = cs.back();
    CHECK(c.at.to_string() == "/pixels#rect=16,16,32,16"); // 4:2:0: 16x16 MCUs
    CHECK(c.metrics["mcus"] == 2);
    CHECK(c.metrics["blocks_changed"] == nlohmann::ordered_json{8, 2, 2});
    CHECK(c.metrics["max_delta"][0].get<int>() > 0);
    // The edge MCU is clipped to the image.
    auto e = fx.diff(a, jpeg::edit_mcus(a, Mcus{{3, 2}}, 8));
    CHECK(e.back().at.to_string() == "/pixels#rect=48,32,13,13");
}

TEST_CASE("jpeg: disjoint MCU edits merge losslessly in every coding") {
    for (auto o : {jpeg::EncodeOptions{}, jpeg::EncodeOptions{.h_samp = 1, .v_samp = 1},
                   jpeg::EncodeOptions{.progressive = true}, jpeg::EncodeOptions{.restart = 3},
                   jpeg::EncodeOptions{.arithmetic = true}, jpeg::EncodeOptions{.gray = true}}) {
        Fx fx;
        auto base = make(o, 70, 50);
        auto ours = jpeg::edit_mcus(base, Mcus{{0, 0}}, 5);
        auto theirs = jpeg::edit_mcus(base, Mcus{{3, 2}}, -7);
        auto r = fx.merge(base, ours, theirs);
        CHECK(r.conflicts.empty());
        auto merged = fx.out(r);
        // Exactly both edits: no pixel difference against the two edits applied in one pass.
        auto both = jpeg::edit_mcus(ours, Mcus{{3, 2}}, -7);
        CHECK(count_kind(fx.diff(merged, both), "pixels") == 0);
        CHECK(count_kind(fx.diff(ours, merged), "pixels") == 1);
        CHECK(merged == both); // one deterministic writer: ours' tables, script, restart interval, segments
    }
}

TEST_CASE("jpeg: passthrough is byte-exact") {
    Fx fx;
    auto base = make();
    auto x = jpeg::edit_mcus(base, Mcus{{1, 0}}, 4);
    CHECK(fx.out(fx.merge(base, base, x)) == x);
    CHECK(fx.out(fx.merge(base, x, base)) == x);
    CHECK(fx.out(fx.merge(base, x, x)) == x);
    CHECK(fx.merge(base, x, x).conflicts.empty());
}

TEST_CASE("jpeg: overlapping and adjacent edits") {
    Fx fx;
    auto base = make();
    auto r = fx.merge(base, jpeg::edit_mcus(base, Mcus{{1, 1}}, 4), jpeg::edit_mcus(base, Mcus{{1, 1}}, -4));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].at.to_string() == "/pixels#rect=16,16,16,16");

    auto ours = jpeg::edit_mcus(base, Mcus{{0, 0}}, 4), theirs = jpeg::edit_mcus(base, Mcus{{1, 0}}, 4);
    auto adj = fx.merge(base, ours, theirs);
    CHECK(adj.conflicts.empty());
    CHECK(std::any_of(adj.warnings.begin(), adj.warnings.end(),
                      [](const Warning& w) { return w.code == "adjacent-edits"; }));
    fx.opts["guard"] = "1";
    auto g = fx.merge(base, ours, theirs);
    REQUIRE(g.conflicts.size() == 1);
    CHECK(g.conflicts[0].reason == "guard-band");
}

TEST_CASE("jpeg: metadata merges with pixels; the scan data is spliced verbatim") {
    Fx fx;
    std::vector<std::byte> com = bytes("reviewed");
    auto base = make();
    auto ours = jpeg::edit_mcus(base, Mcus{{2, 1}}, 6);
    auto theirs = make({.markers = {{0xFE, com}}}); // same coefficients, plus a comment
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    auto merged = fx.out(r);
    CHECK(image_part(merged) == image_part(ours)); // no libjpeg involvement: ours' scans byte-exact
    CHECK(fx.dump(merged).find("app com/0 \"reviewed\"") != std::string::npos);

    // Both change the Exif segment differently: an OpaqueLeaf conflict.
    auto exif = [&](std::string_view v) {
        auto p = bytes(std::string("Exif\0\0", 6));
        auto t = bytes(v);
        p.insert(p.end(), t.begin(), t.end());
        return make({.markers = {{0xE1, p}}});
    };
    auto e = fx.merge(exif("one"), exif("two"), exif("three"));
    REQUIRE(e.conflicts.size() == 1);
    CHECK(e.conflicts[0].at.to_string() == "/exif");
}

TEST_CASE("jpeg: a profile change against a pixel change is a dependency conflict") {
    Fx fx;
    auto icc = [](std::string_view profile) {
        auto p = bytes(std::string("ICC_PROFILE\0\x01\x01", 14));
        auto t = bytes(profile);
        p.insert(p.end(), t.begin(), t.end());
        return std::vector<std::pair<std::uint8_t, std::vector<std::byte>>>{{0xE2, p}};
    };
    auto base = make({.markers = icc("srgb")});
    auto ours = make({.markers = icc("p3")});
    auto theirs = jpeg::edit_mcus(base, Mcus{{0, 0}}, 3);
    auto r = fx.merge(base, ours, theirs);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "dependency");
}

TEST_CASE("jpeg: different quantization tables refuse to merge pixels") {
    Fx fx;
    auto base = make({.quality = 90});
    auto requality = make({.quality = 70});
    // Only one side re-encoded, the other left the pixels alone: that side wins wholesale.
    CHECK(fx.out(fx.merge(base, base, requality)) == requality);
    auto cs = fx.diff(base, requality);
    CHECK(count_kind(cs, "coding") >= 1);
    REQUIRE(count_kind(cs, "pixels") >= 1);
    CHECK(cs.back().metrics["requantized"] == true);
    // Against a pixel edit: conflict encoding-mismatch, ours written.
    auto ours = jpeg::edit_mcus(base, Mcus{{0, 0}}, 3);
    auto r = fx.merge(base, ours, requality);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "encoding-mismatch");
    CHECK(fx.out(r) == ours);
    auto t = fx.merge(base, ours, requality, {Policy::theirs});
    CHECK(fx.out(t) == requality);
}

TEST_CASE("jpeg: churn of one MCU with restart markers stays in its interval") {
    Fx fx;
    auto a = make({.restart = 1}, 128, 64);
    auto b = jpeg::edit_mcus(a, Mcus{{3, 1}}, 9);
    auto da = fx.load(a), db = fx.load(b);
    auto churn = attribute_churn(a, *fx.jpg.envelope(*da), b, *fx.jpg.envelope(*db));
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::unexplained)] == 0);
    CHECK(churn.total() < a.size() / 4);
}

TEST_CASE("jpeg: dump is stable and tiles localize edits") {
    Fx fx;
    auto a = make({}, 256, 128);
    auto d = fx.dump(a);
    CHECK(d.starts_with("jpeg 256x128 sof=baseline precision=8 colorspace=ycbcr sampling=2x2,1x1,1x1\n"));
    CHECK(d.find("quant 0 xxh3=") != std::string::npos);
    CHECK(d == fx.dump(a));
    auto e = fx.dump(jpeg::edit_mcus(a, Mcus{{0, 0}}, 2));
    auto lines = [](const std::string& s) {
        std::vector<std::string> v;
        std::istringstream is(s);
        for (std::string l; std::getline(is, l);)
            v.push_back(l);
        return v;
    };
    auto la = lines(d), le = lines(e);
    REQUIRE(la.size() == le.size());
    std::size_t differ = 0;
    for (std::size_t i = 0; i < la.size(); ++i)
        differ += la[i] != le[i];
    CHECK(differ == 1);
}
