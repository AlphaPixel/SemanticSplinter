#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/formats/tar.hpp>

#include "../container/container.hpp"

#include <bzlib.h>
#include <lzma.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>

namespace semsplinter::tar {

namespace {

using container::Archive;
using container::OutEntry;
using container::Pick;
using Bytes = std::span<const std::byte>;

constexpr std::uint64_t kBlock = 512;

Error bad(const std::string& code, const std::string& what) {
    return Error(ErrorCategory::invalid_input, "tar: " + what + " (" + code + ")");
}

unsigned char at(Bytes b, std::size_t k) {
    return std::to_integer<unsigned char>(b[k]);
}

std::string cstr(Bytes f) {
    std::size_t n = 0;
    while (n < f.size() && at(f, n))
        ++n;
    return {reinterpret_cast<const char*>(f.data()), n};
}

// A numeric header field: octal (space or NUL terminated) or GNU base-256 (containers.md §2.2, §8.1).
std::optional<std::uint64_t> number(Bytes f) {
    if (!f.empty() && (at(f, 0) & 0x80)) {
        if (at(f, 0) & 0x40)
            return std::nullopt; // negative
        std::uint64_t v = at(f, 0) & 0x3F;
        for (std::size_t k = 1; k < f.size(); ++k) {
            if (v >> 56)
                return std::nullopt;
            v = v << 8 | at(f, k);
        }
        return v;
    }
    std::size_t k = 0;
    while (k < f.size() && at(f, k) == ' ')
        ++k;
    std::uint64_t v = 0;
    for (; k < f.size() && at(f, k) >= '0' && at(f, k) <= '7'; ++k) {
        if (v >> 60)
            return std::nullopt;
        v = v * 8 + (at(f, k) - '0');
    }
    for (; k < f.size(); ++k)
        if (at(f, k) != ' ' && at(f, k) != 0)
            return std::nullopt;
    return v;
}

// The header checksum: unsigned byte sum with the checksum field counted as spaces; also the historic
// signed sum.
std::pair<std::uint64_t, std::int64_t> checksums(Bytes h) {
    std::uint64_t u = 0;
    std::int64_t s = 0;
    for (std::size_t k = 0; k < kBlock; ++k) {
        unsigned char c = k >= 148 && k < 156 ? ' ' : at(h, k);
        u += c, s += static_cast<signed char>(c);
    }
    return {u, s};
}

bool zero_block(Bytes h) {
    return std::all_of(h.begin(), h.end(), [](std::byte c) { return c == std::byte{0}; });
}

// 100 for a ustar header with a valid checksum, 50 for an old-style one, else 0.
int header_confidence(Bytes h) {
    if (h.size() < kBlock || zero_block(h.first(kBlock)))
        return 0;
    auto stored = number(h.subspan(148, 8));
    auto [u, s] = checksums(h);
    if (!stored || (*stored != u && static_cast<std::int64_t>(*stored) != s))
        return 0;
    return std::memcmp(h.data() + 257, "ustar", 5) == 0 ? 100 : 50;
}

void put_text(std::vector<std::byte>& h, std::size_t off, std::size_t width, std::string_view s) {
    std::fill(h.begin() + static_cast<std::ptrdiff_t>(off),
              h.begin() + static_cast<std::ptrdiff_t>(off + width), std::byte{0});
    std::memcpy(h.data() + off, s.data(), std::min(width, s.size()));
}

// Octal digits plus NUL; false when the value does not fit.
bool put_octal(std::vector<std::byte>& h, std::size_t off, std::size_t width, std::uint64_t v) {
    const std::size_t digits = width - 1;
    if (digits < 21 && v >> (3 * digits))
        return false;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llo", static_cast<int>(digits), static_cast<unsigned long long>(v));
    put_text(h, off, width, buf);
    return true;
}

void put_base256(std::vector<std::byte>& h, std::size_t off, std::size_t width, std::uint64_t v) {
    h[off] = std::byte{0x80};
    for (std::size_t k = 1; k < width; ++k)
        h[off + k] = std::byte((v >> (8 * (width - 1 - k))) & 0xFF);
}

void finish_header(std::vector<std::byte>& h) {
    std::fill(h.begin() + 148, h.begin() + 156, std::byte{' '});
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06o", static_cast<unsigned>(checksums(h).first));
    std::memcpy(h.data() + 148, buf, 6);
    h[154] = std::byte{0}, h[155] = std::byte{' '};
}

std::vector<std::byte> new_header(std::string_view name, char type, std::uint64_t size, bool gnu) {
    std::vector<std::byte> h(kBlock);
    put_text(h, 0, 100, name);
    put_octal(h, 100, 8, 0644), put_octal(h, 108, 8, 0), put_octal(h, 116, 8, 0);
    if (!put_octal(h, 124, 12, size))
        put_base256(h, 124, 12, size);
    put_octal(h, 136, 12, 0);
    h[156] = std::byte(type);
    std::memcpy(h.data() + 257,
                gnu ? "ustar  "
                    : "ustar\0"
                      "00",
                8);
    return h;
}

std::string pax_record(const std::string& key, const std::string& value) {
    std::size_t n = key.size() + value.size() + 3; // ' ', '=', '\n'
    std::size_t digits = std::to_string(n).size();
    while (std::to_string(n + digits).size() != digits)
        ++digits;
    return std::to_string(n + digits) + " " + key + "=" + value + "\n";
}

using Pax = std::vector<std::pair<std::string, std::string>>;

Pax parse_pax(Bytes d) {
    Pax out;
    std::string_view s(reinterpret_cast<const char*>(d.data()), d.size());
    while (!s.empty() && s.front() != '\0') {
        auto sp = s.find(' ');
        std::size_t len = 0;
        if (sp == std::string_view::npos || sp == 0 || sp > 20)
            throw bad("tar-bad-pax", "malformed pax record");
        for (char c : s.substr(0, sp)) {
            if (c < '0' || c > '9')
                throw bad("tar-bad-pax", "malformed pax record length");
            len = len * 10 + static_cast<std::size_t>(c - '0');
        }
        if (len <= sp + 1 || len > s.size() || s[len - 1] != '\n')
            throw bad("tar-bad-pax", "pax record length does not match");
        auto rec = s.substr(sp + 1, len - sp - 2);
        auto eq = rec.find('=');
        if (eq == std::string_view::npos)
            throw bad("tar-bad-pax", "pax record without '='");
        out.emplace_back(std::string(rec.substr(0, eq)), std::string(rec.substr(eq + 1)));
        s.remove_prefix(len);
    }
    return out;
}

const std::string* pax_get(const Pax& p, std::string_view key) {
    for (auto& [k, v] : p)
        if (k == key)
            return &v;
    return nullptr;
}

// ---- Outer streams (containers.md §2.2, §7.3) ----

Error corrupt(const char* codec) {
    return Error(ErrorCategory::invalid_input, std::string(codec) + ": corrupt or truncated stream");
}

std::vector<std::byte> unxz(Bytes in, std::uint64_t max_out, bool partial) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, std::uint64_t{1} << 30, LZMA_CONCATENATED) != LZMA_OK)
        throw Error(ErrorCategory::internal, "xz: decoder init failed");
    std::unique_ptr<lzma_stream, void (*)(lzma_stream*)> guard(&s, lzma_end);
    s.next_in = reinterpret_cast<const std::uint8_t*>(in.data());
    s.avail_in = in.size();
    std::vector<std::byte> out;
    std::array<std::uint8_t, 65536> buf;
    for (;;) {
        s.next_out = buf.data();
        s.avail_out = buf.size();
        lzma_ret rc = lzma_code(&s, s.avail_in ? LZMA_RUN : LZMA_FINISH);
        auto* b = reinterpret_cast<const std::byte*>(buf.data());
        out.insert(out.end(), b, b + (buf.size() - s.avail_out));
        if (out.size() > max_out) {
            out.resize(max_out + 1);
            return out;
        }
        if (rc == LZMA_STREAM_END)
            return out;
        if (rc != LZMA_OK) {
            if (partial)
                return out;
            throw corrupt("xz");
        }
    }
}

