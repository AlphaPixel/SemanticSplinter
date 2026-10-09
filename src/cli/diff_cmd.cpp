// diff personality (CLI.md §2).
#include "cli.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/formats/binary.hpp>
#include <semsplinter/formats/text.hpp>
#include <semsplinter/report.hpp>
#include <semsplinter/version.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

namespace splinter::cli {

using namespace semsplinter;

namespace {

struct Config {
    OutputStyle style = OutputStyle::normal;
    bool style_set = false;
    bool brief = false, report_identical = false, force_text = false, dump = false;
    RenderOptions render;
    Palette palette;
    std::string color = "never";
    std::vector<std::string> labels;
    bool recursive = false, new_file = false, unidirectional = false, ignore_name_case = false;
    std::vector<std::string> excludes;
    std::string starting_file;
    std::optional<std::string> from_file, to_file;
    text::DiffOptions text;
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

// "1.2 MiB", "830 B".
std::string byte_size(std::uint64_t n) {
    char buf[32];
    if (n < 1024)
        return std::to_string(n) + " B";
    const bool kib = n < (std::uint64_t{1} << 20);
    std::snprintf(buf, sizeof buf, kib ? "%.1f KiB" : "%.1f MiB",
                  static_cast<double>(n) / (kib ? 1024 : 1024 * 1024));
    return buf;
}

// The normal report of a structured format (SPEC §7.2): one line per change.
std::string render_changes(const ChangeSet& cs) {
    std::string o;
    for (auto& c : cs) {
        o += c.op == Op::insert ? '+' : c.op == Op::erase ? '-' : c.op == Op::move ? '>' : '~';
        o += " " + c.at.to_string();
        if (c.before.text && c.after.text)
            o += " " + *c.before.text + " -> " + *c.after.text;
        else if (c.after.text || c.before.text)
            o += " " + *(c.after.text ? c.after.text : c.before.text);
        for (auto& [k, v] : c.metrics.items()) {
            if (k == "bbox" || k == "depth")
                continue;
            std::string val;
            if (v.is_array())
                for (auto& x : v)
                    val += (val.empty() ? "" : ",") + (x.is_string() ? x.get<std::string>() : x.dump());
            else
                val = v.is_string() ? v.get<std::string>() : v.dump();
            o += " " + k + "=" + val;
        }
        o += "\n";
    }
    return o;
}

// "# bytes changed 1.2 MiB: 99.8% encoding, <0.1% integrity; content: 212 px in 1 region" (SPEC §12.6).
std::string churn_line(const Churn& ch, const nlohmann::ordered_json& stats) {
    std::vector<std::pair<std::uint64_t, std::size_t>> parts;
    for (std::size_t k = 0; k < kByteClasses; ++k)
        if (ch.by_class[k])
            parts.emplace_back(ch.by_class[k], k);
    std::stable_sort(parts.begin(), parts.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::string o = "# bytes changed " + byte_size(ch.total()) + ":";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        double pct = 100.0 * static_cast<double>(parts[i].first) / static_cast<double>(ch.total());
        char buf[16];
        std::snprintf(buf, sizeof buf, pct < 0.1 ? "<0.1%%" : "%.1f%%", pct);
        o += std::string(i ? ", " : " ") + buf + " " +
             std::string(to_string(static_cast<ByteClass>(parts[i].second)));
    }
    if (stats.value("regions", std::uint64_t{0})) {
        auto regions = stats.value("regions", std::uint64_t{0});
        o += "; content: " + std::to_string(stats.value("pixels_changed", std::uint64_t{0})) + " px in " +
             std::to_string(regions) + (regions == 1 ? " region" : " regions");
    }
    return o + "\n";
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
        auto src = read_input(path, budget, c_);
        const Format& f =
            cfg_.force_text ? *reg_.find("text") : reg_.detect(src.bytes(), path, cfg_.common.format);
        Context ctx{reg_, budget, 0, cfg_.common.format_options};
        auto doc = f.load(src, {ctx});
        std::ostringstream os;
        f.dump(*doc, os, {ctx});
        out(os.str());
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
        Limits budget = cfg_.common.limits;
        ByteSource sa = absent_a ? ByteSource::from_memory({}, a) : read_input(a, budget, c_);
        ByteSource sb = absent_b ? ByteSource::from_memory({}, b) : read_input(b, budget, c_);
        const auto xa = sa.bytes(), xb = sb.bytes();
        const bool same_bytes = xa.size() == xb.size() && std::equal(xa.begin(), xa.end(), xb.begin());

        const Format* fa = &reg_.detect(xa, a, cfg_.common.format);
        const Format* fb = &reg_.detect(xb, b, cfg_.common.format);
        if (cfg_.force_text)
            fa = fb = reg_.find("text");
        // Different formats on the two sides (or a missing side under -N) compare as bytes.
        const bool binary = fa->info().name == "binary" || fb->info().name == "binary" || fa != fb;
        const std::string fmt = binary ? "binary" : std::string(fa->info().name);

        Report rep;
        rep.mode = "diff";
        rep.format = fmt;
        rep.inputs = {{"a", a, hash128(xa), xa.size()}, {"b", b, hash128(xb), xb.size()}};
        int status = 0;

        if (cfg_.visual && (binary || fa->info().name == "text"))
            throw Error(ErrorCategory::unsupported, "--visual: no visual diff for text or binary files");
        if (binary) {
            auto da = binary::load(sa), db = binary::load(sb);
            auto d = binary::diff(*da, *db, cfg_.gap);
            if (cfg_.common.json) {
                rep.changes = binary::changes(*da, *db, d);
                rep.format_stats = binary::metrics(*da, *db, d);
            }
            status = d.identical() ? 0 : 1;
            if (status)
                out((cfg_.brief ? "Files " : "Binary files ") + name(0, a) + " and " + name(1, b) +
                    " differ\n");
        } else if (fa->info().name != "text") {
            status = structured(*fa, sa, sb, rep, a, b, in_dir, budget);
        } else {
            text::LoadOptions lo;
            lo.strip_trailing_cr = cfg_.strip_cr;
            lo.bom_is_content = true;
            auto da = text::load(sa, lo), db = text::load(sb, lo);
            budget.charge(16 * (da->lines.size() + db->lines.size()));
            text::TextDiff d;
            if (!same_bytes)
                d = text::diff(*da, *db, cfg_.text);
            if (cfg_.common.json) {
                rep.changes = text::changes(*da, *db, d);
                rep.format_stats = text::metrics(*da, *db, d);
            }
            status = d.differs() ? 1 : 0;
            if (status && cfg_.brief) {
                out("Files " + name(0, a) + " and " + name(1, b) + " differ\n");
            } else if (status ||
                       (cfg_.style == OutputStyle::side_by_side && !cfg_.brief)) { // -y shows equal files too
                std::string o;
                if (in_dir) {
                    o = "diff";
                    for (auto& t : cfg_.option_tokens)
                        o += " " + shell_quote(t);
                    o += " " + a + " " + b + "\n";
                }
                auto sa_side = text::diff_side(*da, label(0, a, absent_a));
                auto sb_side = text::diff_side(*db, label(1, b, absent_b));
                render(o, cfg_.style, sa_side, sb_side, d.edits, d.ignorable, cfg_.render);
                out(o);
            }
            // ed scripts cannot express a missing final newline: GNU compares as if it were there, then
            // fails.
            const bool ed = cfg_.style == OutputStyle::ed || cfg_.style == OutputStyle::forward_ed;
            if (ed && !cfg_.brief)
                for (auto [doc, path] : {std::pair{da.get(), &a}, std::pair{db.get(), &b}})
                    if (doc->missing_newline()) {
                        diag(c_, *path + ": No newline at end of file");
                        status = 2;
                    }
        }
        if (!status && cfg_.report_identical)
            out("Files " + name(0, a) + " and " + name(1, b) + " are identical\n");
        rep.options["algorithm"] =
            std::string(cfg_.text.algorithm.algorithm == DiffAlgorithm::myers      ? "myers"
                        : cfg_.text.algorithm.algorithm == DiffAlgorithm::minimal  ? "minimal"
                        : cfg_.text.algorithm.algorithm == DiffAlgorithm::patience ? "patience"
                                                                                   : "histogram");
        if (cfg_.common.json)
            reports_.push_back(to_json(rep));
        return status;
    }

