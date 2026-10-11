#include <semsplinter/emit.hpp>
#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>

#include <algorithm>
#include <map>
#include <string>

namespace semsplinter::emit {

Expr constant(std::uint64_t v) {
    return {Expr::Kind::constant, v};
}
Expr size_of(GroupId g) {
    return {Expr::Kind::size_of, 0, g};
}
Expr offset_of(GroupId g) {
    return {Expr::Kind::offset_of, 0, g};
}
Expr offset_of(GroupId g, GroupId rel) {
    return {Expr::Kind::offset_of, 0, g, rel, true};
}
Expr crc32(GroupId g) {
    return {Expr::Kind::crc32, 0, g};
}
Expr adler32(GroupId g) {
    return {Expr::Kind::adler32, 0, g};
}

Seg bytes(std::span<const std::byte> b) {
    Seg s;
    s.data.assign(b.begin(), b.end());
    return s;
}
Seg bytes(std::string_view s) {
    return bytes(as_bytes(s));
}

Seg raw(const ByteSource& src, std::uint64_t offset, std::uint64_t length) {
    Seg s;
    s.kind = Seg::Kind::raw;
    s.source = src.slice(offset, length); // bounds-checked now
    s.length = length;
    return s;
}

Seg group(GroupId id, std::vector<Seg> children) {
    Seg s;
    s.kind = Seg::Kind::group;
    s.id = id;
    s.children = std::move(children);
    return s;
}

Seg align(unsigned boundary, std::byte fill) {
    Seg s;
    s.kind = Seg::Kind::align;
    s.boundary = boundary ? boundary : 1;
    s.fill = fill;
    return s;
}

Seg field(unsigned width, Endian e, Expr x) {
    if (width != 1 && width != 2 && width != 4 && width != 8)
        throw Error(ErrorCategory::internal, "emit: field width must be 1, 2, 4, or 8");
    Seg s;
    s.kind = Seg::Kind::field;
    s.width = width;
    s.endian = e;
    s.expr = x;
    return s;
}

Seg varfield(VarEncoding enc, Expr x) {
    if (x.kind == Expr::Kind::crc32 || x.kind == Expr::Kind::adler32)
        throw Error(ErrorCategory::internal, "emit: checksums need a fixed-width field");
    Seg s;
    s.kind = Seg::Kind::varfield;
    s.encoding = enc;
    s.width = 1;
    s.expr = x;
    return s;
}

namespace {

[[noreturn]] void fail(const std::string& m) {
    throw Error(ErrorCategory::internal, "emit: " + m);
}

bool is_checksum(const Expr& x) {
    return x.kind == Expr::Kind::crc32 || x.kind == Expr::Kind::adler32;
}

std::string decimal(std::uint64_t v, unsigned width) {
    auto s = std::to_string(v);
    return s.size() < width ? std::string(width - s.size(), '0') + s : s;
}

unsigned uleb_width(std::uint64_t v) {
    unsigned n = 1;
    while (v >>= 7)
        ++n;
    return n;
}

struct Span {
    std::uint64_t begin = 0, end = 0;
};

struct Layout {
    std::map<const Seg*, unsigned> width; // current width of each varfield
    std::map<GroupId, Span> groups;
    std::map<const Seg*, std::uint64_t> at; // offset of each field/varfield
    std::uint64_t size = 0;

    void collect(const Seg& s) {
        if (s.kind == Seg::Kind::varfield)
            width.emplace(&s, std::max(1u, s.width)); // a preset width is the zero-padded minimum
        if (s.kind == Seg::Kind::group && !groups.emplace(s.id, Span{}).second)
            fail("duplicate group " + std::to_string(s.id));
        for (auto& c : s.children)
            collect(c);
    }

    std::uint64_t place(const Seg& s, std::uint64_t pos) {
        switch (s.kind) {
        case Seg::Kind::bytes:
            return pos + s.data.size();
        case Seg::Kind::raw:
            return pos + s.length;
        case Seg::Kind::align:
            return pos + (s.boundary - pos % s.boundary) % s.boundary;
        case Seg::Kind::field:
            at[&s] = pos;
            return pos + s.width;
        case Seg::Kind::varfield:
            at[&s] = pos;
            return pos + width[&s];
        case Seg::Kind::group: {
            auto start = pos;
            for (auto& c : s.children)
                pos = place(c, pos);
            groups[s.id] = {start, pos};
            return pos;
        }
        }
        return pos;
    }

    const Span& group(GroupId g) const {
        auto f = groups.find(g);
        if (f == groups.end())
            fail("unknown group " + std::to_string(g));
        return f->second;
    }