std::vector<std::byte> unbzip2(Bytes in, std::uint64_t max_out, bool partial) {
    std::vector<std::byte> out;
    std::array<char, 65536> buf;
    std::size_t pos = 0;
    while (pos < in.size()) {
        bz_stream s{};
        if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK)
            throw Error(ErrorCategory::internal, "bzip2: decoder init failed");
        std::unique_ptr<bz_stream, int (*)(bz_stream*)> guard(&s, BZ2_bzDecompressEnd);
        s.next_in = const_cast<char*>(reinterpret_cast<const char*>(in.data() + pos));
        s.avail_in = static_cast<unsigned>(std::min<std::size_t>(in.size() - pos, UINT_MAX));
        int rc = BZ_OK;
        do {
            s.next_out = buf.data();
            s.avail_out = static_cast<unsigned>(buf.size());
            rc = BZ2_bzDecompress(&s);
            if (rc != BZ_OK && rc != BZ_STREAM_END) {
                if (partial)
                    return out;
                throw corrupt("bzip2");
            }
            auto* b = reinterpret_cast<const std::byte*>(buf.data());
            out.insert(out.end(), b, b + (buf.size() - s.avail_out));
            if (out.size() > max_out) {
                out.resize(max_out + 1);
                return out;
            }
            if (rc == BZ_OK && s.avail_in == 0 && s.avail_out > 0) {
                if (partial)
                    return out;
                throw corrupt("bzip2");
            }
        } while (rc != BZ_STREAM_END);
        pos = static_cast<std::size_t>(reinterpret_cast<const std::byte*>(s.next_in) - in.data());
        if (in.size() - pos < 3 || std::memcmp(in.data() + pos, "BZh", 3) != 0)
            break; // another stream follows only with its signature
    }
    return out;
}

