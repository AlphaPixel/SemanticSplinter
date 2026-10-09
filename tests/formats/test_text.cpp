#include <semsplinter/formats/text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace semsplinter;

namespace {

std::unique_ptr<text::TextDoc> doc(std::string_view s, text::LoadOptions o = {}) {
    auto b = as_bytes(s);
    return text::load(ByteSource::from_memory({b.begin(), b.end()}), o);
}

std::string str(const std::vector<std::byte>& v) {
    return {reinterpret_cast<const char*>(v.data()), v.size()};
}

std::string merged(std::string_view base, std::string_view ours, std::string_view theirs,
                   text::MergeOptions o = {}) {
    if (o.labels[0].empty())
        o.labels = {"ours", "base", "theirs"};
    return str(text::merge(*doc(base), *doc(ours), *doc(theirs), o).bytes);
}

} // namespace

TEST_CASE("text: encoding detection and binary test") {
    auto bytes = [](std::initializer_list<unsigned> v) {
        std::vector<std::byte> out;
        for (auto x : v)
            out.push_back(static_cast<std::byte>(x));
        return out;
    };
    CHECK(detect_encoding(bytes({0xEF, 0xBB, 0xBF, 'a'})).bom == 3);
    CHECK(detect_encoding(bytes({0xFF, 0xFE, 'a', 0})).encoding == Encoding::utf16le);
    CHECK(detect_encoding(bytes({0xFF, 0xFE, 0, 0, 'a', 0, 0, 0})).encoding == Encoding::utf32le);
    CHECK(detect_encoding(bytes({0, 'a', 0, 'b', 0, '\n'})).encoding == Encoding::utf16be);
    CHECK(looks_binary(bytes({'a', 0, 'b', 'c', 'd', 'e', 'f'})));
    CHECK_FALSE(looks_binary(bytes({0xFF, 0xFE, 'a', 0, '\n', 0})));
    CHECK_FALSE(looks_binary(as_bytes("plain\n")));
}

TEST_CASE("text: GNU splits on LF only; --eol=auto splits LF, CRLF, and CR") {
    auto gnu = doc("a\r\nb\rc\nd");
    REQUIRE(gnu->lines.size() == 3);
    CHECK(gnu->line(0) == "a\r");
    CHECK(gnu->line(1) == "b\rc");
    CHECK(gnu->missing_newline());
    auto any = doc("a\r\nb\rc\nd", {SplitMode::any});
    REQUIRE(any->lines.size() == 4);
    CHECK(any->line(0) == "a");
    CHECK(any->lines[0].eol == Eol::crlf);
    CHECK(any->lines[1].eol == Eol::cr);
    auto strip = doc("a\r\nb\n", {SplitMode::lf, true});
    CHECK(strip->line(0) == "a");
    // UTF-16LE with BOM: lines decode to UTF-8.
    std::string u16 = "\xFF\xFE" + std::string("h\0i\0\n\0\xE9\0", 8);
    auto d16 = doc(u16);
    REQUIRE(d16->lines.size() == 2);
    CHECK(d16->line(0) == "hi");
    CHECK(d16->line(1) == "\xC3\xA9");
}

TEST_CASE("text: normalization options and ignorable changes") {
    auto a = doc("x  y\nA\n\nend\n"), b = doc("x y\na\nend\n");
    CHECK(text::diff(*a, *b).edits == std::vector<Edit>{{0, 3, 0, 2}});
    text::DiffOptions o;
    o.normalize.ignore_space_change = true;
    o.normalize.ignore_case = true;
    auto d = text::diff(*a, *b, o);
    REQUIRE(d.edits.size() == 1); // the deleted blank line
    CHECK(d.differs());
    o.ignore_blank_lines = true;
    CHECK_FALSE(text::diff(*a, *b, o).differs());
    // -b treats a missing final newline as trailing white space; plain comparison does not.
    CHECK(text::diff(*doc("c"), *doc("c\n")).differs());
    text::DiffOptions b_only;
    b_only.normalize.ignore_space_change = true;
    CHECK_FALSE(text::diff(*doc("c"), *doc("c\n"), b_only).differs());
}

