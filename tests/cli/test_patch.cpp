// Patch output (--binary) and the patch personality (SPEC §6, CLI.md §5).
#include "console.hpp"

#include <semsplinter/formats/png.hpp>
#include <semsplinter/patch.hpp>
#include <semsplinter/semsplinter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using semsplinter::raster::Raster;

namespace {

struct Dir {
    fs::path d;
    explicit Dir(const std::string& name) {
        d = fs::temp_directory_path() / ("splinter-test-" + name);
        fs::remove_all(d);
        fs::create_directories(d);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(d, ec);
    }
    std::string path(const std::string& n) const { return (d / n).generic_string(); }
    std::string write(const std::string& n, std::string_view s) const {
        std::ofstream(d / n, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
        return path(n);
    }
    std::string write(const std::string& n, const std::vector<std::byte>& b) const {
        return write(n, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    }
    std::string read(const std::string& n) const {
        std::ifstream f(d / n, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
};

std::uint32_t crc32(std::string_view s) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; ++k)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

// `png` with a tEXt chunk inserted before IEND.
std::vector<std::byte> with_text(std::vector<std::byte> png, const std::string& key,
                                 const std::string& value) {
    std::string body = "tEXt" + key + std::string(1, '\0') + value;
    std::string chunk;
    auto be = [&](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            chunk += static_cast<char>((v >> s) & 0xFF);
    };
    be(static_cast<std::uint32_t>(body.size() - 4));
    chunk += body;
    be(crc32(body));
    auto at = png.end() - 12;
    png.insert(at, reinterpret_cast<const std::byte*>(chunk.data()),
               reinterpret_cast<const std::byte*>(chunk.data()) + chunk.size());
    return png;
}

Raster image(std::initializer_list<std::pair<std::uint64_t, std::uint16_t>> red) {
    Raster r(16, 16, false);
    for (std::uint64_t i = 0; i < r.cells(); ++i)
        r.set(i, {0x2020, 0x4040, 0x6060, 0xFFFF});
    for (auto [i, v] : red)
        r.set(i, {v, 0, 0, 0xFFFF});
    return r;
}

// True when the two PNG files hold the same pixels and chunks.
bool same_png(const std::string& a, const std::string& b) {
    static const auto reg = semsplinter::Registry::with_builtin_formats();
    semsplinter::DiffRequest rq;
    rq.a = {semsplinter::ByteSource::from_file(a, {}), a};
    rq.b = {semsplinter::ByteSource::from_file(b, {}), b};
    return !semsplinter::diff(reg, rq).differs;
}

int cli(CaptureConsole& c, std::vector<std::string> args) {
    return run_cli(c, std::move(args));
}

} // namespace

TEST_CASE("patch: payload codec and envelope round trip") {
    std::vector<std::byte> raw(1000);
    for (std::size_t i = 0; i < raw.size(); ++i)
        raw[i] = std::byte(i * 7 % 251);
    auto enc = semsplinter::encode_payload(raw);
    auto nl = enc.find('\n');
    auto n = std::stoul(enc.substr(0, nl));
    CHECK(semsplinter::decode_payload(enc.substr(nl + 1), n, 1 << 20) == raw);
    CHECK_THROWS(semsplinter::decode_payload(enc.substr(nl + 1), n, 10)); // past the limit

    semsplinter::PatchSection s;
    s.old_name = "a dir/x.png", s.new_name = "b.png", s.format = "png";
    s.old_hash = semsplinter::hash128(std::string_view("a")),
    s.new_hash = semsplinter::hash128(std::string_view("b"));
    s.units.push_back(
        {semsplinter::Op::update, {"/pixels", semsplinter::Rect{1, 2, 3, 4}}, "c-1", {}, {}, raw, raw});
    s.units.push_back(
        {semsplinter::Op::insert, {"/chunks/tEXt:Creation Time"}, "c-2", {}, "\"now\"", {}, raw});
    auto text = semsplinter::write_section(s) + "trailing garbage\n";
    auto back = semsplinter::parse_patch(text, 1 << 20);
    REQUIRE(back.size() == 1);
    CHECK(back[0].format == "png");
    CHECK(back[0].old_name == "a dir/x.png");
    CHECK(back[0].old_hash == s.old_hash);
    REQUIRE(back[0].units.size() == 2);
    CHECK(back[0].units[0].at == s.units[0].at);
    CHECK(back[0].units[1].at.path == "/chunks/tEXt:Creation Time");
    CHECK(back[0].units[1].after_text == "\"now\"");
    CHECK_FALSE(back[0].units[1].before);
    CHECK(*back[0].units[1].after == raw);
    CHECK(semsplinter::write_section(back[0]) == semsplinter::write_section(s));
}

TEST_CASE("patch: a PNG patch reproduces b, applies to a third file, and rejects conflicts") {
    Dir d("patch-png");
    auto a = d.write("a.png", semsplinter::png::write(image({})));
    auto b = d.write("b.png", semsplinter::png::write(image({{17, 0xFFFF}, {18, 0xFFFF}})));
    CaptureConsole mk;
    REQUIRE(cli(mk, {"--binary", a, b}) == 1);
    CHECK(mk.out_text.starts_with("splinter-patch 1\n--- " + a + "\txxh3:"));
    CHECK(mk.out_text.find("\nformat png\n@@ update /pixels#rect=1,1,2,1 c-") != std::string::npos);
    auto patch = d.write("p.patch", mk.out_text);

    SECTION("exit criterion: applied to a, the result equals b") {
        auto t = d.write("t.png", d.read("a.png"));
        CaptureConsole c;
        CHECK(cli(c, {"-P", t, patch}) == 0);
        CHECK(c.out_text == "patching file " + t + "\n");
        CHECK(same_png(t, b));
        CHECK_FALSE(fs::exists(d.d / "t.png.orig")); // exact base: no backup
        // Again: already applied, skipped like GNU -N; -R takes it back out.
        CaptureConsole again;
        CHECK(cli(again, {"-P", t, patch}) == 1);
        CHECK(again.out_text.find("Reversed (or previously applied) patch detected!  Skipping patch.") !=
              std::string::npos);
        CaptureConsole rev;
        CHECK(cli(rev, {"-P", "-R", "-r", "-", t, patch}) == 0);
        CHECK(same_png(t, a));
    }
    SECTION("from standard input, by the name in the patch") {
        auto t = d.write("t.png", d.read("a.png"));
        CaptureConsole c;
        auto text = mk.out_text;
        c.input.assign(reinterpret_cast<const std::byte*>(text.data()),
                       reinterpret_cast<const std::byte*>(text.data()) + text.size());
        CHECK(cli(c, {"-P", "-d", d.d.generic_string(), "-p0"}) == 0); // a.png's path is absolute: -d ignored
        CHECK(same_png(a, b));                                         // a itself was patched
    }
    SECTION("a third file that differs elsewhere") {
        auto t = d.write("t.png", semsplinter::png::write(image({{200, 0x8080}})));
        CaptureConsole c;
        CHECK(cli(c, {"-P", t, patch}) == 0);
        CHECK(c.out_text == "patching file " + t + "\nbase-mismatch: applying by identity\n");
        CHECK(same_png(t, d.write("want.png", semsplinter::png::write(
                                                  image({{17, 0xFFFF}, {18, 0xFFFF}, {200, 0x8080}})))));
        CHECK(fs::exists(d.d / "t.png.orig")); // GNU backs up a file the patch did not match exactly
    }
    SECTION("a conflicting edit is rejected, the rest applies") {
        auto t = d.write("t.png", semsplinter::png::write(image({{17, 0x1111}})));
        CaptureConsole c;
        CHECK(cli(c, {"-P", t, patch}) == 1);
        CHECK(c.out_text.find("Hunk #1 FAILED at /pixels#rect=1,1,2,1 (both-modified).") !=
              std::string::npos);
        CHECK(c.out_text.find("1 out of 1 hunk FAILED -- saving rejects to file " + t + ".rej") !=
              std::string::npos);
        CHECK(same_png(t, d.write("want.png", semsplinter::png::write(image({{17, 0x1111}})))));
        auto rej = semsplinter::parse_patch(d.read("t.png.rej"), 1 << 20);
        REQUIRE(rej.size() == 1);
        CHECK(rej[0].units.size() == 1);
        // --dry-run reports the same and writes nothing.
        CaptureConsole dry;
        auto before = d.read("t.png");
        fs::remove(d.d / "t.png.rej");
        CHECK(cli(dry, {"-P", "--dry-run", t, patch}) == 1);
        CHECK(dry.out_text.starts_with("checking file " + t + "\n"));
        CHECK(d.read("t.png") == before);
        CHECK_FALSE(fs::exists(d.d / "t.png.rej"));
    }
}

TEST_CASE("patch: PNG chunk insert, update, and removal") {
    Dir d("patch-chunks");
    auto base = semsplinter::png::write(image({}));
    auto a = d.write("a.png", with_text(with_text(base, "Author", "Alice"), "Gone", "x"));
    auto b = d.write("b.png", with_text(with_text(base, "Author", "Bob"), "Title", "T"));
    CaptureConsole mk;
    REQUIRE(cli(mk, {"--binary", a, b}) == 1);
    CHECK(mk.out_text.find("@@ update /chunks/tEXt:Author c-") != std::string::npos);
    CHECK(mk.out_text.find("- \"Alice\"\n+ \"Bob\"\n") != std::string::npos);
    auto patch = d.write("p.patch", mk.out_text);
    // A third file with its own chunk: it survives, the patch's edits land.
    auto t =
        d.write("t.png", with_text(with_text(with_text(base, "Author", "Alice"), "Gone", "x"), "Keep", "k"));
    CaptureConsole c;
    CHECK(cli(c, {"-P", t, patch}) == 0);
    CHECK(same_png(t, d.write("want.png", with_text(with_text(with_text(base, "Author", "Bob"), "Keep", "k"),
                                                    "Title", "T"))));
}

TEST_CASE("patch: unified diffs as GNU patch applies them") {
    Dir d("patch-text");
    std::string orig;
    for (int i = 1; i <= 15; ++i)
        orig += std::to_string(i) + "\n";
    auto edit = [](std::string s, const std::string& from, const std::string& to) {
        auto at = s.find("\n" + from + "\n");
        return s.replace(at + 1, from.size(), to);
    };
    const std::string patch = "--- a/sub/x.txt\n+++ b/sub/x.txt\n"
                              "@@ -1,6 +1,6 @@\n 1\n 2\n-3\n+three\n 4\n 5\n 6\n"
                              "@@ -10,6 +10,6 @@\n 10\n 11\n 12\n-13\n+thirteen\n 14\n 15\n";
    fs::create_directories(d.d / "sub");
    auto p = d.write("p.diff", patch);
    const std::string dir = d.d.generic_string();

    SECTION("-p1, -d, offsets, and fuzz") {
        d.write("sub/x.txt", "z\n" + edit(edit(orig, "1", "ONE"), "15", "X"));
        CaptureConsole c;
        CHECK(cli(c, {"-P", "-p1", "-d", dir, "-i", p}) == 0);
        CHECK(c.out_text == "patching file sub/x.txt\nHunk #1 succeeded at 2 with fuzz 2 (offset 1 line).\n"
                            "Hunk #2 succeeded at 11 with fuzz 2 (offset 1 line).\n");
        CHECK(d.read("sub/x.txt") ==
              "z\n" + edit(edit(edit(edit(orig, "1", "ONE"), "15", "X"), "3", "three"), "13", "thirteen"));
        CHECK(fs::exists(d.d / "sub/x.txt.orig"));
    }
    SECTION("a missing file, without -p") {
        CaptureConsole c;
        CHECK(cli(c, {"-P", "-d", dir, "-i", p}) == 1);
        CHECK(c.out_text == "can't find file to patch at input line 3\n"
                            "Perhaps you should have used the -p or --strip option?\n"
                            "The text leading up to this was:\n--------------------------\n"
                            "|--- a/sub/x.txt\n|+++ b/sub/x.txt\n--------------------------\n"
                            "No file to patch.  Skipping patch.\n2 out of 2 hunks ignored\n");
    }
    SECTION("a failed hunk goes to the reject file") {
        d.write("sub/x.txt", edit(orig, "4", "FOUR"));
        CaptureConsole c;
        CHECK(cli(c, {"-P", "-p1", "-d", dir, "-i", p}) == 1);
        CHECK(c.out_text == "patching file sub/x.txt\nHunk #1 FAILED at 1.\n"
                            "1 out of 2 hunks FAILED -- saving rejects to file sub/x.txt.rej\n");
        CHECK(d.read("sub/x.txt.rej") ==
              "--- sub/x.txt\n+++ sub/x.txt\n@@ -1,6 +1,6 @@\n 1\n 2\n-3\n+three\n 4\n 5\n 6\n");
        CHECK(d.read("sub/x.txt") == edit(edit(orig, "4", "FOUR"), "13", "thirteen"));
    }
    SECTION("-o, -b with a numbered backup, -R") {
        d.write("sub/x.txt", orig);
        CaptureConsole o;
        CHECK(cli(o, {"-P", "-p1", "-d", dir, "-o", "out.txt", "-i", p}) == 0);
        CHECK(o.out_text == "patching file out.txt (read from sub/x.txt)\n");
        CHECK(d.read("out.txt") == edit(edit(orig, "3", "three"), "13", "thirteen"));
        CHECK(d.read("sub/x.txt") == orig);
        CaptureConsole b;
        CHECK(cli(b, {"-P", "-p1", "-d", dir, "-b", "-V", "numbered", "-i", p}) == 0);
        CHECK(d.read("sub/x.txt.~1~") == orig);
        CaptureConsole r;
        CHECK(cli(r, {"-P", "-p1", "-d", dir, "-R", "-i", p}) == 0);
        CHECK(d.read("sub/x.txt") == orig);
    }
    SECTION("creation, deletion, and a missing newline") {
        auto q = d.write("q.diff",
                         "--- /dev/null\n+++ new.txt\n@@ -0,0 +1,2 @@\n+a\n+b\n\\ No newline at end of file\n"
                         "--- gone.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-x\n");
        d.write("gone.txt", "x\n");
        CaptureConsole c;
        CHECK(cli(c, {"-P", "-d", dir, "-i", q}) == 0);
        CHECK(d.read("new.txt") == "a\nb");
        CHECK_FALSE(fs::exists(d.d / "gone.txt"));
        CaptureConsole again;
        CHECK(cli(again, {"-P", "-d", dir, "-i", q}) == 1);
        CHECK(again.out_text.starts_with(
            "The next patch would create the file new.txt,\nwhich already exists!  "
            "Skipping patch.\n1 out of 1 hunk ignored\n"));
    }
}

TEST_CASE("patch: a mixed file of unified and envelope sections") {
    Dir d("patch-mixed");
    const std::string dir = d.d.generic_string();
    d.write("a.png", semsplinter::png::write(image({})));
    d.write("b.png", semsplinter::png::write(image({{5, 0xFFFF}})));
    d.write("a.txt", "one\ntwo\n");
    d.write("b.txt", "one\n2\n");
    CaptureConsole x, y;
    {
        auto saved = fs::current_path();
        fs::current_path(d.d);
        CHECK(cli(x, {"--binary", "a.png", "b.png"}) == 1);
        CHECK(cli(y, {"--binary", "-L", "a.txt", "-L", "a.txt", "a.txt", "b.txt"}) == 1);
        fs::current_path(saved);
    }
    auto p = d.write("mixed.patch", "From: someone\n\n" + y.out_text + x.out_text);
    CaptureConsole c;
    CHECK(cli(c, {"-P", "-d", dir, "-i", p}) == 0);
    CHECK(c.out_text == "patching file a.txt\npatching file a.png\n");
    CHECK(d.read("a.txt") == "one\n2\n");
    CHECK(same_png((d.d / "a.png").generic_string(), (d.d / "b.png").generic_string()));
    // Binary files have no patch support.
    CaptureConsole bin;
    CHECK(cli(bin, {"--binary", d.write("x.bin", std::string("\0\1", 2)),
                    d.write("y.bin", std::string("\0\2", 2))}) == 2);
    CHECK(bin.err_text.find("patches for format 'binary' are not supported") != std::string::npos);
}
