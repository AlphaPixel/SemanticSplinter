#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>

#include <algorithm>
#include <map>
#include <tuple>

namespace semsplinter {

std::string_view to_string(ByteClass c) noexcept {
    switch (c) {
    case ByteClass::content:
        return "content";
    case ByteClass::framing:
        return "framing";
    case ByteClass::integrity:
        return "integrity";
    case ByteClass::authenticity:
        return "authenticity";
    case ByteClass::encoding:
        return "encoding";
    case ByteClass::derived:
        return "derived";
    case ByteClass::volatile_:
        return "volatile";
    case ByteClass::unexplained:
        return "unexplained";
    }
    return "unexplained";
}

void EnvelopeMap::validate(std::uint64_t file_size) const {
    std::uint64_t end = 0;
    for (auto& r : ranges) {
        if (r.offset < end || r.length > file_size || r.offset > file_size - r.length)
            throw Error(ErrorCategory::internal,
                        "envelope map: ranges overlap, are unsorted, or exceed the file");
        if (r.cls == ByteClass::unexplained)
            throw Error(ErrorCategory::internal, "envelope map: 'unexplained' is not a range class");
        end = r.offset + r.length;
    }
}

EnvelopeMap EnvelopeMap::whole_content(std::uint64_t file_size) {
    EnvelopeMap m;
    if (file_size)
        m.ranges.push_back({0, file_size, ByteClass::content, "", ""});
    return m;
}

std::uint64_t Churn::total() const noexcept {
    std::uint64_t t = 0;
    for (auto v : by_class)
        t += v;
    return t;
}

nlohmann::ordered_json Churn::to_json() const {
    nlohmann::ordered_json j;
    j["bytes_changed"] = total();
    for (std::size_t i = 0; i < kByteClasses; ++i)
        j[std::string(to_string(static_cast<ByteClass>(i)))] = by_class[i];
    return j;
}

namespace {

using Key = std::tuple<std::string, ByteClass, std::string>;

// Concatenated bytes per (unit, class, layer); gaps between ranges go to ("", unexplained, "").
std::map<Key, std::vector<std::byte>> group_bytes(std::span<const std::byte> f, const EnvelopeMap& m) {
    m.validate(f.size());
    std::map<Key, std::vector<std::byte>> g;
    std::uint64_t pos = 0;
    auto take = [&](const Key& k, std::uint64_t off, std::uint64_t len) {
        auto& v = g[k];
        auto s = f.subspan(static_cast<std::size_t>(off), static_cast<std::size_t>(len));
        v.insert(v.end(), s.begin(), s.end());
    };
    const Key gap{"", ByteClass::unexplained, ""};
    for (auto& r : m.ranges) {
        if (r.offset > pos)
            take(gap, pos, r.offset - pos);
        take({r.unit, r.cls, r.layer}, r.offset, r.length);
        pos = r.offset + r.length;
    }
    if (pos < f.size())
        take(gap, pos, f.size() - pos);
    return g;
}

std::uint64_t changed_bytes(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    if (a.size() == b.size()) {
        std::uint64_t n = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            n += a[i] != b[i];
        return n;
    }
    std::size_t p = 0, s = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p])
        ++p;
    while (s < a.size() - p && s < b.size() - p && a[a.size() - 1 - s] == b[b.size() - 1 - s])
        ++s;
    return std::max(a.size(), b.size()) - p - s;
}

} // namespace

Churn attribute_churn(std::span<const std::byte> a, const EnvelopeMap& ma, std::span<const std::byte> b,
                      const EnvelopeMap& mb) {
    auto ga = group_bytes(a, ma), gb = group_bytes(b, mb);
    static const std::vector<std::byte> none;
    Churn c;
    auto add = [&](const Key& k, const std::vector<std::byte>& x, const std::vector<std::byte>& y) {
        c.by_class[static_cast<std::size_t>(std::get<1>(k))] += changed_bytes(x, y);
    };
    for (auto& [k, v] : ga) {
        auto f = gb.find(k);
        add(k, v, f == gb.end() ? none : f->second);
    }
    for (auto& [k, v] : gb)
        if (!ga.count(k))
            add(k, none, v);
    return c;
}

namespace {
struct CrcTable {
    std::uint32_t t[256];
    constexpr CrcTable() : t{} {
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
    }
};
constexpr CrcTable kCrc;
} // namespace

std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t crc) noexcept {
    crc = ~crc;
    for (auto b : data)
        crc = kCrc.t[(crc ^ static_cast<std::uint32_t>(b)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t adler32(std::span<const std::byte> data, std::uint32_t adler) noexcept {
    std::uint32_t a = adler & 0xFFFF, b = adler >> 16;
    constexpr std::uint32_t mod = 65521;
    std::size_t i = 0;
    while (i < data.size()) {
        std::size_t n = std::min<std::size_t>(data.size() - i, 5552); // no overflow before the modulo
        for (; n; --n, ++i)
            a += static_cast<std::uint32_t>(data[i]), b += a;
        a %= mod, b %= mod;
    }
    return (b << 16) | a;
}

} // namespace semsplinter
