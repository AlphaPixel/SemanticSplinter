#include "cli.hpp"
#include "git.hpp"

#include <semsplinter/semsplinter.hpp>
#include <semsplinter/version.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace splinter::cli {

using namespace semsplinter;

int exit_code(ErrorCategory c, bool extended) {
    if (!extended)
        return 2;
    switch (c) {
    case ErrorCategory::invalid_input:
        return 2;
    case ErrorCategory::unsupported:
        return 3;
    case ErrorCategory::limit:
        return 4;
    case ErrorCategory::io:
        return 5;
    case ErrorCategory::internal:
        return 6;
    }
    return 6;
}

void diag(Console& c, std::string_view message) {
    c.err("splinter: ");
    c.err(message);
    c.err("\n");
}

std::uint64_t parse_number(std::string_view v, std::string_view what) {
    std::uint64_t n = 0;
    if (v.empty())
        throw UsageError("invalid " + std::string(what) + " '" + std::string(v) + "'");
    for (char c : v) {
        if (c < '0' || c > '9' || n > (UINT64_MAX - 9) / 10)
            throw UsageError("invalid " + std::string(what) + " '" + std::string(v) + "'");
        n = n * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return n;
}

std::uint64_t parse_size(std::string_view v, std::string_view what) {
    std::uint64_t mul = 1;
    if (!v.empty()) {
        switch (std::toupper(static_cast<unsigned char>(v.back()))) {
        case 'K':
            mul = 1ull << 10;
            break;
        case 'M':
            mul = 1ull << 20;
            break;
        case 'G':
            mul = 1ull << 30;
            break;
        case 'T':
            mul = 1ull << 40;
            break;
        }
        if (mul != 1)
            v.remove_suffix(1);
    }
    return parse_number(v, what) * mul;
}

bool CommonOptions::apply(const ParsedOpt& o, const Registry& reg) {
    const std::string& v = o.value ? *o.value : std::string{};
    switch (o.id) {
    case o_max_memory:
        limits.max_memory = parse_size(v, "size");
        return true;
    case o_max_pixels:
        limits.max_pixels = parse_size(v, "count");
        return true;
    case o_max_input:
        limits.max_input = parse_size(v, "size");
        return true;
    case o_max_depth:
        limits.max_depth = static_cast<int>(parse_number(v, "depth"));
        return true;
    case o_max_zip_entries:
        limits.max_zip_entries = parse_size(v, "count");
        return true;
    case o_max_zip_ratio:
        limits.max_zip_ratio = parse_number(v, "ratio");
        return true;
    case o_max_zip_size:
        limits.max_zip_total = parse_size(v, "size");
        return true;
    case o_exit_codes:
        if (v != "posix" && v != "extended")
            throw UsageError("invalid exit-codes '" + v + "'");
        extended_exit = v == "extended";
        return true;
    case o_format:
        if (!reg.find(v))
            throw Error(ErrorCategory::unsupported, "unknown format '" + v + "' (see 'splinter formats')");
        format = v;
        return true;
    case o_json:
        json = v;
        return true;
    case o_md:
        throw Error(ErrorCategory::unsupported, "--md is not implemented yet");
    case o_tolerance:
    case o_integrity:
    case o_show:
    case o_lenient:
        // Recorded for formats with grids or envelopes; text and binary have neither (no effect).
        if (o.value)
            format_options[o.spelling.substr(2)] = v;
        return true;
    default:
        break;
    }
    if (o.id >= o_format_option) {
        int k = o.id - o_format_option;
        for (auto& f : reg.formats())
            for (auto& spec : f->info().options)
                if (k-- == 0)
                    format_options[std::string(spec.name)] = v;
        return true;
    }
    return false;
}

std::filesystem::path to_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string from_path(const std::filesystem::path& p) {
    auto u = p.u8string();
    return {u.begin(), u.end()};
}

ByteSource read_input(const std::string& path, const Limits& limits, Console& c) {
    if (path == "-") {
        auto bytes = c.read_stdin();
        if (bytes.size() > limits.max_input)
            throw Error(ErrorCategory::limit, "-: input exceeds --max-input");
        return ByteSource::from_memory(std::move(bytes), "-");
    }
    std::error_code ec;
    auto p = to_path(path);
    if (!std::filesystem::exists(p, ec))
        throw Error(ErrorCategory::io, path + ": No such file or directory");
    return ByteSource::from_file(p, limits);
}

void write_json(Console& c, const std::optional<std::string>& target, const std::string& json) {
    if (!target)
        return;
    if (target->empty() || *target == "-") {
        c.out(json);
        c.out("\n");
        return;
    }
    std::string body = json + "\n";
    atomic_write(to_path(*target), as_bytes(body));
}

std::optional<FileTime> file_mtime(const std::filesystem::path& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
        return std::nullopt;
    std::uint64_t t =
        (std::uint64_t{d.ftLastWriteTime.dwHighDateTime} << 32) | d.ftLastWriteTime.dwLowDateTime;
    constexpr std::uint64_t epoch = 116444736000000000ull; // 1601 -> 1970 in 100 ns units
    std::int64_t rel = static_cast<std::int64_t>(t - epoch);
    std::int64_t sec = rel / 10000000, rem = rel % 10000000;
    if (rem < 0)
        --sec, rem += 10000000;
    return FileTime{sec, static_cast<std::uint32_t>(rem * 100)};
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0)
        return std::nullopt;
#ifdef __APPLE__
    return FileTime{st.st_mtimespec.tv_sec, static_cast<std::uint32_t>(st.st_mtimespec.tv_nsec)};
#else
    return FileTime{st.st_mtim.tv_sec, static_cast<std::uint32_t>(st.st_mtim.tv_nsec)};
#endif
#endif
}

