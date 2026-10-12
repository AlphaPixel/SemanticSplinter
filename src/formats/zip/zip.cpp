#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/formats/zip.hpp>

#include "../container/container.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>

namespace semsplinter::zip {

namespace {

using container::Archive;
using container::OutEntry;
using container::Pick;

constexpr std::uint32_t kLocal = 0x04034b50, kCentral = 0x02014b50, kEnd = 0x06054b50, kEnd64 = 0x06064b50,
                        kLocator = 0x07064b50, kDescriptor = 0x08074b50, k32 = 0xFFFFFFFF;
constexpr std::uint16_t kZip64 = 0x0001, kUnicodePath = 0x7075, kAlignExtra = 0xD935;

using Bytes = std::span<const std::byte>;

std::uint64_t le(Bytes d, std::uint64_t at, int n) {
    std::uint64_t v = 0;
    for (int k = n - 1; k >= 0; --k)
        v = v << 8 | std::to_integer<std::uint64_t>(d[at + static_cast<std::uint64_t>(k)]);
    return v;
}
std::uint16_t le16(Bytes d, std::uint64_t at) {
    return static_cast<std::uint16_t>(le(d, at, 2));
}
std::uint32_t le32(Bytes d, std::uint64_t at) {
    return static_cast<std::uint32_t>(le(d, at, 4));
}
std::uint64_t le64(Bytes d, std::uint64_t at) {
    return le(d, at, 8);
}
void put(std::vector<std::byte>& o, std::uint64_t v, int n) {
    for (int k = 0; k < n; ++k)
        o.push_back(std::byte((v >> (8 * k)) & 0xFF));
}
void set(std::vector<std::byte>& o, std::size_t at, std::uint64_t v, int n) {
    for (int k = 0; k < n; ++k)
        o[at + static_cast<std::size_t>(k)] = std::byte((v >> (8 * k)) & 0xFF);
}
void append(std::vector<std::byte>& o, Bytes b) {
    o.insert(o.end(), b.begin(), b.end());
}
std::string str(Bytes b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

Error bad(const std::string& code, const std::string& what) {
    return Error(ErrorCategory::invalid_input, "zip: " + what + " (" + code + ")");
}

// CP437 code points for bytes 0x80..0xFF (names without the UTF-8 flag, APPNOTE appendix D).
constexpr char16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

std::string cp437(std::string_view raw) {
    std::string o;
    for (char ch : raw) {
        auto c = static_cast<unsigned char>(ch);
        char32_t u = c < 0x80 ? c : kCp437[c - 0x80];
        if (u < 0x80) {
            o += static_cast<char>(u);
        } else if (u < 0x800) {
            o += static_cast<char>(0xC0 | (u >> 6));
            o += static_cast<char>(0x80 | (u & 0x3F));
        } else {
            o += static_cast<char>(0xE0 | (u >> 12));
            o += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return o;
}

// The data of extra field `id`, if present.
std::optional<Bytes> find_extra(Bytes extra, std::uint16_t id) {
    for (std::size_t k = 0; k + 4 <= extra.size();) {
        std::size_t len = le16(extra, k + 2);
        if (k + 4 + len > extra.size())
            break;
        if (le16(extra, k) == id)
            return extra.subspan(k + 4, len);
        k += 4 + len;
    }
    return std::nullopt;
}

// Offset of extra field `id`'s data inside `extra`.
std::optional<std::size_t> extra_offset(Bytes extra, std::uint16_t id) {
    if (auto f = find_extra(extra, id))
        return static_cast<std::size_t>(f->data() - extra.data());
    return std::nullopt;
}

struct Rec {
    std::uint64_t cd = 0, cd_len = 0; // central record
    std::uint16_t made_by = 0, flags = 0, method = 0, time = 0, date = 0;
    std::uint32_t crc = 0, ext_attr = 0;
    std::uint64_t csize = 0, usize = 0, local = 0; // local: file offset of the local header
    std::string name;                              // raw bytes as stored
    std::uint64_t cextra = 0, cextra_len = 0, comment = 0, comment_len = 0;
    std::uint64_t lextra = 0, lextra_len = 0;
    std::uint64_t data = 0, end = 0, span_end = 0; // compressed data, end of descriptor, raw span end
};

struct ZipDoc final : Archive {
    ByteSource src;
    std::vector<Rec> recs; // parallel to entries
    std::uint64_t prefix = 0, shift = 0, cd_start = 0, end_pos = 0;
    std::optional<std::uint64_t> end64, locator, sigblock; // ZIP64 records; APK Signing Block start
    std::uint32_t align = 0, so_align = 0;                 // zipalign profile of stored members
    mutable std::map<std::size_t, std::vector<std::byte>> decoded;
    mutable std::map<std::size_t, Hash128> hashes, raw_hashes;
    mutable std::vector<std::string> crc_mismatch;

    Bytes bytes(std::uint64_t at, std::uint64_t n) const { return src.bytes().subspan(at, n); }
    Bytes packed(std::size_t i) const { return bytes(recs[i].data, recs[i].csize); }
    std::string comment() const { return str(bytes(end_pos + 22, le16(src.bytes(), end_pos + 20))); }
    bool decodable(std::size_t i) const {
        auto m = recs[i].method;
        return !(recs[i].flags & 1) && (m == 0 || m == 8 || m == 93 || m == 20);
    }
    Hash128 raw_hash(std::size_t i) const {
        if (auto f = raw_hashes.find(i); f != raw_hashes.end())
            return f->second;
        return raw_hashes[i] = hash128(packed(i));
    }

    std::span<const std::byte> content(std::size_t i, Limits& l) const override {
        if (auto f = decoded.find(i); f != decoded.end())
            return f->second;
        const Rec& r = recs[i];
        const std::string& path = entries[i].path;
        if (!decodable(i))
            throw Error(ErrorCategory::unsupported,
                        "zip: member " + path +
                            (r.flags & 1 ? " is encrypted" : " uses method " + std::to_string(r.method)));
        // Declared sizes are checked before decoding, actual sizes while decoding (containers.md §8.1).
        if (r.usize > container::decode_limit(l, r.csize))
            throw Error(ErrorCategory::limit, "zip: member " + path + " declares " + std::to_string(r.usize) +
                                                  " bytes, beyond --max-zip-size, --max-zip-ratio, or "
                                                  "--max-memory");
        std::vector<std::byte> out;
        if (r.method == 0)
            out.assign(packed(i).begin(), packed(i).end());
        else if (r.method == 8)
            out = container::inflate(packed(i), r.usize, -15);
        else
            out = container::unzstd(packed(i), r.usize);
        if (out.size() != r.usize)
            throw bad("zip-size-mismatch", "member " + path + " does not decode to its declared size");
        container::charge(l, out.size());
        if (crc32(out) != r.crc)
            crc_mismatch.push_back(container::entry_address(path));
        return decoded.emplace(i, std::move(out)).first->second;
    }

    Hash128 hash(std::size_t i, Limits& l) const override {
        if (auto f = hashes.find(i); f != hashes.end())
            return f->second;
        if (decodable(i))
            return hashes[i] = hash128(content(i, l));
        Hasher h;
        h.add(recs[i].method).add(recs[i].crc).add(recs[i].usize).raw(packed(i));
        return hashes[i] = h.digest();
    }

    std::vector<std::byte> write(const std::array<const Archive*, 3>& docs,
                                 const std::vector<OutEntry>& entries, bool bare, Limits&,
                                 std::vector<Warning>&) const override;

    bool same(std::size_t i, const Archive& other, std::size_t j, Limits& l) const override {
        auto& o = static_cast<const ZipDoc&>(other);
        const Rec &a = recs[i], &b = o.recs[j];
        // Equality shortcut (containers.md §4): no decompression when the stored bytes agree.
        if (a.method == b.method && a.crc == b.crc && a.usize == b.usize && a.csize == b.csize &&
            (a.flags & 1) == (b.flags & 1) && raw_hash(i) == o.raw_hash(j))
            return true;
        if (!decodable(i) || !o.decodable(j) || a.crc != b.crc || a.usize != b.usize)
            return false;
        return hash(i, l) == o.hash(j, l);
    }
};

const ZipDoc& doc(const Document& d) {
    auto* z = dynamic_cast<const ZipDoc*>(&d);
    if (!z)
        throw Error(ErrorCategory::internal, "zip: not a ZIP document");
    return *z;
}

bool option(const OptionValues& o, const char* name) {
    return o.count(name) > 0;
}
std::string option_value(const OptionValues& o, const char* name, std::string fallback = {}) {
    auto f = o.find(name);
    return f == o.end() ? fallback : f->second;
}

std::string dos_time(std::uint16_t date, std::uint16_t time) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u", (date >> 9) + 1980u, (date >> 5) & 15u,
                  date & 31u, static_cast<unsigned>(time >> 11), (time >> 5) & 63u, (time & 31u) * 2);
    return buf;
}

std::string mode_text(const Rec& r) {
    char buf[32];
    if (r.made_by >> 8 == 3)
        std::snprintf(buf, sizeof buf, "0%o", r.ext_attr >> 16);
    else
        std::snprintf(buf, sizeof buf, "dos:0x%02x", r.ext_attr & 0xFF);
    return buf;
}

std::string method_name(std::uint16_t m) {
    switch (m) {
    case 0:
        return "stored";
    case 8:
        return "deflate";
    case 9:
        return "deflate64";
    case 12:
        return "bzip2";
    case 14:
        return "lzma";
    case 20:
    case 93:
        return "zstd";
    case 95:
        return "xz";
    case 98:
        return "ppmd";
    case 99:
        return "aes";
    }
    return "method-" + std::to_string(m);
}

// Reads and checks one archive (containers.md §2.1, §8.1). Every offset is bounded before use.
std::unique_ptr<ZipDoc> load_doc(ByteSource src, Limits& limits, const OptionValues& opts) {
    auto d = std::make_unique<ZipDoc>();
    d->src = std::move(src);
    Bytes b = d->src.bytes();
    const std::uint64_t n = b.size();
    const bool lenient = option(opts, "zip-lenient");
    auto mismatch = [&](const std::string& path, const std::string& what) {
        if (!lenient)
            throw bad("zip-header-mismatch", "local header of " + path +
                                                 " disagrees with the central "
                                                 "directory: " +
                                                 what);
        d->warnings.push_back({"zip-header-mismatch", container::entry_address(path), what});
    };

    // End of central directory: the record whose comment runs exactly to the end of the file.
    std::optional<std::uint64_t> end;
    if (n >= 22)
        for (std::uint64_t k = n - 22;; --k) {
            if (le32(b, k) == kEnd && k + 22 + le16(b, k + 20) == n) {
                end = k;
                break;
            }
            if (k == 0 || n - 22 - k >= 65535)
                break;
        }
    if (!end)
        throw bad("zip-no-end", "no end of central directory record");
    d->end_pos = *end;
    if (le16(b, *end + 4) != 0 || le16(b, *end + 6) != 0)
        throw Error(ErrorCategory::unsupported, "zip: multi-disk archives are not supported");
    std::uint64_t count = le16(b, *end + 10), cd_size = le32(b, *end + 12), cd_off = le32(b, *end + 16);
    std::uint64_t cd_end = *end;
    if (*end >= 20 && le32(b, *end - 20) == kLocator) {
        d->locator = *end - 20;
        std::uint64_t at = le64(b, *end - 12);
        if (*end >= 76 && le32(b, *end - 76) == kEnd64)
            at = *end - 76; // the usual place; the stored offset may be relative to a prefix
        if (at + 56 > *d->locator || le32(b, at) != kEnd64)
            throw bad("zip-bad-zip64", "ZIP64 end of central directory not found");
        d->end64 = at;
        count = le64(b, at + 32), cd_size = le64(b, at + 40), cd_off = le64(b, at + 48);
        cd_end = at;
    }
    if (count > limits.max_zip_entries)
        throw Error(ErrorCategory::limit,
                    "zip: " + std::to_string(count) +
                        " entries exceed --max-zip-entries=" + std::to_string(limits.max_zip_entries));
    if (cd_size > cd_end || cd_size < 46 * count)
        throw bad("zip-bad-directory", "central directory size is inconsistent");
    d->cd_start = cd_end - cd_size;
    if (cd_off > d->cd_start)
        throw bad("zip-bad-directory", "central directory offset points past the directory");
    d->shift = d->cd_start - cd_off; // > 0 when a prefix was prepended without adjusting offsets

    std::set<std::string> names;
    std::uint64_t p = d->cd_start;
    for (std::uint64_t k = 0; k < count; ++k) {
        if (p + 46 > cd_end || le32(b, p) != kCentral)
            throw bad("zip-bad-directory", "central directory record " + std::to_string(k) + " is damaged");
        Rec r;
        r.cd = p;
        r.made_by = le16(b, p + 4), r.flags = le16(b, p + 8), r.method = le16(b, p + 10);
        r.time = le16(b, p + 12), r.date = le16(b, p + 14), r.crc = le32(b, p + 16);
        r.csize = le32(b, p + 20), r.usize = le32(b, p + 24);
        std::uint64_t nlen = le16(b, p + 28), xlen = le16(b, p + 30), clen = le16(b, p + 32);
        r.ext_attr = le32(b, p + 38), r.local = le32(b, p + 42);
        r.cd_len = 46 + nlen + xlen + clen;
        if (p + r.cd_len > cd_end)
            throw bad("zip-bad-directory", "central directory record " + std::to_string(k) + " is truncated");
        r.name = str(b.subspan(p + 46, nlen));
        r.cextra = p + 46 + nlen, r.cextra_len = xlen;
        r.comment = r.cextra + xlen, r.comment_len = clen;
        Bytes cextra = b.subspan(r.cextra, xlen);
        if (r.usize == k32 || r.csize == k32 || r.local == k32) {
            auto z = find_extra(cextra, kZip64);
            std::uint64_t at = 0;
            for (auto* v : {&r.usize, &r.csize, &r.local})
                if (*v == k32) {
                    if (!z || at + 8 > z->size())
                        throw bad("zip-bad-zip64", "ZIP64 extra field missing in " + r.name);
                    *v = le64(*z, at), at += 8;
                }
        }
        std::string path = r.flags & 0x800 ? r.name : cp437(r.name);
        if (auto u = find_extra(cextra, kUnicodePath);
            u && u->size() > 5 && le(*u, 0, 1) == 1 && le32(*u, 1) == crc32(as_bytes(r.name)))
            path = str(u->subspan(5));
        if (!names.insert(path).second)
            throw bad("zip-duplicate-name", "duplicate member " + path);
        if (container::suspicious_path(path))
            d->warnings.push_back(
                {"suspicious-path", container::entry_address(path), "not a safe relative path"});

        // The local header must agree with the central record (parser-differential defence).
        if (r.local > std::numeric_limits<std::uint64_t>::max() - d->shift)
            throw bad("zip-bad-directory", "local header offset out of range");
        r.local += d->shift;
        const std::uint64_t L = r.local;
        if (L + 30 > d->cd_start || le32(b, L) != kLocal)
            throw bad("zip-bad-local", "no local header for " + path);
        std::uint16_t lflags = le16(b, L + 6);
        std::uint64_t lnlen = le16(b, L + 26), lxlen = le16(b, L + 28);
        r.lextra = L + 30 + lnlen, r.lextra_len = lxlen;
        r.data = r.lextra + lxlen;
        if (r.data > d->cd_start || r.csize > d->cd_start - r.data)
            throw bad("zip-bad-local", "member " + path + " runs past the central directory");
        if (str(b.subspan(L + 30, lnlen)) != r.name)
            mismatch(path, "name");
        if (le16(b, L + 8) != r.method)
            mismatch(path, "method");
        Bytes lextra = b.subspan(r.lextra, lxlen);
        bool zip64 = find_extra(lextra, kZip64).has_value() || find_extra(cextra, kZip64).has_value();
        if (!(lflags & 8)) {
            std::uint64_t lc = le32(b, L + 18), lu = le32(b, L + 22);
            if (auto z = find_extra(lextra, kZip64); z && (lu == k32 || lc == k32) && z->size() >= 16)
                lu = le64(*z, 0), lc = le64(*z, 8);
            if (le32(b, L + 14) != r.crc || lc != r.csize || lu != r.usize)
                mismatch(path, "CRC or sizes");
            r.end = r.data + r.csize;
        } else {
            std::uint64_t q = r.data + r.csize;
            if (q + 4 <= d->cd_start && le32(b, q) == kDescriptor)
                q += 4;
            std::uint64_t len = zip64 ? 20 : 12;
            if (q + len > d->cd_start)
                throw bad("zip-bad-local", "data descriptor of " + path + " is truncated");
            if (le32(b, q) != r.crc)
                mismatch(path, "data descriptor CRC");
            r.end = q + len;
        }
        d->recs.push_back(r);
        d->entries.push_back({path,
                              {{"mtime", dos_time(r.date, r.time)},
                               {"mode", mode_text(r)},
                               {"comment", str(b.subspan(r.comment, clen))}},
                              !d->decodable(d->recs.size() - 1)});
        p += r.cd_len;
    }
    if (p != cd_end)
        throw bad("zip-bad-directory", "central directory size does not match its records");

    // Overlaps (containers.md §8.1), raw spans, prefix, APK Signing Block.
    std::vector<std::size_t> order(d->recs.size());
    for (std::size_t k = 0; k < order.size(); ++k)
        order[k] = k;
    std::sort(order.begin(), order.end(),
              [&](auto x, auto y) { return d->recs[x].local < d->recs[y].local; });
    std::uint64_t last_end = order.empty() ? d->cd_start : d->recs[order.front()].local;
    d->prefix = last_end;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Rec& r = d->recs[order[k]];
        if (k + 1 < order.size() && r.end > d->recs[order[k + 1]].local)
            throw bad("zip-overlap", "members " + d->entries[order[k]].path + " and " +
                                         d->entries[order[k + 1]].path + " overlap");
        last_end = r.end;
    }
    if (d->cd_start - last_end >= 32 && str(b.subspan(d->cd_start - 16, 16)) == "APK Sig Block 42") {
        std::uint64_t size = le64(b, d->cd_start - 24);
        if (size <= d->cd_start - last_end - 8 && le64(b, d->cd_start - size - 8) == size)
            d->sigblock = d->cd_start - size - 8;
    }
    for (std::size_t k = 0; k < order.size(); ++k)
        d->recs[order[k]].span_end =
            k + 1 < order.size() ? d->recs[order[k + 1]].local : d->sigblock.value_or(d->cd_start);

    // zipalign profile: every stored member's data aligned (4 bytes; .so libraries 4 or 16 KiB).
    bool any = false, a4 = true, so4k = true, so16k = true, any_so = false;
    for (auto& r : d->recs) {
        if (r.method != 0 || r.csize == 0)
            continue;
        any = true;
        a4 &= r.data % 4 == 0;
        if (r.name.ends_with(".so"))
            any_so = true, so4k &= r.data % 4096 == 0, so16k &= r.data % 16384 == 0;
    }
    d->align = any && a4 ? 4 : 0;
    d->so_align = !d->align || !any_so ? 0 : so16k ? 16384 : so4k ? 4096 : 0;
    return d;
}

// ---- Writer (containers.md §7.2) ----

// Rewrites the ZIP64 extra in `extra` for the slots whose 32-bit field is 0xFFFFFFFF.
void set_zip64(std::vector<std::byte>& extra, std::initializer_list<std::pair<bool, std::uint64_t>> slots) {
    auto at = extra_offset(extra, kZip64);
    if (!at)
        return;
    std::size_t k = *at;
    for (auto [present, v] : slots)
        if (present) {
            if (k + 8 > extra.size())
                throw bad("zip-bad-zip64", "ZIP64 extra field too short");
            set(extra, k, v, 8), k += 8;
        }
}

// A 32-bit field, or 0xFFFFFFFF when the source kept the value in its ZIP64 extra.
std::uint32_t field32(bool zip64_slot, std::uint64_t v, const std::string& path) {
    if (zip64_slot)
        return k32;
    if (v >= k32)
        throw Error(ErrorCategory::unsupported,
                    "zip: " + path + " needs ZIP64 fields its source did not have");
    return static_cast<std::uint32_t>(v);
}

// Pads the local extra field so that the data starting at `data_at` (without padding) is aligned, as
// zipalign does: a 0xD935 field when the source had one, else trailing zero bytes.
void realign(std::vector<std::byte>& extra, std::uint64_t data_at, std::uint32_t align) {
    bool d935 = false;
    std::vector<std::byte> kept;
    std::size_t k = 0;
    while (k + 4 <= extra.size()) {
        std::size_t len = le16(extra, k + 2);
        if (k + 4 + len > extra.size())
            break;
        if (le16(extra, k) == kAlignExtra)
            d935 = true;
        else
            kept.insert(kept.end(), extra.begin() + static_cast<std::ptrdiff_t>(k),
                        extra.begin() + static_cast<std::ptrdiff_t>(k + 4 + len));
        k += 4 + len;
    }
    data_at -= extra.size() - kept.size();
    extra = std::move(kept);
    std::uint64_t at = data_at + extra.size() + (d935 ? 6 : 0);
    std::size_t pad = static_cast<std::size_t>((align - at % align) % align);
    if (d935) {
        put(extra, kAlignExtra, 2), put(extra, 2 + pad, 2), put(extra, align, 2);
    }
    extra.insert(extra.end(), pad, std::byte{0});
}

int deflate_level(const ZipDoc& s, std::size_t i, Limits& l) {
    static constexpr int by_flags[] = {6, 9, 2, 1}; // general-purpose bits 1-2 (containers.md §7.1)
    const Rec& r = s.recs[i];
    int level = by_flags[(r.flags >> 1) & 3];
    if (r.method != 8 || r.usize > (1u << 20) || !s.decodable(i))
        return level;
    auto c = s.content(i, l);
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    int pick = level;
    for (int lv = 1; lv <= 9; ++lv) {
        std::uint64_t n = container::deflate(c, lv).size();
        std::uint64_t dist = n > r.csize ? n - r.csize : r.csize - n;
        if (dist < best || (dist == best && lv == level))
            best = dist, pick = lv;
    }
    return pick;
}

struct Writer {
    std::array<const ZipDoc*, 3> docs;
    const ZipDoc& layout; // ours: prefix, offsets, ZIP64 records, alignment
    Limits& limits;
    std::vector<Warning>& warnings;
    std::uint64_t shift = 0;
    std::vector<std::byte> out, cd;
    std::uint64_t count = 0;

