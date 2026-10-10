#include "../scratch.hpp"
#include "console.hpp"

#include <semsplinter/formats/png.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

// A scratch directory under the system temp dir, removed afterwards.
struct Scratch {
    fs::path dir;
    explicit Scratch(const std::string& name) {
        dir = scratch_path("test-" + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string file(const std::string& name, std::string_view content) const {
        auto p = dir / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary)
            .write(content.data(), static_cast<std::streamsize>(content.size()));
        return splinter::cli::from_path(p);
    }
    std::string read(const std::string& path) const {
        std::ifstream f(splinter::cli::to_path(path), std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
};

} // namespace

TEST_CASE("cli: personality selection") {
    CaptureConsole c;
    CHECK(run_cli(c, {"--version"}) == 0);
    CHECK(c.out_text == "splinter (Semantic Splinter) " + std::string(semsplinter::version()) + "\n");
    for (auto name : {"splinter-diff3.exe", "/usr/bin/splinter-merge", "splinter-patch"}) {
        CaptureConsole k;
        CHECK(splinter::cli::run({name}, k) == 2); // missing operands; patch: empty input
    }
    CaptureConsole d3;
    CHECK(splinter::cli::run({"x/SPLINTER-DIFF3"}, d3) == 2);
    CHECK(d3.err_text.find("missing operand after 'diff3'") != std::string::npos);
    CaptureConsole f;
    CHECK(run_cli(f, {"formats", "--json"}) == 0);
    auto j = nlohmann::json::parse(f.out_text);
    REQUIRE(j.size() >= 2);
    CHECK(j[0]["name"] == "text");
    CHECK(j[1]["name"] == "binary");
}

TEST_CASE("cli diff: GNU messages and exit status") {
    Scratch s("diff");
    auto a = s.file("a.txt", "1\n2\n3\n"), b = s.file("b.txt", "1\nX\n3\n"),
         a2 = s.file("a2.txt", "1\n2\n3\n");
    CaptureConsole c;
    CHECK(run_cli(c, {"--label", "A", "--label", "B", "-u", a, b}) == 1);
    CHECK(c.out_text == "--- A\n+++ B\n@@ -1,3 +1,3 @@\n 1\n-2\n+X\n 3\n");
    CaptureConsole q;
    CHECK(run_cli(q, {"-q", a, b}) == 1);
    CHECK(q.out_text == "Files " + a + " and " + b + " differ\n");
    CaptureConsole same;
    CHECK(run_cli(same, {"-s", a, a2}) == 0);
    CHECK(same.out_text == "Files " + a + " and " + a2 + " are identical\n");
    CaptureConsole missing;
    CHECK(run_cli(missing, {a, s.file("x", "") + "nope"}) == 2);
    CaptureConsole bad;
    CHECK(run_cli(bad, {"--ign", a, b}) == 2);
    CHECK(bad.err_text.starts_with("splinter: ambiguous option -- ign\n"));
    CaptureConsole style;
    CHECK(run_cli(style, {"-c", "-u", a, b}) == 2);
    CHECK(style.err_text.find("conflicting output style options") != std::string::npos);

    auto x = s.file("x.bin", std::string("\0\1\2", 3)), y = s.file("y.bin", std::string("\0\1\3", 3));
    CaptureConsole bin;
    CHECK(run_cli(bin, {x, y}) == 1);
    CHECK(bin.out_text == "Binary files " + x + " and " + y + " differ\n");
    CaptureConsole lim;
    CHECK(run_cli(lim, {"--max-input=2", "--exit-codes=extended", x, y}) == 4);
}

TEST_CASE("cli diff: --json report") {
    Scratch s("json");
    auto a = s.file("a", "1\n2\n"), b = s.file("b", "1\n3\n4\n");
    CaptureConsole c;
    CHECK(run_cli(c, {"--json", a, b}) == 1);
    auto j = nlohmann::json::parse(c.out_text);
    CHECK(j["schema"] == "splinter-report/1");
    CHECK(j["mode"] == "diff");
    CHECK(j["format"] == "text");
    REQUIRE(j["changes"].size() == 1);
    CHECK(j["changes"][0]["op"] == "update");
    CHECK(j["changes"][0]["at"] == "/lines#range=1,2");
    CHECK(j["stats"]["format"]["lines_inserted"] == 2);
}

TEST_CASE("cli diff: directories, -r, -N, -x, and the dump subcommand") {
    Scratch s("dirs");
    s.file("d1/f", "a\n");
    s.file("d2/f", "b\n");
    s.file("d1/only", "o\n");
    s.file("d1/sub/g", "q\n");
    s.file("d2/sub/g", "r\n");
    auto d1 = splinter::cli::from_path(s.dir / "d1"), d2 = splinter::cli::from_path(s.dir / "d2");
    CaptureConsole c;
    CHECK(run_cli(c, {d1, d2}) == 1);
    CHECK(c.out_text == "diff " + d1 + "/f " + d2 + "/f\n1c1\n< a\n---\n> b\nOnly in " + d1 + ": only\n" +
                            "Common subdirectories: " + d1 + "/sub and " + d2 + "/sub\n");
    CaptureConsole r;
    CHECK(run_cli(r, {"-rN", "-x", "f", d1, d2}) == 1);
    CHECK(r.out_text == "diff -rN -x f " + d1 + "/only " + d2 + "/only\n1d0\n< o\ndiff -rN -x f " + d1 +
                            "/sub/g " + d2 + "/sub/g\n1c1\n< q\n---\n> r\n");
    CaptureConsole dump;
    CHECK(run_cli(dump, {"dump", s.file("t.txt", "x\ny")}) == 0);
    CHECK(dump.out_text == "x\ny\n");
}

TEST_CASE("cli merge personalities") {
    Scratch s("merge");
    auto base = s.file("base", "1\n2\n3\n4\n"), ours = s.file("ours", "1\nO\n3\n4\n"),
         theirs = s.file("theirs", "1\n2\n3\nT\n"), other = s.file("other", "1\nX\n3\n4\n");
    CaptureConsole m;
    CHECK(run_cli(m, {"-m", ours, base, theirs}) == 0);
    CHECK(m.out_text == "1\nO\n3\nT\n");
    CaptureConsole mc;
    CHECK(run_cli(mc, {"-m", "-L", "M", "-L", "B", "-L", "Y", ours, base, other}) == 1);
    CHECK(mc.out_text == "1\n<<<<<<< M\nO\n||||||| B\n2\n=======\nX\n>>>>>>> Y\n3\n4\n");

    // RCS merge writes into the first file; merge-file exits with the conflict count.
    auto target = s.file("target", "1\nO\n3\n4\n");
    CaptureConsole r;
    CHECK(run_cli(r, {"merge", "-q", target, base, theirs}) == 0);
    CHECK(s.read(target) == "1\nO\n3\nT\n");
    CaptureConsole g;
    CHECK(run_cli(g, {"merge-file", "-p", ours, base, other}) == 1);
    CHECK(g.out_text == "1\n<<<<<<< " + ours + "\nO\n=======\nX\n>>>>>>> " + other + "\n3\n4\n");
    CaptureConsole gu;
    CHECK(run_cli(gu, {"merge-file", "-p", "--theirs", ours, base, other}) == 0);
    CHECK(gu.out_text == "1\nX\n3\n4\n");

    // --conflicts writes the report; --resolve picks a side by conflict id.
    auto report = splinter::cli::from_path(s.dir / "conflicts.json");
    CaptureConsole cf;
    CHECK(run_cli(cf, {"-m", "--conflicts=" + report, ours, base, other}) == 1);
    auto j = nlohmann::json::parse(s.read(report));
    REQUIRE(j["conflicts"].size() == 1);
    std::string id = j["conflicts"][0]["id"];
    auto res = s.file("res.json", "{\"" + id + "\": \"theirs\"}");
    CaptureConsole rs;
    CHECK(run_cli(rs, {"-m", "--resolve=" + res, ours, base, other}) == 0);
    CHECK(rs.out_text == "1\nX\n3\n4\n");
}

TEST_CASE("cli png: report, merge, sidecar, visual, inspect") {
    using semsplinter::raster::Raster;
    Scratch s("png");
    auto img = [](std::uint8_t r0) {
        Raster r(40, 30, false);
        for (std::uint64_t i = 0; i < r.cells(); ++i)
            r.set(i, {static_cast<std::uint16_t>((i % 40) * 6 * 257), static_cast<std::uint16_t>(r0 * 257), 0,
                      0xFFFF});
        return r;
    };
    auto put = [&](const std::string& name, const Raster& r) {
        auto b = semsplinter::png::write(r);
        return s.file(name, {reinterpret_cast<const char*>(b.data()), b.size()});
    };
    Raster base = img(0), ours = base, theirs = base, clash = base;
    ours.set(5, {0xFFFF, 0, 0, 0xFFFF});
    theirs.set(30 * 40 - 1, {0, 0, 0xFFFF, 0xFFFF});
    clash.set(5, {0, 0xFFFF, 0, 0xFFFF});
    auto pb = put("base.png", base), po = put("ours.png", ours), pt = put("theirs.png", theirs),
         pc = put("clash.png", clash);

    CaptureConsole d;
    CHECK(run_cli(d, {pb, po}) == 1);
    CHECK(d.out_text.starts_with("~ /pixels#rect=5,0,1,1 pixels=1 max_delta=225,0,0,0 "));
    CHECK(d.out_text.find("\n# bytes changed ") != std::string::npos);
    CHECK(d.out_text.ends_with("; content: 1 px in 1 region\n"));
    CaptureConsole ed;
    CHECK(run_cli(ed, {"-e", pb, po}) == 2);
    CHECK(ed.err_text == "splinter: option '-e' not supported for format 'png'\n");
    CaptureConsole js;
    CHECK(run_cli(js, {"--json", pb, po}) == 1);
    auto j = nlohmann::json::parse(js.out_text);
    CHECK(j["format"] == "png");
    CHECK(j["envelope"]["integrity"]["a"] == "ok");
    CHECK(j["envelope"]["churn"]["unexplained"] == 0);
    CHECK(j["stats"]["format"]["pixels_changed"] == 1);

    auto out = splinter::cli::from_path(s.dir / "out.png");
    CaptureConsole m;
    CHECK(run_cli(m, {"-m", "-o", out, po, pb, pt}) == 0);
    CaptureConsole same;
    auto both = ours;
    both.set(30 * 40 - 1, {0, 0, 0xFFFF, 0xFFFF});
    CHECK(run_cli(same, {out, put("both.png", both)}) == 0);

    CaptureConsole x;
    CHECK(run_cli(x, {"-m", "-o", out, po, pb, pc}) == 1);
    CHECK(std::filesystem::exists(s.dir / "out.png.splinter-conflicts.png"));
    CaptureConsole term;
    CHECK(run_cli(term, {"-m", "--visual=" + splinter::cli::from_path(s.dir / "v.png"), "-o", out, po, pb,
                         pt}) == 0);
    CaptureConsole vis;
    CHECK(run_cli(vis, {"inspect", "--json", "--layout", splinter::cli::from_path(s.dir / "v.png")}) == 0);
    auto v = nlohmann::json::parse(vis.out_text);
    CHECK(v["format"] == "png");
    CHECK(v["width"] == 40);
    CHECK(v["layout"][0] ==
          nlohmann::json{
              {"offset", 0}, {"length", 8}, {"class", "framing"}, {"unit", "/signature"}, {"layer", ""}});
    CaptureConsole mf;
    CHECK(run_cli(mf, {"merge-file", "-p", po, pb, pt}) == 255);
}

TEST_CASE("cli: json diff and merge") {
    Scratch s("json");
    auto base = s.file("base.json",
                       "{\n  \"name\": \"splinter\",\n  \"version\": \"0.1.0\",\n  \"tags\": [\"a\"]\n}\n");
    auto ours = s.file("ours.json",
                       "{\n  \"name\": \"splinter\",\n  \"version\": \"0.2.0\",\n  \"tags\": [\"a\"]\n}\n");
    auto theirs =
        s.file("theirs.json",
               "{\n  \"name\": \"splinter\",\n  \"version\": \"0.1.0\",\n  \"tags\": [\"a\", \"b\"]\n}\n");
    auto clash = s.file("clash.json", "{\"name\": \"splinter\", \"version\": \"0.3.0\", \"tags\": [\"a\"]}");
    CaptureConsole d;
    CHECK(run_cli(d, {base, ours}) == 1);
    CHECK(d.out_text == "~ /version \"0.1.0\" -> \"0.2.0\"\n");
    CaptureConsole u;
    CHECK(run_cli(u, {"-u", "--label", "a", "--label", "b", base, ours}) == 1);
    CHECK(u.out_text == "--- a\n+++ b\n@@ -1,3 +1,3 @@\n /name = \"splinter\"\n /tags/0 = \"a\"\n"
                        "-/version = \"0.1.0\"\n+/version = \"0.2.0\"\n");
    CaptureConsole js;
    CHECK(run_cli(js, {"--json", base, theirs}) == 1);
    auto j = nlohmann::json::parse(js.out_text);
    CHECK(j["format"] == "json");
    CHECK(j["changes"][0]["at"] == "/tags/1");
    CaptureConsole dump;
    CHECK(run_cli(dump, {"dump", base}) == 0);
    CHECK(dump.out_text == "/name = \"splinter\"\n/tags/0 = \"a\"\n/version = \"0.1.0\"\n");

    auto out = splinter::cli::from_path(s.dir / "out.json");
    CaptureConsole m;
    CHECK(run_cli(m, {"-m", "-o", out, ours, base, theirs}) == 0);
    CHECK(s.read(out) ==
          "{\n  \"name\": \"splinter\",\n  \"version\": \"0.2.0\",\n  \"tags\": [\"a\", \"b\"]\n}\n");
    CaptureConsole x;
    auto report = splinter::cli::from_path(s.dir / "r.json");
    CHECK(run_cli(x, {"-m", "--json=" + report, "-o", out, ours, base, clash}) == 1);
    CHECK(s.read(out) == s.read(ours));
    auto r = nlohmann::json::parse(s.read(report));
    CHECK(r["conflicts"][0]["at"] == "/version");
    CHECK(r["conflicts"][0]["reason"] == "both-modified");
}