namespace {
struct LocalTime {
    std::tm tm{};
    long offset = 0; // seconds east of UTC
};

LocalTime local(std::int64_t sec) {
    LocalTime lt;
    std::time_t t = static_cast<std::time_t>(sec);
#ifdef _WIN32
    localtime_s(&lt.tm, &t);
    long tz = 0, dst = 0;
    _get_timezone(&tz);
    _get_dstbias(&dst);
    lt.offset = -(tz + (lt.tm.tm_isdst > 0 ? dst : 0));
#else
    localtime_r(&t, &lt.tm);
    lt.offset = lt.tm.tm_gmtoff;
#endif
    return lt;
}
} // namespace

std::string format_unified_time(FileTime ft) {
    auto lt = local(ft.sec);
    char buf[80];
    long off = lt.offset;
    char sign = off < 0 ? '-' : '+';
    off = off < 0 ? -off : off;
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%09u %c%02ld%02ld", lt.tm.tm_year + 1900,
                  lt.tm.tm_mon + 1, lt.tm.tm_mday, lt.tm.tm_hour, lt.tm.tm_min, lt.tm.tm_sec, ft.nsec, sign,
                  off / 3600, off / 60 % 60);
    return buf;
}

std::string format_context_time(FileTime ft) {
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto lt = local(ft.sec);
    char buf[80];
    std::snprintf(buf, sizeof buf, "%s %s %2d %02d:%02d:%02d %d", days[lt.tm.tm_wday], months[lt.tm.tm_mon],
                  lt.tm.tm_mday, lt.tm.tm_hour, lt.tm.tm_min, lt.tm.tm_sec, lt.tm.tm_year + 1900);
    return buf;
}

namespace {

constexpr std::string_view kHelp =
    R"(Usage: splinter [OPTION]... FILE1 FILE2          compare files (GNU diff options)
       splinter -m [OPTION]... MINE OLDER YOURS   merged file (GNU diff3 -m)
       splinter -3 [OPTION]... MINE OLDER YOURS   three-way report (GNU diff3)
       splinter merge [OPTION]... FILE1 FILE2 FILE3   merge into FILE1 (RCS merge)
       splinter merge-file [OPTION]... CURRENT BASE OTHER   (git merge-file)
       splinter dump FILE | splinter inspect [--json] [--layout] FILE
       splinter git-setup [--print|--local|--global]   Git drivers (git-diff, git-merge)
       splinter formats [--json] | splinter version

The personality also follows the program name: splinter-diff, splinter-diff3,
splinter-merge, splinter-patch. Each accepts the options of the tool it emulates.
See docs/CLI.md for the full option tables.

Exit status: 0 same/clean, 1 different/conflicts, 2 trouble
(--exit-codes=extended: 2 invalid input, 3 unsupported, 4 limit, 5 I/O, 6 internal).
)";

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int formats(const std::vector<std::string>& args, Console& c, const Registry& reg) {
    bool json = false;
    for (auto& a : args) {
        if (a == "--json")
            json = true;
        else
            throw UsageError("extra operand '" + a + "'");
    }
    if (json) {
        auto arr = nlohmann::ordered_json::array();
        for (auto& f : reg.formats()) {
            auto& i = f->info();
            nlohmann::ordered_json j;
            j["name"] = i.name;
            j["description"] = i.description;
            j["extensions"] = i.extensions;
            j["capabilities"] = {{"diff", i.caps.diff},
                                 {"merge", i.caps.merge},
                                 {"patch", i.caps.patch},
                                 {"dump", i.caps.dump},
                                 {"visual", i.caps.visual}};
            auto opts = nlohmann::ordered_json::array();
            for (auto& o : i.options)
                opts.push_back(std::string("--") + std::string(o.name));
            j["options"] = opts;
            arr.push_back(j);
        }
        c.out(arr.dump(2) + "\n");
        return 0;
    }
    for (auto& f : reg.formats()) {
        auto& i = f->info();
        std::string line = std::string(i.name);
        line.resize(10, ' ');
        line += i.description;
        c.out(line + "\n");
    }
    return 0;
}

