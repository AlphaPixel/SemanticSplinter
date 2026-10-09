#pragma once

// GNU getopt_long-compatible argument parser (CLI.md §1.2, D-13): short clusters, attached or detached
// values, long options with unique-prefix abbreviation, optional arguments only with '=', `--`, and
// permutation unless POSIXLY_CORRECT is set. Hand-written: musl's getopt_long is a C global-state API, and
// the per-personality tables plus GNU diff's message wording are simpler to own.

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace splinter::cli {

enum class Arg { none, required, optional };

struct Opt {
    std::string_view long_name; // empty: short only
    char short_name = 0;        // 0: long only
    Arg arg = Arg::none;
    int id = 0;
};

struct ParsedOpt {
    int id;
    std::optional<std::string> value;
    std::string spelling; // "-U" or "--unified" as matched, for messages
};

struct Parsed {
    std::vector<ParsedOpt> opts;
    std::vector<std::string> operands;
    // The raw arguments consumed as options (GNU echoes them in -r headers).
    std::vector<std::string> option_tokens;
};

// Usage errors carry GNU diff's wording, e.g. "unknown option -- k".
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Parsed parse(std::span<const std::string> args, std::span<const Opt> table, bool posixly_correct);

// Resolves a long option name: exact match, else a unique prefix (candidates that share id and argument
// kind count as one, as in getopt_long). Returns null when unknown; throws UsageError when ambiguous.
const Opt* find_long(std::string_view name, std::span<const Opt> table);

} // namespace splinter::cli
