// ZIP containers (docs/formats/containers.md): reader defences, raw-copy writer, member merge and recursion.
#include "../../src/formats/container/container.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/png.hpp>
#include <semsplinter/formats/raster.hpp>
#include <semsplinter/formats/zip.hpp>
#include <semsplinter/patch.hpp>
#include <semsplinter/semsplinter.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../cli/console.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace semsplinter;
using raster::Raster;
using Entries = std::vector<zip::NewEntry>;

namespace {

std::vector<std::byte> bytes(std::string_view s) {
    auto b = as_bytes(s);
    return {b.begin(), b.end()};
}
std::string str(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

zip::NewEntry entry(std::string path, std::string_view content, std::uint16_t method = 8) {
    zip::NewEntry e;
    e.path = std::move(path);
    e.content = bytes(content);
    e.method = method;
    return e;
}

Raster pattern() {
    Raster r(32, 24, false);
    for (std::uint64_t i = 0; i < r.cells(); ++i)
        r.set(i, {static_cast<std::uint16_t>((i % 32) * 8 * 257),
                  static_cast<std::uint16_t>((i / 32) * 10 * 257), 0x8080, 0xFFFF});
    return r;
}
void paint(Raster& r, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, raster::Cell c) {
    for (auto y = y0; y < y0 + h; ++y)
        for (auto x = x0; x < x0 + w; ++x)
            r.set(std::uint64_t{y} * r.width + x, c);
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& fmt = *reg.find("zip");

    std::unique_ptr<Document> load(const std::vector<std::byte>& b) {
        return fmt.load(ByteSource::from_memory(b, "x.zip"), {ctx});
    }
    MergeResult merge(const std::vector<std::byte>& b, const std::vector<std::byte>& o,
                      const std::vector<std::byte>& t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return fmt.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::vector<std::byte> save(const MergeResult& r) {
        VectorSink s;
        fmt.save(*r.document, s, {ctx});
        return s.data;
    }
    // Member contents of a ZIP, by path.
    std::map<std::string, std::string> members(const std::vector<std::byte>& b) {
        auto d = load(b);
        auto& a = dynamic_cast<const container::Archive&>(*d);
        std::map<std::string, std::string> m;
        for (std::size_t i = 0; i < a.entries.size(); ++i)
            m[a.entries[i].path] = str(a.content(i, limits));
        return m;
    }
    std::vector<std::string> paths(const std::vector<std::byte>& b) {
        auto d = load(b);
        std::vector<std::string> v;
        for (auto& e : dynamic_cast<const container::Archive&>(*d).entries)
            v.push_back(e.path);
        return v;
    }
};

bool has_prefix(const ChangeSet& cs, std::string_view prefix) {
    return std::any_of(cs.begin(), cs.end(), [&](const Change& c) { return c.at.path.starts_with(prefix); });
}

bool contains(std::span<const std::byte> hay, std::span<const std::byte> needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

} // namespace

TEST_CASE("Phase 6 exit") {
    // A ZIP holding a PNG and a JSON member; both sides edit both members.
    Raster base_img = pattern(), ours_img = base_img, theirs_img = base_img, both_img = base_img;
    paint(ours_img, 2, 2, 6, 5, {0xFFFF, 0, 0, 0xFFFF});
    paint(theirs_img, 20, 15, 7, 6, {0, 0, 0xFFFF, 0xFFFF});
    paint(both_img, 2, 2, 6, 5, {0xFFFF, 0, 0, 0xFFFF});
    paint(both_img, 20, 15, 7, 6, {0, 0, 0xFFFF, 0xFFFF});
    auto config = [](std::string_view name, int size) {
        return "{\n  \"name\": \"" + std::string(name) + "\",\n  \"size\": " + std::to_string(size) +
               ",\n  \"tags\": [\"a\", \"b\"]\n}\n";
    };
    auto archive = [&](const Raster& img, const std::string& json) {
        auto png = zip::NewEntry{"assets/logo.png", png::write(img), 0};
        return zip::build(Entries{png, entry("config.json", json), entry("README", "untouched\n")});
    };
    auto base = archive(base_img, config("logo", 32));
    auto ours = archive(ours_img, config("LOGO", 32));
    auto theirs = archive(theirs_img, config("logo", 64));

    const auto reg = Registry::with_builtin_formats();
    MergeRequest rq;
    rq.base = {ByteSource::from_memory(base, "base.zip"), "base.zip"};
    rq.ours = {ByteSource::from_memory(ours, "ours.zip"), "ours.zip"};
    rq.theirs = {ByteSource::from_memory(theirs, "theirs.zip"), "theirs.zip"};
    auto out = semsplinter::merge(reg, rq);
    CHECK(out.format == "zip");
    CHECK(out.conflicts == 0);
    const Report& rep = out.report;
    CHECK(rep.conflicts.empty());
    // Both sub-merges are reported under /entries/<path>/...
    for (auto* cs : {&rep.changes_ours, &rep.changes_theirs})
        CHECK(has_prefix(*cs, "/entries/assets~1logo.png/content/pixels"));
    CHECK(has_prefix(rep.changes_ours, "/entries/config.json/content/name"));
    CHECK(has_prefix(rep.changes_theirs, "/entries/config.json/content/size"));
    CHECK(std::any_of(rep.auto_merged.begin(), rep.auto_merged.end(), [&](const AutoMerge& a) {
        return std::any_of(rep.changes_theirs.begin(), rep.changes_theirs.end(), [&](const Change& c) {
            return c.id == a.change && c.at.path == "/entries/config.json/content/size";
        });
    }));
    auto json = to_json(rep).dump();
    CHECK(json.find("/entries/assets~1logo.png/content/pixels#rect=") != std::string::npos);
    CHECK(json.find("/entries/config.json/content/size") != std::string::npos);

    Fx fx;
    auto m = fx.members(out.bytes);
    CHECK(m["config.json"] == config("LOGO", 64));
    CHECK(m["README"] == "untouched\n");
    const Format& pngf = *reg.find("png");
    auto merged_png = pngf.load(ByteSource::from_memory(bytes(m["assets/logo.png"]), "m.png"), {fx.ctx});
    auto expected_png = pngf.load(ByteSource::from_memory(png::write(both_img), "e.png"), {fx.ctx});
    auto d = pngf.diff(*merged_png, *expected_png, {fx.ctx});
    CHECK(std::none_of(d.begin(), d.end(), [](const Change& c) { return c.kind == "pixels"; }));
    // The untouched member is raw-copied: its local header and data appear byte for byte.
    auto readme = zip::build(Entries{entry("README", "untouched\n")});
    CHECK(contains(out.bytes, std::span(readme).first(30 + 6)));
}

TEST_CASE("zip: unchanged merges are byte-identical") {
    Fx fx;
    auto base = zip::build(Entries{entry("a.txt", "alpha\n"), entry("b.bin", std::string(300, 'x'), 0)}, "c");
    auto ours = zip::build(Entries{entry("a.txt", "alpha\n"), entry("b.bin", std::string(300, 'y'), 0)}, "c");
    CHECK(fx.save(fx.merge(base, base, base)) == base);
    CHECK(fx.save(fx.merge(base, ours, base)) == ours);
    auto r = fx.merge(base, base, ours); // theirs' member raw-copied into ours' layout
    CHECK(fx.save(r) == ours);
    CHECK(r.conflicts.empty());
}

TEST_CASE("zip: diff lists members, renames, metadata, and recursed content") {
    Fx fx;
    auto a = zip::build(
        Entries{entry("keep.txt", "1\n2\n3\n"), entry("old/name.txt", "moved\n"), entry("gone.txt", "bye\n")},
        "first");
    auto e = entry("keep.txt", "1\nX\n3\n");
    e.dos_time = 0x6000; // 12:00:00
    auto b = zip::build(Entries{e, entry("new/name.txt", "moved\n"), entry("added.txt", "hi\n")}, "second");
    auto cs = fx.fmt.diff(*fx.load(a), *fx.load(b), {fx.ctx});
    auto find = [&](std::string_view at) {
        return std::find_if(cs.begin(), cs.end(), [&](const Change& c) { return c.at.path == at; });
    };
    REQUIRE(find("/entries/new~1name.txt") != cs.end());
    CHECK(find("/entries/new~1name.txt")->op == Op::move);
    CHECK(find("/entries/new~1name.txt")->from->path == "/entries/old~1name.txt");
    CHECK(find("/entries/gone.txt")->op == Op::erase);
    CHECK(find("/entries/added.txt")->op == Op::insert);
    CHECK(find("/entries/keep.txt/meta/mtime")->after.text == "1980-01-01T12:00:00");
    CHECK(find("/archive/comment")->after.text == "second");
    CHECK(has_prefix(cs, "/entries/keep.txt/content/lines"));

    // A time-only change is listed but does not make the archives differ (containers.md §4).
    auto t = zip::build(Entries{e});
    auto u = zip::build(Entries{entry("keep.txt", "1\nX\n3\n")});
    auto cmp = fx.fmt.compare(*fx.load(u), *fx.load(t), {fx.ctx});
    CHECK(cmp->changes().size() == 1);
    CHECK_FALSE(cmp->differs());
    fx.opts["zip-times"] = "strict";
    CHECK(fx.fmt.compare(*fx.load(u), *fx.load(t), {fx.ctx})->differs());
    fx.opts["zip-times"] = "ignore";
    CHECK(fx.fmt.diff(*fx.load(u), *fx.load(t), {fx.ctx}).empty());
}

TEST_CASE("zip: member merge rules") {
    Fx fx;
    auto base = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x02\x03"),
                                   entry("c.txt", "c\n"), entry("old.txt", "renamed content\n")});
    SECTION("rename on one side, edit on the other") {
        auto ours = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x02\x03"),
                                       entry("c.txt", "c\n"), entry("new.txt", "renamed content\n")});
        auto theirs =
            zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x02\x03"),
                               entry("c.txt", "c\n"), entry("old.txt", "renamed content, edited\n")});
        auto r = fx.merge(base, ours, theirs);
        CHECK(r.conflicts.empty());
        auto m = fx.members(fx.save(r));
        CHECK(m.count("old.txt") == 0);
        CHECK(m["new.txt"] == "renamed content, edited\n");
    }
    SECTION("both change an opaque member: conflict, ours by default, theirs on request") {
        auto ours = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x09\x03"),
                                       entry("c.txt", "c\n"), entry("old.txt", "renamed content\n")});
        auto theirs = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x08\x03"),
                                         entry("c.txt", "c\n"), entry("old.txt", "renamed content\n")});
        auto r = fx.merge(base, ours, theirs);
        REQUIRE(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].at.path.starts_with("/entries/b.bin"));
        CHECK(fx.members(fx.save(r))["b.bin"] == "\x01\x09\x03");
        ConflictOptions c;
        c.resolutions[r.conflicts[0].id] = "theirs";
        auto r2 = fx.merge(base, ours, theirs, c);
        CHECK(fx.members(fx.save(r2))["b.bin"] == "\x01\x08\x03");
    }
    SECTION("modify/delete and add/add") {
        auto ours = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x02\x03"),
                                       entry("c.txt", "c changed\n"), entry("old.txt", "renamed content\n"),
                                       entry("same.txt", "x\n")});
        auto theirs = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x02\x03"),
                                         entry("old.txt", "renamed content\n"), entry("same.txt", "x\n")});
        auto r = fx.merge(base, ours, theirs);
        REQUIRE(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].reason == "modify-delete");
        CHECK(r.conflicts[0].at.path == "/entries/c.txt");
        CHECK(fx.members(fx.save(r)).count("same.txt") == 1);
    }
    SECTION("nested text conflict is addressed inside the member and resolvable by id") {
        auto ours = zip::build(Entries{entry("a.txt", "1\nO\n3\n"), entry("b.bin", "\x01\x02\x03"),
                                       entry("c.txt", "c\n"), entry("old.txt", "renamed content\n")});
        auto theirs = zip::build(Entries{entry("a.txt", "1\nT\n3\n"), entry("b.bin", "\x01\x02\x03"),
                                         entry("c.txt", "c\n"), entry("old.txt", "renamed content\n")});
        auto r = fx.merge(base, ours, theirs);
        REQUIRE(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].at.path.starts_with("/entries/a.txt/content/lines"));
        CHECK(fx.members(fx.save(r))["a.txt"] == "1\nO\n3\n");
        ConflictOptions c;
        c.resolutions[r.conflicts[0].id] = "theirs";
        auto r2 = fx.merge(base, ours, theirs, c);
        CHECK(std::none_of(r2.warnings.begin(), r2.warnings.end(),
                           [](const Warning& w) { return w.code == "resolve-unmatched"; }));
        CHECK(fx.members(fx.save(r2))["a.txt"] == "1\nT\n3\n");
    }
    SECTION("both change the time: the later one wins") {
        auto o = entry("c.txt", "c\n"), t = o;
        o.dos_time = 0x6000, t.dos_time = 0x7000;
        auto ours = zip::build(Entries{entry("a.txt", "1\nO\n3\n"), entry("b.bin", "\x01\x02\x03"), o,
                                       entry("old.txt", "renamed content\n")});
        auto theirs = zip::build(Entries{entry("a.txt", "1\n2\n3\n"), entry("b.bin", "\x01\x02\x03"), t,
                                         entry("old.txt", "renamed content\n")});
        auto r = fx.merge(base, ours, theirs);
        CHECK(r.conflicts.empty());
        CHECK(std::any_of(r.auto_merged.begin(), r.auto_merged.end(),
                          [](const AutoMerge& a) { return a.rule == "newest-mtime"; }));
        auto cs = fx.fmt.diff(*fx.load(theirs), *r.document, {fx.ctx});
        CHECK(
            std::none_of(cs.begin(), cs.end(), [](const Change& c) { return c.at.path.ends_with("mtime"); }));
    }
}