Wrapper sniff(Bytes b) {
    auto starts = [&](std::initializer_list<int> m) {
        if (b.size() < m.size())
            return false;
        std::size_t k = 0;
        for (int c : m)
            if (at(b, k++) != c)
                return false;
        return true;
    };
    if (starts({0x1f, 0x8b}))
        return Wrapper::gzip;
    if (starts({0x28, 0xb5, 0x2f, 0xfd}))
        return Wrapper::zstd;
    if (starts({0xfd, '7', 'z', 'X', 'Z', 0}))
        return Wrapper::xz;
    if (starts({'B', 'Z', 'h'}) && b.size() > 3 && at(b, 3) >= '1' && at(b, 3) <= '9')
        return Wrapper::bzip2;
    return Wrapper::none;
}

const char* wrapper_name(Wrapper w) {
    static const char* names[] = {"none", "gzip", "zstd", "xz", "bzip2"};
    return names[static_cast<int>(w)];
}

std::vector<std::byte> unwrap(Wrapper w, Bytes in, std::uint64_t max_out, bool partial = false) {
    switch (w) {
    case Wrapper::gzip:
        return container::inflate(in, max_out, 31, partial);
    case Wrapper::zstd:
        return container::unzstd(in, max_out, partial);
    case Wrapper::xz:
        return unxz(in, max_out, partial);
    case Wrapper::bzip2:
        return unbzip2(in, max_out, partial);
    case Wrapper::none:
        break;
    }
    return {in.begin(), in.end()};
}

// The length of a gzip member header (RFC 1952 §2.3), copied to the output so mtime, OS, and name stay.
std::size_t gzip_header(Bytes b) {
    if (b.size() < 10)
        throw corrupt("gzip");
    const unsigned flags = at(b, 3);
    std::size_t k = 10;
    if (flags & 4) {
        if (k + 2 > b.size())
            throw corrupt("gzip");
        k += 2 + (at(b, k) | at(b, k + 1) << 8);
    }
    for (unsigned f : {8u, 16u})
        if (flags & f) {
            while (k < b.size() && at(b, k))
                ++k;
            ++k;
        }
    if (flags & 2)
        k += 2;
    if (k > b.size())
        throw corrupt("gzip");
    return k;
}

// Re-encodes the TAR stream with ours' wrapper and its recorded parameters (containers.md §7.3).
std::vector<std::byte> wrap(Wrapper w, Bytes ours, Bytes tar) {
    std::vector<std::byte> out;
    switch (w) {
    case Wrapper::none:
        return {tar.begin(), tar.end()};
    case Wrapper::gzip: {
        std::size_t h = gzip_header(ours);
        out.assign(ours.begin(), ours.begin() + static_cast<std::ptrdiff_t>(h));
        const unsigned xfl = at(ours, 8);
        auto body = container::deflate(tar, xfl == 2 ? 9 : xfl == 4 ? 1 : 6);
        out.insert(out.end(), body.begin(), body.end());
        for (std::uint32_t v : {crc32(tar), static_cast<std::uint32_t>(tar.size())})
            for (int s = 0; s < 32; s += 8)
                out.push_back(std::byte((v >> s) & 0xFF));
        return out;
    }
    case Wrapper::zstd:
        return container::zstd(tar, 3, ours.size() > 4 && (at(ours, 4) & 4)); // frame checksum flag
    case Wrapper::xz: {
        const auto check = static_cast<lzma_check>(ours.size() > 7 ? at(ours, 7) & 0x0F : LZMA_CHECK_CRC64);
        out.resize(lzma_stream_buffer_bound(tar.size()));
        std::size_t n = 0;
        if (lzma_easy_buffer_encode(6, check, nullptr, reinterpret_cast<const std::uint8_t*>(tar.data()),
                                    tar.size(), reinterpret_cast<std::uint8_t*>(out.data()), &n,
                                    out.size()) != LZMA_OK)
            throw Error(ErrorCategory::internal, "xz: compression failed");
        out.resize(n);
        return out;
    }
    case Wrapper::bzip2: {
        if (tar.size() > UINT_MAX / 2)
            throw Error(ErrorCategory::unsupported, "bzip2: stream too large to re-encode");
        unsigned n = static_cast<unsigned>(tar.size() + tar.size() / 100 + 600);
        out.resize(n);
        if (BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(out.data()), &n,
                                     const_cast<char*>(reinterpret_cast<const char*>(tar.data())),
                                     static_cast<unsigned>(tar.size()), at(ours, 3) - '0', 0, 0) != BZ_OK)
            throw Error(ErrorCategory::internal, "bzip2: compression failed");
        out.resize(n);
        return out;
    }
    }
    return out;
}

