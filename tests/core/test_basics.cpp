#include <semsplinter/error.hpp>
#include <semsplinter/hash.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/limits.hpp>
#include <semsplinter/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

using namespace semsplinter;
namespace fs = std::filesystem;

TEST_CASE("hash128 is deterministic and hex is 32 digits") {
    REQUIRE(hash128("abc") == hash128(as_bytes("abc")));
    REQUIRE(hash128("abc") != hash128("abd"));
    REQUIRE(hash128("").hex().size() == 32);
}

TEST_CASE("Hasher delimits fields") {
    Hasher a, b;
    a.add("ab").add("c");
    b.add("a").add("bc");
    REQUIRE(a.digest() != b.digest());
}

TEST_CASE("ids are deterministic and shaped as specified") {
    Address at{"/chunks/tEXt:Author", std::nullopt};
    auto id = make_change_id(Side::ours, Op::update, at);
    REQUIRE(id.size() == 14);
    REQUIRE(id.starts_with("c-"));
    REQUIRE(id == make_change_id(Side::ours, Op::update, at));
    REQUIRE(id != make_change_id(Side::theirs, Op::update, at));
    // Conflict ids ignore the order of their change ids.
    REQUIRE(make_conflict_id(at, {"c-1", "c-2"}) == make_conflict_id(at, {"c-2", "c-1"}));
    REQUIRE(make_conflict_id(at, {"c-1"}).starts_with("x-"));
}

TEST_CASE("addresses render regions and escape pointer tokens") {
    REQUIRE(Address{"/pixels", Rect{10, 20, 32, 8}}.to_string() == "/pixels#rect=10,20,32,8");
    REQUIRE(Address{"/lines", Range{120, 4}}.to_string() == "/lines#range=120,4");
    REQUIRE(Address{"/v", Box{1, 2, 3, 4, 5, 6}}.to_string() == "/v#box=1,2,3,4,5,6");
    REQUIRE("/entries/" + pointer_escape("assets/logo~1.png") == "/entries/assets~1logo~01.png");
    REQUIRE(path_within("/a/b", "/a"));
    REQUIRE(path_within("/a", "/a"));
    REQUIRE(path_within("/a#rect=0,0,1,1", "/a"));
    REQUIRE_FALSE(path_within("/ab", "/a"));
}

TEST_CASE("sort_canonical orders by address then op") {
    ChangeSet cs{make_change(Side::ours, Op::update, {"/b"}), make_change(Side::ours, Op::update, {"/a"}),
                 make_change(Side::ours, Op::insert, {"/a"})};
    sort_canonical(cs);
    REQUIRE(cs[0].at.path == "/a");
    REQUIRE(cs[0].op == Op::insert);
    REQUIRE(cs[2].at.path == "/b");
}

TEST_CASE("Limits charge the shared memory budget") {
    Limits l;
    l.max_memory = 100;
    l.charge(60);
    REQUIRE_THROWS_AS(l.charge(41), Error);
    l.release(60);
    l.charge(100);
    try {
        l.charge(1);
        FAIL("expected a limit error");
    } catch (const Error& e) {
        REQUIRE(e.category() == ErrorCategory::limit);
    }
}

namespace {
struct TempDir {
    fs::path path = fs::temp_directory_path() / ("splinter-test-" + std::to_string(std::random_device{}()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
};
} // namespace

TEST_CASE("ByteSource reads files within limits and slices") {
    TempDir d;
    auto p = d.path / "in.bin";
    std::ofstream(p, std::ios::binary) << "hello world";
    Limits l;
    auto s = ByteSource::from_file(p, l);
    REQUIRE(s.size() == 11);
    REQUIRE(s.slice(6, 5).bytes()[0] == std::byte{'w'});
    REQUIRE_THROWS_AS(s.slice(6, 6), Error);
    l.max_input = 10;
    REQUIRE_THROWS_AS(ByteSource::from_file(p, l), Error);
    REQUIRE_THROWS_AS(ByteSource::from_file(d.path / "missing", Limits{}), Error);
}

TEST_CASE("atomic_write creates, replaces, and leaves nothing behind") {
    TempDir d;
    auto p = d.path / "out.bin";
    atomic_write(p, as_bytes("one"));
    atomic_write(p, as_bytes("two!"));
    REQUIRE(ByteSource::from_file(p, Limits{}).size() == 4);
    REQUIRE(std::distance(fs::directory_iterator(d.path), fs::directory_iterator{}) == 1);
    REQUIRE_THROWS_AS(atomic_write(d.path / "no-such-dir" / "x", as_bytes("x")), Error);
    {
        TempFile t(p); // abandoned without commit
        t.write(as_bytes("partial"));
    }
    REQUIRE(std::distance(fs::directory_iterator(d.path), fs::directory_iterator{}) == 1);
}
