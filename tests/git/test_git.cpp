// End-to-end Git integration (CLI.md §7, PLAN Phase 4): real repositories, real git, the built splinter as
// the diff and merge driver. Skipped when git (or git-lfs) is not installed.
#include "../scratch.hpp"
#include "cli.hpp"

#include <semsplinter/formats/png.hpp>
#include <semsplinter/semsplinter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace semsplinter;
using splinter::cli::run_process;

namespace {

bool have(std::vector<std::string> argv) {
    auto r = run_process(argv);
    return r && r->status == 0;
}

// "git version 2.43.0" -> 243 (major * 100 + minor); 0 when unknown.
int git_version() {
    auto r = run_process({"git", "--version"});
    const std::string_view prefix = "git version ";
    if (!r || !r->out.starts_with(prefix))
        return 0;
    const char *p = r->out.data() + prefix.size(), *end = r->out.data() + r->out.size();
    int major = 0, minor = 0;
    auto [q, e1] = std::from_chars(p, end, major);
    if (e1 != std::errc{} || q == end || *q != '.' || std::from_chars(q + 1, end, minor).ec != std::errc{})
        return 0;
    return major * 100 + minor;
}

std::vector<std::byte> png_of(const std::vector<std::array<std::uint32_t, 5>>& paint) {
    raster::Raster r(48, 32, false);
    for (std::uint64_t i = 0; i < r.cells(); ++i)
        r.set(i, {static_cast<std::uint16_t>((i % 48) * 1000), static_cast<std::uint16_t>((i / 48) * 1500),
                  0x4000, 0xFFFF});
    for (auto [x0, y0, w, h, red] : paint)
        for (auto y = y0; y < y0 + h; ++y)
            for (auto x = x0; x < x0 + w; ++x)
                r.set(std::uint64_t{y} * 48 + x, {static_cast<std::uint16_t>(red), 0, 0, 0xFFFF});
    return png::write(r);
}

// A temporary repository that is also the current directory, as Git runs drivers from the work-tree top.
struct Repo {
    fs::path dir, saved;
    explicit Repo(const std::string& name) {
        dir = scratch_path("git-" + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        saved = fs::current_path();
        fs::current_path(dir);
        git({"init", "-q", "-b", "main"});
        for (auto [k, v] : {std::pair{"user.name", "t"},
                            {"user.email", "t@example.invalid"},
                            {"core.autocrlf", "false"},
                            {"commit.gpgsign", "false"}})
            git({"config", k, v});
        REQUIRE(run_process({SPLINTER_EXE, "git-setup", "--local", "--program=" SPLINTER_EXE})->status == 0);
    }
    ~Repo() {
        fs::current_path(saved);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    int git(std::vector<std::string> args, std::string* out = nullptr) {
        args.insert(args.begin(), "git");
        auto r = run_process(args);
        REQUIRE(r);
        if (out)
            *out = r->out;
        return r->status;
    }
    std::string out(std::vector<std::string> args) {
        std::string o;
        git(std::move(args), &o);
        return o;
    }
    static void write(const std::string& name, std::string_view text) {
        std::ofstream(name, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    static void write(const std::string& name, const std::vector<std::byte>& b) {
        write(name, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    }
    void commit(const std::string& msg) {
        REQUIRE(git({"add", "-A"}) == 0);
        REQUIRE(git({"commit", "-q", "-m", msg}) == 0);
    }
};

// Same pixels and metadata, whatever the bytes.
bool same_image(const Registry& reg, const std::string& file, const std::vector<std::byte>& expect) {
    Limits limits;
    DiffRequest rq;
    rq.a = {ByteSource::from_file(file, limits), file};
    rq.b = {ByteSource::from_memory(expect, "expect.png"), "expect.png"};
    return !semsplinter::diff(reg, rq).differs;
}

} // namespace

TEST_CASE("git: PNG merges through the splinter merge driver", "[git]") {
    if (!have({"git", "--version"}))
        SKIP("git is not installed");
    const auto reg = Registry::with_builtin_formats();
    Repo r("merge");
    Repo::write(".gitattributes", "*.png diff=splinter merge=splinter\n");
    Repo::write("img.png", png_of({}));
    r.commit("base");
    const auto base = r.out({"rev-parse", "HEAD"}).substr(0, 40);

    // Disjoint edits: a clean merge commits without intervention.
    REQUIRE(r.git({"checkout", "-q", "-b", "theirs"}) == 0);
    Repo::write("img.png", png_of({{40, 24, 4, 4, 0xFFFF}}));
    r.commit("theirs");
    REQUIRE(r.git({"checkout", "-q", "main"}) == 0);
    Repo::write("img.png", png_of({{2, 2, 6, 6, 0x8000}}));
    r.commit("ours");
    CHECK(r.git({"merge", "-q", "--no-edit", "theirs"}) == 0);
    CHECK(r.out({"rev-list", "--count", "HEAD"}) == "4\n");
    CHECK(same_image(reg, "img.png", png_of({{2, 2, 6, 6, 0x8000}, {40, 24, 4, 4, 0xFFFF}})));
    CHECK_FALSE(fs::exists("img.png.splinter-conflicts.json"));

    // textconv: git log -p shows dump lines.
    auto log = r.out({"log", "-p", "-1", "theirs", "--", "img.png"});
    CHECK(log.find("+pixels xxh3=") != std::string::npos);

    // Overlapping edits: conflict, a valid PNG holding ours, a sidecar, and an unmerged index entry.
    const auto merged = r.out({"rev-parse", "HEAD"}).substr(0, 40);
    REQUIRE(r.git({"checkout", "-q", "-b", "clash", base}) == 0);
    Repo::write("img.png", png_of({{4, 4, 2, 2, 0x1234}}));
    r.commit("clash");
    REQUIRE(r.git({"checkout", "-q", "main"}) == 0);
    CHECK(r.git({"merge", "-q", "--no-edit", "clash"}) != 0);
    CHECK(r.out({"ls-files", "-u", "--", "img.png"}).find(" 2\timg.png") != std::string::npos);
    CHECK(same_image(reg, "img.png", png_of({{2, 2, 6, 6, 0x8000}, {40, 24, 4, 4, 0xFFFF}})));
    REQUIRE(fs::exists("img.png.splinter-conflicts.json"));
    std::ifstream sc("img.png.splinter-conflicts.json");
    auto j = nlohmann::json::parse(sc);
    sc.close();
    CHECK(j["format"] == "png");
    CHECK(j["conflicts"].size() == 1);
    CHECK(j["conflicts"][0]["reason"] == "both-modified");

    // git merge --abort restores the pre-merge state.
    CHECK(r.git({"merge", "--abort"}) == 0);
    CHECK(r.out({"rev-parse", "HEAD"}).substr(0, 40) == merged);
    CHECK(r.out({"status", "--porcelain", "--untracked-files=no"}).empty());
}

TEST_CASE("git: text conflicts get markers of the requested size", "[git]") {
    if (!have({"git", "--version"}))
        SKIP("git is not installed");
    Repo r("text");
    Repo::write(".gitattributes", "*.txt merge=splinter conflict-marker-size=9\n");
    Repo::write("a.txt", "1\n2\n3\n");
    r.commit("base");
    REQUIRE(r.git({"checkout", "-q", "-b", "other"}) == 0);
    Repo::write("a.txt", "1\nB\n3\n");
    r.commit("other");
    REQUIRE(r.git({"checkout", "-q", "main"}) == 0);
    Repo::write("a.txt", "1\nA\n3\n");
    r.commit("main");
    CHECK(r.git({"merge", "-q", "--no-edit", "other"}) != 0);
    std::ifstream f("a.txt", std::ios::binary);
    std::string s{std::istreambuf_iterator<char>(f), {}};
    CHECK(s.find("<<<<<<<<< ours\nA\n=========\nB\n>>>>>>>>> theirs\n") != std::string::npos);
}

TEST_CASE("git: the external diff driver", "[git]") {
    if (!have({"git", "--version"}))
        SKIP("git is not installed");
    Repo r("diff");
    REQUIRE(r.git({"config", "diff.splinter.command", SPLINTER_EXE " git-diff"}) == 0);
    Repo::write(".gitattributes", "*.png diff=splinter\n*.txt diff=splinter\n");
    Repo::write("img.png", png_of({}));
    std::string lines;
    for (int i = 1; i <= 40; ++i)
        lines += std::to_string(i) + "\n";
    Repo::write("notes.txt", lines);
    r.commit("base");
    Repo::write("img.png", png_of({{10, 10, 3, 3, 0xFFFF}}));
    REQUIRE(r.git({"mv", "notes.txt", "renamed.txt"}) == 0);
    lines.replace(lines.find("\n20\n"), 4, "\ntwenty\n");
    Repo::write("renamed.txt", lines);
    r.commit("change");

    std::string out;
    CHECK(r.git({"diff", "-M", "--ext-diff", "HEAD~1", "HEAD"}, &out) == 0);
    CHECK(out.find("diff --git a/img.png b/img.png\n~ /pixels#rect=10,10,3,3 ") != std::string::npos);
    CHECK(out.find("# bytes changed ") != std::string::npos);
    CHECK(out.find("diff --git a/notes.txt b/renamed.txt\n") != std::string::npos);
    CHECK(out.find("rename from notes.txt\nrename to renamed.txt\n") != std::string::npos);
    CHECK(out.find("--- a/notes.txt\n+++ b/renamed.txt\n") != std::string::npos);
    // D-16: untrusted, differences exit 0; trusted (Git 2.46+), git diff --exit-code reports them. Older Git
    // has no trustExitCode and stops at the first non-zero external diff.
    if (git_version() >= 246)
        CHECK(r.git({"-c", "diff.splinter.trustExitCode=true", "diff", "--ext-diff", "--exit-code", "HEAD~1",
                     "HEAD", "--", "img.png"}) == 1);
}

// #53: a fixed name collided when two suites (worktrees, ctest -j) ran at once.
TEST_CASE("git: each test process gets its own repository directory", "[git]") {
    CHECK(scratch_path("git-lfs") != scratch_path("git-lfs"));
}

TEST_CASE("git: the Git LFS merge-driver wrapper", "[git][lfs]") {
    if (!have({"git", "--version"}) || !have({"git", "lfs", "version"}))
        SKIP("git-lfs is not installed");
    const auto reg = Registry::with_builtin_formats();
    Repo r("lfs");
    REQUIRE(r.git({"lfs", "install", "--local"}) == 0);
    Repo::write(".gitattributes", "*.png filter=lfs diff=splinter merge=splinter-lfs -text\n");
    Repo::write("img.png", png_of({}));
    r.commit("base");
    REQUIRE(r.git({"checkout", "-q", "-b", "theirs"}) == 0);
    Repo::write("img.png", png_of({{40, 24, 4, 4, 0xFFFF}}));
    r.commit("theirs");
    REQUIRE(r.git({"checkout", "-q", "main"}) == 0);
    Repo::write("img.png", png_of({{2, 2, 6, 6, 0x8000}}));
    r.commit("ours");
    CHECK(r.git({"merge", "-q", "--no-edit", "theirs"}) == 0);
    CHECK(same_image(reg, "img.png", png_of({{2, 2, 6, 6, 0x8000}, {40, 24, 4, 4, 0xFFFF}})));
    CHECK(r.out({"lfs", "ls-files"}).find("img.png") != std::string::npos);
}