// ---- Reader (containers.md §2.2, §8.1) ----

struct Rec {
    std::uint64_t start = 0, header = 0, data = 0, size = 0, end = 0; // start: first extension header
    std::vector<std::pair<char, std::uint64_t>> aux;                  // x, g, L, K headers before it
    Pax pax;
    std::string name, linkname;
    char type = '0';
};

struct TarDoc final : Archive {
    ByteSource src, tar; // the file, and the decoded TAR stream (src when not wrapped)
    Wrapper wrapper = Wrapper::none;
    std::vector<Rec> recs; // parallel to entries
    std::uint64_t tail = 0;
    std::vector<std::string> checksum_bad;

    Bytes data(std::size_t i) const { return tar.bytes().subspan(recs[i].data, recs[i].size); }

    std::span<const std::byte> content(std::size_t i, Limits&) const override {
        if (entries[i].opaque)
            throw Error(ErrorCategory::unsupported, "tar: sparse member " + entries[i].path + " is opaque");
        return data(i);
    }
    Hash128 hash(std::size_t i, Limits&) const override { return hash128(data(i)); }
    bool same(std::size_t i, const Archive& other, std::size_t j, Limits&) const override {
        auto a = data(i), b = static_cast<const TarDoc&>(other).data(j);
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }
};

const TarDoc& doc(const Document& d) {
    auto* t = dynamic_cast<const TarDoc*>(&d);
    if (!t)
        throw Error(ErrorCategory::internal, "tar: not a TAR document");
    return *t;
}

std::string decimal_or(const Pax& pax, const char* key, Bytes field) {
    if (auto* v = pax_get(pax, key))
        return *v;
    return std::to_string(number(field).value_or(0));
}

