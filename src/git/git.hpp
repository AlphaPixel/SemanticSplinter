#pragma once

// The Git adapter (CLI.md §7): argument and exit-code mapping over the public library API (D-53). It is built
// with the public headers only, so any version-control host can be written the same way.

#include <semsplinter/format.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace splinter::git {

// What the adapter needs from the program that hosts it.
class Host {
public:
    virtual ~Host() = default;
    virtual void out(std::string_view) = 0;
    virtual void err(std::string_view) = 0;
    virtual std::optional<std::string> env(const char* name) const = 0;
    // Runs `git ARGS...` without a shell: (exit status, stdout), or nullopt when git cannot be started.
    virtual std::optional<std::pair<int, std::string>> git(const std::vector<std::string>& args) = 0;
};

// External diff driver: `git-diff [--trust-exit-code] [--driver=NAME] [--format=NAME] path old-file old-hex
// old-mode new-file new-hex new-mode [new-path rename-info]`. Exit 0 unless trusted (D-16) or trouble.
int git_diff(const std::vector<std::string>& args, Host&, const semsplinter::Registry&);

// Merge driver: `git-merge %O %A %B [%L [%P [%S %X %Y]]]` or the named form `--base --ours --theirs
// [--output] [--path] [--marker-size]`; `--conflict=ours|theirs|base|mark`, `-L LABEL` (ours, base, theirs),
// `--format=NAME`, `--quiet`. Writes the result into %A (or --output); exit 0 clean, 1 conflicts, 2 trouble.
int git_merge(const std::vector<std::string>& args, Host&, const semsplinter::Registry&);

// `git-setup [--print|--local|--global] [--program=CMD]`: the diff and merge driver configuration.
int git_setup(const std::vector<std::string>& args, Host&);

} // namespace splinter::git