TEST_CASE("zip: hostile input fails fast") {
    Fx fx;
    auto good = zip::build(Entries{entry("a.txt", "hello hello hello\n"), entry("b.txt", "b\n")});
    auto fails = [&](std::vector<std::byte> b, ErrorCategory cat, std::string_view code = {}) {
        try {
            auto d = fx.load(b);
            auto& a = dynamic_cast<const container::Archive&>(*d);
            for (std::size_t i = 0; i < a.entries.size(); ++i)
                a.content(i, fx.limits);
        } catch (const Error& e) {
            CHECK(e.category() == cat);
            CHECK(std::string(e.what()).find(code) != std::string::npos);
            return true;
        }
        return false;
    };
    const std::size_t cd = good.size() - 22 - (2 * 46 + 5 + 5);
    SECTION("truncated") {
        CHECK(fails({good.begin(), good.end() - 1}, ErrorCategory::invalid_input, "zip-no-end"));
    }
    SECTION("entry count") {
        fx.limits.max_zip_entries = 1;
        CHECK(fails(good, ErrorCategory::limit));
    }
    SECTION("duplicate names") {
        auto dup = zip::build(Entries{entry("a.txt", "1"), entry("a.txt", "2")});
        CHECK(fails(dup, ErrorCategory::invalid_input, "zip-duplicate-name"));
    }
    SECTION("local header disagrees with the central directory") {
        auto b = good;
        b[30] = std::byte{'A'}; // first local name
        CHECK(fails(b, ErrorCategory::invalid_input, "zip-header-mismatch"));
        fx.opts["zip-lenient"] = "";
        auto d = fx.load(b);
        auto& a = dynamic_cast<const container::Archive&>(*d);
        CHECK(a.warnings.at(0).code == "zip-header-mismatch");
    }
    SECTION("overlapping members (Fifield)") {
        // a.txt stores a complete local header for b.txt; b's central record points inside a's data.
        auto inner = zip::build(Entries{entry("b.txt", "b\n", 0)});
        std::vector<std::byte> local(inner.begin(), inner.begin() + 30 + 5 + 2);
        auto b = zip::build(Entries{zip::NewEntry{"a.txt", local, 0}, entry("b.txt", "b\n", 0)});
        const std::size_t at = b.size() - 22 - (2 * 46 + 10) + 46 + 5 + 42;
        b[at] = std::byte{30 + 5};
        CHECK(fails(b, ErrorCategory::invalid_input, "zip-overlap"));
    }
    SECTION("declared size lies") {
        auto b = good;
        b[22] = std::byte{3};      // local usize
        b[cd + 24] = std::byte{3}; // central usize
        CHECK(fails(b, ErrorCategory::invalid_input, "zip-size-mismatch"));
    }
    SECTION("expansion ratio") {
        auto bomb = zip::build(Entries{entry("z", std::string(4 << 20, '\0'))});
        fx.limits.max_zip_ratio = 10;
        CHECK(fails(bomb, ErrorCategory::limit));
    }
    SECTION("shared budget across members") {
        fx.limits.max_zip_total = 19; // the two members decode to 20 bytes
        CHECK(fails(good, ErrorCategory::limit));
    }
}