std::unique_ptr<TarDoc> load_doc(ByteSource src, Limits& l) {
    auto d = std::make_unique<TarDoc>();
    d->src = std::move(src);
    d->wrapper = sniff(d->src.bytes());
    if (d->wrapper == Wrapper::none) {
        d->tar = d->src;
    } else {
        const std::uint64_t limit = container::decode_limit(l, d->src.size());
        auto out = unwrap(d->wrapper, d->src.bytes(), limit);
        if (out.size() > limit)
            throw Error(ErrorCategory::limit, std::string("tar: the ") + wrapper_name(d->wrapper) +
                                                  " stream decodes beyond --max-zip-size, --max-zip-ratio, "
                                                  "or --max-memory");
        container::charge(l, out.size());
        d->tar = ByteSource::from_memory(std::move(out), d->src.name());
    }
    Bytes b = d->tar.bytes();
    const std::uint64_t n = b.size();
    std::uint64_t pos = 0;
    std::optional<std::uint64_t> start;
    std::vector<std::pair<char, std::uint64_t>> aux;
    Pax pax;
    std::optional<std::string> long_name, long_link;
    std::map<std::string, std::vector<std::size_t>> by_path;
    for (;;) {
        if (pos == n) {
            d->tail = n;
            break;
        }
        if (n - pos < kBlock)
            throw bad("tar-truncated", "archive ends inside a header block");
        Bytes h = b.subspan(pos, kBlock);
        if (zero_block(h)) {
            if (start)
                throw bad("tar-truncated", "extension header without a member");
            d->tail = pos;
            break;
        }
        auto stored = number(h.subspan(148, 8));
        auto [u, s] = checksums(h);
        if (!stored || (*stored != u && static_cast<std::int64_t>(*stored) != s))
            throw bad("tar-checksum", "header checksum mismatch at offset " + std::to_string(pos));
        if (*stored != u)
            d->warnings.push_back(
                {"tar-signed-checksum",
                 {},
                 "header at " + std::to_string(pos) + " uses the historic signed checksum"});
        const char type = static_cast<char>(at(h, 156));
        auto size = number(h.subspan(124, 12));
        if (!size)
            throw bad("tar-bad-number", "bad size field at offset " + std::to_string(pos));
        const bool member = type != 'x' && type != 'g' && type != 'L' && type != 'K';
        if (member)
            if (auto* v = pax_get(pax, "size")) {
                char* e = nullptr;
                *size = std::strtoull(v->c_str(), &e, 10);
                if (v->empty() || *e)
                    throw bad("tar-bad-pax", "bad pax size");
            }
        const std::uint64_t data = pos + kBlock;
        if (*size > n - data || (*size + kBlock - 1) / kBlock * kBlock > n - data)
            throw bad("tar-truncated", "member data runs past the end at offset " + std::to_string(pos));
        const std::uint64_t end = data + (*size + kBlock - 1) / kBlock * kBlock;
        if (!start)
            start = pos;
        Bytes body = b.subspan(data, *size);
        if (!member) {
            if (*size > (std::uint64_t{1} << 20))
                throw Error(ErrorCategory::limit, "tar: extension header larger than 1 MiB");
            if (type == 'x')
                for (auto& kv : parse_pax(body))
                    pax.push_back(kv);
            else if (type == 'L')
                long_name = cstr(body);
            else if (type == 'K')
                long_link = cstr(body);
            aux.emplace_back(type, pos);
            pos = end;
            continue;
        }
        Rec r{*start, pos, data, *size, end, std::move(aux), std::move(pax), {}, {}, type ? type : '0'};
        const bool posix = std::memcmp(h.data() + 257, "ustar\0", 6) == 0;
        r.name = cstr(h.first(100));
        if (posix && at(h, 345))
            r.name = cstr(h.subspan(345, 155)) + "/" + r.name;
        if (long_name)
            r.name = *long_name;
        if (auto* v = pax_get(r.pax, "path"))
            r.name = *v;
        r.linkname = cstr(h.subspan(157, 100));
        if (long_link)
            r.linkname = *long_link;
        if (auto* v = pax_get(r.pax, "linkpath"))
            r.linkname = *v;
        char mode[16];
        std::snprintf(mode, sizeof mode, "0%llo",
                      static_cast<unsigned long long>(number(h.subspan(100, 8)).value_or(0)));
        const bool sparse = r.type == 'S' || std::any_of(r.pax.begin(), r.pax.end(), [](auto& kv) {
                                return kv.first.starts_with("GNU.sparse.");
                            });
        Archive::Entry e{
            r.name,
            {{"mtime", decimal_or(r.pax, "mtime", h.subspan(136, 12))},
             {"mode", mode},
             {"uid", decimal_or(r.pax, "uid", h.subspan(108, 8))},
             {"gid", decimal_or(r.pax, "gid", h.subspan(116, 8))},
             {"uname", pax_get(r.pax, "uname") ? *pax_get(r.pax, "uname") : cstr(h.subspan(265, 32))},
             {"gname", pax_get(r.pax, "gname") ? *pax_get(r.pax, "gname") : cstr(h.subspan(297, 32))},
             {"type", std::string(1, r.type)},
             {"linkname", r.linkname}},
            sparse};
        if (container::suspicious_path(r.name))
            d->warnings.push_back(
                {"suspicious-path", container::entry_address(r.name), "not a safe relative path"});
        by_path[r.name].push_back(d->recs.size());
        d->recs.push_back(std::move(r));
        d->entries.push_back(std::move(e));
        aux.clear(), pax.clear(), long_name.reset(), long_link.reset(), start.reset();
        pos = end;
    }
    if (d->entries.size() > l.max_zip_entries)
        throw Error(ErrorCategory::limit,
                    "tar: more than --max-zip-entries=" + std::to_string(l.max_zip_entries) + " members");
    // Duplicates: the last occurrence keeps the path, earlier ones become path#N (Q-TAR-2).
    for (auto& [path, idx] : by_path)
        if (idx.size() > 1) {
            for (std::size_t k = 0; k + 1 < idx.size(); ++k)
                d->entries[idx[k]].path = path + "#" + std::to_string(k + 1);
            d->warnings.push_back({"tar-duplicate-path", container::entry_address(path),
                                   std::to_string(idx.size()) + " members share this path"});
        }
    return d;
}

// ---- Writer (containers.md §7.3) ----