    const ZipDoc& of(Side s) const { return *docs[static_cast<int>(s)]; }

    std::uint32_t alignment(const Rec& r, std::uint16_t method) const {
        if (method != 0 || r.csize == 0 || !layout.align)
            return 0;
        return layout.so_align && r.name.ends_with(".so") ? layout.so_align : layout.align;
    }

    void entry(const OutEntry& e) {
        const ZipDoc& s = of(e.body.side);
        const std::size_t i = e.body.index;
        const Rec& r = s.recs[i];
        const std::uint64_t pos = out.size();
        const std::string& path = s.entries[i].path;
        auto pick = [&](const char* field) -> std::pair<const ZipDoc*, const Rec*> {
            auto f = e.from.find(field);
            if (f == e.from.end())
                return {&s, &r};
            const ZipDoc& d = of(f->second.side);
            return {&d, &d.recs[f->second.index]};
        };
        const std::uint32_t want = alignment(r, r.method);
        if (e.raw() && (!want || (pos + (r.data - r.local)) % want == 0)) {
            append(out, s.bytes(r.local, r.span_end - r.local));
            std::vector<std::byte> c(s.bytes(r.cd, r.cd_len).begin(), s.bytes(r.cd, r.cd_len).end());
            if (le32(c, 42) == k32) {
                std::vector<std::byte> x(c.begin() + 46 + static_cast<std::ptrdiff_t>(r.name.size()),
                                         c.begin() + 46 +
                                             static_cast<std::ptrdiff_t>(r.name.size() + r.cextra_len));
                set_zip64(
                    x, {{le32(c, 24) == k32, r.usize}, {le32(c, 20) == k32, r.csize}, {true, pos - shift}});
                std::copy(x.begin(), x.end(), c.begin() + 46 + static_cast<std::ptrdiff_t>(r.name.size()));
            } else {
                set(c, 42, field32(false, pos - shift, path), 4);
            }
            append(cd, c);
            ++count;
            return;
        }

        // A touched member: headers rebuilt from the source records, data recompressed if it changed.
        std::uint16_t method = r.method == 20 ? 93 : r.method; // 20 is the deprecated zstd id (read only)
        std::vector<std::byte> packed;
        std::uint32_t crc = r.crc;
        std::uint64_t usize = r.usize;
        if (e.content) {
            const auto& c = *e.content;
            usize = c.size(), crc = crc32(c);
            packed = method == 0   ? c
                     : method == 8 ? container::deflate(c, deflate_level(s, i, limits))
                                   : container::zstd(c, 3, false);
            warnings.push_back({"recompressed", container::entry_address(path),
                                "member written with " + method_name(method) + " (content is exact)"});
        } else {
            method = r.method;
            packed.assign(s.packed(i).begin(), s.packed(i).end());
        }
        auto [nd, nr] = pick("path");
        auto [td, tr] = pick("mtime");
        auto [md, mr] = pick("mode");
        auto [cdoc, cr] = pick("comment");
        const std::uint16_t flags =
            static_cast<std::uint16_t>((r.flags & ~std::uint16_t{0x808}) | (nr->flags & 0x800));
        Bytes src = s.src.bytes();
        std::vector<std::byte> lextra(s.bytes(r.lextra, r.lextra_len).begin(),
                                      s.bytes(r.lextra, r.lextra_len).end());
        const bool lz64 = find_extra(lextra, kZip64).has_value();
        set_zip64(lextra, {{lz64, usize}, {lz64, packed.size()}});
        if (want) {
            const std::uint64_t before = pos + 30 + nr->name.size() + lextra.size();
            if (before % want) {
                realign(lextra, before, want);
                warnings.push_back(
                    {"realigned", container::entry_address(path),
                     "local extra field padded to keep " + std::to_string(want) + "-byte alignment"});
            }
        }
        out.reserve(out.size() + 30 + nr->name.size() + lextra.size() + packed.size());
        put(out, kLocal, 4);
        put(out, le16(src, r.local + 4), 2); // version needed
        put(out, flags, 2);
        put(out, method, 2);
        put(out, tr->time, 2), put(out, tr->date, 2);
        put(out, crc, 4);
        put(out, field32(lz64, packed.size(), path), 4);
        put(out, field32(lz64, usize, path), 4);
        put(out, nr->name.size(), 2), put(out, lextra.size(), 2);
        append(out, as_bytes(nr->name));
        append(out, lextra);
        append(out, packed);

        std::vector<std::byte> cextra(s.bytes(r.cextra, r.cextra_len).begin(),
                                      s.bytes(r.cextra, r.cextra_len).end());
        const bool zu = le32(src, r.cd + 24) == k32, zc = le32(src, r.cd + 20) == k32,
                   zo = le32(src, r.cd + 42) == k32;
        set_zip64(cextra, {{zu, usize}, {zc, packed.size()}, {zo, pos - shift}});
        Bytes comment = cdoc->bytes(cr->comment, cr->comment_len);
        put(cd, kCentral, 4);
        put(cd, mr->made_by, 2);
        put(cd, le16(src, r.cd + 6), 2); // version needed
        put(cd, flags, 2);
        put(cd, method, 2);
        put(cd, tr->time, 2), put(cd, tr->date, 2);
        put(cd, crc, 4);
        put(cd, field32(zc, packed.size(), path), 4);
        put(cd, field32(zu, usize, path), 4);
        put(cd, nr->name.size(), 2), put(cd, cextra.size(), 2), put(cd, comment.size(), 2);
        put(cd, le16(src, r.cd + 34), 2), put(cd, le16(src, r.cd + 36), 2); // disk, internal attributes
        put(cd, mr->ext_attr, 4);
        put(cd, field32(zo, pos - shift, path), 4);
        append(cd, as_bytes(nr->name));
        append(cd, cextra);
        append(cd, comment);
        ++count;
    }