TEST_CASE("zip: recursion depth is bounded and degrades to an opaque member") {
    Fx fx;
    auto nest = [](std::string_view leaf) {
        auto z = zip::build(Entries{entry("leaf.txt", leaf)});
        for (int k = 0; k < 3; ++k)
            z = zip::build(Entries{zip::NewEntry{"inner.zip", z, 0}});
        return z;
    };
    auto a = nest("one\n"), b = nest("two\n");
    auto cs = fx.fmt.diff(*fx.load(a), *fx.load(b), {fx.ctx});
    CHECK(has_prefix(cs, "/entries/inner.zip/content/entries/inner.zip/content/entries/inner.zip/content/"
                         "entries/leaf.txt/content/lines"));
    fx.limits.max_depth = 2;
    cs = fx.fmt.diff(*fx.load(a), *fx.load(b), {fx.ctx});
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/entries/inner.zip/content/entries/inner.zip/content/entries/inner.zip/content");
    CHECK(cs[0].kind == "content");
}

TEST_CASE("zip: data descriptors, CP437 names, prefix data, and comments survive") {
    Fx fx;
    // One stored member written with a data descriptor (flag bit 3), a CP437 name, and a stub before it.
    auto plain = zip::build(Entries{entry("caf\x82.txt", "data!", 0)}, "archive comment");
    std::vector<std::byte> b(bytes("MZ-stub"));
    std::vector<std::byte> local(plain.begin(), plain.begin() + 30 + 8 + 5);
    local[6] = std::byte{8}, local[7] = std::byte{0}; // flag bit 3; bit 11 off: the name is CP437
    std::fill(local.begin() + 14, local.begin() + 26, std::byte{0});
    b.insert(b.end(), local.begin(), local.end());
    auto desc = std::vector<std::byte>{std::byte{'P'}, std::byte{'K'}, std::byte{7}, std::byte{8}};
    desc.insert(desc.end(), plain.begin() + 14, plain.begin() + 26);
    b.insert(b.end(), desc.begin(), desc.end());
    std::vector<std::byte> rest(plain.begin() + 30 + 8 + 5, plain.end());
    rest[8] = std::byte{8}, rest[9] = std::byte{0}; // central flags
    rest[42] = std::byte{7};                        // central local-header offset: after the stub
    rest[54 + 16] = std::byte{7 + 43 + 16};         // end record: the directory moved by stub and descriptor
    b.insert(b.end(), rest.begin(), rest.end());
    auto d = fx.load(b);
    auto& a = dynamic_cast<const container::Archive&>(*d);
    REQUIRE(a.entries.size() == 1);
    CHECK(a.entries[0].path == "caf\xC3\xA9.txt");
    CHECK(str(a.content(0, fx.limits)) == "data!");
    CHECK(fx.save(fx.merge(b, b, b)) == b);
    std::ostringstream os;
    fx.fmt.dump(*d, os, {fx.ctx});
    CHECK(os.str().starts_with(
        "# zip 1 entries, comment \"archive comment\"\nentry caf\xC3\xA9.txt  stored  5"));
}