const std::string& meta(const TarDoc& d, std::size_t i, std::string_view field) {
    for (auto& [k, v] : d.entries[i].meta)
        if (k == field)
            return v;
    throw Error(ErrorCategory::internal, "tar: no meta field " + std::string(field));
}

// A touched member: extension headers and the ustar header rebuilt in the source member's flavour.
void write_member(std::vector<std::byte>& out, const std::array<const TarDoc*, 3>& docs, const OutEntry& e) {
    const TarDoc& s = *docs[static_cast<int>(e.body.side)];
    const std::size_t i = e.body.index;
    const Rec& r = s.recs[i];
    Bytes tb = s.tar.bytes();
    auto value = [&](const char* field) -> const std::string& {
        auto f = e.from.find(field);
        if (f == e.from.end())
            return meta(s, i, field);
        return meta(*docs[static_cast<int>(f->second.side)], f->second.index, field);
    };
    auto path_pick = e.from.find("path");
    const std::string& path =
        path_pick == e.from.end()
            ? r.name
            : docs[static_cast<int>(path_pick->second.side)]->recs[path_pick->second.index].name;
    Bytes content = e.content ? Bytes(*e.content) : s.data(i);

    std::vector<std::byte> h(tb.begin() + static_cast<std::ptrdiff_t>(r.header),
                             tb.begin() + static_cast<std::ptrdiff_t>(r.header + kBlock));
    const bool gnu = std::memcmp(h.data() + 257, "ustar  ", 8) == 0;
    const bool posix = std::memcmp(h.data() + 257, "ustar\0", 6) == 0;
    Pax pax = r.pax;
    auto set_pax = [&](const std::string& key, const std::string& v, bool add) {
        for (auto& kv : pax)
            if (kv.first == key) {
                kv.second = v;
                return;
            }
        if (add)
            pax.emplace_back(key, v);
    };
    const bool had_l = std::any_of(r.aux.begin(), r.aux.end(), [](auto& a) { return a.first == 'L'; });
    const bool had_k = std::any_of(r.aux.begin(), r.aux.end(), [](auto& a) { return a.first == 'K'; });
    bool long_name = had_l, long_link = had_k;

    // Path: ustar name (and prefix), else GNU long name or pax path.
    if (path != r.name) {
        bool fits = path.size() <= 100;
        std::string prefix;
        std::string name = path;
        if (!fits && posix)
            for (auto slash = path.rfind('/'); slash != std::string::npos && slash > 0;
                 slash = path.rfind('/', slash - 1))
                if (slash <= 155 && path.size() - slash - 1 <= 100 && slash + 1 < path.size()) {
                    prefix = path.substr(0, slash), name = path.substr(slash + 1), fits = true;
                    break;
                }
        put_text(h, 0, 100, name);
        if (posix)
            put_text(h, 345, 155, prefix);
        set_pax("path", path, !fits && !gnu);
        long_name = had_l || (!fits && gnu);
    }
    const std::string& link = value("linkname");
    if (link != meta(s, i, "linkname")) {
        put_text(h, 157, 100, link);
        set_pax("linkpath", link, link.size() > 100 && !gnu);
        long_link = had_k || (link.size() > 100 && gnu);
    }
    if (content.size() != r.size) {
        if (!put_octal(h, 124, 12, content.size()))
            gnu ? put_base256(h, 124, 12, content.size())
                : set_pax("size", std::to_string(content.size()), true);
        set_pax("size", std::to_string(content.size()), false);
    }
    struct Num {
        const char* field;
        std::size_t off, width;
    };
    for (auto [field, off, width] : {Num{"uid", 108, 8}, Num{"gid", 116, 8}, Num{"mtime", 136, 12}}) {
        const std::string& v = value(field);
        if (v == meta(s, i, field))
            continue;
        std::uint64_t n = std::strtoull(v.c_str(), nullptr, 10);
        if (!put_octal(h, off, width, n))
            set_pax(field, v, true);
        set_pax(field, v, false);
    }
    if (const std::string& m = value("mode"); m != meta(s, i, "mode"))
        put_octal(h, 100, 8, std::strtoull(m.c_str(), nullptr, 8));
    for (auto [field, off] : {std::pair{"uname", std::size_t{265}}, std::pair{"gname", std::size_t{297}}})
        if (const std::string& v = value(field); v != meta(s, i, field)) {
            put_text(h, off, 32, v);
            set_pax(field, v, v.size() > 32);
        }
    h[156] = std::byte(value("type")[0]);
    finish_header(h);

    auto block = [&](std::vector<std::byte> hdr, std::string_view body) {
        if (!put_octal(hdr, 124, 12, body.size()))
            put_base256(hdr, 124, 12, body.size());
        finish_header(hdr);
        out.insert(out.end(), hdr.begin(), hdr.end());
        out.insert(out.end(), as_bytes(body).begin(), as_bytes(body).end());
        out.resize(out.size() + (kBlock - body.size() % kBlock) % kBlock);
    };
    auto old_header = [&](char type) -> std::optional<std::vector<std::byte>> {
        for (auto& [t, off] : r.aux)
            if (t == type)
                return std::vector<std::byte>(tb.begin() + static_cast<std::ptrdiff_t>(off),
                                              tb.begin() + static_cast<std::ptrdiff_t>(off + kBlock));
        return std::nullopt;
    };
    for (auto& [t, off] : r.aux) // global pax headers stay byte-exact
        if (t == 'g') {
            auto size = number(tb.subspan(off + 124, 12)).value_or(0);
            auto blocks = (size + kBlock - 1) / kBlock * kBlock;
            out.insert(out.end(), tb.begin() + static_cast<std::ptrdiff_t>(off),
                       tb.begin() + static_cast<std::ptrdiff_t>(off + kBlock + blocks));
        }
    if (!pax.empty()) {
        std::string body;
        for (auto& [k, v] : pax)
            body += pax_record(k, v);
        block(old_header('x').value_or(new_header("././@PaxHeader", 'x', 0, false)), body);
    }
    if (long_link)
        block(old_header('K').value_or(new_header("././@LongLink", 'K', 0, true)), link + '\0');
    if (long_name)
        block(old_header('L').value_or(new_header("././@LongLink", 'L', 0, true)), path + '\0');
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), content.begin(), content.end());
    out.resize(out.size() + (kBlock - content.size() % kBlock) % kBlock);
}

