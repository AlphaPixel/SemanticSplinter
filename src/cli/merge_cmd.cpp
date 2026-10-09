// diff3, merge (RCS), and merge-file (Git) personalities (CLI.md §3, §4; text.md §6).
#include "cli.hpp"

#include <semsplinter/formats/binary.hpp>
#include <semsplinter/formats/text.hpp>
#include <semsplinter/report.hpp>
#include <semsplinter/version.hpp>

#include <fstream>
#include <sstream>

namespace splinter::cli {

using namespace semsplinter;

namespace {

struct Config {
    Personality personality = Personality::diff3;
    std::vector<std::string> labels;
    ConflictOptions conflict{Policy::mark};
    std::optional<std::string> output;
    bool to_stdout = false; // merge / merge-file: -p
    std::optional<std::string> conflicts_file;
    CommonOptions common;
    bool force_text = false, strip_cr = false, initial_tab = false, quiet = false, write_quit = false;
    bool merged_output = false; // diff3 -m
    bool mode_set = false;
    std::optional<bool> eol_auto;
    text::MergeOptions text;
    std::size_t guard = 0;
};

void set_mode(Config& c, text::Diff3Mode m) {
    c.text.gnu_mode = m, c.mode_set = true;
}

void set_algorithm(Config& c, const std::string& v) {
    auto& a = c.text.algorithm.algorithm;
    if (v == "myers" || v == "default")
        a = DiffAlgorithm::myers;
    else if (v == "minimal")
        a = DiffAlgorithm::minimal;
    else if (v == "patience")
        a = DiffAlgorithm::patience;
    else if (v == "histogram")
        a = DiffAlgorithm::histogram;
    else
        throw UsageError("invalid diff algorithm '" + v + "'");
}

// Options shared by the merging personalities. Returns false when `o` is not one of them.
bool apply_merge_option(Config& c, const ParsedOpt& o, const Registry& reg, Console& con) {
    const std::string v = o.value.value_or("");
    switch (o.id) {
    case o_label:
        if (c.labels.size() == 3)
            throw UsageError("too many file label options");
        c.labels.push_back(v);
        return true;
    case o_output:
        c.output = v;
        return true;
    case o_conflict:
        c.conflict.policy = v == "ours"     ? Policy::ours
                            : v == "theirs" ? Policy::theirs
                            : v == "base"   ? Policy::base
                            : v == "mark"   ? Policy::mark
                                            : throw UsageError("invalid conflict policy '" + v + "'");
        return true;
    case o_ours:
        c.conflict.resolve_all = ResolveAll::ours;
        return true;
    case o_theirs:
        c.conflict.resolve_all = ResolveAll::theirs;
        return true;
    case o_union:
        c.conflict.resolve_all = ResolveAll::union_;
        return true;
    case o_no_ours:
    case o_no_theirs:
    case o_no_union:
        c.conflict.resolve_all = ResolveAll::none;
        return true;
    case o_resolve: {
        std::ifstream f(to_path(v), std::ios::binary);
        if (!f)
            throw Error(ErrorCategory::io, v + ": No such file or directory");
        std::stringstream ss;
        ss << f.rdbuf();
        c.conflict.resolutions = parse_resolutions(ss.str());
        return true;
    }
    case o_conflicts:
        c.conflicts_file = v;
        return true;
    case o_conflict_style:
        if (v == "merge")
            c.text.style = text::MarkerStyle::merge;
        else if (v == "diff3")
            c.text.style = text::MarkerStyle::diff3;
        else if (v == "zdiff3")
            c.text.style = text::MarkerStyle::zdiff3;
        else
            throw UsageError("invalid conflict style '" + v + "'");
        return true;
    case o_diff3_style:
        c.text.style = text::MarkerStyle::diff3;
        return true;
    case o_zdiff3_style:
        c.text.style = text::MarkerStyle::zdiff3;
        return true;
    case o_no_style:
        c.text.style = text::MarkerStyle::merge;
        return true;
    case o_marker_size:
        c.text.marker_size = o.value ? static_cast<std::size_t>(parse_number(v, "marker size")) : 7;
        return true;
    case o_diff_algorithm:
        set_algorithm(c, v);
        return true;
    case o_eol:
        if (v != "auto" && v != "keep")
            throw UsageError("invalid eol mode '" + v + "'");
        c.eol_auto = v == "auto";
        return true;
    case o_guard:
        c.guard = static_cast<std::size_t>(parse_number(v, "guard"));
        c.common.format_options["guard"] = v;
        return true;
    case o_gap:
        parse_number(v, "gap");
        return true;
    case o_sign_key:
    case o_sign_cert:
    case o_sign_pass_file:
        throw Error(ErrorCategory::unsupported, "unsupported (built without crypto) (D-37)");
    case o_layout:
    case o_signatures:
    case o_sign_command:
    case o_mac_key:
    case o_verify_key:
    case o_verbose:
        return true; // envelope options: text and binary carry no envelope fields (text.md §7.1)
    case o_visual:
        throw Error(ErrorCategory::unsupported, "--visual: no visual diff for text or binary files");
    case o_object_id:
        throw Error(ErrorCategory::unsupported, "--object-id is not supported (D-15)");
    case o_help:
        con.out(help_text());
        throw 0;
    case o_version:
        con.out("splinter (Semantic Splinter) " + std::string(version()) + "\n");
        throw 0;
    default:
        return c.common.apply(o, reg);
    }
}

std::array<std::string, 3> labels_for(const Config& c, const std::vector<std::string>& ops) {
    // Operand order: mine/current, older/base, yours/other. Labels follow it.
    std::array<std::string, 3> l{ops[0], ops[1], ops[2]};
    for (std::size_t k = 0; k < c.labels.size(); ++k)
        l[k] = c.labels[k];
    return l;
}

Report merge_report(const std::string& fmt, const std::vector<std::string>& ops,
                    const std::array<ByteSource, 3>& in, const MergeResult& r, const Config& c,
                    std::span<const std::byte> out, const std::string& out_path) {
    Report rep;
    rep.mode = "merge";
    rep.format = fmt;
    rep.inputs = {{"base", ops[1], hash128(in[1].bytes()), in[1].size()},
                  {"ours", ops[0], hash128(in[0].bytes()), in[0].size()},
                  {"theirs", ops[2], hash128(in[2].bytes()), in[2].size()}};
    static const char* policies[] = {"ours", "theirs", "base", "mark"};
    rep.options["conflict"] = policies[static_cast<int>(c.conflict.policy)];
    rep.options["guard"] = c.guard;
    rep.changes_ours = r.changes_ours;
    rep.changes_theirs = r.changes_theirs;
    rep.auto_merged = r.auto_merged;
    rep.conflicts = r.conflicts;
    rep.warnings = r.warnings;
    rep.result = ResultInfo{out_path, hash128(out), r.unresolved_conflicts() ? "conflicts" : "clean"};
    return rep;
}

// Merges MINE OLDER YOURS and writes the result. Returns the number of unresolved (marked) conflicts.
std::size_t do_merge(Config& c, const std::vector<std::string>& ops, Console& con, const Registry& reg,
                     const std::optional<std::string>& target) {
    Limits budget = c.common.limits;
    std::array<ByteSource, 3> in{read_input(ops[0], budget, con), read_input(ops[1], budget, con),
                                 read_input(ops[2], budget, con)};
    bool binary = false;
    if (!c.force_text) {
        for (int k = 0; k < 3; ++k) {
            auto& f = reg.detect(in[static_cast<std::size_t>(k)].bytes(), ops[static_cast<std::size_t>(k)],
                                 c.common.format);
            binary |= f.info().name == "binary";
            if (f.info().name != "binary" && f.info().name != "text")
                throw Error(ErrorCategory::unsupported,
                            "format '" + std::string(f.info().name) + "' cannot merge yet");
        }
    }
    const auto labels = labels_for(c, ops);
    std::vector<std::byte> out;
    MergeResult r;
    std::size_t marked = 0;
    std::string fmt = binary ? "binary" : "text";
    if (c.personality == Personality::merge_file && binary)
        throw Error(ErrorCategory::unsupported, "Cannot merge binary files: " + ops[0]);
    if (binary) {
        if (!target && con.out_is_terminal())
            throw Error(ErrorCategory::invalid_input,
                        "refusing to write a binary merge to a terminal; use -o FILE");
        std::optional<binary::ByteMergeOptions> bm;
        if (auto it = c.common.format_options.find("binary-merge"); it != c.common.format_options.end())
            bm = binary::ByteMergeOptions{c.guard, it->second == "force"};
        auto pb = binary::load(in[1]), po = binary::load(in[0]), pt = binary::load(in[2]);
        ConflictOptions co = c.conflict;
        if (co.policy == Policy::mark && c.personality != Personality::diff3)
            co.policy = Policy::ours; // D-21: binary output cannot carry markers
        r = binary::merge(*pb, *po, *pt, co, bm ? &*bm : nullptr, out);
        marked = r.unresolved_conflicts();
    } else {
        text::LoadOptions lo;
        lo.strip_trailing_cr = c.strip_cr;
        lo.bom_is_content = c.text.engine == text::Engine::gnu;
        lo.split = c.text.eol_auto ? SplitMode::any : SplitMode::lf;
        auto tb = text::load(in[1], lo), to = text::load(in[0], lo), tt = text::load(in[2], lo);
        budget.charge(16 * (tb->lines.size() + to->lines.size() + tt->lines.size()));
        c.text.labels = labels;
        c.text.conflict = c.conflict;
        if (c.text.engine != text::Engine::gnu)
            c.text.crlf_markers = to->dominant_eol() == Eol::crlf;
        auto m = text::merge(*tb, *to, *tt, c.text);
        out = std::move(m.bytes);
        r = std::move(m.result);
        marked = m.marked;
    }
    for (auto& w : r.warnings)
        if (!c.quiet && w.code != "mark-unsupported")
            diag(con, "warning: " + w.message + (w.at.empty() ? "" : " (" + w.at + ")"));
    if (target)
        atomic_write(to_path(*target), out);
    else
        con.out(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));

