#include "git.hpp"

#include <semsplinter/error.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/report.hpp>
#include <semsplinter/semsplinter.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace splinter::git {

using namespace semsplinter;

namespace {

std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

void diag(Host& h, const std::string& msg) {
    h.err("splinter: " + msg + "\n");
}

// A missing side: Git passes "/dev/null"; Git for Windows passes its null device, "nul".
bool null_device(std::string f) {
    std::transform(f.begin(), f.end(), f.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return f == "/dev/null" || f == "nul";
}

Input read(const std::string& file, const std::string& logical, const Limits& limits) {
    if (null_device(file))
        return {ByteSource::from_memory({}, logical), logical};
    return {ByteSource::from_file(fs_path(file), limits), logical};
}

bool truthy(std::string v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
        v.pop_back();
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "true" || v == "yes" || v == "on" || v == "1";
}

std::optional<std::string> config(Host& h, const std::string& key, bool boolean = false) {
    std::vector<std::string> args{"config"};
    if (boolean)
        args.push_back("--type=bool");
    args.insert(args.end(), {"--get", key});
    auto r = h.git(args);
    if (!r || r->first != 0)
        return std::nullopt;
    auto v = r->second;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r'))
        v.pop_back();
    return v;
}

Policy policy(const std::string& v) {
    if (v == "ours")
        return Policy::ours;
    if (v == "theirs")
        return Policy::theirs;
    if (v == "base")
        return Policy::base;
    if (v == "mark")
        return Policy::mark;
    throw Error(ErrorCategory::invalid_input,
                "invalid conflict policy '" + v + "' (ours, theirs, base, mark)");
}

// "--name=value" or "--name value".
bool option(const std::vector<std::string>& args, std::size_t& i, std::string_view name, std::string& value) {
    const std::string& a = args[i];
    if (a == name) {
        if (i + 1 == args.size())
            throw Error(ErrorCategory::invalid_input, "option requires an argument -- " + std::string(name));
        value = args[++i];
        return true;
    }
    if (a.size() > name.size() && a.starts_with(name) && a[name.size()] == '=') {
        value = a.substr(name.size() + 1);
        return true;
    }
    return false;
}

std::size_t number(const std::string& v, const char* what) {
    if (v.empty() || !std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); }))
        throw Error(ErrorCategory::invalid_input, std::string("invalid ") + what + " '" + v + "'");
    return static_cast<std::size_t>(std::stoull(v));
}

} // namespace

int git_diff(const std::vector<std::string>& args, Host& h, const Registry& reg) {
    try {
        bool trust_flag = false;
        std::string driver = "splinter", format, v;
        std::vector<std::string> pos;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--trust-exit-code")
                trust_flag = true;
            else if (option(args, i, "--driver", v))
                driver = v;
            else if (option(args, i, "--format", v))
                format = v;
            else
                pos.push_back(args[i]);
        }
        if (pos.size() == 1) { // an unmerged path
            h.out("* Unmerged path " + pos[0] + "\n");
            return 0;
        }
        if (pos.size() != 7 && pos.size() != 9)
            throw Error(ErrorCategory::invalid_input,
                        "git-diff: expected 7 or 9 arguments from Git, got " + std::to_string(pos.size()));
        const std::string& path = pos[0];
        const std::string& new_path = pos.size() == 9 ? pos[7] : path;
        const bool a_absent = null_device(pos[1]), b_absent = null_device(pos[4]);
        DiffRequest rq;
        rq.a = read(pos[1], path, rq.limits);
        rq.b = read(pos[4], new_path, rq.limits);
        rq.format = format;
        const bool text = detect_format(reg, {&rq.a, &rq.b}, format).info().name == "text";
        // No timestamps in Git modes (CLI.md §9); text as a unified diff, other formats as their report.
        const std::string la = a_absent ? "/dev/null" : "a/" + path,
                          lb = b_absent ? "/dev/null" : "b/" + new_path;
        rq.render = RenderRequest{
            text ? OutputStyle::unified : OutputStyle::normal, {}, {pos[1], pos[4]}, {la, lb}, {la, lb}};
        auto r = semsplinter::diff(reg, rq);
        for (auto& [p, w] : r.warnings)
            diag(h, "warning: " + (p.empty() ? "" : p + ": ") + w.message +
                        (w.at.empty() ? "" : " (" + w.at + ")"));

        std::string extra;
        if (pos.size() == 9 && !pos[8].empty())
            extra += pos[8].ends_with('\n') ? pos[8] : pos[8] + "\n";
        if (a_absent)
            extra += "new file mode " + pos[6] + "\n";
        else if (b_absent)
            extra += "deleted file mode " + pos[3] + "\n";
        else if (pos[3] != pos[6])
            extra += "old mode " + pos[3] + "\nnew mode " + pos[6] + "\n";
        if (!extra.empty() || !r.text.empty())
            h.out("diff --git a/" + path + " b/" + new_path + "\n" + extra + r.text);
        if (!r.differs)
            return 0;
        // D-16: GNU exit codes only when Git trusts them (Git 2.46+).
        const bool trust = trust_flag || truthy(h.env("GIT_EXTERNAL_DIFF_TRUST_EXIT_CODE").value_or("")) ||
                           truthy(config(h, "diff." + driver + ".trustExitCode", true).value_or("")) ||
                           truthy(config(h, "diff.trustExitCode", true).value_or(""));
        return trust ? 1 : 0;
    } catch (const Error& e) {
        diag(h, e.what());
        return 2;
    }
}

