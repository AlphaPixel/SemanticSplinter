#pragma once

#include <semsplinter/hash.hpp>
#include <semsplinter/model.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace semsplinter {

inline constexpr const char* kReportSchema = "splinter-report/1";

struct InputInfo {
    std::string role; // "a"/"b" for diffs; "base"/"ours"/"theirs" for merges
    std::string path;
    Hash128 hash;
    std::uint64_t bytes = 0;
};

struct ResultInfo {
    std::string path;
    Hash128 hash;
    std::string status; // "clean" or "conflicts"
};

// The JSON report (SPEC §7.1). A diff (mode "diff") fills `changes`; a merge fills the rest.
struct Report {
    std::string mode;
    std::string format;
    std::vector<InputInfo> inputs;
    nlohmann::ordered_json options = nlohmann::ordered_json::object();
    ChangeSet changes;
    ChangeSet changes_ours, changes_theirs;
    std::vector<AutoMerge> auto_merged;
    std::vector<Conflict> conflicts;
    nlohmann::ordered_json format_stats = nlohmann::ordered_json::object();
    std::optional<nlohmann::ordered_json> envelope; // SPEC §12.6: integrity, signatures, churn
    bool valid = true;                              // output passed validation
    std::vector<Warning> warnings;
    std::optional<ResultInfo> result;
};

std::string hash_ref(const Hash128&); // "xxh3:<32 hex>"

nlohmann::ordered_json to_json(const Change&);
nlohmann::ordered_json to_json(const Conflict&);
nlohmann::ordered_json to_json(const Report&);

// The report as Markdown (SPEC §7.2, D-22): a summary table, then one section per conflict and changed unit.
std::string render_markdown(const Report&);

} // namespace semsplinter
