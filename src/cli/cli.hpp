#pragma once

#include "args.hpp"
#include "tables.hpp"

#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/io.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace splinter::cli {

struct ProcessResult {
    int status = 0;
    std::string out; // standard output; standard error passes through
};

// Runs argv[0] (searched on PATH) without a shell. nullopt when it cannot be started.
std::optional<ProcessResult> run_process(const std::vector<std::string>& argv);

// Where output goes. The executable binds it to stdout/stderr; tests capture it.
class Console {
public:
    virtual ~Console() = default;
    virtual void out(std::string_view) = 0;
    virtual void err(std::string_view) = 0;
    virtual bool out_is_terminal() const { return false; }
    virtual std::optional<std::string> env(const char*) const { return std::nullopt; }
    virtual std::vector<std::byte> read_stdin() { return {}; }
    virtual std::optional<ProcessResult> run(const std::vector<std::string>& argv) {
        return run_process(argv);
    }
};

// Entry point: args[0] is the program name (personality rule 1).
int run(std::vector<std::string> args, Console&);

// ---- shared by the commands ----

std::string_view help_text();

// Exit code for an error under --exit-codes (SPEC §8); `extended` maps categories to 2..6.
int exit_code(semsplinter::ErrorCategory, bool extended);

// "splinter: " + message on stderr.
void diag(Console&, std::string_view message);

std::uint64_t parse_size(std::string_view value, std::string_view what);   // 10, 4K, 2M, 1G
std::uint64_t parse_number(std::string_view value, std::string_view what); // throws UsageError

// Applies --max-* limits and --exit-codes; returns true when the option was one of them.
struct CommonOptions {
    semsplinter::Limits limits;
    bool extended_exit = false;
    std::string format;              // --format
    std::optional<std::string> json; // --json[=FILE]: "" means stdout
    semsplinter::OptionValues format_options;
    bool apply(const ParsedOpt&, const semsplinter::Registry&);
};

// Reads FILE ("-" = stdin) within the input limit.
semsplinter::ByteSource read_input(const std::string& path, const semsplinter::Limits&, Console&);

// Writes the JSON report to stdout ("") or a file (atomic).
void write_json(Console&, const std::optional<std::string>& target, const std::string& json);

// File timestamps for unified/context headers (text.md §5.2, Q-TEXT-11).
struct FileTime {
    std::int64_t sec = 0;
    std::uint32_t nsec = 0;
};
std::optional<FileTime> file_mtime(const std::filesystem::path&);
std::string format_unified_time(FileTime);
std::string format_context_time(FileTime);

std::filesystem::path to_path(const std::string& utf8);
std::string from_path(const std::filesystem::path&);

int run_diff(const std::vector<std::string>& args, Console&, const semsplinter::Registry&);
int run_diff3(const std::vector<std::string>& args, Console&, const semsplinter::Registry&, bool merge_flag);
int run_merge(const std::vector<std::string>& args, Console&, const semsplinter::Registry&);
int run_merge_file(const std::vector<std::string>& args, Console&, const semsplinter::Registry&);

} // namespace splinter::cli