    std::uint64_t value(const Expr& x) const {
        switch (x.kind) {
        case Expr::Kind::constant:
            return x.value;
        case Expr::Kind::size_of:
            return group(x.group).end - group(x.group).begin;
        case Expr::Kind::offset_of: {
            auto off = group(x.group).begin, rel = x.relative ? group(x.relative_to).begin : 0;
            if (off < rel)
                fail("negative relative offset");
            return off - rel;
        }
        default:
            fail("checksum evaluated as a layout value");
        }
    }
};

void materialize(const Seg& s, std::vector<std::byte>& out, const Layout& L) {
    switch (s.kind) {
    case Seg::Kind::bytes:
        out.insert(out.end(), s.data.begin(), s.data.end());
        break;
    case Seg::Kind::raw: {
        auto b = s.source.bytes();
        out.insert(out.end(), b.begin(), b.end());
        break;
    }
    case Seg::Kind::align:
        out.resize(out.size() + (s.boundary - out.size() % s.boundary) % s.boundary, s.fill);
        break;
    case Seg::Kind::field:
        out.resize(out.size() + s.width);
        break;
    case Seg::Kind::varfield:
        out.resize(out.size() + L.width.at(&s));
        break;
    case Seg::Kind::group:
        for (auto& c : s.children)
            materialize(c, out, L);
        break;
    }
}

void fields(const Seg& s, std::vector<const Seg*>& out) {
    if (s.kind == Seg::Kind::field || s.kind == Seg::Kind::varfield)
        out.push_back(&s);
    for (auto& c : s.children)
        fields(c, out);
}

} // namespace

std::vector<std::byte> emit(const Seg& root) {
    Layout L;
    L.collect(root);
    bool stable = false;
    for (int iter = 0; iter < 16 && !stable; ++iter) {
        L.size = L.place(root, 0);
        stable = true;
        for (auto& [seg, w] : L.width) {
            auto v = L.value(seg->expr);
            unsigned need = seg->encoding == VarEncoding::decimal
                                ? static_cast<unsigned>(std::to_string(v).size())
                                : uleb_width(v);
            if (need > w)
                w = need, stable = false;
        }
    }
    if (!stable)
        fail("layout did not reach a fixed point in 16 iterations");

    // Materialize with fields zeroed; values are written below.
    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(L.size));
    materialize(root, out, L);
    std::vector<const Seg*> fs;
    fields(root, fs);
    if (out.size() != L.size)
        fail("internal layout mismatch");

    auto write = [&](const Seg* f, std::uint64_t v) {
        auto at = static_cast<std::size_t>(L.at[f]);
        if (f->kind == Seg::Kind::varfield) {
            unsigned w = L.width[f];
            if (f->encoding == VarEncoding::decimal) {
                auto s = decimal(v, w);
                for (unsigned i = 0; i < w; ++i)
                    out[at + i] = static_cast<std::byte>(s[i]);
            } else {
                for (unsigned i = 0; i < w; ++i, v >>= 7) // padded with continuation bytes
                    out[at + i] = static_cast<std::byte>((v & 0x7F) | (i + 1 < w ? 0x80 : 0));
            }
            return;
        }
        if (f->width < 8 && v >> (8 * f->width))
            fail("value " + std::to_string(v) + " overflows a " + std::to_string(f->width) + "-byte field");
        for (unsigned i = 0; i < f->width; ++i) {
            unsigned shift = 8 * (f->endian == Endian::little ? i : f->width - 1 - i);
            out[at + i] = static_cast<std::byte>(v >> shift);
        }
    };

    std::vector<const Seg*> sums;
    for (auto* f : fs) {
        if (is_checksum(f->expr))
            sums.push_back(f);
        else
            write(f, L.value(f->expr));
    }
    // Checksums innermost first: one covering another's position waits for it.
    std::vector<bool> done(sums.size());
    for (std::size_t pass = 0; pass < sums.size(); ++pass) {
        bool progress = false;
        for (std::size_t i = 0; i < sums.size(); ++i) {
            if (done[i])
                continue;
            auto& g = L.group(sums[i]->expr.group);
            bool ready = true;
            for (std::size_t j = 0; j < sums.size() && ready; ++j) {
                auto p = L.at[sums[j]];
                ready = done[j] || j == i || p + sums[j]->width <= g.begin || p >= g.end;
            }
            if (!ready)
                continue;
            std::span<const std::byte> data(out.data() + g.begin, static_cast<std::size_t>(g.end - g.begin));
            write(sums[i], sums[i]->expr.kind == Expr::Kind::crc32 ? semsplinter::crc32(data)
                                                                   : semsplinter::adler32(data));
            done[i] = progress = true;
        }
        if (!progress)
            break;
    }
    if (std::find(done.begin(), done.end(), false) != done.end())
        fail("checksums cover each other");
    return out;
}

} // namespace semsplinter::emit