    // Central directory, ZIP64 records, and the end record (copied from ours, counts and offsets updated).
    void finish(std::optional<Bytes> sigblock, Bytes comment) {
        if (sigblock)
            append(out, *sigblock);
        const std::uint64_t cd_at = out.size();
        append(out, cd);
        Bytes b = layout.src.bytes();
        if (layout.end64) {
            std::uint64_t at = *layout.end64;
            std::vector<std::byte> rec(b.begin() + static_cast<std::ptrdiff_t>(at),
                                       b.begin() + static_cast<std::ptrdiff_t>(*layout.locator));
            set(rec, 24, count, 8), set(rec, 32, count, 8), set(rec, 40, cd.size(), 8),
                set(rec, 48, cd_at - shift, 8);
            const std::uint64_t rec_at = out.size();
            append(out, rec);
            std::vector<std::byte> loc(b.begin() + static_cast<std::ptrdiff_t>(*layout.locator),
                                       b.begin() + static_cast<std::ptrdiff_t>(*layout.locator + 20));
            set(loc, 8, rec_at - shift, 8);
            append(out, loc);
        }
        std::vector<std::byte> end(b.begin() + static_cast<std::ptrdiff_t>(layout.end_pos),
                                   b.begin() + static_cast<std::ptrdiff_t>(layout.end_pos + 20));
        const bool z = layout.end64.has_value();
        auto keep = [&](std::size_t at, int width, std::uint64_t v) {
            const std::uint64_t all = width == 2 ? 0xFFFF : k32;
            if (z && le(end, at, width) == all)
                return;
            if (v >= all)
                throw Error(ErrorCategory::unsupported,
                            "zip: the result needs ZIP64 records ours did not have");
            set(end, at, v, width);
        };
        keep(8, 2, count), keep(10, 2, count), keep(12, 4, cd.size()), keep(16, 4, cd_at - shift);
        put(end, comment.size(), 2);
        append(end, comment);
        append(out, end);
    }
};

std::vector<std::byte> ZipDoc::write(const std::array<const Archive*, 3>& docs,
                                     const std::vector<OutEntry>& list, bool bare, Limits& l,
                                     std::vector<Warning>& notes) const {
    std::array<const ZipDoc*, 3> zd{};
    for (std::size_t k = 0; k < 3; ++k)
        zd[k] = static_cast<const ZipDoc*>(docs[k]);
    Writer w{zd, *this, l, notes, bare || !shift ? 0 : prefix, {}, {}, 0};
    if (!bare)
        append(w.out, bytes(0, prefix));
    for (auto& e : list)
        w.entry(e);
    w.finish(!bare && sigblock ? std::optional(bytes(*sigblock, cd_start - *sigblock)) : std::nullopt,
             bare ? Bytes{} : as_bytes(comment()));
    return std::move(w.out);
}

// ---- Signatures (containers.md §6.3, §7.1; D-50) ----

bool signature_member(std::string_view path) {
    if (path == ".signature.p7s")
        return true;
    if (!path.starts_with("META-INF/") || path.find('/', 9) != std::string_view::npos)
        return false;
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    std::string ext(path.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext == "SF" || ext == "RSA" || ext == "DSA" || ext == "EC";
}

// MANIFEST.MF without the *-Digest lines of `names` (a section left with only its Name goes too).
// Untouched lines keep their bytes, including 72-byte wrapping and line terminators.
std::optional<std::string> drop_digests(std::string_view mf, const std::set<std::string>& names,
                                        std::vector<std::string>& dropped) {
    struct Logical {
        std::string_view raw; // physical lines with terminators
        std::string text;     // joined, without terminators
    };
    std::vector<Logical> lines;
    for (std::size_t at = 0; at < mf.size();) {
        std::size_t e = mf.find_first_of("\r\n", at);
        std::size_t next =
            e == std::string_view::npos ? mf.size() : e + (mf.compare(e, 2, "\r\n") == 0 ? 2 : 1);
        std::string_view body = mf.substr(at, (e == std::string_view::npos ? mf.size() : e) - at);
        if (!lines.empty() && body.starts_with(' ') && !lines.back().text.empty()) {
            auto& l = lines.back();
            l.raw = std::string_view(l.raw.data(), static_cast<std::size_t>(mf.data() + next - l.raw.data()));
            l.text += body.substr(1);
        } else {
            lines.push_back({mf.substr(at, next - at), std::string(body)});
        }
        at = next;
    }
    std::string out;
    bool changed = false;
    for (std::size_t s = 0; s < lines.size();) {
        std::size_t e = s;
        while (e < lines.size() && !lines[e].text.empty())
            ++e;
        std::string name;
        for (std::size_t k = s; k < e; ++k)
            if (lines[k].text.starts_with("Name: "))
                name = lines[k].text.substr(6);
        std::vector<std::size_t> keep;
        for (std::size_t k = s; k < e; ++k) {
            auto colon = lines[k].text.find(':');
            std::string key = lines[k].text.substr(0, colon);
            for (char& c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!name.empty() && names.count(name) && key.ends_with("-digest"))
                continue;
            keep.push_back(k);
        }
        std::size_t end = e < lines.size() ? e + 1 : e; // with the blank separator line
        if (keep.size() == e - s) {
            for (std::size_t k = s; k < end; ++k)
                out += lines[k].raw;
        } else {
            changed = true;
            dropped.push_back(name);
            if (!(keep.size() == 1 && lines[keep[0]].text.starts_with("Name: "))) {
                for (std::size_t k : keep)
                    out += lines[k].raw;
                if (end > e)
                    out += lines[e].raw;
            }
        }
        s = end;
    }
    return changed ? std::optional(out) : std::nullopt;
}

class ZipFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "zip",
            "ZIP archives (JAR, APK, NuGet, VSIX, EPUB, ...): members diffed and merged by path",
            {".zip", ".jar", ".war", ".ear", ".apk", ".aab", ".nupkg", ".whl", ".xpi", ".vsix", ".crx",
             ".epub"},
            {true, true, true, true, false},
            {{"zip-lenient", OptionSpec::Arg::none,
              "trust the central directory when a local header disagrees (warning)"},
             {"zip-times", OptionSpec::Arg::required,
              "member time changes: listed (default), ignore, or strict (they count as differences)"}}};
        return i;
    }

    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        if (head.size() >= 4 && (le32(head, 0) == kLocal || le32(head, 0) == kEnd))
            return {90};
        if (head.size() >= 4 && str(head.first(4)) == "Cr24")
            return {80};
        // Self-extracting stubs: an end record near the end of a small file.
        if (head.size() >= 22)
            for (std::size_t k = head.size() - 22;; --k) {
                if (le32(head, k) == kEnd && k + 22 + le16(head, k + 20) == head.size())
                    return {60};
                if (k == 0 || head.size() - 22 - k >= 65535)
                    break;
            }
        return {0};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        auto d = load_doc(std::move(src), o.ctx.budget, o.ctx.format_options);
        d->remember(o.ctx);
        return d;
    }

    // Patches (containers.md §6.5): the member layer's payloads, plus the archive comment as its text.
    std::optional<std::vector<std::byte>> payload(const Document& d, const Address& at) const override {
        if (at.path == "/archive/comment") {
            auto c = as_bytes(doc(d).comment());
            return std::vector<std::byte>(c.begin(), c.end());
        }
        return container::payload(doc(d), at);
    }

    std::unique_ptr<Document> implant(const Document& t, std::span<const UnitValue> units,
                                      const LoadOptions& o) const override {
        std::vector<UnitValue> members;
        const std::vector<std::byte>* comment = nullptr;
        for (auto& u : units)
            if (u.at.path == "/archive/comment")
                comment = u.value;
            else
                members.push_back(u);
        std::vector<Warning> warnings;
        auto bytes = container::implant(*this, doc(t), members, o, warnings);
        Limits budget = o.ctx.budget.fresh(); // a new document (#51)
        Context ctx{o.ctx.registry, budget, o.ctx.depth, o.ctx.format_options};
        auto d = load_doc(ByteSource::from_memory(std::move(bytes), doc(t).src.name()), budget,
                          o.ctx.format_options);
        if (comment) { // the end record is last: its comment length, then the comment
            if (comment->size() > 0xFFFF)
                throw Error(ErrorCategory::invalid_input, "patch: a zip comment holds at most 65535 bytes");
            auto b = d->src.bytes();
            std::vector<std::byte> out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(d->end_pos + 20));
            put(out, comment->size(), 2);
            append(out, *comment);
            d = load_doc(ByteSource::from_memory(std::move(out), doc(t).src.name()), budget,
                         o.ctx.format_options);
        }
        d->remember(ctx);
        d->warnings.insert(d->warnings.end(), warnings.begin(), warnings.end());
        return d;
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        auto& da = doc(a);
        auto& db = doc(b);
        ChangeSet cs = container::diff(da, db, o);
        if (option_value(o.ctx.format_options, "zip-times") == "ignore")
            std::erase_if(cs, [](const Change& c) { return c.at.path.ends_with("/meta/mtime"); });
        if (da.comment() != db.comment()) {
            Change c = make_change(Side::theirs, Op::update, {"/archive/comment"}, "metadata");
            c.before.text = da.comment(), c.after.text = db.comment();
            cs.push_back(std::move(c));
        }
        if (prefix_hash(da) != prefix_hash(db)) {
            Change c = make_change(Side::theirs, Op::update, {"/archive/prefix"}, "content");
            c.before.hash = prefix_hash(da), c.after.hash = prefix_hash(db);
            cs.push_back(std::move(c));
        }
        if (da.sigblock.has_value() != db.sigblock.has_value())
            cs.push_back(make_change(Side::theirs, db.sigblock ? Op::insert : Op::erase,
                                     {"/archive/apk-signing-block"}, "authenticity"));
        sort_canonical(cs);
        return cs;
    }

    std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                        const DiffOptions& o) const override {
        return container::compare(Format::compare(a, b, o),
                                  option_value(o.ctx.format_options, "zip-times") == "strict");
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override;

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions& o) const override {
        auto& z = doc(d);
        const bool times = option_value(o.ctx.format_options, "zip-times") == "strict";
        os << "# zip " << z.entries.size() << " entries, comment \"" << z.comment() << "\"\n";
        std::vector<std::size_t> order(z.entries.size());
        for (std::size_t k = 0; k < order.size(); ++k)
            order[k] = k;
        std::sort(order.begin(), order.end(),
                  [&](auto x, auto y) { return z.entries[x].path < z.entries[y].path; });
        for (auto i : order) {
            const Rec& r = z.recs[i];
            char crc[16];
            std::snprintf(crc, sizeof crc, "%08x", r.crc);
            os << "entry " << z.entries[i].path << "  " << method_name(r.method) << "  " << r.usize;
            if (r.method != 0)
                os << " -> " << r.csize;
            os << "  crc=" << crc << "  mode=" << mode_text(r);
            if (r.flags & 1)
                os << "  encrypted";
            if (times)
                os << "  mtime=" << dos_time(r.date, r.time);
            os << "\n";
        }
        container::dump_members(z, os, o);
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& z = doc(d);
        nlohmann::ordered_json j;
        j["entries"] = z.entries.size();
        j["comment"] = z.comment();
        if (z.prefix)
            j["prefix"] = z.prefix;
        if (z.sigblock)
            j["apk_signing_block"] = z.cd_start - *z.sigblock;
        if (z.align)
            j["alignment"] = z.align;
        if (z.crc_mismatch.empty())
            j["integrity"] = "not-checked";
        else
            j["integrity"] = {{"status", "mismatch"}, {"at", z.crc_mismatch}};
        auto& units = j["units"] = nlohmann::ordered_json::array();
        for (std::size_t i = 0; i < z.entries.size(); ++i)
            units.push_back(
                {{"at", container::entry_address(z.entries[i].path)},
                 {"value", method_name(z.recs[i].method) + " " + std::to_string(z.recs[i].usize)}});
        auto& warn = j["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : z.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return j;
    }

private:
    static Hash128 prefix_hash(const ZipDoc& z) { return hash128(z.bytes(0, z.prefix)); }
};

