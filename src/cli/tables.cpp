#include "tables.hpp"

namespace splinter::cli {

namespace {

using enum Arg;

// GNU diff 3.12 (including its undocumented --forward-ed, --inhibit-hunk-merge, --sdiff-merge-assist, -L,
// -H).
const Opt kGnuDiff[] = {
    {"normal", 0, none, o_normal},
    {"brief", 'q', none, o_brief},
    {"report-identical-files", 's', none, o_report_identical},
    {"", 'c', none, o_context_c},
    {"", 'C', required, o_context_n},
    {"context", 0, optional, o_context},
    {"", 'u', none, o_unified_u},
    {"", 'U', required, o_unified_n},
    {"unified", 0, optional, o_unified},
    {"ed", 'e', none, o_ed},
    {"forward-ed", 'f', none, o_forward_ed},
    {"rcs", 'n', none, o_rcs},
    {"side-by-side", 'y', none, o_side_by_side},
    {"width", 'W', required, o_width},
    {"left-column", 0, none, o_left_column},
    {"suppress-common-lines", 0, none, o_suppress_common},
    {"show-c-function", 'p', none, o_show_c_function},
    {"show-function-line", 'F', required, o_show_function_line},
    {"label", 'L', required, o_label},
    {"expand-tabs", 't', none, o_expand_tabs},
    {"initial-tab", 'T', none, o_initial_tab},
    {"tabsize", 0, required, o_tabsize},
    {"suppress-blank-empty", 0, none, o_suppress_blank_empty},
    {"paginate", 'l', none, o_paginate},
    {"recursive", 'r', none, o_recursive},
    {"no-dereference", 0, none, o_no_dereference},
    {"new-file", 'N', none, o_new_file},
    {"unidirectional-new-file", 0, none, o_unidirectional_new_file},
    {"ignore-file-name-case", 0, none, o_ignore_file_name_case},
    {"no-ignore-file-name-case", 0, none, o_no_ignore_file_name_case},
    {"exclude", 'x', required, o_exclude},
    {"exclude-from", 'X', required, o_exclude_from},
    {"starting-file", 'S', required, o_starting_file},
    {"from-file", 0, required, o_from_file},
    {"to-file", 0, required, o_to_file},
    {"ignore-case", 'i', none, o_ignore_case},
    {"ignore-tab-expansion", 'E', none, o_ignore_tab_expansion},
    {"ignore-trailing-space", 'Z', none, o_ignore_trailing_space},
    {"ignore-space-change", 'b', none, o_ignore_space_change},
    {"ignore-all-space", 'w', none, o_ignore_all_space},
    {"ignore-blank-lines", 'B', none, o_ignore_blank_lines},
    {"ignore-matching-lines", 'I', required, o_ignore_matching_lines},
    {"text", 'a', none, o_text},
    {"strip-trailing-cr", 0, none, o_strip_trailing_cr},
    {"binary", 0, none, o_binary},
    {"ifdef", 'D', required, o_ifdef},
    {"old-group-format", 0, required, o_group_format},
    {"new-group-format", 0, required, o_group_format},
    {"unchanged-group-format", 0, required, o_group_format},
    {"changed-group-format", 0, required, o_group_format},
    {"line-format", 0, required, o_line_format},
    {"old-line-format", 0, required, o_line_format},
    {"new-line-format", 0, required, o_line_format},
    {"unchanged-line-format", 0, required, o_line_format},
    {"minimal", 'd', none, o_minimal},
    {"horizon-lines", 0, required, o_horizon_lines},
    {"speed-large-files", 'H', none, o_speed_large_files},
    {"inhibit-hunk-merge", 0, none, o_inhibit_hunk_merge},
    {"sdiff-merge-assist", 0, none, o_sdiff_merge_assist},
    {"color", 0, optional, o_color},
    {"palette", 0, required, o_palette},
    {"help", 0, none, o_help},
    {"version", 'v', none, o_version},
    {"", '0', none, o_digit},
    {"", '1', none, o_digit},
    {"", '2', none, o_digit},
    {"", '3', none, o_digit},
    {"", '4', none, o_digit},
    {"", '5', none, o_digit},
    {"", '6', none, o_digit},
    {"", '7', none, o_digit},
    {"", '8', none, o_digit},
    {"", '9', none, o_digit},
};

// GNU diff3 3.12.
const Opt kGnuDiff3[] = {
    {"show-all", 'A', none, o_show_all},
    {"ed", 'e', none, o_ed},
    {"show-overlap", 'E', none, o_show_overlap},
    {"easy-only", '3', none, o_easy_only},
    {"overlap-only", 'x', none, o_overlap_only},
    {"", 'X', none, o_overlap_only_bracket},
    {"", 'i', none, o_write_quit},
    {"merge", 'm', none, o_merge},
    {"text", 'a', none, o_text},
    {"strip-trailing-cr", 0, none, o_strip_trailing_cr},
    {"initial-tab", 'T', none, o_initial_tab},
    {"diff-program", 0, required, o_diff_program},
    {"label", 'L', required, o_label},
    {"help", 0, none, o_help},
    {"version", 'v', none, o_version},
};

// RCS merge plus the git merge-file options CLI.md §4 adds.
const Opt kMerge[] = {
    {"", 'A', none, o_show_all},
    {"", 'E', none, o_show_overlap},
    {"", 'e', none, o_ed},
    {"", 'p', none, o_stdout},
    {"quiet", 'q', none, o_quiet},
    {"", 'L', required, o_label},
    {"diff3", 0, none, o_diff3_style},
    {"zdiff3", 0, none, o_zdiff3_style},
    {"marker-size", 0, required, o_marker_size},
    {"object-id", 0, none, o_object_id},
    {"diff-algorithm", 0, required, o_diff_algorithm},
    {"eol", 0, required, o_eol},
    {"help", 0, none, o_help},
    {"version", 'v', none, o_version},
};

// git merge-file (Git 2.4x).
const Opt kMergeFile[] = {
    {"stdout", 'p', none, o_stdout},
    {"no-stdout", 0, none, o_no_stdout},
    {"object-id", 0, none, o_object_id},
    {"no-object-id", 0, none, o_object_id},
    {"diff3", 0, none, o_diff3_style},
    {"no-diff3", 0, none, o_no_style},
    {"zdiff3", 0, none, o_zdiff3_style},
    {"no-zdiff3", 0, none, o_no_style},
    {"ours", 0, none, o_ours},
    {"no-ours", 0, none, o_no_ours},
    {"theirs", 0, none, o_theirs},
    {"no-theirs", 0, none, o_no_theirs},
    {"union", 0, none, o_union},
    {"no-union", 0, none, o_no_union},
    {"diff-algorithm", 0, required, o_diff_algorithm},
    {"marker-size", 0, required, o_marker_size},
    {"no-marker-size", 0, none, o_marker_size},
    {"quiet", 'q', none, o_quiet},
    {"no-quiet", 0, none, o_no_quiet},
    {"", 'L', required, o_label},
    {"help", 0, none, o_help},
};

// GNU patch 2.8 (Phase 5 implements the personality; the table exists for the D-42 check).
const Opt kGnuPatch[] = {
    {"strip", 'p', required, o_patch_any},
    {"fuzz", 'F', required, o_patch_any},
    {"ignore-whitespace", 'l', none, o_patch_any},
    {"context", 'c', none, o_patch_any},
    {"ed", 'e', none, o_patch_any},
    {"normal", 'n', none, o_patch_any},
    {"unified", 'u', none, o_patch_any},
    {"forward", 'N', none, o_patch_any},
    {"reverse", 'R', none, o_patch_any},
    {"input", 'i', required, o_patch_any},
    {"output", 'o', required, o_patch_any},
    {"reject-file", 'r', required, o_patch_any},
    {"ifdef", 'D', required, o_patch_any},
    {"merge", 0, optional, o_patch_any},
    {"remove-empty-files", 'E', none, o_patch_any},
    {"set-utc", 'Z', none, o_patch_any},
    {"set-time", 'T', none, o_patch_any},
    {"quoting-style", 0, required, o_patch_any},
    {"backup", 'b', none, o_patch_any},
    {"backup-if-mismatch", 0, none, o_patch_any},
    {"no-backup-if-mismatch", 0, none, o_patch_any},
    {"version-control", 'V', required, o_patch_any},
    {"prefix", 'B', required, o_patch_any},
    {"basename-prefix", 'Y', required, o_patch_any},
    {"suffix", 'z', required, o_patch_any},
    {"get", 'g', required, o_patch_any},
    {"batch", 't', none, o_patch_any},
    {"force", 'f', none, o_patch_any},
    {"quiet", 's', none, o_patch_any},
    {"silent", 0, none, o_patch_any},
    {"verbose", 0, none, o_patch_any},
    {"dry-run", 0, none, o_patch_any},
    {"posix", 0, none, o_patch_any},
    {"directory", 'd', required, o_patch_any},
    {"reject-format", 0, required, o_patch_any},
    {"binary", 0, none, o_patch_any},
    {"read-only", 0, required, o_patch_any},
    {"follow-symlinks", 0, none, o_patch_any},
    {"", 'x', required, o_patch_any},
    {"help", 0, none, o_help},
    {"version", 'v', none, o_version},
};

// Splinter-only options (CLI.md §2.2, §3), checked against the GNU tables by the D-42 test.
const Opt kLimits[] = {
    {"max-memory", 0, required, o_max_memory},
    {"max-pixels", 0, required, o_max_pixels},
    {"max-input", 0, required, o_max_input},
    {"max-depth", 0, required, o_max_depth},
    {"max-zip-entries", 0, required, o_max_zip_entries},
    {"max-zip-ratio", 0, required, o_max_zip_ratio},
    {"max-zip-size", 0, required, o_max_zip_size},
    {"exit-codes", 0, required, o_exit_codes},
    {"format", 0, required, o_format},
    {"json", 0, optional, o_json},
    {"md", 0, optional, o_md},
    {"tolerance", 0, required, o_tolerance},
    {"integrity", 0, required, o_integrity},
    {"show", 0, required, o_show},
    {"lenient", 0, none, o_lenient},
};

const Opt kDiffOnly[] = {
    {"visual", 0, required, o_visual},
    {"dump", 0, none, o_dump},
    {"semantic", 0, none, o_semantic},
    {"raw", 0, none, o_raw},
    {"gap", 0, required, o_gap},
    {"annotate", 0, none, o_annotate},
    {"ignore", 0, required, o_ignore},
    {"verbose", 0, none, o_verbose},
    {"quiet", 0, none, o_quiet},
    {"verify-key", 0, required, o_verify_key},
    {"scan-streams", 0, none, o_scan_streams},
    {"exact", 0, none, o_exact},
    {"diff-algorithm", 0, required, o_diff_algorithm},
};

const Opt kMergeExtras[] = {
    {"output", 'o', required, o_output},
    {"conflict", 0, required, o_conflict},
    {"ours", 0, none, o_ours},
    {"theirs", 0, none, o_theirs},
    {"union", 0, none, o_union},
    {"resolve", 0, required, o_resolve},
    {"conflicts", 0, required, o_conflicts},
    {"layout", 0, required, o_layout},
    {"signatures", 0, required, o_signatures},
    {"sign-key", 0, required, o_sign_key},
    {"sign-cert", 0, required, o_sign_cert},
    {"sign-pass-file", 0, required, o_sign_pass_file},
    {"sign-command", 0, required, o_sign_command},
    {"mac-key", 0, required, o_mac_key},
    {"guard", 0, required, o_guard},
    {"gap", 0, required, o_gap},
    {"visual", 0, required, o_visual},
    {"verify-key", 0, required, o_verify_key},
    {"verbose", 0, none, o_verbose},
    {"conflict-style", 0, required, o_conflict_style},
};

const Opt kDiff3Extras[] = {
    {"marker-size", 0, required, o_marker_size},
    {"eol", 0, required, o_eol},
};

void add(std::vector<Opt>& t, std::span<const Opt> s) {
    t.insert(t.end(), s.begin(), s.end());
}

} // namespace

std::vector<Opt> table(Personality p, const semsplinter::Registry& reg) {
    std::vector<Opt> t;
    switch (p) {
    case Personality::diff:
        add(t, kGnuDiff), add(t, kLimits), add(t, kDiffOnly);
        break;
    case Personality::diff3:
        add(t, kGnuDiff3), add(t, kLimits), add(t, kMergeExtras), add(t, kDiff3Extras);
        break;
    case Personality::merge:
        add(t, kMerge), add(t, kLimits), add(t, kMergeExtras);
        break;
    case Personality::merge_file:
        add(t, kMergeFile);
        return t; // exact git merge-file surface
    case Personality::patch:
        add(t, kGnuPatch), add(t, kLimits);
        return t;
    }
    // Format-declared options (D-51), numbered in registry order.
    int k = 0;
    for (auto& f : reg.formats())
        for (auto& o : f->info().options) {
            if (!o.merge_only || p != Personality::diff)
                t.push_back({o.name, 0,
                             o.arg == semsplinter::OptionSpec::Arg::none       ? none
                             : o.arg == semsplinter::OptionSpec::Arg::required ? required
                                                                               : optional,
                             o_format_option + k});
            ++k;
        }
    return t;
}

} // namespace splinter::cli
