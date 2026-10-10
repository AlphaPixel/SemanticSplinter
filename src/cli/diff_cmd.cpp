// diff personality (CLI.md §2).
#include "cli.hpp"

#include <semsplinter/report.hpp>
#include <semsplinter/semsplinter.hpp>
#include <semsplinter/version.hpp>

#include <algorithm>
#include <fstream>
#include <regex>

namespace splinter::cli {

using namespace semsplinter;

namespace {

struct Config {
    OutputStyle style = OutputStyle::normal;
    bool style_set = false;
    bool brief = false, report_identical = false, force_text = false, dump = false, patch = false;
    RenderOptions render;
    Palette palette;
    std::string color = "never";
    std::vector<std::string> labels;
    bool recursive = false, new_file = false, unidirectional = false, ignore_name_case = false;
    std::vector<std::string> excludes;
    std::string starting_file;
    std::optional<std::string> from_file, to_file;
    TextDiffOptions text;
    bool strip_cr = false;
    std::size_t gap = 8;
    std::optional<std::string> visual; // --visual=FILE
    CommonOptions common;
    std::vector<std::string> option_tokens;
};

void set_style(Config& c, OutputStyle s) {
    if (c.style_set && c.style != s)
        throw UsageError("conflicting output style options");
    c.style = s, c.style_set = true;
}

std::size_t context_length(const std::string& v) {
    return static_cast<std::size_t>(parse_number(v, "context length"));
}

// Shell-quotes an argument as GNU diff does when echoing its command line in -r headers.
std::string shell_quote(const std::string& s) {
    bool safe = !s.empty() && std::all_of(s.begin(), s.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) ||
               std::string_view("_@%+,./:-").find(ch) != std::string_view::npos;
    });
    if (safe)
        return s;
    std::string q = "'";
    for (char ch : s)
        q += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    return q + "'";
}

// fnmatch(3) subset: *, ?, [set], [!set], backslash escapes.
bool glob(std::string_view p, std::string_view s) {
    if (p.empty())
        return s.empty();
    if (p[0] == '*') {
        for (std::size_t i = 0; i <= s.size(); ++i)
            if (glob(p.substr(1), s.substr(i)))
                return true;
        return false;
    }
    if (s.empty())
        return false;
    if (p[0] == '?')
        return glob(p.substr(1), s.substr(1));
    if (p[0] == '[') {
        auto close = p.find(']', 2);
        if (close != std::string_view::npos) {
            auto set = p.substr(1, close - 1);
            bool neg = !set.empty() && (set[0] == '!' || set[0] == '^');
            if (neg)
                set.remove_prefix(1);
            bool hit = false;
            for (std::size_t i = 0; i < set.size(); ++i) {
                if (i + 2 < set.size() && set[i + 1] == '-') {
                    hit |= s[0] >= set[i] && s[0] <= set[i + 2];
                    i += 2;
                } else {
                    hit |= s[0] == set[i];
                }
            }
            return hit != neg && glob(p.substr(close + 1), s.substr(1));
        }
    }
    if (p[0] == '\\' && p.size() > 1)
        p.remove_prefix(1);
    return p[0] == s[0] && glob(p.substr(1), s.substr(1));
}

class Differ {
public:
    Differ(Config& cfg, Console& c, const Registry& reg) : cfg_(cfg), c_(c), reg_(reg) {}

    // Compares two operands (files or directories). Returns 0, 1, or 2.
    int operands(const std::string& a, const std::string& b) {
        std::error_code ec;
        bool da = a != "-" && std::filesystem::is_directory(to_path(a), ec);
        bool db = b != "-" && std::filesystem::is_directory(to_path(b), ec);
        if (da && db)
            return dirs(a, b, true);
        if (da)
            return files(join(a, from_path(to_path(b).filename())), b, false);
        if (db)
            return files(a, join(b, from_path(to_path(a).filename())), false);
        return files(a, b, false);
    }

    void finish_json() {
        if (!cfg_.common.json || reports_.empty())
            return;
        write_json(c_, cfg_.common.json,
                   (reports_.size() == 1 ? reports_[0] : nlohmann::ordered_json(reports_)).dump(2));
    }