// splinter inspect [--json] [--layout] [--format=NAME] FILE (CLI.md §6).
int inspect(const std::vector<std::string>& args, Console& c, const Registry& reg) {
    bool json = false, layout = false;
    std::string format;
    std::vector<std::string> files;
    for (auto& a : args) {
        if (a == "--json")
            json = true;
        else if (a == "--layout")
            layout = true;
        else if (a.starts_with("--format="))
            format = a.substr(9);
        else if (a.size() > 1 && a[0] == '-')
            throw UsageError("unrecognized option '" + a + "'");
        else
            files.push_back(a);
    }
    if (files.size() != 1)
        throw UsageError(files.empty() ? "missing operand after 'inspect'"
                                       : "extra operand '" + files[1] + "'");
    Limits limits;
    auto j = semsplinter::inspect(reg, {read_input(files[0], limits, c), files[0]}, format, layout);
    c.out(json ? j.dump(2) + "\n" : render_inspect(j));
    return 0;
}

// The Git adapter's view of the console (D-53: the adapter does not include CLI headers).
class GitHost final : public git::Host {
public:
    explicit GitHost(Console& c) : c_(c) {}
    void out(std::string_view s) override { c_.out(s); }
    void err(std::string_view s) override { c_.err(s); }
    std::optional<std::string> env(const char* name) const override { return c_.env(name); }
    std::optional<std::pair<int, std::string>> git(const std::vector<std::string>& args) override {
        std::vector<std::string> argv{"git"};
        argv.insert(argv.end(), args.begin(), args.end());
        auto r = c_.run(argv);
        return r ? std::optional(std::pair{r->status, r->out}) : std::nullopt;
    }

private:
    Console& c_;
};

int not_yet(Console& c, std::string_view what, std::string_view phase) {
    diag(c, std::string(what) + " is not implemented yet (" + std::string(phase) + ")");
    return 2;
}

} // namespace

int run(std::vector<std::string> args, Console& c) {
    const Registry reg = Registry::with_builtin_formats();
    std::string prog = args.empty() ? "splinter" : args[0];
    std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    if (auto env = c.env("SPLINTER_OPTIONS")) { // prepended to the arguments (CLI.md §1.2)
        std::vector<std::string> pre;
        std::string cur;
        for (char ch : *env + " ") {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!cur.empty())
                    pre.push_back(std::move(cur));
                cur.clear();
            } else {
                cur += ch;
            }
        }
        rest.insert(rest.begin(), pre.begin(), pre.end());
    }

    // Rule 1: the program name.
    auto base = lower(from_path(to_path(prog).filename()));
    if (base.ends_with(".exe"))
        base.resize(base.size() - 4);
    try {
        if (base.find("diff3") != std::string::npos)
            return run_diff3(rest, c, reg, false);
        if (base.find("merge") != std::string::npos)
            return run_merge(rest, c, reg);
        if (base.find("patch") != std::string::npos)
            return not_yet(c, "patch mode", "Phase 5");
        if (base.find("diff") == std::string::npos && !rest.empty()) {
            // Rule 2: a subcommand word. Rule 3: a mode flag.
            const std::string first = rest[0];
            std::vector<std::string> tail(rest.begin() + 1, rest.end());
            if (first == "diff" || first == "--diff")
                return run_diff(tail, c, reg);
            if (first == "diff3" || first == "-3" || first == "--diff3")
                return run_diff3(tail, c, reg, false);
            if (first == "-m" || first == "--merge")
                return run_diff3(tail, c, reg, true);
            if (first == "merge")
                return run_merge(tail, c, reg);
            if (first == "merge-file")
                return run_merge_file(tail, c, reg);
            if (first == "patch" || first == "-P" || first == "--patch")
                return not_yet(c, "patch mode", "Phase 5");
            if (first == "dump") {
                tail.insert(tail.begin(), "--dump");
                return run_diff(tail, c, reg);
            }
            if (first == "formats")
                return formats(tail, c, reg);
            if (first == "inspect")
                return inspect(tail, c, reg);
            if (first == "git-diff" || first == "git-merge" || first == "git-setup") {
                GitHost host(c);
                return first == "git-diff"    ? git::git_diff(tail, host, reg)
                       : first == "git-merge" ? git::git_merge(tail, host, reg)
                                              : git::git_setup(tail, host);
            }
            if (first == "help") {
                c.out(kHelp);
                return 0;
            }
            if (first == "version") {
                c.out("splinter (Semantic Splinter) " + std::string(version()) + "\n");
                return 0;
            }
        }
        return run_diff(rest, c, reg); // rule 4
    } catch (const UsageError& e) {
        diag(c, e.what());
        c.err("splinter: Try 'splinter --help' for more information.\n");
        return 2;
    } catch (const Error& e) {
        diag(c, e.what());
        return 2;
    } catch (const std::exception& e) {
        diag(c, std::string("internal error: ") + e.what());
        return 2;
    }
}

std::string_view help_text() {
    return kHelp;
}

} // namespace splinter::cli
