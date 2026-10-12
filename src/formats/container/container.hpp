#pragma once

// Member layer shared by the zip and tar modules (containers.md §3-§6): member identity by path, renames by
// content hash, KeyedMerge of member sets, per-field metadata merge, and recursion into members through
// the registry with the shared budget and depth (D-47). Also the zlib and zstd codecs both modules use.

#include <semsplinter/format.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace semsplinter::container {

using Meta = std::vector<std::pair<std::string, std::string>>; // (field, display value), in a fixed order

struct OutEntry;

// A loaded archive as the member layer sees it.
struct Archive : Document {
    struct Entry {
        std::string path;    // identity key (UTF-8, '/' separated; TAR: "#N" suffix on earlier duplicates)
        Meta meta;           // compared and merged per field
        bool opaque = false; // content cannot be decoded (encrypted, unknown method): no recursion
    };
    std::vector<Entry> entries;
    std::vector<Warning> warnings; // load-time

    // Equal content; decodes only when the cheap checks cannot decide.
    virtual bool same(std::size_t i, const Archive& other, std::size_t j, Limits&) const = 0;
    virtual Hash128 hash(std::size_t i, Limits&) const = 0; // content identity, for renames
    // Decoded content, within the budget. Throws Error{unsupported} for opaque members.
    virtual std::span<const std::byte> content(std::size_t i, Limits&) const = 0;

    // Patches (containers.md §6.5): `entries` picked from `docs` (base, ours, theirs; null where unused),
    // written in this archive's layout (prefix, comment, end records, outer compression), or without any of
    // it when `bare` (payload archives).
    virtual std::vector<std::byte> write(const std::array<const Archive*, 3>& docs,
                                         const std::vector<OutEntry>& entries, bool bare, Limits&,
                                         std::vector<Warning>&) const = 0;
    // What nested payloads load members with; set by the format's load() through remember().
    const Registry* registry = nullptr;
    OptionValues options;
    Limits limits;
    int depth = 0;
    void remember(const Context& c) {
        registry = &c.registry, options = c.format_options, limits = c.budget.fresh(), depth = c.depth;
    }
};

std::string entry_address(std::string_view path); // "/entries/" + escaped path

// Absolute, drive-letter, backslash, NUL, or ".." paths: warned `suspicious-path` (never used for file
// access).
bool suspicious_path(std::string_view);

// Two-way member diff (containers.md §5): added, removed, renamed (equal content hash), metadata per field,
// and content changes from the member format's own diff, re-addressed under /entries/<path>/content.
ChangeSet diff(const Archive& a, const Archive& b, const DiffOptions&, Side side = Side::theirs);

struct Pick {
    Side side = Side::ours;
    std::size_t index = 0;
    bool operator==(const Pick&) const = default;
};

// One member of a merge result: the record of `body` written again, with `content` replacing its content
// when set, and the fields in `from` ("path" and the meta fields) taken from other members.
struct OutEntry {
    Pick body;
    std::optional<std::vector<std::byte>> content;
    std::map<std::string, Pick> from;
    bool raw() const noexcept { return !content && from.empty(); }
};

struct Merged {
    std::vector<OutEntry> entries; // output order (SPEC KeyedMerge order)
    MergeResult result;            // everything but the document
};

// Three-way merge of the member sets (containers.md §6.1, §6.2). Conflicts are already resolved by policy.
Merged merge(const Archive& base, const Archive& ours, const Archive& theirs, const MergeOptions&);

// True when the merge result is ours member for member, so ours' bytes can be written verbatim.
bool is_ours(const Merged&, const Archive& ours);

// Appends the members' own dumps, sorted by path, after the caller's entry lines (containers.md §5).
void dump_members(const Archive&, std::ostream&, const DumpOptions&);

// Patch payloads (containers.md §6.5) under /entries: a member is an archive of its format holding it alone
// (for a /meta/ unit, with empty content); /content is the member's bytes; a nested address is the member
// format's payload when that format has patches, else the member's bytes.
std::optional<std::vector<std::byte>> payload(const Archive&, const Address&);
// `t` with the /entries units set, as bytes in its layout. Members the patch adds go last. Throws
// Error{unsupported} for other addresses. `self` loads the payload archives.
std::vector<std::byte> implant(const Format& self, const Archive& t, std::span<const UnitValue>,
                               const LoadOptions&, std::vector<Warning>&);

// A Comparison whose exit status ignores `/meta/mtime` changes unless `count_times` (containers.md §4).
std::unique_ptr<Comparison> compare(std::unique_ptr<Comparison> inner, bool count_times);

// ---- Codecs ----

// Decoders return at most max_out + 1 bytes, so one byte past max_out means "more follows". Corrupt data
// is Error{invalid_input}; with `partial`, input that ends early is not an error (probing).
// window_bits: -15 raw deflate, 31 gzip (consecutive gzip members are decoded as one stream).
std::vector<std::byte> inflate(std::span<const std::byte>, std::uint64_t max_out, int window_bits,
                               bool partial = false);
std::vector<std::byte> deflate(std::span<const std::byte>, int level, int window_bits = -15);
std::vector<std::byte> unzstd(std::span<const std::byte>, std::uint64_t max_out, bool partial = false);
std::vector<std::byte> zstd(std::span<const std::byte>, int level, bool checksum);

// The decode limit for `compressed` bytes of a container: the shared --max-zip-size and --max-memory
// budget, and --max-zip-ratio above 1 MiB (containers.md §8.1).
std::uint64_t decode_limit(const Limits&, std::uint64_t compressed);
// Charges `n` decoded bytes to both budgets; Error{limit} when either is exhausted.
void charge(Limits&, std::uint64_t n);

} // namespace semsplinter::container
