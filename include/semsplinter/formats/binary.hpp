#pragma once

// The binary fallback module (docs/formats/binary.md): content-defined chunking, anchor diff with exact
// refinement, hex dump for textconv, opaque merge, and the opt-in byte merge.

#include <semsplinter/format.hpp>
#include <semsplinter/hash.hpp>
#include <semsplinter/io.hpp>
#include <semsplinter/linediff.hpp>
#include <semsplinter/model.hpp>

#include <memory>
#include <ostream>
#include <vector>

namespace semsplinter::binary {

struct Chunk {
    std::size_t off = 0, len = 0;
    std::uint64_t hash = 0; // XXH3 of the chunk bytes (confirmed by memcmp before use as an anchor)
};

struct BinDoc final : Document {
    ByteSource src;
    Hash128 hash;
    std::vector<Chunk> chunks;
};

// Gear-hash CDC with normalized chunking: 2/8/64 KiB, or 16/64/256 KiB above 256 MiB (Q-BIN-2).
std::vector<Chunk> chunk(std::span<const std::byte>);
std::unique_ptr<BinDoc> load(ByteSource);

struct BinDiff {
    std::vector<Edit> ranges; // byte ranges, coalesced
    std::uint64_t inserted = 0, deleted = 0, replaced = 0;
    double anchor_coverage = 1.0; // fraction of B inside matching chunks
    bool identical() const noexcept { return ranges.empty(); }
};

// `gap`: ranges closer than this many bytes are reported as one (Q-BIN-4).
BinDiff diff(const BinDoc& a, const BinDoc& b, std::size_t gap = 8);
ChangeSet changes(const BinDoc& a, const BinDoc& b, const BinDiff&);
nlohmann::ordered_json metrics(const BinDoc& a, const BinDoc& b, const BinDiff&);

struct DumpOptions {
    bool absolute_offsets = false;                   // --dump-offsets
    std::uint64_t max_dump = std::uint64_t{1} << 20; // above this input size, chunk headers only
};
void dump(const BinDoc&, std::ostream&, const DumpOptions& = {});

struct ByteMergeOptions {
    std::size_t guard = 0; // --guard: edits closer than this conflict as one (guard-band)
    bool force = false;    // --binary-merge=force: skip the size-change refusal
};

// OpaqueLeaf merge by default; the byte-level diff3 when `byte_merge` is set (always warns
// binary-merge-unverified). `output` receives the merged bytes.
MergeResult merge(const BinDoc& base, const BinDoc& ours, const BinDoc& theirs, const ConflictOptions&,
                  const ByteMergeOptions* byte_merge, std::vector<std::byte>& output);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::binary