    if (c.common.json || c.conflicts_file) {
        auto rep = to_json(merge_report(fmt, ops, in, r, c, out, target.value_or("-")));
        write_json(con, c.common.json, rep.dump(2));
        if (c.conflicts_file)
            atomic_write(to_path(*c.conflicts_file), as_bytes(rep.dump(2) + "\n"));
    }
    return marked;
}

std::vector<std::string> three_operands(const Parsed& p, const char* prog) {
    auto& ops = p.operands;
    if (ops.size() < 3)
        throw UsageError("missing operand after '" + (ops.empty() ? std::string(prog) : ops.back()) + "'");
    if (ops.size() > 3)
        throw UsageError("extra operand '" + ops[3] + "'");
    return ops;
}

} // namespace

int run_diff3(const std::vector<std::string>& args, Console& con, const Registry& reg, bool merge_flag) {
    Config c;
    c.personality = Personality::diff3;
    c.text.engine = text::Engine::gnu;
    c.text.algorithm = {DiffAlgorithm::myers, true, Compaction::gnu, 0};
    c.merged_output = merge_flag;
    auto p = parse(args, table(Personality::diff3, reg), con.env("POSIXLY_CORRECT").has_value());
    try {
        for (auto& o : p.opts) {
            switch (o.id) {
            case o_show_all:
                set_mode(c, text::Diff3Mode::show_all);
                break;
            case o_show_overlap:
                set_mode(c, text::Diff3Mode::show_overlap);
                break;
            case o_ed:
                set_mode(c, text::Diff3Mode::ed);
                break;
            case o_overlap_only:
            case o_overlap_only_bracket: // GNU 3.12 prints -X like -x
                set_mode(c, text::Diff3Mode::overlap_only);
                break;
            case o_easy_only:
                set_mode(c, text::Diff3Mode::easy_only);
                break;
            case o_write_quit:
                c.write_quit = true;
                break;
            case o_merge:
                c.merged_output = true;
                break;
            case o_text:
                c.force_text = true;
                break;
            case o_strip_trailing_cr:
                c.strip_cr = true;
                break;
            case o_initial_tab:
                c.initial_tab = true;
                break;
            case o_diff_program:
                throw Error(ErrorCategory::unsupported,
                            "--diff-program is not supported (no external processes, SPEC §9)");
            default:
                if (!apply_merge_option(c, o, reg, con))
                    throw UsageError("unhandled option " + o.spelling);
            }
        }
        // GNU: labels only where brackets are written (-A, -E, or -m alone); -i never with -m.
        const bool brackets_mode = c.mode_set ? c.text.gnu_mode == text::Diff3Mode::show_all ||
                                                    c.text.gnu_mode == text::Diff3Mode::show_overlap
                                              : c.merged_output;
        if ((!c.labels.empty() && !brackets_mode) || (c.merged_output && c.write_quit))
            throw UsageError("incompatible options");
        auto ops = three_operands(p, "diff3");
        if (c.eol_auto)
            c.text.eol_auto = *c.eol_auto;
        if (c.merged_output) {
            if (!c.mode_set)
                c.text.gnu_mode = text::Diff3Mode::show_all;
            return do_merge(c, ops, con, reg, c.output) ? 1 : 0;
        }
        // Report or ed script: text only, as GNU diff3.
        Limits budget = c.common.limits;
        std::array<ByteSource, 3> in{read_input(ops[0], budget, con), read_input(ops[1], budget, con),
                                     read_input(ops[2], budget, con)};
        if (!c.force_text)
            for (int k = 0; k < 3; ++k)
                if (looks_binary(in[static_cast<std::size_t>(k)].bytes()))
                    throw Error(ErrorCategory::invalid_input,
                                ops[static_cast<std::size_t>(k)] + ": binary file");
        text::LoadOptions lo;
        lo.strip_trailing_cr = c.strip_cr;
        lo.bom_is_content = true;
        auto to = text::load(in[0], lo), tb = text::load(in[1], lo), tt = text::load(in[2], lo);
        auto labels = labels_for(c, ops);
        if (!c.mode_set) {
            con.out(text::diff3_report(*tb, *to, *tt, labels, c.initial_tab));
            return 0;
        }
        auto script = text::diff3_ed(*tb, *to, *tt, c.text.gnu_mode, labels, c.write_quit);
        con.out(script);
        // GNU: 1 when brackets were written.
        bool brackets = (c.text.gnu_mode == text::Diff3Mode::show_all ||
                         c.text.gnu_mode == text::Diff3Mode::show_overlap) &&
                        script.find(std::string(7, '<')) != std::string::npos;
        return brackets ? 1 : 0;
    } catch (const Error& e) {
        diag(con, e.what());
        return exit_code(e.category(), c.common.extended_exit);
    } catch (int code) {
        return code;
    }
}

