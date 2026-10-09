#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace semsplinter {

// XXH3-128 (SPEC §3.3, D-8). Content identity, not a security guarantee.
struct Hash128 {
    std::uint64_t hi = 0, lo = 0;
    auto operator<=>(const Hash128&) const = default;
    std::string hex() const; // 32 lowercase hex digits, high half first
};

Hash128 hash128(std::span<const std::byte>) noexcept;
Hash128 hash128(std::string_view) noexcept;

// Incremental hashing. add() of a string or integer is length- or width-delimited, so field
// sequences hash unambiguously ("ab"+"c" differs from "a"+"bc").
class Hasher {
public:
    Hasher();
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    Hasher& raw(std::span<const std::byte>);
    Hasher& add(std::string_view);
    Hasher& add(std::uint64_t);
    Hash128 digest() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace semsplinter