// Archive properties merge as opaque leaves: the comment and the prefix data (SFX stub, CRX header).
struct Property {
    const char* address;
    std::array<std::string, 3> values; // base, ours, theirs
    std::optional<Side> take;
};

MergeResult ZipFormat::merge(const Document& base_d, const Document& ours_d, const Document& theirs_d,
                             const MergeOptions& mo) const {
    const ZipDoc& b = doc(base_d);
    const ZipDoc& o = doc(ours_d);
    const ZipDoc& t = doc(theirs_d);
    const std::array<const ZipDoc*, 3> docs{&b, &o, &t};

    std::array<Property, 2> props{
        Property{"/archive/comment", {b.comment(), o.comment(), t.comment()}, {}},
        Property{"/archive/prefix",
                 {str(b.bytes(0, b.prefix)), str(o.bytes(0, o.prefix)), str(t.bytes(0, t.prefix))},
                 {}}};
    std::vector<Conflict> prop_conflicts;
    std::vector<std::size_t> prop_of;
    for (std::size_t k = 0; k < props.size(); ++k) {
        auto& v = props[k].values;
        auto out =
            merge_leaf(&v[0], &v[1], &v[2], std::equal_to<>{},
                       [](const std::string*, const std::string&, const std::string&) { return false; });
        props[k].take = out.take;
        if (out.rule == MergeRule::conflict) {
            prop_conflicts.push_back(make_conflict({props[k].address}, {}, out.reason));
            prop_of.push_back(k);
        }
    }
    ConflictOptions pc = mo.conflict, rest = mo.conflict;
    pc.resolutions.clear();
    for (auto& c : prop_conflicts)
        if (auto f = mo.conflict.resolutions.find(c.id); f != mo.conflict.resolutions.end())
            pc.resolutions.insert(*f), rest.resolutions.erase(c.id);
    std::vector<Warning> prop_warnings;
    apply_conflict_policy(prop_conflicts, pc, prop_warnings);
    for (std::size_t k = 0; k < prop_conflicts.size(); ++k)
        props[prop_of[k]].take = take_for(prop_conflicts[k].resolved_as, true, true, true);

    MergeOptions mo2{mo.ctx, rest, mo.labels, mo.visual, mo.text};
    container::Merged m = container::merge(b, o, t, mo2);
    MergeResult& r = m.result;
    r.conflicts.insert(r.conflicts.end(), prop_conflicts.begin(), prop_conflicts.end());
    r.warnings.insert(r.warnings.end(), prop_warnings.begin(), prop_warnings.end());
    auto prop = [&](std::size_t k) { return props[k].take.value_or(Side::ours); };

    std::vector<std::byte> bytes;
    if (container::is_ours(m, o) && prop(0) == Side::ours && prop(1) == Side::ours) {
        bytes.assign(o.src.bytes().begin(), o.src.bytes().end()); // no-op shortcut: ours verbatim
    } else {
        // Profile: mimetype first (EPUB OCF and similar), when ours has it first.
        if (!o.entries.empty() && o.entries[0].path == "mimetype")
            std::stable_partition(m.entries.begin(), m.entries.end(), [&](const OutEntry& e) {
                return docs[static_cast<int>(e.body.side)]->entries[e.body.index].path == "mimetype";
            });
        // Signatures: any content change invalidates them (SPEC §12.5, Q-ZIP-5).
        const std::string policy = option_value(mo.ctx.format_options, "signatures", "auto");
        auto path_of = [&](const OutEntry& e) -> const std::string& {
            auto f = e.from.find("path");
            const Pick& p = f == e.from.end() ? e.body : f->second;
            return docs[static_cast<int>(p.side)]->entries[p.index].path;
        };
        bool keep_block = false;
        const bool is_signed = o.sigblock || std::any_of(m.entries.begin(), m.entries.end(), [&](auto& e) {
                                   return signature_member(path_of(e));
                               });
        if (is_signed && policy == "resign")
            throw Error(ErrorCategory::unsupported,
                        "--signatures=resign needs --sign-command (not implemented)");
        if (policy == "keep") {
            keep_block = o.sigblock.has_value();
            for (auto& e : m.entries)
                if (signature_member(path_of(e)))
                    r.warnings.push_back({"signature-invalidated", container::entry_address(path_of(e)),
                                          "kept; the result fails verification until re-signed"});
            if (keep_block)
                r.warnings.push_back({"signature-invalidated", "/archive/apk-signing-block",
                                      "kept; the result fails verification until re-signed"});
        } else {
            std::erase_if(m.entries, [&](const OutEntry& e) {
                if (!signature_member(path_of(e)))
                    return false;
                r.warnings.push_back({"signature-stripped", container::entry_address(path_of(e)),
                                      "signature removed (--signatures=" + policy + ")"});
                return true;
            });
            if (o.sigblock)
                r.warnings.push_back({"signature-stripped", "/archive/apk-signing-block",
                                      "APK Signing Block removed (--signatures=" + policy + ")"});
            // D-50: digests of changed or added members leave MANIFEST.MF (no crypto in v1).
            std::map<std::string, std::size_t> in_ours;
            for (std::size_t k = 0; k < o.entries.size(); ++k)
                in_ours[o.entries[k].path] = k;
            std::set<std::string> touched;
            OutEntry* manifest = nullptr;
            for (auto& e : m.entries) {
                const std::string& p = path_of(e);
                if (p == "META-INF/MANIFEST.MF")
                    manifest = &e;
                auto f = in_ours.find(p);
                const ZipDoc& s = *docs[static_cast<int>(e.body.side)];
                if (f == in_ours.end() || e.content || !s.same(e.body.index, o, f->second, mo.ctx.budget))
                    touched.insert(p);
            }
            if (manifest && !touched.empty()) {
                const ZipDoc& s = *docs[static_cast<int>(manifest->body.side)];
                auto c = manifest->content ? Bytes(*manifest->content)
                                           : s.content(manifest->body.index, mo.ctx.budget);
                std::vector<std::string> dropped;
                if (auto mf = drop_digests(str(c), touched, dropped)) {
                    manifest->content = std::vector<std::byte>(as_bytes(*mf).begin(), as_bytes(*mf).end());
                    for (auto& name : dropped)
                        r.warnings.push_back({"manifest-digest-removed", container::entry_address(name),
                                              "MANIFEST.MF digest removed (D-50: no crypto)"});
                }
            }
        }

        const ZipDoc& pd = *docs[static_cast<int>(prop(1))];
        Bytes prefix = pd.bytes(0, pd.prefix);
        Writer w{docs, o, mo.ctx.budget, r.warnings, o.shift ? prefix.size() : 0, {}, {}, 0};
        append(w.out, prefix);
        for (auto& e : m.entries)
            w.entry(e);
        w.finish(keep_block ? std::optional(o.bytes(*o.sigblock, o.cd_start - *o.sigblock)) : std::nullopt,
                 as_bytes(props[0].values[static_cast<int>(prop(0))]));
        bytes = std::move(w.out);
    }
    // Validation (containers.md §8): the result is read back by the same reader.
    Limits budget = mo.ctx.budget.fresh(); // the result is a new document (#51)
    r.document =
        load_doc(ByteSource::from_memory(std::move(bytes), o.src.name()), budget, mo.ctx.format_options);
    return std::move(r);
}

} // namespace