    int dump(const std::string& path) {
        Limits budget = cfg_.common.limits;
        out(semsplinter::dump(reg_, {read_input(path, budget, c_), path}, cfg_.common.format, cfg_.force_text,
                              budget, cfg_.common.format_options));
        return 0;
    }

private:
    static std::string join(const std::string& dir, const std::string& name) {
        return dir.ends_with('/') || dir.ends_with('\\') ? dir + name : dir + "/" + name;
    }

    void out(std::string_view s) {
        if (!(cfg_.common.json && cfg_.common.json->empty()))
            c_.out(s);
    }

    // The name GNU uses in messages: the --label if given, else the path.
    std::string name(std::size_t k, const std::string& path) const {
        return cfg_.labels.size() > k ? cfg_.labels[k] : path;
    }

    std::string label(std::size_t k, const std::string& path, bool absent) {
        if (cfg_.labels.size() > k)
            return cfg_.labels[k];
        if (cfg_.style != OutputStyle::unified && cfg_.style != OutputStyle::context)
            return path;
        std::optional<FileTime> t = absent        ? std::optional<FileTime>(FileTime{})
                                    : path == "-" ? std::nullopt
                                                  : file_mtime(to_path(path));
        if (!t)
            return path;
        return path + "\t" +
               (cfg_.style == OutputStyle::unified ? format_unified_time(*t) : format_context_time(*t));
    }

    int files(const std::string& a, const std::string& b, bool in_dir, bool absent_a = false,
              bool absent_b = false) {
        DiffRequest rq;
        rq.limits = cfg_.common.limits;
        rq.a = {absent_a ? ByteSource::from_memory({}, a) : read_input(a, rq.limits, c_), a};
        rq.b = {absent_b ? ByteSource::from_memory({}, b) : read_input(b, rq.limits, c_), b};
        rq.format = cfg_.common.format;
        rq.force_text = cfg_.force_text;
        rq.format_options = cfg_.common.format_options;
        rq.text_load.strip_trailing_cr = cfg_.strip_cr;
        rq.text_load.bom_is_content = true; // GNU
        rq.text = cfg_.text;
        rq.report = cfg_.common.json.has_value();
        rq.visual = cfg_.visual.has_value();
        rq.patch = cfg_.patch;
        if (!cfg_.brief)
            rq.render = RenderRequest{cfg_.style,
                                      cfg_.render,
                                      {a, b},
                                      {name(0, a), name(1, b)},
                                      {label(0, a, absent_a), label(1, b, absent_b)}};
        auto r = semsplinter::diff(reg_, rq);
        if (!cfg_.common.json)
            for (auto& [path, w] : r.warnings)
                diag(c_, "warning: " + (path.empty() ? "" : path + ": ") + w.message +
                             (w.at.empty() ? "" : " (" + w.at + ")"));
        if (cfg_.visual)
            atomic_write(to_path(*cfg_.visual), r.visual);
        int status = r.differs ? 1 : 0;
        if (status && cfg_.brief) {
            out("Files " + name(0, a) + " and " + name(1, b) + " differ\n");
        } else if (!r.text.empty() || !r.patch.empty()) {
            std::string o;
            if (in_dir) {
                o = "diff";
                for (auto& t : cfg_.option_tokens)
                    o += " " + shell_quote(t);
                o += " " + a + " " + b + "\n";
            }
            out(o + (r.patch.empty() ? r.text : r.patch));
        }
        for (auto& e : r.errors) {
            diag(c_, e);
            status = 2;
        }
        if (!status && cfg_.report_identical)
            out("Files " + name(0, a) + " and " + name(1, b) + " are identical\n");
        if (r.report)
            reports_.push_back(to_json(*r.report));
        return status;
    }

    bool excluded(const std::string& name) const {
        for (auto& p : cfg_.excludes)
            if (glob(p, name))
                return true;
        return false;
    }

    std::vector<std::string> list(const std::string& dir, bool absent) {
        std::vector<std::string> v;
        if (absent)
            return v;
        for (auto& e : std::filesystem::directory_iterator(to_path(dir))) {
            auto n = from_path(e.path().filename());
            if (!excluded(n))
                v.push_back(n);
        }
        return v;
    }