TEST_CASE("zip: signed JARs lose their signatures and touched digests (D-50)") {
    Fx fx;
    const std::string mf = "Manifest-Version: 1.0\r\nCreated-By: test\r\n\r\n"
                           "Name: a/A.class\r\nSHA-256-Digest: AAAA\r\n\r\n"
                           "Name: b/B.class\r\nSHA-256-Digest: BBBB\r\n\r\n";
    auto jar = [&](std::string_view a_class) {
        return zip::build(Entries{entry("META-INF/MANIFEST.MF", mf), entry("META-INF/CERT.SF", "sf"),
                                  entry("META-INF/CERT.RSA", "rsa"), entry("a/A.class", a_class),
                                  entry("b/B.class", "b")});
    };
    auto base = jar("a"), theirs = jar("a2");
    auto r = fx.merge(base, base, theirs);
    auto m = fx.members(fx.save(r));
    CHECK(m.count("META-INF/CERT.SF") == 0);
    CHECK(m.count("META-INF/CERT.RSA") == 0);
    CHECK(m["META-INF/MANIFEST.MF"] ==
          "Manifest-Version: 1.0\r\nCreated-By: test\r\n\r\nName: b/B.class\r\nSHA-256-Digest: BBBB\r\n\r\n");
    auto count = [&](std::string_view code) {
        return std::count_if(r.warnings.begin(), r.warnings.end(),
                             [&](const Warning& w) { return w.code == code; });
    };
    CHECK(count("signature-stripped") == 2);
    CHECK(count("manifest-digest-removed") == 1);

    fx.opts["signatures"] = "keep";
    auto k = fx.merge(base, base, theirs);
    CHECK(fx.members(fx.save(k)).count("META-INF/CERT.SF") == 1);
    CHECK(std::any_of(k.warnings.begin(), k.warnings.end(),
                      [](const Warning& w) { return w.code == "signature-invalidated"; }));
    // Nothing changed: the signature stays valid and untouched.
    fx.opts.erase("signatures");
    CHECK(fx.save(fx.merge(base, base, base)) == base);
}