std::vector<std::byte> build(std::span<const NewEntry> entries, std::string_view comment) {
    std::vector<std::byte> out, cd;
    for (auto& e : entries) {
        std::vector<std::byte> packed = e.method == 0   ? e.content
                                        : e.method == 8 ? container::deflate(e.content, 6)
                                                        : container::zstd(e.content, 3, false);
        const std::uint32_t crc = crc32(e.content);
        const bool utf8 = std::any_of(e.path.begin(), e.path.end(), [](char c) { return c & 0x80; });
        const std::uint64_t at = out.size();
        for (auto* v : {&out, &cd}) {
            put(*v, v == &out ? kLocal : kCentral, 4);
            if (v == &cd)
                put(*v, 0x0314, 2); // made by: Unix, 2.0
            put(*v, e.method == 93 ? 63 : 20, 2);
            put(*v, utf8 ? 0x800 : 0, 2);
            put(*v, e.method, 2);
            put(*v, e.dos_time, 2), put(*v, e.dos_date, 2);
            put(*v, crc, 4), put(*v, packed.size(), 4), put(*v, e.content.size(), 4);
            put(*v, e.path.size(), 2), put(*v, 0, 2);
            if (v == &cd) {
                put(*v, e.comment.size(), 2), put(*v, 0, 2), put(*v, 0, 2);
                put(*v, std::uint64_t{e.unix_mode} << 16, 4);
                put(*v, at, 4);
            }
            append(*v, as_bytes(e.path));
        }
        append(out, packed);
        append(cd, as_bytes(e.comment));
    }
    const std::uint64_t cd_at = out.size();
    append(out, cd);
    put(out, kEnd, 4), put(out, 0, 4);
    put(out, entries.size(), 2), put(out, entries.size(), 2);
    put(out, cd.size(), 4), put(out, cd_at, 4);
    put(out, comment.size(), 2);
    append(out, as_bytes(comment));
    return out;
}

std::unique_ptr<Format> make_format() {
    return std::make_unique<ZipFormat>();
}

} // namespace semsplinter::zip