std::string type_name(char t) {
    switch (t) {
    case '0':
    case '7':
        return "file";
    case '1':
        return "hardlink";
    case '2':
        return "symlink";
    case '3':
        return "char";
    case '4':
        return "block";
    case '5':
        return "dir";
    case '6':
        return "fifo";
    case 'S':
        return "sparse";
    }
    return std::string("type-") + t;
}

class TarFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "tar",
            "TAR archives (ustar, pax, GNU), plain or gzip/zstd/xz/bzip2: members by path",
            {".tar", ".tgz", ".gz", ".tzst", ".zst", ".txz", ".xz", ".tbz2", ".tbz", ".bz2"},
            {true, true, false, true, false},
            {}};
        return i;
    }

    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        Wrapper w = sniff(head);
        if (w == Wrapper::none)
            return {header_confidence(head)};
        try {
            auto inner = unwrap(w, head, kBlock, true);
            return {inner.size() >= kBlock && header_confidence(inner) ? 100 : 0};
        } catch (const Error&) {
            return {0};
        }
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return load_doc(std::move(src), o.ctx.budget);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        ChangeSet cs = container::diff(doc(a), doc(b), o);
        if (times(o.ctx) == "ignore")
            std::erase_if(cs, [](const Change& c) { return c.at.path.ends_with("/meta/mtime"); });
        return cs;
    }

    std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                        const DiffOptions& o) const override {
        return container::compare(Format::compare(a, b, o), times(o.ctx) == "strict");
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override {
        const TarDoc& o = doc(ours);
        const std::array<const TarDoc*, 3> docs{&doc(base), &o, &doc(theirs)};
        container::Merged m = container::merge(*docs[0], o, *docs[2], mo);
        MergeResult& r = m.result;
        std::vector<std::byte> bytes;
        if (container::is_ours(m, o)) {
            bytes.assign(o.src.bytes().begin(), o.src.bytes().end()); // no-op shortcut: ours verbatim
        } else {
            std::vector<std::byte> tar;
            for (auto& e : m.entries) {
                const TarDoc& s = *docs[static_cast<int>(e.body.side)];
                const Rec& rec = s.recs[e.body.index];
                if (e.raw())
                    tar.insert(tar.end(), s.tar.bytes().begin() + static_cast<std::ptrdiff_t>(rec.start),
                               s.tar.bytes().begin() + static_cast<std::ptrdiff_t>(rec.end));
                else
                    write_member(tar, docs, e);
            }
            tar.insert(tar.end(), o.tar.bytes().begin() + static_cast<std::ptrdiff_t>(o.tail),
                       o.tar.bytes().end());
            bytes = wrap(o.wrapper, o.src.bytes(), tar);
            if (o.wrapper != Wrapper::none)
                r.warnings.push_back({"recompressed",
                                      {},
                                      std::string("the ") + wrapper_name(o.wrapper) +
                                          " stream was re-encoded (the TAR content is exact)"});
        }
        Limits budget = mo.ctx.budget.fresh(); // the result is a new document (#51)
        r.document = load_doc(ByteSource::from_memory(std::move(bytes), o.src.name()), budget);
        return std::move(r);
    }

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions& o) const override {
        auto& t = doc(d);
        const bool strict = times(o.ctx) == "strict";
        os << "# tar " << t.entries.size() << " entries";
        if (t.wrapper != Wrapper::none)
            os << " (" << wrapper_name(t.wrapper) << ")";
        os << "\n";
        std::vector<std::size_t> order(t.entries.size());
        for (std::size_t k = 0; k < order.size(); ++k)
            order[k] = k;
        std::sort(order.begin(), order.end(),
                  [&](auto x, auto y) { return t.entries[x].path < t.entries[y].path; });
        for (auto i : order) {
            const Rec& r = t.recs[i];
            os << "entry " << t.entries[i].path << "  " << type_name(r.type) << "  " << r.size
               << "  mode=" << meta(t, i, "mode") << "  owner=" << meta(t, i, "uid") << "/"
               << meta(t, i, "gid");
            if (!r.linkname.empty())
                os << "  -> " << r.linkname;
            if (strict)
                os << "  mtime=" << meta(t, i, "mtime");
            os << "\n";
        }
        container::dump_members(t, os, o);
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& t = doc(d);
        nlohmann::ordered_json j;
        j["entries"] = t.entries.size();
        j["wrapper"] = wrapper_name(t.wrapper);
        j["integrity"] = "ok"; // every header checksum is verified on load
        auto& units = j["units"] = nlohmann::ordered_json::array();
        for (std::size_t i = 0; i < t.entries.size(); ++i)
            units.push_back({{"at", container::entry_address(t.entries[i].path)},
                             {"value", type_name(t.recs[i].type) + " " + std::to_string(t.recs[i].size)}});
        auto& warn = j["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : t.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return j;
    }

private:
    static std::string times(const Context& c) {
        auto f = c.format_options.find("zip-times"); // declared by the zip module, honoured here too
        return f == c.format_options.end() ? std::string() : f->second;
    }
};

} // namespace

