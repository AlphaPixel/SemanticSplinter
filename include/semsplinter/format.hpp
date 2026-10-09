#pragma once

// Format module contract and registry (SPEC §4, §11).

#include <semsplinter/envelope.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/limits.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/model.hpp>

#include <array>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

struct Capabilities {
    bool diff = true, merge = true, patch = false, dump = true, visual = false;
};

// A format-specific long option (`--png-...`), merged into the CLI tables at registration (D-51).
struct OptionSpec {
    enum class Arg : std::uint8_t { none, required, optional };
    std::string_view name; // without "--"
    Arg arg = Arg::none;
    std::string_view help;
    bool merge_only = false; // offered only by the merging personalities (diff3, merge, merge-file)
};

struct FormatInfo {
    std::string_view name; // "png"
    std::string_view description;
    std::vector<std::string_view> extensions; // ".png"
    Capabilities caps;
    std::vector<OptionSpec> options;
};

struct Probe {
    int confidence = 0; // 0..100
};

class Registry;
using OptionValues = std::map<std::string, std::string, std::less<>>;

// Shared by every call of one invocation, including recursion into container members (D-47).
struct Context {
    const Registry& registry;
    Limits& budget;
    int depth = 0;
    const OptionValues& format_options;
};

struct LoadOptions {
    Context& ctx;
};
struct DiffOptions {
    Context& ctx;
};
struct MergeOptions {
    Context& ctx;
    ConflictOptions conflict;
    std::array<std::string, 3> labels; // ours, base, theirs
    // Fill MergeResult::visual: always for --visual, on_conflict for the --conflict=mark sidecar.
    enum class Visual : std::uint8_t { none, on_conflict, always } visual = Visual::none;
};
struct SaveOptions {
    Context& ctx;
};
struct DumpOptions {
    Context& ctx;
};

class Format {
public:
    virtual ~Format() = default;
    virtual const FormatInfo& info() const = 0;

    // Magic-number sniff over the first <= 64 KiB plus the logical name.
    virtual Probe probe(std::span<const std::byte> head, std::string_view logical_name) const = 0;
    // Throws Error{invalid_input} on bad data; enforces ctx.budget.
    virtual std::unique_ptr<Document> load(ByteSource, const LoadOptions&) const = 0;
    virtual ChangeSet diff(const Document& a, const Document& b, const DiffOptions&) const = 0;
    // The output document is complete; conflicted units hold the policy-selected side (SPEC §5.4).
    virtual MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                              const MergeOptions&) const = 0;
    virtual void save(const Document&, ByteSink&, const SaveOptions&) const = 0;
    // Canonical, line-oriented, diff-friendly text for textconv and --dump.
    virtual void dump(const Document&, std::ostream&, const DumpOptions&) const = 0;
    virtual const EnvelopeMap* envelope(const Document&) const { return nullptr; }
    // The visual diff for --visual (a heatmap PNG, ...). Formats without one throw Error{unsupported}.
    virtual std::vector<std::byte> visual(const Document& a, const Document& b, const DiffOptions&) const;
    // Structure overview for `inspect`: integrity ("ok" or {status, at}), warnings, units.
    virtual nlohmann::ordered_json inspect(const Document&) const { return nlohmann::ordered_json::object(); }
    // Format-level statistics for the report's stats.format.
    virtual nlohmann::ordered_json stats(const Document&, const Document&, const ChangeSet&) const {
        return nlohmann::ordered_json::object();
    }
};

class Registry {
public:
    // Registration order breaks probe ties (SPEC §11).
    void add(std::unique_ptr<Format>);
    const Format* find(std::string_view name) const noexcept;
    const std::vector<std::unique_ptr<Format>>& formats() const noexcept { return formats_; }

    // SPEC §11: explicit name, then logical-name extension, then content probe, then text or binary.
    // Throws Error{unsupported} for an unknown explicit name.
    const Format& detect(std::span<const std::byte> content, std::string_view logical_name,
                         std::string_view explicit_name = {}) const;

    // Every compiled-in format module (defined by the formats library, which CMake assembles).
    static Registry with_builtin_formats();

private:
    std::vector<std::unique_ptr<Format>> formats_;
};

} // namespace semsplinter
