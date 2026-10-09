#include <semsplinter/error.hpp>
#include <semsplinter/io.hpp>

#include <atomic>
#include <random>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace semsplinter {

namespace fs = std::filesystem;

namespace {
std::FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::char_traits<char>::length(mode));
    std::FILE* f = nullptr;
    return _wfopen_s(&f, p.c_str(), m.c_str()) == 0 ? f : nullptr;
#else
    return std::fopen(p.c_str(), mode);
#endif
}

std::string utf8(const fs::path& p) {
    auto u = p.u8string();
    return {u.begin(), u.end()};
}

[[noreturn]] void io_error(const std::string& what, const fs::path& p) {
    throw Error(ErrorCategory::io, what + ": " + utf8(p));
}
} // namespace

std::span<const std::byte> as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

ByteSource ByteSource::from_file(const fs::path& path, const Limits& limits) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec)
        io_error("cannot read", path);
    if (size > limits.max_input)
        throw Error(ErrorCategory::limit, "input exceeds --max-input=" + std::to_string(limits.max_input));
    std::FILE* f = open_file(path, "rb");
    if (!f)
        io_error("cannot open", path);
    auto buf = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
    std::size_t got = std::fread(buf->data(), 1, buf->size(), f);
    bool more = std::fgetc(f) != EOF; // the file grew after file_size()
    std::fclose(f);
    if (got != buf->size() || more)
        io_error("short or changing read", path);
    ByteSource s;
    s.size_ = buf->size();
    s.data_ = std::move(buf);
    s.name_ = utf8(path);
    return s;
}

ByteSource ByteSource::from_memory(std::vector<std::byte> bytes, std::string logical_name) {
    ByteSource s;
    s.size_ = bytes.size();
    s.data_ = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    s.name_ = std::move(logical_name);
    return s;
}

ByteSource ByteSource::slice(std::uint64_t offset, std::uint64_t length) const {
    if (offset > size_ || length > size_ - offset)
        throw Error(ErrorCategory::invalid_input, "slice out of bounds");
    ByteSource s = *this;
    s.offset_ = offset_ + static_cast<std::size_t>(offset);
    s.size_ = static_cast<std::size_t>(length);
    return s;
}

TempFile::TempFile(const fs::path& target) : target_(target) {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    for (int attempt = 0; attempt < 16 && !file_; ++attempt) {
        fs::path name = target.filename();
        name += ".splinter-tmp-" + std::to_string(rd()) + "-" + std::to_string(counter++);
        path_ = target.parent_path() / name;
        file_ = open_file(path_, "wbx"); // exclusive create
    }
    if (!file_)
        io_error("cannot create temp file next to", target);
}

TempFile::~TempFile() {
    if (file_) {
        std::fclose(file_);
        std::error_code ec;
        fs::remove(path_, ec);
    }
}

void TempFile::write(std::span<const std::byte> b) {
    if (!file_ || std::fwrite(b.data(), 1, b.size(), file_) != b.size())
        io_error("write failed", path_);
}

void TempFile::commit() {
    bool ok = std::fflush(file_) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(file_)) == 0;
#else
    ok = ok && fsync(fileno(file_)) == 0;
#endif
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    std::error_code ec;
    if (ok)
        fs::rename(path_, target_, ec); // replaces an existing target
    if (!ok || ec) {
        fs::remove(path_, ec);
        io_error("cannot write", target_);
    }
}

void atomic_write(const fs::path& target, std::span<const std::byte> bytes) {
    TempFile t(target);
    t.write(bytes);
    t.commit();
}

} // namespace semsplinter