std::vector<std::byte> build(std::span<const NewEntry> entries, Wrapper w) {
    std::vector<std::byte> tar;
    for (auto& e : entries) {
        auto h = new_header(e.path.size() <= 100 ? e.path : "././@LongName", e.type, e.content.size(), false);
        put_octal(h, 100, 8, e.mode);
        put_octal(h, 136, 12, e.mtime);
        put_text(h, 157, 100, e.linkname);
        if (e.path.size() > 100) {
            std::string body = pax_record("path", e.path);
            auto x = new_header("././@PaxHeader", 'x', body.size(), false);
            finish_header(x);
            tar.insert(tar.end(), x.begin(), x.end());
            tar.insert(tar.end(), as_bytes(body).begin(), as_bytes(body).end());
            tar.resize(tar.size() + (kBlock - body.size() % kBlock) % kBlock);
        }
        finish_header(h);
        tar.insert(tar.end(), h.begin(), h.end());
        tar.insert(tar.end(), e.content.begin(), e.content.end());
        tar.resize(tar.size() + (kBlock - e.content.size() % kBlock) % kBlock);
    }
    tar.resize(tar.size() + 2 * kBlock);
    if (w == Wrapper::none)
        return tar;
    // Template headers for wrap(): gzip with mtime 0 and OS 255, a zstd frame with a checksum, xz CRC64,
    // bzip2 level 9.
    static const unsigned char gz[] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    static const unsigned char zs[] = {0x28, 0xb5, 0x2f, 0xfd, 0x04};
    static const unsigned char xz[] = {0xfd, '7', 'z', 'X', 'Z', 0, 0, 4};
    static const unsigned char bz[] = {'B', 'Z', 'h', '9'};
    auto tpl = [](const unsigned char* p, std::size_t n) {
        return Bytes(reinterpret_cast<const std::byte*>(p), n);
    };
    Bytes t = w == Wrapper::gzip   ? tpl(gz, sizeof gz)
              : w == Wrapper::zstd ? tpl(zs, sizeof zs)
              : w == Wrapper::xz   ? tpl(xz, sizeof xz)
                                   : tpl(bz, sizeof bz);
    return wrap(w, t, tar);
}

std::unique_ptr<Format> make_format() {
    return std::make_unique<TarFormat>();
}

} // namespace semsplinter::tar
