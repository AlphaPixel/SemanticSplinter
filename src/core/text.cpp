#include <semsplinter/text.hpp>

#include <algorithm>
#include <cstring>

namespace semsplinter {

std::string_view to_string(Encoding e) noexcept {
    switch (e) {
    case Encoding::utf8:
        return "utf-8";
    case Encoding::utf16le:
        return "utf-16le";
    case Encoding::utf16be:
        return "utf-16be";
    case Encoding::utf32le:
        return "utf-32le";
    case Encoding::utf32be:
        return "utf-32be";
    }
    return "utf-8";
}

std::string_view to_string(Eol e) noexcept {
    switch (e) {
    case Eol::none:
        return "none";
    case Eol::lf:
        return "lf";
    case Eol::crlf:
        return "crlf";
    case Eol::cr:
        return "cr";
    }
    return "none";
}

namespace {

unsigned u8(std::byte b) {
    return std::to_integer<unsigned>(b);
}

bool starts(std::span<const std::byte> s, std::initializer_list<unsigned> p) {
    if (s.size() < p.size())
        return false;
    std::size_t i = 0;
    for (unsigned v : p)
        if (u8(s[i++]) != v)
            return false;
    return true;
}

std::size_t unit_size(Encoding e) {
    switch (e) {
    case Encoding::utf8:
        return 1;
    case Encoding::utf16le:
    case Encoding::utf16be:
        return 2;
    default:
        return 4;
    }
}

std::uint32_t unit_at(std::span<const std::byte> s, std::size_t i, Encoding e) {
    switch (e) {
    case Encoding::utf8:
        return u8(s[i]);
    case Encoding::utf16le:
        return u8(s[i]) | u8(s[i + 1]) << 8;
    case Encoding::utf16be:
        return u8(s[i]) << 8 | u8(s[i + 1]);
    case Encoding::utf32le:
        return u8(s[i]) | u8(s[i + 1]) << 8 | u8(s[i + 2]) << 16 |
               static_cast<std::uint32_t>(u8(s[i + 3])) << 24;
    case Encoding::utf32be:
        return static_cast<std::uint32_t>(u8(s[i])) << 24 | u8(s[i + 1]) << 16 | u8(s[i + 2]) << 8 |
               u8(s[i + 3]);
    }
    return 0;
}

void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

EncodingInfo detect_encoding(std::span<const std::byte> s) noexcept {
    // UTF-32LE's BOM starts with UTF-16LE's, so test the longer one first.
    if (starts(s, {0xFF, 0xFE, 0x00, 0x00}))
        return {Encoding::utf32le, 4};
    if (starts(s, {0x00, 0x00, 0xFE, 0xFF}))
        return {Encoding::utf32be, 4};
    if (starts(s, {0xEF, 0xBB, 0xBF}))
        return {Encoding::utf8, 3};
    if (starts(s, {0xFF, 0xFE}))
        return {Encoding::utf16le, 2};
    if (starts(s, {0xFE, 0xFF}))
        return {Encoding::utf16be, 2};
    // BOM-less UTF-16: >= 40% NULs on one byte parity, < 2% on the other.
    std::size_t n = std::min<std::size_t>(s.size(), 4096) & ~std::size_t{1};
    if (n >= 4) {
        std::size_t even = 0, odd = 0;
        for (std::size_t i = 0; i < n; i += 2) {
            even += u8(s[i]) == 0;
            odd += u8(s[i + 1]) == 0;
        }
        std::size_t half = n / 2;
        if (even * 100 >= half * 40 && odd * 100 < half * 2)
            return {Encoding::utf16be, 0};
        if (odd * 100 >= half * 40 && even * 100 < half * 2)
            return {Encoding::utf16le, 0};
    }
    return {Encoding::utf8, 0};
}

bool looks_binary(std::span<const std::byte> s) noexcept {
    if (detect_encoding(s).encoding != Encoding::utf8)
        return false;
    auto head = s.first(std::min<std::size_t>(s.size(), 8192));
    return std::find(head.begin(), head.end(), std::byte{0}) != head.end();
}

std::vector<Line> split_lines(std::span<const std::byte> s, const EncodingInfo& ei, SplitMode mode) {
    std::vector<Line> lines;
    const std::size_t w = unit_size(ei.encoding);
    const std::size_t end =
        ei.bom + (s.size() - ei.bom) / w * w; // a trailing partial unit stays in the last line
    std::size_t start = ei.bom;
    if (w == 1 && mode == SplitMode::lf) { // fast path
        auto p = reinterpret_cast<const char*>(s.data());
        for (std::size_t i = start; i < s.size();) {
            auto nl = static_cast<const char*>(std::memchr(p + i, '\n', s.size() - i));
            if (!nl) {
                lines.push_back({i, s.size() - i, Eol::none});
                break;
            }
            std::size_t j = static_cast<std::size_t>(nl - p);
            lines.push_back({i, j - i, Eol::lf});
            i = j + 1;
        }
        return lines;
    }
    for (std::size_t i = start; i < end; i += w) {
        auto c = unit_at(s, i, ei.encoding);
        if (c == '\n') {
            lines.push_back({start, i - start, Eol::lf});
            start = i + w;
        } else if (c == '\r' && mode == SplitMode::any) {
            bool crlf = i + w < end && unit_at(s, i + w, ei.encoding) == '\n';
            lines.push_back({start, i - start, crlf ? Eol::crlf : Eol::cr});
            if (crlf)
                i += w;
            start = i + w;
        }
    }
    if (start < s.size())
        lines.push_back({start, s.size() - start, Eol::none});
    return lines;
}

std::string line_utf8(std::span<const std::byte> s, const Line& l, Encoding e) {
    if (e == Encoding::utf8)
        return {reinterpret_cast<const char*>(s.data()) + l.off, l.len};
    std::string out;
    const std::size_t w = unit_size(e), end = l.off + l.len / w * w;
    for (std::size_t i = l.off; i < end; i += w) {
        std::uint32_t c = unit_at(s, i, e);
        if (w == 2 && c >= 0xD800 && c < 0xDC00 && i + 2 < end) {
            std::uint32_t lo = unit_at(s, i + 2, e);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        put_utf8(out, c);
    }
    if (end < l.off + l.len)
        put_utf8(out, 0xFFFD);
    return out;
}

std::string encode_utf8_as(std::string_view in, Encoding e) {
    if (e == Encoding::utf8)
        return std::string(in);
    std::string out;
    auto put = [&](std::uint32_t v, std::size_t w, bool le) {
        for (std::size_t k = 0; k < w; ++k) {
            std::size_t shift = 8 * (le ? k : w - 1 - k);
            out += static_cast<char>(v >> shift & 0xFF);
        }
    };
    const bool le = e == Encoding::utf16le || e == Encoding::utf32le;
    for (std::size_t i = 0; i < in.size();) {
        auto b = static_cast<unsigned char>(in[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t n = b < 0x80 ? 1 : b >> 5 == 6 ? 2 : b >> 4 == 14 ? 3 : b >> 3 == 30 ? 4 : 0;
        if (n && i + n <= in.size()) {
            cp = n == 1 ? b : b & (0x7F >> n);
            for (std::size_t k = 1; k < n; ++k)
                cp = cp << 6 | (static_cast<unsigned char>(in[i + k]) & 0x3F);
            i += n;
        } else {
            ++i;
        }
        if (unit_size(e) == 4) {
            put(cp, 4, le);
        } else if (cp >= 0x10000) {
            cp -= 0x10000;
            put(0xD800 + (cp >> 10), 2, le);
            put(0xDC00 + (cp & 0x3FF), 2, le);
        } else {
            put(cp, 2, le);
        }
    }
    return out;
}

} // namespace semsplinter