int run_merge(const std::vector<std::string>& args, Console& con, const Registry& reg) {
    Config c;
    c.personality = Personality::merge;
    c.text.engine = text::Engine::native;
    c.text.style = text::MarkerStyle::merge; // RCS -E
    auto p = parse(args, table(Personality::merge, reg), con.env("POSIXLY_CORRECT").has_value());
    try {
        for (auto& o : p.opts) {
            switch (o.id) {
            case o_show_all:
                c.text.style = text::MarkerStyle::diff3;
                break;
            case o_show_overlap:
                c.text.style = text::MarkerStyle::merge;
                break;
            case o_ed: // RCS -e: diff3 -e style, the other side wins unmarked
                c.conflict.resolve_all = ResolveAll::theirs;
                break;
            case o_stdout:
                c.to_stdout = true;
                break;
            case o_quiet:
                c.quiet = true;
                break;
            default:
                if (!apply_merge_option(c, o, reg, con))
                    throw UsageError("unhandled option " + o.spelling);
            }
        }
        auto ops = three_operands(p, "merge");
        c.text.eol_auto = c.eol_auto.value_or(true); // native mode (Q-TEXT-4)
        auto target = c.output ? c.output : c.to_stdout ? std::nullopt : std::optional<std::string>(ops[0]);
        std::size_t n = do_merge(c, ops, con, reg, target);
        if (n && !c.quiet)
            diag(con, "warning: conflicts during merge");
        if (c.common.extended_exit)
            return static_cast<int>(std::min<std::size_t>(n, 127)); // D-5
        return n ? 1 : 0;
    } catch (const Error& e) {
        diag(con, e.what());
        return exit_code(e.category(), c.common.extended_exit);
    } catch (int code) {
        return code;
    }
}

