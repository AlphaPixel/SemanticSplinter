#include <semsplinter/error.hpp>
#include <semsplinter/formats/ifd.hpp>

#include <algorithm>
#include <set>

namespace semsplinter::ifd {

namespace {

[[noreturn]] void bad(const std::string& what) {
    throw Error(ErrorCategory::invalid_input, "TIFF: " + what);
}

struct Parser {
    std::span<const std::byte> b;
    Tiff& t;
    std::size_t max_ifds;
    std::set<std::uint64_t> seen;

    std::uint64_t get(std::uint64_t at, unsigned width) const {
        if (at > b.size() || b.size() - at < width)
            bad("read past the end of the block");
        return t.get(b.subspan(at, width), width);
    }

    Ifd ifd(std::uint64_t at, int depth) {
        if (depth > 8)
            bad("IFDs nested too deeply");
        if (!seen.insert(at).second)
            bad("IFD cycle at offset " + std::to_string(at));
        if (seen.size() > max_ifds)
            bad("more than " + std::to_string(max_ifds) + " IFDs");
        const unsigned cw = t.bigtiff ? 8 : 2, ew = t.bigtiff ? 20 : 12, ow = t.offset_size();
        Ifd d;
        d.offset = at;
        const std::uint64_t n = get(at, cw);
        if (n > (b.size() - at - cw) / ew)
            bad("IFD at " + std::to_string(at) + " runs past the end");
        d.size = cw + n * ew + ow;
        for (std::uint64_t k = 0; k < n; ++k) {
            const std::uint64_t e = at + cw + k * ew;
            Entry x;
            x.tag = static_cast<std::uint16_t>(get(e, 2));
            x.type = static_cast<std::uint16_t>(get(e + 2, 2));
            x.count = get(e + 4, t.bigtiff ? 8 : 4);
            const std::uint64_t vat = e + 4 + (t.bigtiff ? 8 : 4);
            const unsigned sz = type_size(x.type);
            if (sz) {
                if (x.count > b.size() / sz)
                    bad("tag " + std::to_string(x.tag) + " count too large");
                const std::uint64_t len = x.count * sz;
                x.in_line = len <= ow;
                x.at = x.in_line ? vat : get(vat, ow);
                if (x.at > b.size() || b.size() - x.at < len)
                    bad("tag " + std::to_string(x.tag) + " value outside the block");
                x.value = b.subspan(x.at, len);
            } else {
                x.at = vat; // unknown type: only the entry's own value field is known
            }
            d.entries.push_back(std::move(x));
        }
        d.next = get(at + cw + n * ew, ow);
        for (auto& x : d.entries)
            if (is_pointer_tag(x.tag) && type_size(x.type) && type_size(x.type) >= 4)
                for (std::size_t i = 0; i < x.count; ++i) {
                    auto off = t.number(x, i);
                    if (off)
                        x.sub.push_back(ifd(off, depth + 1));
                }
        return d;
    }
};

} // namespace

unsigned type_size(std::uint16_t type) noexcept {
    switch (type) {
    case BYTE:
    case ASCII:
    case SBYTE:
    case UNDEFINED:
        return 1;
    case SHORT:
    case SSHORT:
        return 2;
    case LONG:
    case SLONG:
    case FLOAT:
    case IFD:
        return 4;
    case RATIONAL:
    case SRATIONAL:
    case DOUBLE:
    case LONG8:
    case SLONG8:
    case IFD8:
        return 8;
    default:
        return 0;
    }
}

bool is_pointer_tag(std::uint16_t tag) noexcept {
    return tag == 330 || tag == 34665 || tag == 34853 || tag == 40965;
}

const Entry* Ifd::find(std::uint16_t tag) const noexcept {
    for (auto& e : entries)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

std::uint64_t Tiff::get(std::span<const std::byte> d, unsigned width) const noexcept {
    std::uint64_t v = 0;
    for (unsigned k = 0; k < width; ++k)
        v |= std::uint64_t(std::to_integer<unsigned>(d[big_endian ? width - 1 - k : k])) << (8 * k);
    return v;
}

std::uint64_t Tiff::number(const Entry& e, std::size_t i) const {
    unsigned sz = type_size(e.type);
    if (e.type == RATIONAL || e.type == SRATIONAL)
        sz = 4; // i-th 32-bit word: numerator at 2k, denominator at 2k + 1
    if (!sz || e.type == FLOAT || e.type == DOUBLE || (i + 1) * sz > e.value.size())
        bad("tag " + std::to_string(e.tag) + " is not an integer list of that length");
    return get(e.value.subspan(i * sz, sz), sz);
}

std::vector<std::uint64_t> Tiff::numbers(const Entry& e) const {
    std::vector<std::uint64_t> v;
    for (std::size_t i = 0; i < e.count * (e.type == RATIONAL || e.type == SRATIONAL ? 2 : 1); ++i)
        v.push_back(number(e, i));
    return v;
}

Tiff parse(std::span<const std::byte> block, std::size_t max_ifds) {
    Tiff t;
    if (block.size() < 8)
        bad("too short for a header");
    const auto b0 = std::to_integer<char>(block[0]), b1 = std::to_integer<char>(block[1]);
    if (b0 != b1 || (b0 != 'I' && b0 != 'M'))
        bad("bad byte-order mark");
    t.big_endian = b0 == 'M';
    const auto magic = t.get(block.subspan(2, 2), 2);
    if (magic == 43) {
        if (block.size() < 16 || t.get(block.subspan(4, 2), 2) != 8 || t.get(block.subspan(6, 2), 2) != 0)
            bad("bad BigTIFF header");
        t.bigtiff = true;
        t.first = t.get(block.subspan(8, 8), 8);
    } else if (magic == 42) {
        t.first = t.get(block.subspan(4, 4), 4);
    } else {
        bad("bad magic number");
    }
    Parser p{block, t, max_ifds, {}};
    for (std::uint64_t at = t.first; at;) {
        t.chain.push_back(p.ifd(at, 0));
        at = t.chain.back().next;
    }
    if (t.chain.empty())
        bad("no IFD");
    return t;
}

std::string text(const Entry& e) {
    std::string s(reinterpret_cast<const char*>(e.value.data()), e.value.size());
    return s.substr(0, s.find('\0'));
}

emit::Expr Writer::offset(emit::GroupId g) const {
    return relative_to ? emit::offset_of(g, *relative_to) : emit::offset_of(g);
}

emit::Seg Writer::number(std::uint64_t v, unsigned width) const {
    return emit::field(width, endian(), emit::constant(v));
}

emit::Seg Writer::pointer(emit::GroupId g) const {
    return emit::field(bigtiff ? 8 : 4, endian(), offset(g));
}

emit::Seg Writer::pointer(std::uint64_t at) const {
    return number(at, bigtiff ? 8 : 4);
}

OutEntry Writer::copy(const Entry& e) const {
    return {e.tag, e.type, e.count, {emit::bytes(e.value)}, std::nullopt};
}

OutEntry Writer::keep(const Entry& e) const {
    auto o = copy(e);
    if (!e.in_line)
        o.kept = e.at;
    return o;
}

OutEntry Writer::numbers(std::uint16_t tag, std::uint16_t type, const std::vector<std::uint64_t>& v) const {
    const bool rational = type == RATIONAL || type == SRATIONAL; // v holds numerator, denominator pairs
    OutEntry o{tag, type, rational ? v.size() / 2 : v.size(), {}, std::nullopt};
    for (auto x : v)
        o.value.push_back(number(x, rational ? 4 : type_size(type)));
    return o;
}

emit::Seg Writer::write(emit::GroupId gid, std::vector<OutEntry> entries, emit::Seg next) const {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const OutEntry& a, const OutEntry& b) { return a.tag < b.tag; });
    const unsigned ow = bigtiff ? 8 : 4;
    std::vector<emit::Seg> head, tail;
    head.push_back(number(entries.size(), bigtiff ? 8 : 2));
    for (auto& e : entries) {
        head.push_back(number(e.tag, 2));
        head.push_back(number(e.type, 2));
        head.push_back(number(e.count, ow));
        const std::uint64_t len = e.count * type_size(e.type);
        if (len <= ow) {
            for (auto& s : e.value)
                head.push_back(std::move(s));
            if (len < ow)
                head.push_back(emit::bytes(std::vector<std::byte>(ow - len)));
        } else if (e.kept) {
            head.push_back(pointer(*e.kept));
        } else {
            auto v = id();
            head.push_back(pointer(v));
            tail.push_back(emit::align(2));
            tail.push_back(emit::group(v, std::move(e.value)));
        }
    }
    head.push_back(std::move(next));
    std::vector<emit::Seg> all{emit::align(2), emit::group(gid, std::move(head))};
    for (auto& s : tail)
        all.push_back(std::move(s));
    return emit::group(id(), std::move(all));
}

} // namespace semsplinter::ifd
