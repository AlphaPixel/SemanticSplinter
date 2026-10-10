#pragma once

#include <cstdint>

namespace semsplinter {

// Resource limits (SPEC §9), enforced before allocation. One instance is the shared budget of an
// invocation, including recursion into container members (D-47).
struct Limits {
    std::uint64_t max_input = std::uint64_t{2} << 30;     // --max-input
    std::uint64_t max_memory = std::uint64_t{4} << 30;    // --max-memory, decoded bytes per invocation
    std::uint64_t max_pixels = std::uint64_t{1} << 30;    // --max-pixels
    std::uint64_t max_zip_entries = 65536;                // --max-zip-entries
    std::uint64_t max_zip_total = std::uint64_t{4} << 30; // --max-zip-total
    std::uint64_t max_zip_ratio = 200;                    // --max-zip-ratio
    int max_depth = 8;                                    // --max-depth

    std::uint64_t memory_used = 0;
    std::uint64_t zip_used = 0; // decompressed container bytes, across every recursion level

    // Reserves decoded memory from the budget. Throws Error{limit} when it would exceed max_memory.
    void charge(std::uint64_t bytes);
    void release(std::uint64_t bytes) noexcept { memory_used -= bytes < memory_used ? bytes : memory_used; }
};

} // namespace semsplinter