    int dirs(const std::string& a, const std::string& b, bool top, bool absent_a = false,
             bool absent_b = false) {
        auto la = list(a, absent_a), lb = list(b, absent_b);
        auto key = [&](std::string s) {
            if (cfg_.ignore_name_case)
                for (char& ch : s)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        auto by_key = [&](const std::string& x, const std::string& y) { return key(x) < key(y); };
        std::sort(la.begin(), la.end(), by_key);
        std::sort(lb.begin(), lb.end(), by_key);
        int status = 0;
        std::size_t i = 0, j = 0;
        std::error_code ec;
        while (i < la.size() || j < lb.size()) {
            int cmp = i == la.size() ? 1 : j == lb.size() ? -1 : key(la[i]).compare(key(lb[j]));
            std::string na = cmp <= 0 ? la[i] : lb[j], nb = cmp >= 0 ? lb[j] : la[i];
            if (cmp <= 0)
                ++i;
            if (cmp >= 0)
                ++j;
            if (top && !cfg_.starting_file.empty() && na < cfg_.starting_file)
                continue;
            std::string pa = join(a, na), pb = join(b, nb);
            bool ina = cmp <= 0, inb = cmp >= 0;
            bool dir_a = ina && std::filesystem::is_directory(to_path(pa), ec);
            bool dir_b = inb && std::filesystem::is_directory(to_path(pb), ec);
            int s = 0;
            if (!ina || !inb) {
                bool create = cfg_.new_file || (!ina && cfg_.unidirectional);
                if (!create) {
                    out("Only in " + (ina ? a : b) + ": " + na + "\n");
                    s = 1;
                } else if (dir_a || dir_b) {
                    s = cfg_.recursive ? dirs(pa, pb, false, !ina, !inb) : 0;
                } else {
                    s = files(pa, pb, true, !ina, !inb);
                }
            } else if (dir_a && dir_b) {
                if (cfg_.recursive) {
                    s = dirs(pa, pb, false);
                } else {
                    out("Common subdirectories: " + pa + " and " + pb + "\n");
                }
            } else if (dir_a != dir_b) {
                out("File " + pa + " is a " + (dir_a ? "directory" : "regular file") + " while file " + pb +
                    " is a " + (dir_b ? "directory" : "regular file") + "\n");
                s = 1;
            } else {
                try {
                    s = files(pa, pb, true);
                } catch (const Error& e) {
                    diag(c_, e.what());
                    s = 2;
                }
            }
            status = std::max(status, s);
        }
        return status;
    }

    Config& cfg_;
    Console& c_;
    const Registry& reg_;
    std::vector<nlohmann::ordered_json> reports_;
};

} // namespace

