// TAR containers (docs/formats/containers.md): ustar/pax/GNU reader, wrappers, verbatim member copy, merge.
#include "../../src/formats/container/container.hpp"

#include <semsplinter/formats/tar.hpp>
#include <semsplinter/semsplinter.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <sstream>

using namespace semsplinter;
using Entries = std::vector<tar::NewEntry>;

namespace {

tar::NewEntry entry(std::string path, std::string_view content, std::uint64_t mtime = 1700000000) {
    tar::NewEntry e;
    e.path = std::move(path);
    auto b = as_bytes(content);
    e.content.assign(b.begin(), b.end());
    e.mtime = mtime;
    return e;
}

std::string str(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& fmt = *reg.find("tar");

    std::unique_ptr<Document> load(const std::vector<std::byte>& b) {
        return fmt.load(ByteSource::from_memory(b, "x.tar"), {ctx});
    }
    MergeResult merge(const std::vector<std::byte>& b, const std::vector<std::byte>& o,
                      const std::vector<std::byte>& t) {
        auto db = load(b), dob = load(o), dt = load(t);
        return fmt.merge(*db, *dob, *dt, MergeOptions{ctx, {}, {}});
    }
    std::vector<std::byte> save(const MergeResult& r) {
        VectorSink s;
        fmt.save(*r.document, s, {ctx});
        return s.data;
    }
    const container::Archive& archive(const std::unique_ptr<Document>& d) {
        return dynamic_cast<const container::Archive&>(*d);
    }
    std::map<std::string, std::string> members(const std::vector<std::byte>& b) {
        auto d = load(b);
        auto& a = archive(d);
        std::map<std::string, std::string> m;
        for (std::size_t i = 0; i < a.entries.size(); ++i)
            m[a.entries[i].path] = str(a.content(i, limits));
        return m;
    }
};

} // namespace

TEST_CASE("tar: every wrapper round-trips and merges") {
    auto w = GENERATE(tar::Wrapper::none, tar::Wrapper::gzip, tar::Wrapper::zstd, tar::Wrapper::xz,
                      tar::Wrapper::bzip2);
    Fx fx;
    auto base = tar::build(Entries{entry("a.txt", "1\n2\n3\n4\n5\n"), entry("dir/b.bin", "\x01\x02")}, w);
    auto ours = tar::build(Entries{entry("a.txt", "1\nO\n3\n4\n5\n"), entry("dir/b.bin", "\x01\x02")}, w);
    auto theirs = tar::build(
        Entries{entry("a.txt", "1\n2\n3\n4\nT\n"), entry("dir/b.bin", "\x01\x02"), entry("new.txt", "new\n")},
        w);
    CHECK(fx.reg.detect(base, "x.tar.gz").info().name == "tar");
    CHECK(fx.reg.detect(base, "").info().name == "tar");
    CHECK(fx.save(fx.merge(base, base, base)) == base);
    CHECK(fx.save(fx.merge(base, ours, base)) == ours);
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    auto out = fx.save(r);
    CHECK(out.front() == base.front()); // same wrapper
    auto m = fx.members(out);
    CHECK(m["a.txt"] == "1\nO\n3\n4\nT\n");
    CHECK(m["new.txt"] == "new\n");
    CHECK(m["dir/b.bin"] == "\x01\x02");
    if (w == tar::Wrapper::none) {
        // Untouched members are copied verbatim inside the stream.
        auto plain_b = tar::build(Entries{entry("dir/b.bin", "\x01\x02")});
        CHECK(std::search(out.begin(), out.end(), plain_b.begin(), plain_b.begin() + 1024) != out.end());
    }
}