TEST_CASE("text merge: identities over random inputs") {
    std::mt19937 rng(5);
    auto make = [&](std::size_t n) {
        std::string s;
        for (std::size_t i = 0; i < n; ++i)
            s += std::string(1, static_cast<char>('a' + rng() % 4)) + "\n";
        return s;
    };
    auto mutate = [&](std::string s) {
        for (int k = 0; k < 3 && !s.empty(); ++k) {
            std::size_t pos = (rng() % (s.size() / 2)) * 2;
            if (rng() % 2)
                s.insert(pos, "z\n");
            else
                s.erase(pos, 2);
        }
        return s;
    };
    for (int round = 0; round < 100; ++round) {
        auto b = make(rng() % 12), x = mutate(b);
        for (auto engine : {text::Engine::gnu, text::Engine::git, text::Engine::native}) {
            text::MergeOptions o;
            o.engine = engine;
            CHECK(merged(b, b, b, o) == b);
            CHECK(merged(b, x, b, o) == x);
            CHECK(merged(b, b, x, o) == x);
            if (engine != text::Engine::gnu) // GNU -A brackets identical changes
                CHECK(merged(b, x, x, o) == x);
        }
    }
}

TEST_CASE("text merge: conflict styles, policies, and refinement") {
    const std::string base = "1\n2\n3\n", ours = "1\nO\n3\n", theirs = "1\nT\n3\n";
    CHECK(merged(base, ours, theirs) == "1\n<<<<<<< ours\nO\n=======\nT\n>>>>>>> theirs\n3\n");
    text::MergeOptions d3;
    d3.style = text::MarkerStyle::diff3;
    CHECK(merged(base, ours, theirs, d3) ==
          "1\n<<<<<<< ours\nO\n||||||| base\n2\n=======\nT\n>>>>>>> theirs\n3\n");
    text::MergeOptions u;
    u.conflict.resolve_all = ResolveAll::union_;
    CHECK(merged(base, ours, theirs, u) == "1\nO\nT\n3\n");
    text::MergeOptions th;
    th.conflict.resolve_all = ResolveAll::theirs;
    CHECK(merged(base, ours, theirs, th) == "1\nT\n3\n");
    text::MergeOptions small;
    small.marker_size = 3;
    CHECK(merged(base, ours, theirs, small) == "1\n<<< ours\nO\n===\nT\n>>> theirs\n3\n");

    // Zealous refinement moves lines common to both sides out of the conflict; zdiff3 keeps the base whole.
    const std::string b2 = "1\n2\n", o2 = "1\nX\nsame\nO\n", t2 = "1\nX\nsame\nT\n";
    CHECK(merged(b2, o2, t2) == "1\nX\nsame\n<<<<<<< ours\nO\n=======\nT\n>>>>>>> theirs\n");
    text::MergeOptions z;
    z.style = text::MarkerStyle::zdiff3;
    CHECK(merged(b2, o2, t2, z) ==
          "1\nX\nsame\n<<<<<<< ours\nO\n||||||| base\n2\n=======\nT\n>>>>>>> theirs\n");
    auto r = text::merge(*doc(base), *doc(ours), *doc(theirs), text::MergeOptions{});
    REQUIRE(r.result.conflicts.size() == 1);
    CHECK(r.result.conflicts[0].at.to_string() == "/lines#range=1,1");
    CHECK(r.marked == 1);
}

TEST_CASE("text merge: GNU diff3 -A brackets identical changes against the base") {
    text::MergeOptions g;
    g.engine = text::Engine::gnu;
    g.labels = {"M", "O", "Y"};
    CHECK(merged("1\n2\n3\n", "1\nx\n3\n", "1\nx\n3\n", g) == "1\n<<<<<<< O\n2\n=======\nx\n>>>>>>> Y\n3\n");
    g.gnu_mode = text::Diff3Mode::show_overlap;
    CHECK(merged("1\n2\n3\n", "1\nx\n3\n", "1\nx\n3\n", g) == "1\nx\n3\n");
}

TEST_CASE("text merge: --eol=auto lifts a whole-file line-ending conversion") {
    text::MergeOptions o;
    o.eol_auto = true;
    text::LoadOptions any{SplitMode::any};
    auto r = text::merge(*doc("a\nb\nc\n", any), *doc("a\r\nb\r\nc\r\n", any), *doc("a\nB\nc\n", any), o);
    CHECK(str(r.bytes) == "a\r\nB\r\nc\r\n");
    CHECK(r.result.conflicts.empty());
    bool lifted = false;
    for (auto& a : r.result.auto_merged)
        lifted |= a.rule == "eol-convert";
    CHECK(lifted);
}
