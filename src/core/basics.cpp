// Errors, limits, hashing.
#include <semsplinter/error.hpp>
#include <semsplinter/hash.hpp>
#include <semsplinter/limits.hpp>

#include <xxhash.h>

#include <cstdio>

namespace semsplinter {

std::string_view to_string(ErrorCategory c) noexcept {
    switch (c) {
    case ErrorCategory::invalid_input:
        return "invalid_input";
    case ErrorCategory::unsupported:
        return "unsupported";
    case ErrorCategory::limit:
        return "limit";
    case ErrorCategory::io:
        return "io";
    case ErrorCategory::internal:
        return "internal";
    }
    return "internal";
}

void Limits::charge(std::uint64_t bytes) {
    if (bytes > max_memory - memory_used)
        throw Error(ErrorCategory::limit,
                    "memory limit exceeded (--max-memory=" + std::to_string(max_memory) + ")");
    memory_used += bytes;
}

std::string Hash128::hex() const {
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return buf;
}

Hash128 hash128(std::span<const std::byte> b) noexcept {
    XXH128_hash_t h = XXH3_128bits(b.data(), b.size());
    return {h.high64, h.low64};
}

Hash128 hash128(std::string_view s) noexcept {
    XXH128_hash_t h = XXH3_128bits(s.data(), s.size());
    return {h.high64, h.low64};
}

struct Hasher::State {
    XXH3_state_t* s = XXH3_createState();
    ~State() { XXH3_freeState(s); }
};

Hasher::Hasher() : state_(std::make_unique<State>()) {
    if (!state_->s)
        throw std::bad_alloc();
    XXH3_128bits_reset(state_->s);
}
Hasher::~Hasher() = default;

Hasher& Hasher::raw(std::span<const std::byte> b) {
    XXH3_128bits_update(state_->s, b.data(), b.size());
    return *this;
}

Hasher& Hasher::add(std::uint64_t v) {
    unsigned char le[8];
    for (int i = 0; i < 8; ++i)
        le[i] = static_cast<unsigned char>(v >> (8 * i));
    XXH3_128bits_update(state_->s, le, sizeof le);
    return *this;
}

Hasher& Hasher::add(std::string_view s) {
    add(static_cast<std::uint64_t>(s.size()));
    XXH3_128bits_update(state_->s, s.data(), s.size());
    return *this;
}

Hash128 Hasher::digest() const {
    XXH128_hash_t h = XXH3_128bits_digest(state_->s);
    return {h.high64, h.low64};
}

} // namespace semsplinter