int run_diff(const std::vector<std::string>& args, Console& c, const Registry& reg) {
    Config cfg;
    auto tbl = table(Personality::diff, reg);
    auto p = parse(args, tbl, c.env("POSIXLY_CORRECT").has_value());
    cfg.option_tokens = p.option_tokens;
    std::optional<std::size_t> digits, context; // GNU uses the largest context length given
    std::size_t horizon = 0;
    bool context_flag_c = false, context_flag_u = false; // -c / -u mean 3 lines
    std::optional<std::string> function_re;
    bool show_c_function = false;
    int last_id = -1;
    try {
        for (auto& o : p.opts) {
            const std::string v = o.value.value_or("");
            if (o.id != o_digit)
                digits.reset();
            switch (o.id) {
            case o_normal:
                set_style(cfg, OutputStyle::normal);
                break;
            case o_brief:
                cfg.brief = true;
                break;
            case o_report_identical:
                cfg.report_identical = true;
                break;
            case o_context_c:
                set_style(cfg, OutputStyle::context);
                context_flag_c = true;
                break;
            case o_context_n:
            case o_context:
                set_style(cfg, OutputStyle::context);
                if (o.value)
                    context = std::max(context.value_or(0), context_length(v));
                break;
            case o_unified_u:
                set_style(cfg, OutputStyle::unified);
                context_flag_u = true;
                break;
            case o_unified_n:
            case o_unified:
                set_style(cfg, OutputStyle::unified);
                if (o.value)
                    context = std::max(context.value_or(0), context_length(v));
                break;
            case o_digit: { // obsolete -NUM: context length
                std::size_t d = static_cast<std::size_t>(o.spelling[1] - '0');
                digits = last_id == o_digit && digits ? *digits * 10 + d : d;
                context = std::max(context.value_or(0), *digits);
                break;
            }
            case o_ed:
                set_style(cfg, OutputStyle::ed);
                break;
            case o_forward_ed:
                set_style(cfg, OutputStyle::forward_ed);
                break;
            case o_rcs:
                set_style(cfg, OutputStyle::rcs);
                break;
            case o_side_by_side:
                set_style(cfg, OutputStyle::side_by_side);
                break;
            case o_width:
                cfg.render.width = static_cast<std::size_t>(parse_number(v, "width"));
                if (!cfg.render.width)
                    throw UsageError("invalid width '" + v + "'");
                break;
            case o_left_column:
                cfg.render.left_column = true;
                break;
            case o_suppress_common:
                cfg.render.suppress_common_lines = true;
                break;
            case o_show_c_function:
                show_c_function = true;
                break;
            case o_show_function_line:
                function_re = v;
                break;
            case o_label:
                if (cfg.labels.size() == 2)
                    throw UsageError("too many file label options");
                cfg.labels.push_back(v);
                break;
            case o_expand_tabs:
                cfg.render.expand_tabs = true;
                break;
            case o_initial_tab:
                cfg.render.initial_tab = true;
                break;
            case o_tabsize:
                cfg.render.tabsize = static_cast<std::size_t>(parse_number(v, "tabsize"));
                if (!cfg.render.tabsize)
                    throw UsageError("invalid tabsize '" + v + "'");
                cfg.text.normalize.tabsize = cfg.render.tabsize;
                break;
            case o_suppress_blank_empty:
                cfg.render.suppress_blank_empty = true;
                break;
            case o_paginate:
                throw Error(ErrorCategory::unsupported, "option '-l' (--paginate) is not supported (D-14)");
            case o_recursive:
                cfg.recursive = true;
                break;
            case o_no_dereference:
                break; // directory_iterator does not follow symlinks into the listing; files are read through
                       // them
            case o_new_file:
                cfg.new_file = true;
                break;
            case o_unidirectional_new_file:
                cfg.unidirectional = true;
                break;
            case o_ignore_file_name_case:
                cfg.ignore_name_case = true;
                break;
            case o_no_ignore_file_name_case:
                cfg.ignore_name_case = false;
                break;
            case o_exclude:
                cfg.excludes.push_back(v);
                break;
            case o_exclude_from: {
                std::ifstream f(to_path(v));
                if (!f)
                    throw Error(ErrorCategory::io, v + ": No such file or directory");
                for (std::string line; std::getline(f, line);) {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    if (!line.empty())
                        cfg.excludes.push_back(line);
                }
                break;
            }
            case o_starting_file:
                cfg.starting_file = v;
                break;
            case o_from_file:
                cfg.from_file = v;
                break;
            case o_to_file:
                cfg.to_file = v;
                break;
            case o_ignore_case:
                cfg.text.normalize.ignore_case = true;
                break;
            case o_ignore_tab_expansion:
                cfg.text.normalize.ignore_tab_expansion = true;
                break;
            case o_ignore_trailing_space:
                cfg.text.normalize.ignore_trailing_space = true;
                break;
            case o_ignore_space_change:
                cfg.text.normalize.ignore_space_change = true;
                break;
            case o_ignore_all_space:
                cfg.text.normalize.ignore_all_space = true;
                break;
            case o_ignore_blank_lines:
                cfg.text.ignore_blank_lines = true;
                break;
            case o_ignore_matching_lines:
                try {
                    cfg.text.ignore_re.emplace_back(v, std::regex::basic);
                } catch (const std::regex_error&) {
                    throw UsageError("invalid regular expression '" + v + "'");
                }
                break;
            case o_text:
                cfg.force_text = true;
                break;
            case o_strip_trailing_cr:
                cfg.strip_cr = true;
                break;
            case o_binary:
                cfg.patch = true; // SPEC §6: an applicable patch (unified for text)
                break;
            case o_ifdef:
            case o_group_format:
            case o_line_format:
                throw Error(ErrorCategory::unsupported,
                            "option '" + o.spelling + "' (ifdef/group formats) is not implemented yet");
            case o_minimal:
                cfg.text.algorithm.algorithm = DiffAlgorithm::minimal;
                break;
            case o_horizon_lines:
                horizon = static_cast<std::size_t>(parse_number(v, "horizon length"));
                break;
            case o_speed_large_files:
            case o_inhibit_hunk_merge:
            case o_sdiff_merge_assist:
                break;
            case o_color:
                cfg.color = o.value ? v : "auto";
                if (cfg.color != "never" && cfg.color != "always" && cfg.color != "auto")
                    throw UsageError("invalid color '" + v + "'");
                break;
            case o_palette:
                cfg.palette = parse_palette(v, cfg.palette);
                break;
            case o_help:
                c.out(help_text());
                return 0;
            case o_version:
                c.out("splinter (Semantic Splinter) " + std::string(version()) + "\n");
                return 0;
            case o_dump:
                cfg.dump = true;
                break;
            case o_gap:
                cfg.gap = static_cast<std::size_t>(parse_number(v, "gap"));
                cfg.common.format_options["gap"] = v;
                break;
            case o_diff_algorithm:
                if (v == "myers" || v == "default")
                    cfg.text.algorithm.algorithm = DiffAlgorithm::myers;
                else if (v == "minimal")
                    cfg.text.algorithm.algorithm = DiffAlgorithm::minimal;
                else if (v == "patience")
                    cfg.text.algorithm.algorithm = DiffAlgorithm::patience;
                else if (v == "histogram")
                    cfg.text.algorithm.algorithm = DiffAlgorithm::histogram;
                else
                    throw UsageError("invalid diff algorithm '" + v + "'");
                break;
            case o_visual:
                cfg.visual = v;
                break;
            case o_scan_streams:
                throw Error(ErrorCategory::unsupported, "--scan-streams is post-v1 (D-49)");
            case o_semantic:
            case o_raw:
            case o_annotate:
            case o_ignore:
            case o_verbose:
            case o_quiet:
            case o_verify_key:
            case o_exact:
                break; // no effect on text and binary
            default:
                if (!cfg.common.apply(o, reg))
                    throw UsageError("unhandled option " + o.spelling);
            }
            last_id = o.id;
        }
    } catch (const Error& e) {
        diag(c, e.what());
        return exit_code(e.category(), cfg.common.extended_exit);
    }
    if (cfg.patch) {
        if (cfg.common.json) {
            diag(c, "--binary --json (a JSON patch) is not implemented yet");
            return exit_code(ErrorCategory::unsupported, cfg.common.extended_exit);
        }
        if (!cfg.style_set)
            cfg.style = OutputStyle::unified;
    }
    if (context)
        cfg.render.context = (cfg.style == OutputStyle::context || cfg.style == OutputStyle::unified) &&
                                     (context_flag_c || context_flag_u)
                                 ? std::max<std::size_t>(*context, 3)
                                 : *context;
    cfg.text.ignore_missing_newline = cfg.style == OutputStyle::ed || cfg.style == OutputStyle::forward_ed;
    // GNU keeps max(--horizon-lines, output context) identical lines around the analysed region.
    const bool with_context = cfg.style == OutputStyle::unified || cfg.style == OutputStyle::context;
    cfg.text.algorithm.horizon = std::max(horizon, with_context ? cfg.render.context : 0);
    if (show_c_function && !function_re)
        function_re = "^[[:alpha:]$_]";
    if (function_re) {
        auto re = std::make_shared<std::regex>(*function_re, std::regex::basic);
        cfg.render.is_function = [re](std::string_view s) {
            return std::regex_search(s.begin(), s.end(), *re);
        };
    }
    if (cfg.color == "always" || (cfg.color == "auto" && c.out_is_terminal()))
        cfg.render.color = &cfg.palette;

    Differ d(cfg, c, reg);
    try {
        auto& ops = p.operands;
        if (cfg.dump) {
            if (ops.size() != 1)
                throw UsageError(ops.empty() ? "missing operand after 'dump'"
                                             : "extra operand '" + ops[1] + "'");
            return d.dump(ops[0]);
        }
        int status = 0;
        if (cfg.from_file || cfg.to_file) {
            if (cfg.from_file && cfg.to_file)
                throw UsageError("--from-file and --to-file both specified");
            for (auto& op : ops)
                status = std::max(status, cfg.from_file ? d.operands(*cfg.from_file, op)
                                                        : d.operands(op, *cfg.to_file));
        } else {
            if (ops.size() < 2)
                throw UsageError("missing operand after '" +
                                 (ops.empty() ? std::string("splinter") : ops.back()) + "'");
            if (ops.size() > 2)
                throw UsageError("extra operand '" + ops[2] + "'");
            status = d.operands(ops[0], ops[1]);
        }
        d.finish_json();
        return status;
    } catch (const Error& e) {
        diag(c, e.what());
        return exit_code(e.category(), cfg.common.extended_exit);
    }
}

} // namespace splinter::cli
