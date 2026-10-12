#include <semsplinter/report.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

using namespace semsplinter;
using json = nlohmann::ordered_json;

TEST_CASE("merge report has the SPEC 7.1 shape") {
    Report r;
    r.mode = "merge";
    r.format = "png";
    r.inputs = {{"base", "base.png", hash128("b"), 10}, {"ours", "ours.png", hash128("o"), 11}};
    r.options = {{"guard", 0}, {"conflict", "ours"}};
    auto c = make_change(Side::ours, Op::update, {"/pixels", Rect{10, 20, 32, 8}}, "pixels");
    c.metrics["pixels"] = 212;
    r.changes_ours = {c};
    r.auto_merged = {{c.id, "independent"}};
    r.conflicts = {make_conflict({"/chunks/tEXt:Author"}, {c.id}, "both-modified")};
    r.warnings = {{"derived-stale", "/composite", ""}};
    r.result = ResultInfo{"merged.png", hash128("m"), "conflicts"};

    auto j = to_json(r);
    std::vector<std::string> keys;
    for (auto& [k, v] : j.items())
        keys.push_back(k);
    REQUIRE(keys == std::vector<std::string>{"schema", "tool", "mode", "format", "inputs", "options",
                                             "changes_ours", "changes_theirs", "auto_merged", "conflicts",
                                             "stats", "validation", "result"});
    REQUIRE(j["schema"] == "splinter-report/1");
    REQUIRE(j["inputs"]["ours"]["hash"].get<std::string>().starts_with("xxh3:"));
    REQUIRE(j["changes_ours"][0]["at"] == "/pixels#rect=10,20,32,8");
    REQUIRE(j["changes_ours"][0]["metrics"]["pixels"] == 212);
    REQUIRE(j["conflicts"][0]["resolved_as"] == "ours");
    REQUIRE(j["stats"]["conflicts"] == 1);
    REQUIRE(j["validation"]["warnings"][0]["code"] == "derived-stale");
    REQUIRE(to_json(r).dump() == j.dump()); // deterministic
}

TEST_CASE("diff report uses `changes`") {
    Report r;
    r.mode = "diff";
    r.format = "text";
    auto j = to_json(r);
    REQUIRE(j.contains("changes"));
    REQUIRE_FALSE(j.contains("conflicts"));
}

TEST_CASE("the report JSON Schema is valid JSON") {
    std::ifstream f(SEMSPLINTER_SOURCE_DIR "/docs/schema/splinter-report-1.schema.json");
    REQUIRE(f);
    std::stringstream s;
    s << f.rdbuf();
    auto schema = json::parse(s.str());
    REQUIRE(schema["title"] == "splinter-report/1");
}

TEST_CASE("markdown report: summary table, sections, escaping") {
    Report r;
    r.mode = "merge";
    r.format = "png";
    r.inputs = {{"base", "a|b.png", hash128("b"), 10}};
    auto c = make_change(Side::ours, Op::update, {"/pixels", Rect{1, 2, 3, 4}}, "pixels");
    c.before.text = "x|y\nz";
    c.metrics["pixels"] = 12;
    r.changes_ours = {c};
    r.auto_merged = {{c.id, "independent"}};
    r.conflicts = {make_conflict({"/chunks/tEXt:Author"}, {c.id}, "both-modified")};
    r.envelope = json{{"churn", {{"bytes_changed", 7}, {"content", 5}}}};
    auto m = render_markdown(r);
    REQUIRE(m.starts_with("# splinter merge report\n\n| Field | Value |\n|---|---|\n| Format | png |\n"));
    REQUIRE(m.find("| Input base | a\\|b.png (10 bytes, xxh3:") != std::string::npos);
    REQUIRE(m.find("| Conflicts | 1 |\n") != std::string::npos);
    REQUIRE(m.find("| Churn | bytes\\_changed 7, content 5 |\n") != std::string::npos);
    REQUIRE(m.find("\n## Conflicts\n\n### " + r.conflicts[0].id + ": /chunks/tEXt:Author\n") !=
            std::string::npos);
    REQUIRE(m.find("| Resolved as | ours (unresolved) |\n") != std::string::npos);
    REQUIRE(m.find("\n## Changes (ours)\n\n### " + c.id + ": update /pixels\\#rect=1,2,3,4\n") !=
            std::string::npos);
    REQUIRE(m.find("| Before | x\\|y<br>z |\n") != std::string::npos);
    REQUIRE(m.find("| Auto-merged | independent |\n| pixels | 12 |\n") != std::string::npos);
    REQUIRE(render_markdown(r) == m); // deterministic
}
