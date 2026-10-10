// The Git adapter's argument and exit-code mapping (CLI.md §7), with a fake host: no git needed.
#include "git.hpp"

#include <semsplinter/formats/png.hpp>
#include <semsplinter/io.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using splinter::git::Host;

namespace {

struct FakeHost final : Host {
    std::string out_text, err_text;
    std::map<std::string, std::string> vars, config; // git config key -> value
    std::vector<std::vector<std::string>> calls;
    void out(std::string_view s) override { out_text += s; }
    void err(std::string_view s) override { err_text += s; }
    std::optional<std::string> env(const char* n) const override {
        auto it = vars.find(n);
        return it == vars.end() ? std::nullopt : std::optional(it->second);
    }
    std::optional<std::pair<int, std::string>> git(const std::vector<std::string>& args) override {
        calls.push_back(args);
        if (args.size() >= 2 && args[0] == "config" && args[args.size() - 2] == "--get") {
            auto it = config.find(args.back());
            return it == config.end() ? std::pair{1, std::string{}} : std::pair{0, it->second + "\n"};
        }
        return std::pair{0, std::string{}};
    }
};

struct Dir {
    fs::path d;
    Dir() {
        d = fs::temp_directory_path() / "splinter-test-adapter";
        fs::remove_all(d);
        fs::create_directories(d);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(d, ec);
    }
    std::string file(const std::string& n, std::string_view s) const {
        std::ofstream(d / n, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
        return (d / n).generic_string();
    }
    std::string read(const std::string& n) const {
        std::ifstream f(d / n, std::ios::binary);
        return {std::istreambuf_iterator<char>(f), {}};
    }
};

} // namespace

TEST_CASE("git adapter: git-diff arguments and exit codes") {
    const auto reg = semsplinter::Registry::with_builtin_formats();
    Dir d;
    auto a = d.file("a", "1\n2\n"), b = d.file("b", "1\n3\n");
    FakeHost h;
    CHECK(splinter::git::git_diff({"x.txt", a, "1111", "100644", b, "2222", "100644"}, h, reg) == 0);
    CHECK(h.out_text ==
          "diff --git a/x.txt b/x.txt\n--- a/x.txt\n+++ b/x.txt\n@@ -1,2 +1,2 @@\n 1\n-2\n+3\n");
    // Trust (D-16): flag, environment, or git config; then 1 for differences.
    FakeHost t1;
    CHECK(splinter::git::git_diff({"--trust-exit-code", "x.txt", a, "1", "100644", b, "2", "100644"}, t1,
                                  reg) == 1);
    FakeHost t2;
    t2.vars["GIT_EXTERNAL_DIFF_TRUST_EXIT_CODE"] = "true";
    CHECK(splinter::git::git_diff({"x.txt", a, "1", "100644", b, "2", "100644"}, t2, reg) == 1);
    FakeHost t3;
    t3.config["diff.mine.trustExitCode"] = "true";
    CHECK(splinter::git::git_diff({"--driver=mine", "x.txt", a, "1", "100644", b, "2", "100644"}, t3, reg) ==
          1);
    // A missing side: /dev/null, or nul from Git for Windows.
    FakeHost n;
    CHECK(splinter::git::git_diff({"y.txt", "nul", "0", "000000", b, "2", "100644"}, n, reg) == 0);
    CHECK(n.out_text.starts_with(
        "diff --git a/y.txt b/y.txt\nnew file mode 100644\n--- /dev/null\n+++ b/y.txt\n"));
    // Unmerged paths get one argument; anything else is trouble (Git aborts on 2).
    FakeHost u;
    CHECK(splinter::git::git_diff({"z.png"}, u, reg) == 0);
    CHECK(u.out_text == "* Unmerged path z.png\n");
    FakeHost bad;
    CHECK(splinter::git::git_diff({"a", "b"}, bad, reg) == 2);
    CHECK(bad.err_text.starts_with("splinter: git-diff: expected 7 or 9 arguments"));
}

TEST_CASE("git adapter: git-merge forms, policy, sidecar") {
    const auto reg = semsplinter::Registry::with_builtin_formats();
    Dir d;
    auto saved = fs::current_path();
    fs::current_path(d.d);
    auto o = d.file("o", "1\n2\n3\n"), a = d.file("a", "1\nA\n3\n"), b = d.file("b", "1\nB\n3\n");
    FakeHost h;
    // %O %A %B %L %P %S %X %Y: labels from %X %S %Y, markers sized by %L, result in %A.
    CHECK(splinter::git::git_merge({o, a, b, "3", "f.txt", "base-c", "local-c", "other-c"}, h, reg) == 1);
    CHECK(d.read("a") == "1\n<<< local-c\nA\n===\nB\n>>> other-c\n3\n");
    CHECK(fs::exists("f.txt.splinter-conflicts.json"));
    CHECK(h.err_text.find("f.txt: 1 conflict (text); wrote conflict markers") != std::string::npos);

    // splinter.conflict from git config; a clean merge removes the stale sidecar.
    d.file("a", "1\nA\n3\n");
    FakeHost c;
    c.config["splinter.conflict"] = "theirs";
    CHECK(splinter::git::git_merge({o, a, b, "7", "f.txt"}, c, reg) == 1);
    CHECK(d.read("a") == "1\nB\n3\n");
    auto clean = d.file("clean", "1\n2\n3\nend\n");
    FakeHost k;
    CHECK(splinter::git::git_merge({"--base", o, "--ours", clean, "--theirs", o, "--path", "f.txt"}, k,
                                   reg) == 0);
    CHECK_FALSE(fs::exists("f.txt.splinter-conflicts.json"));

    // The LFS wrapper's form: no %P, the result goes to --output (%D).
    auto out = (d.d / "dest").generic_string();
    FakeHost l;
    CHECK(splinter::git::git_merge({o, clean, o, "7", "--output", out}, l, reg) == 0);
    CHECK(d.read("dest") == "1\n2\n3\nend\n");
    FakeHost bad;
    CHECK(splinter::git::git_merge({o, a, b, "--conflict=maybe"}, bad, reg) == 2);
    fs::current_path(saved);
}

TEST_CASE("git adapter: git-setup") {
    FakeHost p;
    CHECK(splinter::git::git_setup({"--print", "--program=C:\\Tools\\splinter.exe"}, p) == 0);
    CHECK(p.out_text.starts_with("[diff \"splinter\"]\n\ttextconv = C:/Tools/splinter.exe dump\n"));
    CHECK(p.out_text.find("\tdriver = C:/Tools/splinter.exe git-merge %O %A %B %L %P\n") !=
          std::string::npos);
    CHECK(p.out_text.find("--program 'C:/Tools/splinter.exe git-merge %%O %%A %%B %%L --output %%D'") !=
          std::string::npos);
    FakeHost w;
    CHECK(splinter::git::git_setup({"--global"}, w) == 0);
    REQUIRE(w.calls.size() == 8);
    CHECK(w.calls[3] == std::vector<std::string>{"config", "--global", "merge.splinter.driver",
                                                 "splinter git-merge %O %A %B %L %P"});
    FakeHost bad;
    CHECK(splinter::git::git_setup({"--bogus"}, bad) == 2);
}

TEST_CASE("git adapter: mark on a PNG puts the visual next to %P, not next to %A") {
    const auto reg = semsplinter::Registry::with_builtin_formats();
    Dir d;
    auto saved = fs::current_path();
    fs::current_path(d.d);
    auto img = [&](const char* name, std::uint16_t v) {
        semsplinter::raster::Raster r(4, 4, false);
        r.set(5, {v, 0, 0, 0xFFFF});
        auto b = semsplinter::png::write(r);
        return d.file(name, {reinterpret_cast<const char*>(b.data()), b.size()});
    };
    auto o = img(".merge_file_o", 0), a = img(".merge_file_a", 0xFFFF), b = img(".merge_file_b", 0x8080);
    FakeHost h;
    h.config["splinter.conflict"] = "mark";
    fs::create_directories("art"); // %P's directory exists in a work tree
    CHECK(splinter::git::git_merge({o, a, b, "7", "art/x.png"}, h, reg) == 1);
    CHECK_FALSE(fs::exists(".merge_file_a.splinter-conflicts.png"));
    CHECK(fs::exists("art/x.png.splinter-conflicts.png"));
    CHECK(fs::exists("art/x.png.splinter-conflicts.json"));
    fs::current_path(saved);
}