    // A structured format (SPEC §7.2): normal report, or unified/context/side-by-side over dump lines.
    int structured(const Format& f, const ByteSource& sa, const ByteSource& sb, Report& rep,
                   const std::string& a, const std::string& b, bool in_dir, Limits& budget) {
        if (cfg_.style == OutputStyle::ed || cfg_.style == OutputStyle::forward_ed ||
            cfg_.style == OutputStyle::rcs)
            throw Error(ErrorCategory::unsupported, std::string("option '") +
                                                        (cfg_.style == OutputStyle::ed    ? "-e"
                                                         : cfg_.style == OutputStyle::rcs ? "-n"
                                                                                          : "-f") +
                                                        "' not supported for format '" +
                                                        std::string(f.info().name) + "'");
        Context ctx{reg_, budget, 0, cfg_.common.format_options};
        auto da = f.load(sa, {ctx}), db = f.load(sb, {ctx});
        auto cs = f.diff(*da, *db, {ctx});
        // Encoding-only changes leave the files semantically identical (SPEC §12.2).
        int status =
            std::any_of(cs.begin(), cs.end(), [](const Change& c) { return c.kind != "encoding"; }) ? 1 : 0;
        Churn churn;
        auto *ma = f.envelope(*da), *mb = f.envelope(*db);
        if (ma && mb)
            churn = attribute_churn(sa.bytes(), *ma, sb.bytes(), *mb);
        auto stats = f.stats(*da, *db, cs);
        auto ia = f.inspect(*da), ib = f.inspect(*db);
        for (auto [j, path] : {std::pair{&ia, &a}, std::pair{&ib, &b}})
            if (j->contains("warnings"))
                for (auto& w : (*j)["warnings"]) {
                    rep.warnings.push_back({w.value("code", ""), w.value("at", ""), w.value("message", "")});
                    if (!cfg_.common.json)
                        diag(c_, "warning: " + *path + ": " + w.value("message", "") + " (" +
                                     w.value("at", "") + ")");
                }
        if (cfg_.common.json) {
            rep.changes = cs;
            rep.format_stats = stats;
            nlohmann::ordered_json env;
            env["integrity"] = {{"a", ia.value("integrity", nlohmann::ordered_json("not-checked"))},
                                {"b", ib.value("integrity", nlohmann::ordered_json("not-checked"))}};
            env["churn"] = churn.to_json();
            rep.envelope = std::move(env);
        }
        if (cfg_.visual)
            atomic_write(to_path(*cfg_.visual), f.visual(*da, *db, {ctx}));
        if (status && cfg_.brief) {
            out("Files " + name(0, a) + " and " + name(1, b) + " differ\n");
        } else if (!cfg_.brief && (!cs.empty() || cfg_.style == OutputStyle::side_by_side)) {
            std::string o;
            if (in_dir) {
                o = "diff";
                for (auto& t : cfg_.option_tokens)
                    o += " " + shell_quote(t);
                o += " " + a + " " + b + "\n";
            }
            if (cfg_.style == OutputStyle::normal) {
                o += render_changes(cs);
                if (churn.total())
                    o += churn_line(churn, stats);
            } else {
                auto dump = [&](const Document& d) {
                    std::ostringstream os;
                    f.dump(d, os, {ctx});
                    auto str = os.str();
                    auto* p = reinterpret_cast<const std::byte*>(str.data());
                    return text::load(ByteSource::from_memory({p, p + str.size()}), {});
                };
                auto ta = dump(*da), tb = dump(*db);
                auto d = text::diff(*ta, *tb, cfg_.text);
                render(o, cfg_.style, text::diff_side(*ta, label(0, a, false)),
                       text::diff_side(*tb, label(1, b, false)), d.edits, d.ignorable, cfg_.render);
            }
            out(o);
        }
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
                throw Error(ErrorCategory::unsupported,
                            "--binary (patch output) is not implemented yet (Phase 5)");
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
