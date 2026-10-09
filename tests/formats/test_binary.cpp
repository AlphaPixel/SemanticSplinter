#include <semsplinter/formats/binary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <sstream>

using namespace semsplinter;

namespace {

std::vector<std::byte> random_bytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::byte> v(n);
    for (auto& b : v)
        b = static_cast<std::byte>(rng() & 0xFF);
    return v;
}

std::unique_ptr<binary::BinDoc> doc(std::vector<std::byte> v) {
    return binary::load(ByteSource::from_memory(std::move(v)));
}

// Applies reported ranges to A: must give B exactly.
std::vector<std::byte> apply(std::span<const std::byte> a, std::span<const std::byte> b,
                             const binary::BinDiff& d) {
    std::vector<std::byte> out;
    std::size_t at = 0;
    for (auto& e : d.ranges) {
        out.insert(out.end(), a.begin() + static_cast<long>(at), a.begin() + static_cast<long>(e.a0));
        out.insert(out.end(), b.begin() + static_cast<long>(e.b0), b.begin() + static_cast<long>(e.b1));
        at = e.a1;
    }
    out.insert(out.end(), a.begin() + static_cast<long>(at), a.end());
    return out;
}

} // namespace

TEST_CASE("binary: chunking is deterministic and bounded") {
    auto v = random_bytes(1 << 20, 7);
    auto c1 = binary::chunk(v), c2 = binary::chunk(v);
    REQUIRE(c1.size() == c2.size());
    std::size_t total = 0;
    for (std::size_t k = 0; k < c1.size(); ++k) {
        CHECK(c1[k].off == c2[k].off);
        CHECK(c1[k].len <= 64 * 1024);
        if (k + 1 < c1.size())
            CHECK(c1[k].len >= 2 * 1024);
        total += c1[k].len;
    }
    CHECK(total == v.size());
    CHECK(c1.size() > 64); // about 8 KiB average
}

TEST_CASE("binary: reported ranges reconstruct B, and metrics add up") {
    std::mt19937 rng(11);
    for (int round = 0; round < 12; ++round) {
        auto a = random_bytes(static_cast<std::size_t>(rng() % (round < 6 ? 4000 : 400000)), rng());
        auto b = a;
        for (int k = 0; k < 5 && !b.empty(); ++k) {
            std::size_t pos = rng() % b.size();
            switch (rng() % 3) {
            case 0:
                b[pos] = static_cast<std::byte>(rng());
                break;
            case 1:
                b.insert(b.begin() + static_cast<long>(pos), static_cast<std::byte>(rng()));
                break;
            default:
                b.erase(b.begin() + static_cast<long>(pos));
            }
        }
        auto da = doc(a), db = doc(b);
        auto d = binary::diff(*da, *db);
        CHECK(apply(a, b, d) == b);
        std::uint64_t la = 0, lb = 0;
        for (auto& e : d.ranges)
            la += e.a1 - e.a0, lb += e.b1 - e.b0;
        CHECK(d.replaced + d.deleted == la);
        CHECK(d.replaced + d.inserted == lb);
    }
}

TEST_CASE("binary: one insertion into a large file is one range (resynchronization)") {
    auto a = random_bytes(4 << 20, 3);
    auto b = a;
    b.insert(b.begin() + 1'000'003, {std::byte{1}, std::byte{2}, std::byte{3}});
    auto d = binary::diff(*doc(a), *doc(b));
    REQUIRE(d.ranges.size() == 1);
    CHECK(d.inserted == 3);
    CHECK(d.anchor_coverage > 0.9);
    CHECK(binary::diff(*doc(a), *doc(a)).identical());
}

TEST_CASE("binary: dump is chunk-sectioned with chunk-relative offsets") {
    std::vector<std::byte> v;
    for (char c : std::string("Hello, binary world!\n"))
        v.push_back(static_cast<std::byte>(c));
    std::ostringstream os;
    binary::dump(*doc(v), os);
    auto s = os.str();
    CHECK(s.starts_with("# binary 21 bytes xxh3:"));
    CHECK(s.find("## chunk 0 21 bytes xxh3:") != std::string::npos);
    CHECK(s.find("00000000  48656c6c6f2c2062696e61727920776f  |Hello, binary wo|\n") != std::string::npos);
    CHECK(s.find("00000010  726c64210a                        |rld!.|\n") != std::string::npos);
}

TEST_CASE("binary: opaque merge takes the one changed side, conflicts otherwise") {
    auto base = random_bytes(100, 1), ours = base, theirs = base;
    ours[10] = std::byte{0xAA};
    std::vector<std::byte> out;
    auto r = binary::merge(*doc(base), *doc(ours), *doc(base), {}, nullptr, out);
    CHECK(out == ours);
    CHECK(r.conflicts.empty());
    theirs[90] = std::byte{0xBB};
    r = binary::merge(*doc(base), *doc(ours), *doc(theirs), {}, nullptr, out);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(out == ours);
    ConflictOptions take_theirs{Policy::theirs};
    binary::merge(*doc(base), *doc(ours), *doc(theirs), take_theirs, nullptr, out);
    CHECK(out == theirs);
}

TEST_CASE("binary: --binary-merge merges disjoint edits and refuses risky ones") {
    auto base = random_bytes(1000, 2);
    auto ours = base, theirs = base;
    ours[100] = std::byte{1};
    theirs[800] = std::byte{2};
    binary::ByteMergeOptions bm;
    std::vector<std::byte> out;
    auto r = binary::merge(*doc(base), *doc(ours), *doc(theirs), {}, &bm, out);
    auto expect = base;
    expect[100] = std::byte{1}, expect[800] = std::byte{2};
    CHECK(out == expect);
    CHECK(r.conflicts.empty());
    REQUIRE(!r.warnings.empty());
    CHECK(r.warnings[0].code == "binary-merge-unverified");

    // Guard band: edits 4 bytes apart conflict as one.
    theirs = base;
    theirs[104] = std::byte{3};
    bm.guard = 8;
    r = binary::merge(*doc(base), *doc(ours), *doc(theirs), {}, &bm, out);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "guard-band");

    // Size rule: ours inserts early, theirs edits later -> dependency conflict unless forced.
    ours = base;
    ours.insert(ours.begin() + 10, std::byte{9});
    theirs = base;
    theirs[900] = std::byte{4};
    bm.guard = 0;
    r = binary::merge(*doc(base), *doc(ours), *doc(theirs), {}, &bm, out);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "dependency");
    bm.force = true;
    r = binary::merge(*doc(base), *doc(ours), *doc(theirs), {}, &bm, out);
    CHECK(r.conflicts.empty());
    CHECK(out.size() == base.size() + 1);
    CHECK(out[901] == std::byte{4});
}
