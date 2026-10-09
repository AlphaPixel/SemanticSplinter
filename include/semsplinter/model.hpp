#pragma once

#include <semsplinter/hash.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace semsplinter {

// Core data model (SPEC §3).

enum class Op : std::uint8_t { insert, erase, update, move };
enum class Side : std::uint8_t { base, ours, theirs };

std::string_view to_string(Op) noexcept;
std::string_view to_string(Side) noexcept;

struct Rect {
    std::uint64_t x, y, w, h;
    bool operator==(const Rect&) const = default;
};
struct Range {
    std::uint64_t start, length;
    bool operator==(const Range&) const = default;
};
struct Box {
    std::uint64_t x, y, z, w, h, d;
    bool operator==(const Box&) const = default;
};
using Region = std::variant<Rect, Range, Box>;

std::string to_string(const Region&); // "rect=x,y,w,h", "range=start,length", "box=x,y,z,w,h,d"

// A semantic unit, independent of byte layout: path [ "#" region ] (SPEC §3.1).
struct Address {
    std::string path; // RFC 6901 JSON Pointer over identity keys
    std::optional<Region> region;

    std::string to_string() const;
    bool operator==(const Address&) const = default;
};

// Escapes one JSON Pointer reference token ("~" -> "~0", "/" -> "~1").
std::string pointer_escape(std::string_view segment);

// True when `path` equals `ancestor` or lies below it ("/a/b" is within "/a", "/ab" is not).
bool path_within(std::string_view path, std::string_view ancestor) noexcept;

struct Value {
    std::optional<std::string> text; // human-readable rendering (truncated for reports)
    std::optional<Hash128> hash;     // content hash of the exact bytes
};

struct Change {
    std::string id; // make_change_id(side, op, at)
    Op op = Op::update;
    Address at;
    std::optional<Address> from; // for move
    Value before, after;
    Side side = Side::theirs;            // ours/theirs for merges; theirs for a 2-way diff
    std::vector<std::string> depends_on; // addresses of entities this change requires
    std::string kind;                    // format-specific subtype: "pixels", "text", "metadata", ...
    std::optional<double> confidence;    // only when heuristic matching produced the pairing
    nlohmann::ordered_json metrics = nlohmann::ordered_json::object();
};

struct Conflict {
    std::string id; // make_conflict_id(at, changes)
    Address at;
    std::vector<std::string> changes; // competing change ids
    std::string reason;               // "both-modified", "modify-delete", "add-add", ...
    std::vector<std::string> choices = {"ours", "theirs", "base"};
    std::string resolved_as = "ours"; // what the written output contains (SPEC §5.4)
    bool resolved = false;            // true when a --resolve entry or --ours/--theirs/--union decided it
};

using ChangeSet = std::vector<Change>;

// "c-" + first 12 hex digits of hash(side, op, address).
std::string make_change_id(Side, Op, const Address&);
// "x-" + first 12 hex digits of hash(address, sorted change ids).
std::string make_conflict_id(const Address&, std::vector<std::string> change_ids);

// Builds a change with its deterministic id.
Change make_change(Side, Op, Address, std::string kind = {});
Conflict make_conflict(Address, std::vector<std::string> change_ids, std::string reason);

// Canonical order: by address, then op (SPEC §3.2).
void sort_canonical(ChangeSet&);

struct AutoMerge {
    std::string change; // change id
    std::string rule; // "independent", "identical", "both-inserted", "dependency-touched", "renumbered", ...
    bool operator==(const AutoMerge&) const = default;
};

struct Warning {
    std::string code; // "derived-stale", "mark-unsupported", ...
    std::string at;   // address, may be empty
    std::string message;
    bool operator==(const Warning&) const = default;
};

// Format-private model of a loaded file.
struct Document {
    virtual ~Document() = default;
};

struct MergeResult {
    std::unique_ptr<Document> document; // complete; conflicted units hold the policy-selected side
    ChangeSet changes_ours, changes_theirs;
    std::vector<AutoMerge> auto_merged;
    std::vector<Conflict> conflicts;
    std::vector<Warning> warnings;

    std::size_t unresolved_conflicts() const noexcept;
};

} // namespace semsplinter
