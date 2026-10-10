#pragma once

// Format module contract and registry (SPEC §4, §11).

#include <semsplinter/envelope.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/limits.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/model.hpp>
#include <semsplinter/render.hpp>
#include <semsplinter/textopts.hpp>

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
    TextLoadOptions text; // for text-bearing units
};
struct DiffOptions {
    Context& ctx;
    TextDiffOptions text;
};
struct MergeOptions {
    Context& ctx;
    ConflictOptions conflict;
    std::array<std::string, 3> labels; // ours, base, theirs
    // Fill MergeResult::visual: always for --visual, on_conflict for the --conflict=mark sidecar.
    enum class Visual : std::uint8_t { none, on_conflict, always } visual = Visual::none;
    TextMergeOptions text; // its labels and conflict are taken from the fields above
};
struct SaveOptions {
    Context& ctx;
};
struct DumpOptions {
    Context& ctx;
};

// How a comparison is shown (SPEC §7.2).
struct RenderRequest {
    OutputStyle style = OutputStyle::normal;
    RenderOptions options;
    std::array<std::string, 2> paths;  // operands as given (diagnostics)
    std::array<std::string, 2> names;  // --label or path (messages such as "Binary files A and B differ")
    std::array<std::string, 2> labels; // unified/context header labels, timestamps included
    const Churn* churn = nullptr;      // byte churn, summarized under a structured normal report
};

// A computed two-way diff of one format, rendered on demand. It refers to the documents and the
// DiffOptions it was made from, which must outlive it.
class Comparison {
public:
    virtual ~Comparison() = default;
    virtual bool differs() const = 0;      // a semantic difference: diff exit status 1
    virtual ChangeSet changes() const = 0; // for the JSON report
    virtual nlohmann::ordered_json stats() const { return nlohmann::ordered_json::object(); }
    virtual nlohmann::ordered_json options() const { return nlohmann::ordered_json::object(); }
    // Appends the output for `r` (nothing when there is nothing to show). Returns diagnostics that make
    // the diff fail (exit 2) after printing, as GNU does for ed scripts over a missing final newline.
    virtual std::vector<std::string> render(std::string& out, const RenderRequest& r) const = 0;
};

// A unit set by Format::implant(): the unit at `at` gets `value`, or is removed when it is null.
struct UnitValue {
    Address at;
    const std::vector<std::byte>* value = nullptr;
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
    // The diff behind the CLI and library API. The default wraps diff(): any non-encoding change differs;
    // the normal style lists changes plus the churn line, other styles diff the dump() lines as text.
    virtual std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                                const DiffOptions&) const;
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
    // Patch support (SPEC §4, §6), at the content layer: the value of the unit at an address diff()
    // reports (nullopt when the document has no such unit), and a copy of `target` with units set to such
    // values. A patch applies as merge(implant(target, before), target, implant(target, after)). Formats
    // with caps.patch implement both; the defaults throw Error{unsupported}.
    virtual std::optional<std::vector<std::byte>> payload(const Document&, const Address&) const;
    virtual std::unique_ptr<Document> implant(const Document& target, std::span<const UnitValue>,
                                              const LoadOptions&) const;
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
