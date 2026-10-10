// Golden comparisons against GNU diffutils and Git (tests/golden/generate.py records them).
#include "../scratch.hpp"
#include "console.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace fs = std::filesystem;

namespace {

nlohmann::json load(const char* name) {
    std::ifstream f(fs::path(SEMSPLINTER_SOURCE_DIR) / "tests" / "golden" / name, std::ios::binary);
    REQUIRE(f);
    return nlohmann::json::parse(f);
}

// The corpus stores bytes as Latin-1 code points; turn them back into bytes.
std::string bytes(const nlohmann::json& s) {
    std::string out;
    auto u = s.get<std::string>(); // UTF-8 from the JSON parser
    for (std::size_t i = 0; i < u.size(); ++i) {
        auto c = static_cast<unsigned char>(u[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else { // two-byte sequence for U+0080..U+00FF
            out += static_cast<char>(((c & 0x1F) << 6) | (static_cast<unsigned char>(u[++i]) & 0x3F));
        }
    }
    return out;
}

// Runs where splinter deliberately differs from the oracle (Q-TEXT-1), as "file case options-json".
const std::set<std::string> kKnown = {
    // Q-TEXT-8: side-by-side counts UTF-8 code points; GNU in the C locale counts the BOM's three bytes.
    R"(diff.json 12 ["-y","-t","-W","50"])",
    // GNU diff3 3.12 -A/-E ed scripts and two -m outputs contradict its own "====" report for the same inputs
    // (non-overlapping changes next to a conflict); splinter follows the report.
    R"(diff3.json 2 ["-A"])",
    R"(diff3.json 2 ["-A","-i"])",
    R"(diff3.json 2 ["-E"])",
    R"(diff3.json 9 ["-A"])",
    R"(diff3.json 9 ["-A","-i"])",
    R"(diff3.json 9 ["-E"])",
    R"(diff3.json 9 ["-m"])",
    R"(diff3.json 9 ["-m","-E"])",
    R"(diff3.json 14 ["-A"])",
    R"(diff3.json 14 ["-A","-i"])",
    R"(diff3.json 14 ["-E"])",
    R"(diff3.json 17 ["-A"])",
    R"(diff3.json 17 ["-A","-i"])",
    R"(diff3.json 17 ["-E"])",
    R"(diff3.json 22 ["-A"])",
    R"(diff3.json 22 ["-A","-i"])",
    R"(diff3.json 22 ["-E"])",
    R"(diff3.json 34 ["-A"])",
    R"(diff3.json 34 ["-A","-i"])",
    R"(diff3.json 34 ["-E"])",
};

// Runs every case of a corpus file; `command` builds the argument list from the case's options.
template <class F> void check_corpus(const char* name, F command) {
    auto corpus = load(name);
    auto dir = scratch_path(std::string("golden-") + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::size_t runs = 0, failures = 0;
    for (std::size_t k = 0; k < corpus.size(); ++k) {
        auto& c = corpus[k];
        std::map<std::string, std::string> paths;
        for (auto& [file, content] : c["files"].items()) {
            auto p = dir / file;
            std::ofstream(p, std::ios::binary) << bytes(content);
            paths[file] = splinter::cli::from_path(p);
        }
        for (auto& r : c["runs"]) {
            std::vector<std::string> opts = r["args"];
            if (kKnown.count(std::string(name) + " " + std::to_string(k) + " " + nlohmann::json(opts).dump()))
                continue;
            CaptureConsole con;
            int rc = run_cli(con, command(opts, paths));
            ++runs;
            if (rc != r["rc"].get<int>() || con.out_text != bytes(r["stdout"])) {
                ++failures;
                INFO(name << " case " << k << " options " << nlohmann::json(opts).dump());
                CHECK(rc == r["rc"].get<int>());
                CHECK(con.out_text == bytes(r["stdout"]));
            }
        }
    }
    INFO(name << ": " << failures << " of " << runs << " runs differ");
    CHECK(failures == 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

TEST_CASE("golden: GNU diff") {
    check_corpus("diff.json", [](std::vector<std::string> o, auto& p) {
        o.insert(o.end(), {"--label", "A", "--label", "B", p.at("a"), p.at("b")});
        return o;
    });
}

TEST_CASE("golden: GNU diff3") {
    check_corpus("diff3.json", [](std::vector<std::string> o, auto& p) {
        o.insert(o.begin(), "diff3");
        o.insert(o.end(), {"-L", "M", "-L", "O", "-L", "Y", p.at("mine"), p.at("older"), p.at("yours")});
        return o;
    });
}

TEST_CASE("golden: git merge-file") {
    check_corpus("merge-file.json", [](std::vector<std::string> o, auto& p) {
        o.insert(o.begin(), {"merge-file", "-p"});
        o.insert(o.end(), {"-L", "M", "-L", "O", "-L", "Y", p.at("mine"), p.at("older"), p.at("yours")});
        return o;
    });
}