int git_merge(const std::vector<std::string>& args, Host& h, const Registry& reg) {
    try {
        std::optional<std::string> base, ours, theirs, output, path, conflict;
        std::string format, marker = "7", v;
        std::vector<std::string> labels, pos;
        bool quiet = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (option(args, i, "--base", v))
                base = v;
            else if (option(args, i, "--ours", v))
                ours = v;
            else if (option(args, i, "--theirs", v))
                theirs = v;
            else if (option(args, i, "--output", v))
                output = v;
            else if (option(args, i, "--path", v))
                path = v;
            else if (option(args, i, "--marker-size", v))
                marker = v;
            else if (option(args, i, "--conflict", v))
                conflict = v;
            else if (option(args, i, "--format", v))
                format = v;
            else if (option(args, i, "-L", v))
                labels.push_back(v);
            else if (args[i] == "-q" || args[i] == "--quiet")
                quiet = true;
            else
                pos.push_back(args[i]);
        }
        // Positional form: %O %A %B %L %P %S %X %Y (CLI.md §7.3).
        if (pos.size() > 8)
            throw Error(ErrorCategory::invalid_input, "git-merge: too many arguments");
        auto take = [&](std::size_t k, std::optional<std::string>& slot) {
            if (pos.size() > k && !slot)
                slot = pos[k];
        };
        take(0, base), take(1, ours), take(2, theirs), take(4, path);
        if (pos.size() > 3)
            marker = pos[3];
        if (pos.size() > 5 && labels.empty()) // %S %X %Y: ancestor, local, other
            labels = {pos.size() > 6 ? pos[6] : "ours", pos[5], pos.size() > 7 ? pos[7] : "theirs"};
        if (!base || !ours || !theirs)
            throw Error(ErrorCategory::invalid_input,
                        "git-merge: expected %O %A %B (or --base, --ours, --theirs)");
        if (labels.size() > 3)
            throw Error(ErrorCategory::invalid_input, "too many -L labels");

        MergeRequest rq;
        // Git's temp files have no extensions: detection uses %P, then content probing (CLI.md §7.3).
        const std::string logical = path.value_or("");
        rq.base = read(*base, logical, rq.limits);
        rq.ours = read(*ours, logical, rq.limits);
        rq.theirs = read(*theirs, logical, rq.limits);
        rq.format = format;
        const bool text = detect_format(reg, {&rq.base, &rq.ours, &rq.theirs}, format).info().name == "text";
        // D-21: markers for text, ours for everything else; `git config splinter.conflict` or --conflict
        // override.
        if (!conflict)
            conflict = config(h, "splinter.conflict");
        rq.conflict.policy = conflict ? policy(*conflict) : text ? Policy::mark : Policy::ours;
        rq.text.engine = MergeEngine::git;
        rq.text.algorithm = {DiffAlgorithm::myers, true, Compaction::gnu, 0};
        rq.text.marker_size = number(marker, "marker size");
        rq.labels = {"ours", "base", "theirs"};
        for (std::size_t k = 0; k < labels.size(); ++k)
            rq.labels[k] = labels[k];
        const std::string out = output.value_or(*ours);
        rq.output = fs_path(out);
        rq.output_name = path.value_or(out);
        // Sidecars belong next to the work-tree path, never next to Git's temp file (%A).
        rq.mark_visual = path ? fs_path(*path + ".splinter-conflicts.png") : std::filesystem::path{};

        auto m = semsplinter::merge(reg, rq);
        for (auto& w : m.warnings)
            if (!quiet && w.code != "mark-unsupported" && w.code != "encoding-reencoded")
                diag(h, "warning: " + w.message + (w.at.empty() ? "" : " (" + w.at + ")"));
        // The sidecar report next to the work-tree path; a clean merge removes a stale one.
        std::optional<std::filesystem::path> sidecar;
        if (path)
            sidecar = fs_path(*path + ".splinter-conflicts.json");
        if (m.conflicts && sidecar) {
            atomic_write(*sidecar, as_bytes(to_json(m.report).dump(2) + "\n"));
        } else if (sidecar) {
            std::error_code ec;
            std::filesystem::remove(*sidecar, ec);
        }
        if (m.conflicts && !quiet) {
            static const char* written[] = {"ours", "theirs", "base", "conflict markers"};
            diag(h, rq.output_name + ": " + std::to_string(m.conflicts) +
                        (m.conflicts == 1 ? " conflict" : " conflicts") + " (" + m.format + "); wrote " +
                        written[static_cast<int>(rq.conflict.policy)] +
                        (sidecar ? "; details in " + *path + ".splinter-conflicts.json" : ""));
        }
        return m.conflicts ? 1 : 0;
    } catch (const Error& e) {
        diag(h, e.what());
        return 2;
    }
}

