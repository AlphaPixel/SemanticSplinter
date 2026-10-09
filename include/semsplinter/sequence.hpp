#pragma once

#include <semsplinter/hash.hpp>
#include <semsplinter/model.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace semsplinter {

// ---- Two-way sequence diff (src/core/diff) ----

using Token = std::uint64_t;                       // equal tokens mean equal elements
using Match = std::pair<std::size_t, std::size_t>; // (index in a, index in b)

enum class DiffAlgorithm : std::uint8_t {
    myers,     // Myers O(ND), linear space
    minimal,   // -d/--minimal: always a minimal edit script
    histogram, // Git's default family; falls back to Myers where no element is rare enough
};

// Matched pairs, strictly increasing in both indices. Deterministic.
std::vector<Match> diff_match(std::span<const Token> a, std::span<const Token> b, DiffAlgorithm);

// ---- SequenceMerge (SPEC §5.1) ----

struct SeqItem {
    Hash128 content;           // hash of the element's exact content
    std::optional<Hash128> id; // explicit identity, if the format has one; unique within a side
};

enum class SeqKind : std::uint8_t {
    stable,        // unchanged on both sides
    ours,          // only ours changed
    theirs,        // only theirs changed
    identical,     // both changed the same way
    both_inserted, // both inserted at one anchor where order is insignificant: ours then theirs
    recursed,      // an identity-matched element changed on both sides, merged by the format's callback
    conflict,
};

std::string_view to_string(SeqKind) noexcept;

// One output chunk. Index lists refer to the original inputs, in order.
struct SeqChunk {
    SeqKind kind = SeqKind::stable;
    std::vector<std::size_t> base, ours, theirs;
    bool moved = false; // an ID-matched element relocated by a side; placed at the mover's position
    std::string reason; // conflict reason: both-modified, modify-delete, add-add, move-move
};

struct SeqMergeOptions {
    DiffAlgorithm algorithm = DiffAlgorithm::histogram;
    bool insertion_order_insignificant = false; // add-add at one anchor keeps both, ours first
    // Called for an identity-matched element that both sides changed differently. Returns true when the
    // format merged it (chunk kind `recursed`), false for a `both-modified` conflict.
    std::function<bool(std::optional<std::size_t> base, std::size_t ours, std::size_t theirs)> recurse;
};

std::vector<SeqChunk> sequence_merge(std::span<const SeqItem> base, std::span<const SeqItem> ours,
                                     std::span<const SeqItem> theirs, const SeqMergeOptions& = {});

struct SeqPick {
    Side side;
    std::size_t index;
    bool operator==(const SeqPick&) const = default;
};

// The elements a chunk contributes to the output. `choice` applies to conflicts: "ours", "theirs",
// "base", or "union" (ours then theirs). A recursed chunk yields ours' element as the placeholder
// for the format's merged element.
std::vector<SeqPick> picks(const SeqChunk&, std::string_view choice = "ours");

} // namespace semsplinter
