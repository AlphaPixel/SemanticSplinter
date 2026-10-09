#pragma once

// The library API (SPEC §10, D-53): diff, merge, dump, and inspect over a Registry. Command-line tools and
// version-control adapters map their arguments onto these requests and print the results; everything else
// (format detection, conflict policy, atomic output, sidecars, reports) happens here.

#include <semsplinter/format.hpp>
#include <semsplinter/report.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace semsplinter {

struct Input {
    ByteSource source;
    std::string path; // shown in reports and messages; its extension also guides detection
};

struct DiffRequest {
    Input a, b;
    std::string format;      // --format; empty: detect (SPEC §11)
    bool force_text = false; // -a
    Limits limits;
    OptionValues format_options;
    TextLoadOptions text_load;
    TextDiffOptions text;
    bool report = false;                 // fill DiffResult::report (--json)
    bool visual = false;                 // fill DiffResult::visual (--visual)
    std::optional<RenderRequest> render; // fill DiffResult::text
};

struct DiffResult {
    std::string format;              // "binary" when the two sides are of different formats
    bool differs = false;            // exit status 1
    std::string text;                // the rendering, when requested
    std::vector<std::string> errors; // render diagnostics: the diff fails (exit 2) after printing
    std::vector<std::pair<std::string, Warning>> warnings; // (input path, load-time warning)
    std::optional<Report> report;
    std::vector<std::byte> visual;
};

DiffResult diff(const Registry&, const DiffRequest&);

struct MergeRequest {
    Input base, ours, theirs;
    std::string format;
    bool force_text = false;
    bool text_only = false; // git merge-file: anything but text is an error
    Limits limits;
    OptionValues format_options;
    TextLoadOptions text_load;
    TextMergeOptions text;
    ConflictOptions conflict;
    std::array<std::string, 3> labels;                   // ours, base, theirs
    std::optional<std::filesystem::path> output;         // written atomically (SPEC §9)
    std::string output_name = "-";                       // the result path recorded in the report
    std::optional<std::filesystem::path> visual;         // --visual
    std::optional<std::filesystem::path> conflicts_file; // --conflicts: the JSON report
};

struct MergeOutcome {
    std::string format;
    std::vector<std::byte> bytes; // the merged file (also written to MergeRequest::output)
    std::size_t conflicts = 0;    // MergeResult::conflict_count(): drives the exit status
    std::vector<Warning> warnings;
    Report report;
};

// With --conflict=mark and an output path, a format without in-band markers also writes its merge
// visual next to the output as <output>.splinter-conflicts.png (png.md §6.6).
MergeOutcome merge(const Registry&, const MergeRequest&);

// The canonical dump (textconv).
std::string dump(const Registry&, const Input&, const std::string& format = {}, bool force_text = false,
                 const Limits& = {}, const OptionValues& = {});

// Structure overview: format, size, hash, the format's inspect() fields, and with `layout` the EnvelopeMap.
nlohmann::ordered_json inspect(const Registry&, const Input&, const std::string& format = {},
                               bool layout = false);
std::string render_inspect(const nlohmann::ordered_json&); // "key value" lines, then the layout table

} // namespace semsplinter
