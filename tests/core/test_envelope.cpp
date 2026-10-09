#include <semsplinter/emit.hpp>
#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace semsplinter;
namespace e = semsplinter::emit;

namespace {
std::string str(const std::vector<std::byte>& v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}
std::vector<std::byte> vec(std::string_view s) {
    auto b = as_bytes(s);
    return {b.begin(), b.end()};
}
} // namespace

TEST_CASE("checksums match reference vectors") {
    REQUIRE(crc32(as_bytes("123456789")) == 0xCBF43926u);
    REQUIRE(crc32(as_bytes("6789"), crc32(as_bytes("12345"))) == 0xCBF43926u);
    REQUIRE(adler32(as_bytes("Wikipedia")) == 0x11E60398u);
    std::string big(100000, 'z');
    REQUIRE(adler32(as_bytes(big)) ==
            adler32(as_bytes(big.substr(5000)), adler32(as_bytes(big.substr(0, 5000)))));
}

TEST_CASE("Emitter: a PNG IEND chunk") {
    // length(data) | "IEND" data(empty) | crc32("IEND")
    auto chunk = e::group(0, {e::field(4, e::Endian::big, e::size_of(2)),
                              e::group(1, {e::bytes("IEND"), e::group(2, {})}),
                              e::field(4, e::Endian::big, e::crc32(1))});
    REQUIRE(e::emit(chunk) == vec(std::string("\0\0\0\0IEND\xAE\x42\x60\x82", 12)));
}

TEST_CASE("Emitter: offsets, alignment, raw copies") {
    auto src = ByteSource::from_memory(vec("xxPAYLOADxx"));
    auto root =
        e::group(0, {e::field(2, e::Endian::little, e::offset_of(1)), e::align(4, std::byte{'.'}),
                     e::group(1, {e::raw(src, 2, 7)}), e::field(1, e::Endian::little, e::size_of(1))});
    REQUIRE(str(e::emit(root)) == std::string("\x04\x00..PAYLOAD\x07", 12));
    REQUIRE_THROWS_AS(e::raw(src, 8, 4), Error);
}

TEST_CASE("Emitter: variable-width fields reach a fixed point") {
    // 9 filler bytes, then the decimal offset of g: width 1 gives 10 (2 digits), width 2 gives 11.
    auto root = e::group(0, {e::bytes("123456789"), e::varfield(e::VarEncoding::decimal, e::offset_of(1)),
                             e::group(1, {e::bytes("!")})});
    REQUIRE(str(e::emit(root)) == "12345678911!");
    auto leb = e::group(0, {e::varfield(e::VarEncoding::uleb128, e::constant(300))});
    REQUIRE(e::emit(leb) == std::vector<std::byte>{std::byte{0xAC}, std::byte{0x02}});
}

TEST_CASE("Emitter: nested checksums run innermost first; self-coverage sums a zeroed field") {
    // outer crc covers the inner crc field.
    auto root =
        e::group(0, {e::group(1, {e::group(2, {e::bytes("ab")}), e::field(4, e::Endian::big, e::crc32(2))}),
                     e::field(4, e::Endian::big, e::crc32(1))});
    auto out = e::emit(root);
    auto inner = crc32(as_bytes("ab"));
    std::vector<std::byte> g1 = vec("ab");
    for (int i = 3; i >= 0; --i)
        g1.push_back(static_cast<std::byte>(inner >> (8 * i)));
    auto outer = crc32(g1);
    REQUIRE(std::to_integer<unsigned>(out[9]) == (outer & 0xFF));

    // TAR-style: the checksum field lies inside the group it covers and is zero while summing.
    auto self = e::group(0, {e::bytes("hi"), e::field(4, e::Endian::little, e::adler32(0))});
    auto s = e::emit(self);
    REQUIRE(std::to_integer<unsigned>(s[2]) == (adler32(vec(std::string("hi\0\0\0\0", 6))) & 0xFF));
}

TEST_CASE("Emitter: errors are internal") {
    REQUIRE_THROWS_AS(e::emit(e::group(0, {e::field(1, e::Endian::little, e::constant(256))})), Error);
    REQUIRE_THROWS_AS(e::emit(e::group(0, {e::field(4, e::Endian::little, e::size_of(9))})), Error);
    REQUIRE_THROWS_AS(e::emit(e::group(0, {e::group(0, {})})), Error);
}

TEST_CASE("EnvelopeMap validation and churn attribution") {
    EnvelopeMap bad;
    bad.ranges = {{0, 4, ByteClass::framing, "", ""}, {2, 4, ByteClass::content, "", ""}};
    REQUIRE_THROWS_AS(bad.validate(10), Error);

    // [len:2 framing][payload:4 encoding][crc:2 integrity]; payload recompressed, length unchanged.
    auto a = vec(std::string("\x00\x04"
                             "abcd"
                             "XY",
                             8)),
         b = vec(std::string("\x00\x04"
                             "abzz"
                             "QY",
                             8));
    EnvelopeMap m;
    m.ranges = {{0, 2, ByteClass::framing, "/c", ""},
                {2, 4, ByteClass::encoding, "/c", "zlib"},
                {6, 2, ByteClass::integrity, "/c", ""}};
    auto c = attribute_churn(a, m, b, m);
    REQUIRE(c.by_class[static_cast<std::size_t>(ByteClass::encoding)] == 2);
    REQUIRE(c.by_class[static_cast<std::size_t>(ByteClass::integrity)] == 1);
    REQUIRE(c.total() == 3);
    REQUIRE(c.to_json()["bytes_changed"] == 3);

    // Bytes no range covers are unexplained.
    auto u = attribute_churn(vec("aaaa"), EnvelopeMap{}, vec("aaab"), EnvelopeMap{});
    REQUIRE(u.by_class[static_cast<std::size_t>(ByteClass::unexplained)] == 1);
    REQUIRE(EnvelopeMap::whole_content(4).ranges.size() == 1);
}
