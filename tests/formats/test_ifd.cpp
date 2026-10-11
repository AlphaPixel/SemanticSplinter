#include <semsplinter/error.hpp>
#include <semsplinter/formats/ifd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace semsplinter;

namespace {

// IFD0 {ImageWidth=7 SHORT, Software="splinter" ASCII (out of line), ExifIFD -> {ExposureTime RATIONAL}},
// written with the IFD writer itself, in either byte order.
std::vector<std::byte> sample(bool big, bool bigtiff) {
    emit::GroupId ids = 10;
    ifd::Writer w{big, bigtiff, std::nullopt, &ids};
    const emit::GroupId ifd0 = 1, exif = 2;
    std::vector<emit::Seg> file;
    std::vector<std::byte> head{std::byte(big ? 'M' : 'I'), std::byte(big ? 'M' : 'I')};
    file.push_back(emit::bytes(head));
    file.push_back(w.number(bigtiff ? 43 : 42, 2));
    if (bigtiff)
        file.push_back(w.number(8, 2)), file.push_back(w.number(0, 2));
    file.push_back(w.pointer(ifd0));
    std::string sw = "splinter";
    std::vector<std::byte> text(sw.size() + 1);
    std::memcpy(text.data(), sw.data(), sw.size());
    std::vector<ifd::OutEntry> e0{
        w.numbers(256, ifd::SHORT, {7}),
        {305, ifd::ASCII, text.size(), {emit::bytes(text)}, std::nullopt},
        {34665, ifd::LONG, 1, {emit::field(4, w.endian(), w.offset(exif))}, std::nullopt}};
    if (bigtiff)
        e0[2].type = ifd::IFD8, e0[2].value = {w.pointer(exif)};
    file.push_back(w.write(ifd0, e0, w.pointer(std::uint64_t{0})));
    file.push_back(w.write(exif, {w.numbers(33434, ifd::RATIONAL, {1, 250})}, w.pointer(std::uint64_t{0})));
    return emit::emit(emit::group(ids++, std::move(file)));
}

} // namespace

TEST_CASE("ifd: parse what the writer wrote, in both byte orders and BigTIFF") {
    for (bool big : {false, true})
        for (bool bigtiff : {false, true}) {
            INFO("big " << big << " bigtiff " << bigtiff);
            auto b = sample(big, bigtiff);
            auto t = ifd::parse(b);
            CHECK(t.big_endian == big);
            CHECK(t.bigtiff == bigtiff);
            REQUIRE(t.chain.size() == 1);
            auto& d = t.chain[0];
            REQUIRE(d.entries.size() == 3);
            CHECK(t.number(*d.find(256)) == 7);
            CHECK(ifd::text(*d.find(305)) == "splinter");
            CHECK(!d.find(305)->in_line);
            auto* ex = d.find(34665);
            REQUIRE(ex->sub.size() == 1);
            CHECK(t.numbers(*ex->sub[0].find(33434)) == std::vector<std::uint64_t>{1, 250});
        }
}

TEST_CASE("ifd: keep() leaves an out-of-line value at its offset") {
    auto b = sample(false, false);
    auto t = ifd::parse(b);
    emit::GroupId ids = 100;
    ifd::Writer w{false, false, std::nullopt, &ids};
    auto& sw = *t.chain[0].find(305);
    std::vector<emit::Seg> file{emit::bytes(std::span(b)),
                                w.write(99, {w.keep(sw)}, w.pointer(std::uint64_t{0}))};
    auto out = emit::emit(emit::group(ids++, std::move(file)));
    // Point the header at the appended IFD and parse: the value is read from the old place.
    const std::uint32_t at = static_cast<std::uint32_t>(b.size() + (b.size() % 2));
    for (int k = 0; k < 4; ++k)
        out[4 + static_cast<std::size_t>(k)] = std::byte((at >> (8 * k)) & 0xFF);
    auto t2 = ifd::parse(out);
    CHECK(t2.chain[0].find(305)->at == sw.at);
    CHECK(ifd::text(*t2.chain[0].find(305)) == "splinter");
}

TEST_CASE("ifd: cycles, bounds and bad headers are rejected") {
    auto b = sample(false, false);
    auto t = ifd::parse(b);
    // NextIFD of IFD0 pointing back at IFD0.
    auto loop = b;
    const auto next_at = t.chain[0].offset + 2 + 12 * t.chain[0].entries.size();
    for (int k = 0; k < 4; ++k)
        loop[next_at + static_cast<std::size_t>(k)] = std::byte((t.chain[0].offset >> (8 * k)) & 0xFF);
    CHECK_THROWS_AS(ifd::parse(loop), Error);
    auto oob = b;
    oob[4] = std::byte{0xF0}, oob[5] = std::byte{0xFF};
    CHECK_THROWS_AS(ifd::parse(oob), Error);
    auto magic = b;
    magic[2] = std::byte{41};
    CHECK_THROWS_AS(ifd::parse(magic), Error);
    CHECK_THROWS_AS(ifd::parse(std::span(b).first(6)), Error);
}