TEST_CASE("tar: long paths, metadata, duplicates") {
    Fx fx;
    const std::string deep = std::string(60, 'd') + "/" + std::string(60, 'e') + "/file.txt";
    auto base = tar::build(Entries{entry(deep, "x\n"), entry("m.txt", "m\n")});
    auto d = fx.load(base);
    CHECK(fx.archive(d).entries[0].path == deep);

    SECTION("mode on one side, content on the other") {
        auto t = Entries{entry(deep, "x\n"), entry("m.txt", "m\n")};
        t[1].mode = 0755;
        auto o = Entries{entry(deep, "x\n"), entry("m.txt", "m2\n")};
        auto r = fx.merge(base, tar::build(o), tar::build(t));
        CHECK(r.conflicts.empty());
        auto out = fx.save(r);
        auto doc = fx.load(out);
        auto& a = fx.archive(doc);
        CHECK(str(a.content(1, fx.limits)) == "m2\n");
        CHECK(a.entries[1].meta[1] == std::pair<std::string, std::string>{"mode", "0755"});
        auto cs = fx.fmt.diff(*fx.load(tar::build(t)), *doc, {fx.ctx});
        REQUIRE(cs.size() == 1);
        CHECK(cs[0].at.to_string() == "/entries/m.txt/content/lines#range=0,1");
    }
    SECTION("long path renamed and edited") {
        auto o =
            Entries{entry(deep + ".renamed-to-a-much-longer-name-than-before", "x\n"), entry("m.txt", "m\n")};
        auto t = Entries{entry(deep, "x\n"), entry("m.txt", "m\nmore\n")};
        auto r = fx.merge(base, tar::build(o), tar::build(t));
        CHECK(r.conflicts.empty());
        auto m = fx.members(fx.save(r));
        CHECK(m.count(deep + ".renamed-to-a-much-longer-name-than-before") == 1);
        CHECK(m["m.txt"] == "m\nmore\n");
    }
    SECTION("both change the time: newest wins") {
        auto o = Entries{entry(deep, "x\n"), entry("m.txt", "m\n", 1700000100)};
        auto t = Entries{entry(deep, "x\n"), entry("m.txt", "m\n", 1700000200)};
        auto r = fx.merge(base, tar::build(o), tar::build(t));
        CHECK(r.conflicts.empty());
        auto doc = fx.load(fx.save(r));
        CHECK(fx.archive(doc).entries[1].meta[0].second == "1700000200");
    }
    SECTION("duplicates keep every occurrence") {
        auto dup = tar::build(Entries{entry("a", "1"), entry("a", "2")});
        auto dd = fx.load(dup);
        auto& a = fx.archive(dd);
        CHECK(a.entries[0].path == "a#1");
        CHECK(a.entries[1].path == "a");
        CHECK(a.warnings.at(0).code == "tar-duplicate-path");
    }
}

TEST_CASE("tar: damaged headers and limits fail fast") {
    Fx fx;
    auto good = tar::build(Entries{entry("a.txt", std::string(3000, 'a'))});
    auto fails = [&](const std::vector<std::byte>& b, ErrorCategory cat) {
        try {
            fx.load(b);
        } catch (const Error& e) {
            CHECK(e.category() == cat);
            return true;
        }
        return false;
    };
    auto b = good;
    b[0] = std::byte{'b'};
    CHECK(fails(b, ErrorCategory::invalid_input));                                   // checksum
    CHECK(fails({good.begin(), good.begin() + 1500}, ErrorCategory::invalid_input)); // truncated data
    auto gz = tar::build(Entries{entry("z", std::string(8 << 20, '\0'))}, tar::Wrapper::gzip);
    fx.limits.max_zip_ratio = 10;
    CHECK(fails(gz, ErrorCategory::limit));
    fx.limits.max_zip_ratio = 200;
    fx.limits.max_zip_total = 1 << 20;
    CHECK(fails(gz, ErrorCategory::limit));
}

TEST_CASE("tar: dump") {
    Fx fx;
    auto t = Entries{entry("b.txt", "hello\n"), entry("a", "")};
    t[1].type = '5';
    std::ostringstream os;
    fx.fmt.dump(*fx.load(tar::build(t, tar::Wrapper::zstd)), os, {fx.ctx});
    CHECK(os.str() == "# tar 2 entries (zstd)\n"
                      "entry a  dir  0  mode=0644  owner=0/0\n"
                      "entry b.txt  file  6  mode=0644  owner=0/0\n"
                      "=== b.txt (text)\nhello\n");
}

// #51: each input and the result have their own --max-zip-size budget, so three archives that each fit
// merge although their decoded total does not.
TEST_CASE("tar: merge budgets are per document") {
    std::string big;
    for (int k = 0; k < 4000; ++k)
        big += "line " + std::to_string(k) + "\n";
    auto with = [&](std::string_view a) { return Entries{entry("a.txt", a), entry("big.txt", big)}; };
    auto input = [](std::vector<std::byte> b) { return Input{ByteSource::from_memory(b, "x.tgz"), "x.tgz"}; };
    MergeRequest rq;
    rq.base = input(tar::build(with("1\n2\n3\n"), tar::Wrapper::gzip));
    rq.ours = input(tar::build(with("O\n2\n3\n"), tar::Wrapper::gzip));
    rq.theirs = input(tar::build(with("1\n2\nT\n"), tar::Wrapper::gzip));
    rq.limits.max_zip_total = tar::build(with("1\n2\n3\n")).size() * 3 / 2;
    const auto reg = Registry::with_builtin_formats();
    auto out = merge(reg, rq);
    CHECK(out.conflicts == 0);
    Fx fx;
    CHECK(fx.members(out.bytes)["a.txt"] == "O\n2\nT\n");
}