namespace {

// A git-config value as it is written in a config file.
std::string ini_value(const std::string& v) {
    if (v.find_first_of("\"\\;#") == std::string::npos && !v.empty() && v.front() != ' ' && v.back() != ' ')
        return v;
    std::string q = "\"";
    for (char c : v)
        q += c == '"' || c == '\\' ? std::string{'\\', c} : std::string(1, c);
    return q + "\"";
}

} // namespace

int git_setup(const std::vector<std::string>& args, Host& h) {
    try {
        std::string scope, program = "splinter", v;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--print")
                scope.clear();
            else if (args[i] == "--local" || args[i] == "--global")
                scope = args[i];
            else if (option(args, i, "--program", v))
                program = v;
            else
                throw Error(ErrorCategory::invalid_input,
                            "git-setup: unrecognized argument '" + args[i] + "'");
        }
        // Git runs drivers through sh: forward slashes, and quotes around a path with spaces.
        std::replace(program.begin(), program.end(), '\\', '/');
        if (program.find_first_of(" \t'\"") != std::string::npos)
            program = "\"" + program + "\"";
        const std::vector<std::pair<std::string, std::string>> entries = {
            {"diff.splinter.textconv", program + " dump"},
            {"diff.splinter.cachetextconv", "true"},
            {"merge.splinter.name", "Semantic Splinter three-way merge"},
            {"merge.splinter.driver", program + " git-merge %O %A %B %L %P"},
            {"merge.splinter.recursive", "binary"},
            // Git LFS (D-17, git-lfs 3.2.0 or later): the wrapper smudges the three versions; %P is not
            // available.
            {"merge.splinter-lfs.name", "Semantic Splinter three-way merge (Git LFS)"},
            {"merge.splinter-lfs.driver",
             "git lfs merge-driver --ancestor %O --current %A --other %B --marker-size %L "
             "--output %A --program '" +
                 program + " git-merge %%O %%A %%B %%L --output %%D'"},
            {"merge.splinter-lfs.recursive", "binary"},
        };
        if (scope.empty()) {
            std::string o, section;
            for (auto& [key, value] : entries) {
                auto first = key.find('.'), last = key.rfind('.');
                auto sec =
                    "[" + key.substr(0, first) + " \"" + key.substr(first + 1, last - first - 1) + "\"]";
                if (sec != section)
                    o += (section.empty() ? "" : "\n") + sec + "\n", section = sec;
                o += "\t" + key.substr(last + 1) + " = " + ini_value(value) + "\n";
            }
            o += "\n# .gitattributes:\n#   *.png diff=splinter merge=splinter\n"
                 "#   *.png filter=lfs diff=splinter merge=splinter-lfs -text   (Git LFS paths)\n"
                 "# .gitignore:\n#   *.splinter-conflicts.json\n";
            h.out(o);
            return 0;
        }
        for (auto& [key, value] : entries) {
            auto r = h.git({"config", scope, key, value});
            if (!r)
                throw Error(ErrorCategory::io, "git-setup: cannot run git");
            if (r->first != 0)
                throw Error(ErrorCategory::io, "git-setup: 'git config " + scope + " " + key + "' failed");
        }
        h.out("Configured diff.splinter, merge.splinter, and merge.splinter-lfs (" + scope.substr(2) +
              "). Add attributes such as '*.png diff=splinter merge=splinter' to .gitattributes.\n");
        return 0;
    } catch (const Error& e) {
        diag(h, e.what());
        return 2;
    }
}

} // namespace splinter::git