TEST_CASE("zip: mimetype stays first, zipalign padding is kept") {
    Fx fx;
    SECTION("mimetype first") {
        auto base = zip::build(Entries{entry("mimetype", "application/epub+zip", 0), entry("a.xhtml", "a")});
        auto theirs = zip::build(Entries{entry("b.xhtml", "b"), entry("mimetype", "application/epub+zip", 0),
                                         entry("a.xhtml", "a")});
        auto ours = zip::build(Entries{entry("mimetype", "application/epub+zip", 0), entry("a.xhtml", "a2")});
        auto p = fx.paths(fx.save(fx.merge(base, ours, theirs)));
        REQUIRE(p.size() == 3);
        CHECK(p[0] == "mimetype");
    }
    SECTION("alignment of stored members") {
        // Name lengths chosen so that every stored member's data starts on a 4-byte boundary.
        auto base = zip::build(Entries{entry("aa", "abcd", 0), entry("bbbbbb", "raw", 0)});
        auto theirs = zip::build(Entries{entry("aa", "abcdefg", 0), entry("bbbbbb", "raw", 0)});
        auto r = fx.merge(base, base, theirs);
        auto out = fx.save(r);
        CHECK(std::any_of(r.warnings.begin(), r.warnings.end(), [](const Warning& w) {
            return w.code == "realigned" && w.at == "/entries/bbbbbb";
        }));
        auto raw = bytes("raw");
        auto at = std::search(out.begin(), out.end(), raw.begin(), raw.end()) - out.begin();
        CHECK(at % 4 == 0);
        CHECK(fx.members(out)["bbbbbb"] == "raw");
        CHECK(fx.save(fx.merge(base, base, base)) == base);
    }
}

