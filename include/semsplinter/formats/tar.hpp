#pragma once

// The TAR module (docs/formats/containers.md): ustar, pax, and GNU archives, plain or wrapped in gzip,
// zstd, xz, or bzip2. Untouched members are copied verbatim inside the decoded stream; members are diffed
// and merged by path through the registry.

#include <semsplinter/format.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace semsplinter::tar {

enum class Wrapper : std::uint8_t { none, gzip, zstd, xz, bzip2 };

// A member for build(): a POSIX ustar header (pax when the path does not fit).
struct NewEntry {
    std::string path;
    std::vector<std::byte> content;
    char type = '0';
    std::uint32_t mode = 0644;
    std::uint64_t mtime = 0;
    std::string linkname;
};

// A deterministic archive with two end blocks, no blocking padding. Used by tests.
std::vector<std::byte> build(std::span<const NewEntry>, Wrapper = Wrapper::none);

std::unique_ptr<Format> make_format();

} // namespace semsplinter::tar
