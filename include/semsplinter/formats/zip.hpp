#pragma once

// The ZIP module (docs/formats/containers.md): own reader and writer (D-27 as amended). Untouched members
// are copied as raw byte spans, touched ones are written deterministically, and members are diffed and
// merged through the registry by their path.

#include <semsplinter/format.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace semsplinter::zip {

// A member for build(): stored (0), deflate (8, level 6), or zstd (93, level 3).
struct NewEntry {
    std::string path;
    std::vector<std::byte> content;
    std::uint16_t method = 8;
    std::uint16_t dos_time = 0, dos_date = 0x21; // 1980-01-01 00:00:00
    std::uint32_t unix_mode = 0100644;           // external attributes, host Unix
    std::string comment;
};

// A plain, deterministic archive (no data descriptors, no ZIP64, no extra fields). Used by tests.
std::vector<std::byte> build(std::span<const NewEntry>, std::string_view comment = {});

std::unique_ptr<Format> make_format();

} // namespace semsplinter::zip