int run_merge_file(const std::vector<std::string>& args, Console& con, const Registry& reg) {
    Config c;
    c.personality = Personality::merge_file;
    c.text.engine = text::Engine::git;
    c.text.algorithm = {DiffAlgorithm::myers, true, Compaction::gnu, 0};
    auto p = parse(args, table(Personality::merge_file, reg), con.env("POSIXLY_CORRECT").has_value());
    try {
        for (auto& o : p.opts) {
            switch (o.id) {
            case o_stdout:
                c.to_stdout = true;
                break;
            case o_no_stdout:
                c.to_stdout = false;
                break;
            case o_quiet:
                c.quiet = true;
                break;
            case o_no_quiet:
                c.quiet = false;
                break;
            default:
                if (!apply_merge_option(c, o, reg, con))
                    throw UsageError("unhandled option " + o.spelling);
            }
        }
        auto ops = three_operands(p, "merge-file");
        auto target = c.to_stdout ? std::nullopt : std::optional<std::string>(ops[0]);
        // Git: the exit status is the number of conflicts, capped at 127.
        return static_cast<int>(std::min<std::size_t>(do_merge(c, ops, con, reg, target), 127));
    } catch (const Error& e) {
        diag(con, e.what());
        return 255; // text.md §6.4
    } catch (int code) {
        return code;
    }
}

} // namespace splinter::cli
