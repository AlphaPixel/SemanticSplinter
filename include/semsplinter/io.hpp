#pragma once

#include <semsplinter/limits.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace semsplinter {

// Immutable input bytes plus a logical name (used for detection). Copies share the buffer.
// ponytail: whole file in memory; switch to mapped or streamed reads when inputs near max_input matter.
class ByteSource {
public:
    ByteSource() = default;

    // Throws Error{limit} if the file exceeds limits.max_input (checked before reading), Error{io} on
    // failure.
    static ByteSource from_file(const std::filesystem::path&, const Limits&);
    static ByteSource from_memory(std::vector<std::byte>, std::string logical_name = {});

    std::span<const std::byte> bytes() const noexcept {
        return {data_ ? data_->data() + offset_ : nullptr, size_};
    }
    std::size_t size() const noexcept { return size_; }
    const std::string& name() const noexcept { return name_; }

    // Shares the buffer. Throws Error{invalid_input} when out of bounds.
    ByteSource slice(std::uint64_t offset, std::uint64_t length) const;

private:
    std::shared_ptr<const std::vector<std::byte>> data_;
    std::size_t offset_ = 0, size_ = 0;
    std::string name_;
};

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual void write(std::span<const std::byte>) = 0;
};

class VectorSink final : public ByteSink {
public:
    void write(std::span<const std::byte> b) override { data.insert(data.end(), b.begin(), b.end()); }
    std::vector<std::byte> data;
};

// A uniquely named file next to `target`, removed on destruction unless commit() renamed it over target.
class TempFile {
public:
    explicit TempFile(const std::filesystem::path& target);
    ~TempFile();
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    void write(std::span<const std::byte>); // appends, throws Error{io}
    void commit();                          // flush to disk, close, rename over target

private:
    std::filesystem::path target_, path_;
    std::FILE* file_ = nullptr;
};

// Writes `bytes` to a sibling temp file, flushes it to disk, and renames it over `target` (SPEC §9).
// On failure the temp file is removed and the target is untouched.
void atomic_write(const std::filesystem::path& target, std::span<const std::byte> bytes);

std::span<const std::byte> as_bytes(std::string_view) noexcept;

} // namespace semsplinter
