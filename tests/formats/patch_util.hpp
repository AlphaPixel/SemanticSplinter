#pragma once

// Patch round trips through the library (SPEC §6): `splinter --binary a b` applied to a target file.
#include "../scratch.hpp"

#include <semsplinter/semsplinter.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace patch_util {

inline std::vector<std::byte> bytes(std::string_view s) {
    auto b = semsplinter::as_bytes(s);
    return {b.begin(), b.end()};
}

inline std::string text(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

inline const semsplinter::Registry& registry() {
    static const auto reg = semsplinter::Registry::with_builtin_formats();
    return reg;
}

inline semsplinter::Input input(const std::vector<std::byte>& b, const std::string& name) {
    return {semsplinter::ByteSource::from_memory(b, name), name};
}

// The patch from `a` to `b`, files named like `name` (its extension picks the format).
inline std::string make(const std::string& name, const std::vector<std::byte>& a,
                        const std::vector<std::byte>& b) {
    semsplinter::DiffRequest rq;
    rq.a = input(a, "a/" + name), rq.b = input(b, "b/" + name);
    rq.patch = true;
    return semsplinter::diff(registry(), rq).patch;
}

struct Applied {
    std::vector<std::byte> bytes;
    std::string messages;
    std::size_t failed = 0;
};

// `patch` applied to a file holding `target`; rejects are discarded.
inline Applied apply(const std::string& name, const std::string& patch,
                     const std::vector<std::byte>& target) {
    const auto dir = scratch_path("payload");
    std::filesystem::create_directories(dir);
    const auto file = dir / name;
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char*>(target.data()), static_cast<std::streamsize>(target.size()));
    semsplinter::PatchRequest rq;
    rq.patch = patch;
    rq.target = file.generic_string();
    rq.output = "-";
    rq.reject_file = "-";
    auto out = semsplinter::apply_patch(registry(), rq);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return {std::move(out.output), std::move(out.messages), out.failed};
}

// True when the two files hold the same content for their format.
inline bool same(const std::string& name, const std::vector<std::byte>& x, const std::vector<std::byte>& y) {
    semsplinter::DiffRequest rq;
    rq.a = input(x, name), rq.b = input(y, name);
    return !semsplinter::diff(registry(), rq).differs;
}

} // namespace patch_util
