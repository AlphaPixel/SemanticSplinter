#pragma once

// The library API (SPEC §10, D-53): diff, merge, patch, dump, and inspect over a Registry. Command-line tools
// and version-control adapters map their arguments onto these requests and print the results; everything else
// (format detection, conflict policy, atomic output, sidecars, reports) happens here.

#include <semsplinter/format.hpp>
#include <semsplinter/patch.hpp>
#include <semsplinter/report.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace semsplinter {

struct Input {
    ByteSource source;
    std::string path; // shown in reports and messages; its extension also guides detection
};

// The format all inputs share (SPEC §11). Any binary input gives "binary". Inputs of different formats give
// "text" when every one is text underneath (no NUL bytes), with a warning in *note; otherwise "binary".
const Format& detect_format(const Registry&, std::initializer_list<const Input*>,
                            const std::string& format = {}, bool force_text = false,
                            std::string* note = nullptr);

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
    bool patch = false;                  // fill DiffResult::patch (--binary), except for text (render -u)
    std::optional<RenderRequest> render; // fill DiffResult::text
};

struct DiffResult {
    std::string format;              // "text" or "binary" when the two sides are of different formats
    bool differs = false;            // exit status 1
    std::string text;                // the rendering, when requested
    std::vector<std::string> errors; // render diagnostics: the diff fails (exit 2) after printing
    std::vector<std::pair<std::string, Warning>> warnings; // (input path, load-time warning)
    std::optional<Report> report;
    std::vector<std::byte> visual;
    std::string patch; // an envelope section (SPEC §6) when the inputs differ
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
    // Where --conflict=mark puts the merge visual of a format without markers: by default
    // <output>.splinter-conflicts.png; an empty path writes none.
    std::optional<std::filesystem::path> mark_visual;
};

struct MergeOutcome {
    std::string format;
    std::vector<std::byte> bytes; // the merged file (also written to MergeRequest::output)
    std::size_t conflicts = 0;    // MergeResult::conflict_count(): drives the exit status
    std::vector<Warning> warnings;
    Report report;
};

// With --conflict=mark, a format without in-band markers also writes its merge visual (png.md §6.6), next to
// the output unless MergeRequest::mark_visual says otherwise.
MergeOutcome merge(const Registry&, const MergeRequest&);

// GNU patch's backup options (-b, -V, -z, -B, -Y, --backup-if-mismatch).
struct BackupOptions {
    bool always = false;     // -b
    bool if_mismatch = true; // GNU's default unless POSIXLY_CORRECT
    enum class Method : std::uint8_t { simple, numbered, existing } method = Method::simple;
    std::string suffix = ".orig", prefix, basename_prefix;
};

struct PatchRequest {
    std::string patch;                      // envelope sections and unified diffs, mixed
    std::optional<std::string> target;      // ORIGFILE: every section applies to it
    std::filesystem::path directory;        // -d: section paths resolve against it
    std::optional<std::size_t> strip;       // -p; unset: names are used whole
    std::optional<std::string> output;      // -o; "-" returns the result in PatchOutcome::output
    std::optional<std::string> reject_file; // -r; "-" discards rejects
    bool reverse = false;                   // -R
    TextApplyOptions text;                  // -F, -l, -N / -t / -f
    bool dry_run = false;
    bool remove_empty = false; // -E
    bool mark = false;         // --merge: conflicts as --conflict=mark instead of rejects
    BackupOptions backup;
    Limits limits;
    OptionValues format_options;
};

struct PatchOutcome {
    std::string messages;          // GNU patch's report: "patching file x", "Hunk #2 FAILED at 7." ...
    std::vector<std::byte> output; // the patched file for -o -
    std::size_t failed = 0;        // failed or ignored hunks: exit status 1
    std::vector<Warning> warnings;
};

// Applies a patch (SPEC §4, §6): unified sections as GNU patch does, envelope sections by base hash, else by
// identity as a three-way merge whose conflicts are rejects. Writes files atomically; throws Error on
// trouble.
PatchOutcome apply_patch(const Registry&, const PatchRequest&);

// The canonical dump (textconv).
std::string dump(const Registry&, const Input&, const std::string& format = {}, bool force_text = false,
                 const Limits& = {}, const OptionValues& = {});

// Structure overview: format, size, hash, the format's inspect() fields, and with `layout` the EnvelopeMap.
nlohmann::ordered_json inspect(const Registry&, const Input&, const std::string& format = {},
                               bool layout = false);
std::string render_inspect(const nlohmann::ordered_json&); // "key value" lines, then the layout table

} // namespace semsplinter