TEST_CASE("zip: the zstd module's patch payload codec (D-7)") {
    // The patch payload codec follows D-7: zstd when the build has it, zlib payloads still read.
    CHECK(payload_codec() == "zstd+base85");
    std::vector<std::byte> raw(5000, std::byte{'a'});
    auto enc = encode_payload(raw);
    auto nl = enc.find('\n');
    CHECK(decode_payload(enc.substr(nl + 1), std::stoul(enc.substr(0, nl)), 1 << 20) == raw);
    CHECK_THROWS_AS(decode_payload("", 0, 10, "lz4+base85"), Error);
}

TEST_CASE("cli zip: diff report and merge options") {
    auto dir = std::filesystem::temp_directory_path() / "splinter-test-zip";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto put = [&](const std::string& name, const std::vector<std::byte>& b) {
        std::ofstream(dir / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        return splinter::cli::from_path(dir / name);
    };
    auto a = put("a.zip", zip::build(Entries{entry("x.txt", "1\n2\n")}));
    auto b = put("b.zip", zip::build(Entries{entry("x.txt", "1\nB\n")}));
    CaptureConsole d;
    CHECK(run_cli(d, {a, b}) == 1);
    CHECK(d.out_text.starts_with("~ /entries/x.txt/content/lines#range=1,1"));
    CaptureConsole u;
    CHECK(run_cli(u, {"-u", a, b}) == 1);
    CHECK(u.out_text.find("-2\n+B\n") != std::string::npos);
    CaptureConsole bad;
    CHECK(run_cli(bad, {"merge", "--signatures=maybe", a, a, b}) == 2);
    auto out = splinter::cli::from_path(dir / "out.zip");
    CaptureConsole m;
    CHECK(run_cli(m, {"merge", "--signatures=strip", "-o", out, a, a, b}) == 0);
    CaptureConsole same;
    CHECK(run_cli(same, {out, b}) == 0);
    std::filesystem::remove_all(dir);
}
