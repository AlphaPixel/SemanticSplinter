#include "internal.hpp"

#include <semsplinter/error.hpp>

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#ifdef _MSC_VER
#pragma warning(disable : 4611) // setjmp: no C++ objects live in decode_rows()
#endif

namespace semsplinter::png {

namespace {

std::uint32_t be32(const std::byte* p) {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 |
           std::uint32_t(p[3]);
}

[[noreturn]] void bad(const std::string& what) {
    throw Error(ErrorCategory::invalid_input, "PNG: " + what);
}

int channels(std::uint8_t ct) {
    return ct == 0 || ct == 3 ? 1 : ct == 2 ? 3 : ct == 4 ? 2 : 4;
}

std::uint64_t row_bytes(std::uint64_t w, const Encoding& e) {
    return (w * static_cast<std::uint64_t>(channels(e.color_type)) * e.bit_depth + 7) / 8;
}

struct Pass {
    std::uint32_t x0, y0, dx, dy;
};
constexpr Pass kAdam7[] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                           {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
constexpr Pass kWhole{0, 0, 1, 1};

std::uint32_t extent(std::uint32_t n, std::uint32_t o, std::uint32_t d) {
    return n > o ? (n - o + d - 1) / d : 0;
}

std::uint64_t filtered_size(const Encoding& e, std::uint32_t w, std::uint32_t h) {
    if (!e.interlace)
        return std::uint64_t{h} * (1 + row_bytes(w, e));
    std::uint64_t n = 0;
    for (auto& p : kAdam7) {
        auto pw = extent(w, p.x0, p.dx), ph = extent(h, p.y0, p.dy);
        if (pw && ph)
            n += ph * (1 + row_bytes(pw, e));
    }
    return n;
}

} // namespace

std::string Encoding::name() const {
    static const char* names[] = {"gray", "?", "rgb", "index", "graya", "?", "rgba"};
    return std::string(color_type < 7 ? names[color_type] : "?") + std::to_string(bit_depth);
}

std::string escape_text(std::string_view s, std::size_t max) {
    std::string out = "\"";
    std::size_t n = 0;
    for (char c : s) {
        if (n++ == max) {
            out += "...";
            break;
        }
        out += c == '"'    ? "\\\""
               : c == '\\' ? "\\\\"
               : c == '\n' ? "\\n"
               : c == '\t' ? "\\t"
               : c == '\r' ? "\\r"
                           : std::string(1, c);
    }
    return out + "\"";
}

// ---- Chunk layer ----

std::vector<Chunk> parse_chunks(std::span<const std::byte> f) {
    if (f.size() < 8 || std::memcmp(f.data(), kSignature.data(), 8) != 0)
        bad("not a PNG file (bad signature)");
    std::vector<Chunk> out;
    std::size_t pos = 8;
    bool idat_done = false;
    while (true) {
        if (f.size() - pos < 12)
            bad("truncated chunk at offset " + std::to_string(pos));
        Chunk c;
        c.offset = pos;
        c.length = be32(f.data() + pos);
        if (c.length > 0x7FFFFFFFu || c.length > f.size() - pos - 12)
            bad("chunk length out of bounds at offset " + std::to_string(pos));
        for (int k = 0; k < 4; ++k) {
            char ch = static_cast<char>(f[pos + 4 + k]);
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
                bad("invalid chunk type at offset " + std::to_string(pos));
            c.type += ch;
        }
        c.crc = be32(f.data() + c.end() - 4);
        c.crc_ok = crc32(f.subspan(pos + 8, c.length), crc32(f.subspan(pos + 4, 4))) == c.crc;
        if (c.type == "IDAT" && idat_done)
            bad("IDAT chunks are not consecutive");
        if (!out.empty() && out.back().type == "IDAT" && c.type != "IDAT")
            idat_done = true;
        out.push_back(c);
        pos = c.end();
        if (c.type == "IEND")
            break;
    }
    if (out[0].type != "IHDR" || out[0].length != 13)
        bad(out[0].type == "CgBI" ? "Apple CgBI PNG files are not supported"
                                  : "first chunk is not a 13-byte IHDR");
    if (std::none_of(out.begin(), out.end(), [](const Chunk& c) { return c.type == "IDAT"; }))
        bad("no IDAT chunk");
    return out;
}

std::vector<std::byte> PngDoc::idat_stream() const {
    std::vector<std::byte> s;
    for (auto& c : chunks)
        if (c.type == "IDAT") {
            auto d = data(c);
            s.insert(s.end(), d.begin(), d.end());
        }
    return s;
}

// ---- libpng decode ----

namespace {

struct ReadState {
    const std::byte* data;
    std::size_t size, pos;
    char message[256];
};

void read_fn(png_structp pp, png_bytep out, png_size_t n) {
    auto* st = static_cast<ReadState*>(png_get_io_ptr(pp));
    if (n > st->size - st->pos)
        png_error(pp, "unexpected end of data");
    std::memcpy(out, st->data + st->pos, n);
    st->pos += n;
}

void error_fn(png_structp pp, png_const_charp msg) {
    auto* st = static_cast<ReadState*>(png_get_error_ptr(pp));
    std::snprintf(st->message, sizeof st->message, "%s", msg);
    png_longjmp(pp, 1);
}

void warn_fn(png_structp, png_const_charp) {}

// Only C objects between setjmp and longjmp. Returns false with st->message set on error.
bool decode_rows(png_structp pp, png_infop info, png_bytep* rows, std::size_t row_size) {
    if (setjmp(png_jmpbuf(pp)))
        return false;
    png_set_read_fn(pp, png_get_io_ptr(pp), read_fn);
    png_set_crc_action(pp, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE); // our chunk layer reports CRCs
    png_set_benign_errors(pp, 1);
    png_set_user_limits(pp, 0x7FFFFFFF, 0x7FFFFFFF);
    png_read_info(pp, info);
    int ct = png_get_color_type(pp, info), bd = png_get_bit_depth(pp, info);
    bool trns = png_get_valid(pp, info, PNG_INFO_tRNS) != 0;
    if (ct == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(pp);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8)
        png_set_expand_gray_1_2_4_to_8(pp); // bit replication
    if (trns)
        png_set_tRNS_to_alpha(pp);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(pp);
    if (!(ct & PNG_COLOR_MASK_ALPHA) && !trns)
        png_set_add_alpha(pp, bd == 16 ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(pp);
    png_read_update_info(pp, info);
    if (png_get_rowbytes(pp, info) != row_size)
        png_error(pp, "unexpected row size after transforms");
    png_read_image(pp, rows);
    png_read_end(pp, nullptr);
    return true;
}

} // namespace

raster::Raster decode_pixels(const PngDoc& doc, Limits& budget) {
    const std::uint64_t cells = std::uint64_t{doc.width} * doc.height;
    if (cells > budget.max_pixels)
        throw Error(ErrorCategory::limit, "PNG: " + std::to_string(doc.width) + "x" +
                                              std::to_string(doc.height) + " exceeds --max-pixels");
    const bool wide = doc.enc.bit_depth == 16;
    budget.charge(cells * (wide ? 8 : 4));
    raster::Raster r(doc.width, doc.height, wide);

    // libpng sees only the critical chunks (plus tRNS): ancillary chunks are ours to interpret.
    std::vector<std::byte> mini(reinterpret_cast<const std::byte*>(kSignature.data()),
                                reinterpret_cast<const std::byte*>(kSignature.data()) + 8);
    for (auto& c : doc.chunks)
        if (c.type == "IHDR" || c.type == "PLTE" || c.type == "tRNS" || c.type == "IDAT" ||
            c.type == "IEND") {
            auto b = doc.bytes(c);
            mini.insert(mini.end(), b.begin(), b.end());
        }
    ReadState st{mini.data(), mini.size(), 8, {}};
    std::vector<png_bytep> rows(doc.height);
    const std::size_t row_size = std::size_t{doc.width} * r.cell_bytes();
    for (std::uint32_t y = 0; y < doc.height; ++y)
        rows[y] = r.data.data() + y * row_size;
    png_structp pp = png_create_read_struct(PNG_LIBPNG_VER_STRING, &st, error_fn, warn_fn);
    png_infop info = pp ? png_create_info_struct(pp) : nullptr;
    if (!info) {
        png_destroy_read_struct(&pp, nullptr, nullptr);
        throw Error(ErrorCategory::internal, "libpng initialization failed");
    }
    png_set_read_fn(pp, &st, read_fn);
    png_set_sig_bytes(pp, 8);
    bool ok = decode_rows(pp, info, rows.data(), row_size);
    png_destroy_read_struct(&pp, &info, nullptr);
    if (!ok)
        bad(std::string("pixel data: ") + st.message);
    if (wide && std::endian::native == std::endian::little)
        for (std::size_t i = 0; i + 1 < r.data.size(); i += 2)
            std::swap(r.data[i], r.data[i + 1]);
    return r;
}

// ---- zlib ----

std::vector<std::byte> inflate(std::span<const std::byte> in, std::uint64_t limit) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK)
        throw Error(ErrorCategory::internal, "zlib inflateInit failed");
    std::vector<std::byte> out;
    std::array<Bytef, 65536> buf;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = buf.data();
        zs.avail_out = static_cast<uInt>(buf.size());
        ret = ::inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            bad(ret == Z_BUF_ERROR ? "truncated zlib stream" : "corrupt zlib stream");
        }
        out.insert(out.end(), reinterpret_cast<std::byte*>(buf.data()),
                   reinterpret_cast<std::byte*>(buf.data()) + (buf.size() - zs.avail_out));
        if (out.size() > limit) {
            inflateEnd(&zs);
            throw Error(ErrorCategory::limit,
                        "PNG: zlib stream inflates past " + std::to_string(limit) + " bytes");
        }
    }
    inflateEnd(&zs);
    return out;
}

namespace {

// Compresses `in`; with `finish` false, returns only what deflate emitted before the end (a true prefix
// of the full stream).
std::vector<std::byte> run_deflate(std::span<const std::byte> in, const DeflateParams& p, bool finish) {
    z_stream zs{};
    if (deflateInit2(&zs, p.level, Z_DEFLATED, p.window_bits, p.mem_level, p.strategy) != Z_OK)
        throw Error(ErrorCategory::internal, "zlib deflateInit2 failed");
    std::vector<std::byte> out(deflateBound(&zs, static_cast<uLong>(in.size())) + 64);
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int ret = ::deflate(&zs, finish ? Z_FINISH : Z_NO_FLUSH);
    deflateEnd(&zs);
    if (finish ? ret != Z_STREAM_END : ret != Z_OK)
        throw Error(ErrorCategory::internal, "zlib deflate failed");
    out.resize(out.size() - zs.avail_out);
    return out;
}

} // namespace

std::vector<std::byte> deflate(std::span<const std::byte> in, const DeflateParams& p) {
    return run_deflate(in, p, true);
}

Source source_of(const PngDoc& doc) {
    Source s;
    s.doc = &doc;
    try {
        auto expected = filtered_size(doc.enc, doc.width, doc.height);
        s.filtered = inflate(doc.idat_stream(), expected);
        if (s.filtered.size() != expected) {
            s.filtered.clear();
            return s;
        }
        if (!doc.enc.interlace) {
            auto rb = row_bytes(doc.width, doc.enc) + 1;
            for (std::uint32_t y = 0; y < doc.height; ++y)
                s.filters.push_back(std::to_integer<std::uint8_t>(s.filtered[y * rb]));
            if (std::any_of(s.filters.begin(), s.filters.end(), [](std::uint8_t f) { return f > 4; }))
                s.filters.clear();
        }
    } catch (const Error&) {
        s.filtered.clear(), s.filters.clear();
    }
    return s;
}

DeflateParams infer_params(const Source& src) {
    DeflateParams fallback;
    if (!src.doc || src.filtered.empty())
        return fallback;
    auto stream = src.doc->idat_stream();
    if (stream.size() < 2)
        return fallback;
    const int cmf = std::to_integer<int>(stream[0]), flg = std::to_integer<int>(stream[1]);
    const int window = std::max((cmf >> 4) + 8, 9), flevel = flg >> 6;
    static const std::vector<int> levels[] = {{0, 1}, {2, 3, 4, 5}, {6}, {7, 8, 9}};
    // Feeds the source's filtered data until deflate has emitted at least 64 bytes (a true prefix of the
    // full stream), or to the end, and compares with the source's stream.
    auto reproduces = [&](const DeflateParams& p) {
        z_stream zs{};
        if (deflateInit2(&zs, p.level, Z_DEFLATED, p.window_bits, p.mem_level, p.strategy) != Z_OK)
            return false;
        std::vector<Bytef> out(1 << 16);
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        std::size_t fed = 0, produced = 0;
        bool finished = false;
        while (produced < 64 && !finished) {
            std::size_t n = std::min<std::size_t>(64 * 1024, src.filtered.size() - fed);
            const bool last = fed + n == src.filtered.size();
            zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src.filtered.data() + fed));
            zs.avail_in = static_cast<uInt>(n);
            fed += n;
            int ret = ::deflate(&zs, last ? Z_FINISH : Z_NO_FLUSH);
            produced = out.size() - zs.avail_out;
            finished = ret == Z_STREAM_END || (last && zs.avail_out == 0) || ret == Z_STREAM_ERROR;
            if (zs.avail_out == 0)
                break;
        }
        deflateEnd(&zs);
        if (produced > stream.size() || (finished && zs.avail_out != 0 && produced != stream.size()))
            return false;
        return std::equal(out.begin(), out.begin() + static_cast<long>(produced),
                          reinterpret_cast<const Bytef*>(stream.data()));
    };
    for (int level : levels[flevel])
        for (int strategy : {Z_FILTERED, Z_DEFAULT_STRATEGY, Z_RLE})
            for (int mem : {8, 9}) {
                DeflateParams p{level, window, mem, strategy, true};
                if (reproduces(p))
                    return p;
            }
    return fallback;
}

// ---- Encoder ----

namespace {

// Smallest bit depth whose replication pattern gives the 16-bit value v.
int depth_of(std::uint16_t v) {
    if (v % 257)
        return 16;
    unsigned v8 = v / 257;
    if (v8 % 17)
        return 8;
    unsigned v4 = v8 / 17;
    if (v4 % 5)
        return 4;
    return v4 / 5 % 3 ? 2 : 1;
}

struct Analysis {
    bool gray = true, opaque = true;
    int color_depth = 1, alpha_depth = 1;
    bool key_ok = true;              // a tRNS key color can express the alpha channel
    std::optional<raster::Cell> key; // the color of the transparent cells
};

Analysis analyze(const raster::Raster& r) {
    Analysis a;
    for (std::uint64_t i = 0; i < r.cells(); ++i) {
        auto c = r.get(i);
        a.gray &= c[0] == c[1] && c[1] == c[2];
        if (a.color_depth < 16)
            a.color_depth = std::max({a.color_depth, depth_of(c[0]), depth_of(c[1]), depth_of(c[2])});
        if (c[3] != 0xFFFF) {
            a.opaque = false;
            a.alpha_depth = std::max(a.alpha_depth, depth_of(c[3]));
            if (c[3] != 0)
                a.key_ok = false;
            else if (!a.key)
                a.key = c;
            else if (c[0] != (*a.key)[0] || c[1] != (*a.key)[1] || c[2] != (*a.key)[2])
                a.key_ok = false;
        }
    }
    if (a.key_ok && a.key)
        for (std::uint64_t i = 0; i < r.cells() && a.key_ok; ++i) {
            auto c = r.get(i);
            a.key_ok = !(c[3] == 0xFFFF && c[0] == (*a.key)[0] && c[1] == (*a.key)[1] && c[2] == (*a.key)[2]);
        }
    return a;
}

std::uint32_t rgba8(const raster::Cell& c) {
    return std::uint32_t(c[0] >> 8) << 24 | std::uint32_t(c[1] >> 8) << 16 | std::uint32_t(c[2] >> 8) << 8 |
           (c[3] >> 8);
}

std::uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

template <class Pred>
void filter_with(const std::uint8_t* cur, const std::uint8_t* prev, std::size_t n, std::size_t bpp,
                 std::uint8_t* out, Pred pred) {
    std::size_t i = 0;
    for (; i < std::min(bpp, n); ++i)
        out[i] = static_cast<std::uint8_t>(cur[i] - pred(0, prev[i], 0));
    for (; i < n; ++i)
        out[i] = static_cast<std::uint8_t>(cur[i] - pred(cur[i - bpp], prev[i], prev[i - bpp]));
}

void filter_row(int type, const std::uint8_t* cur, const std::uint8_t* prev, std::size_t n, std::size_t bpp,
                std::uint8_t* out) {
    switch (type) {
    case 1:
        return filter_with(cur, prev, n, bpp, out, [](int a, int, int) { return a; });
    case 2:
        return filter_with(cur, prev, n, bpp, out, [](int, int b, int) { return b; });
    case 3:
        return filter_with(cur, prev, n, bpp, out, [](int a, int b, int) { return (a + b) / 2; });
    case 4:
        return filter_with(cur, prev, n, bpp, out, [](int a, int b, int c) { return int{paeth(a, b, c)}; });
    default:
        std::copy(cur, cur + n, out);
    }
}

bool rows_equal(const raster::Raster& a, const raster::Raster& b, std::uint32_t y) {
    const std::uint64_t base = std::uint64_t{y} * a.width;
    if (a.wide == b.wide)
        return std::memcmp(a.at(base), b.at(base), std::size_t{a.width} * a.cell_bytes()) == 0;
    for (std::uint32_t x = 0; x < a.width; ++x)
        if (a.get(base + x) != b.get(base + x))
            return false;
    return true;
}

} // namespace

Image encode(const raster::Raster& r, Encoding want, const Source* src, std::vector<Warning>& warnings,
             const DeflateParams* params) {
    const Analysis an = analyze(r);
    Encoding e = want;
    std::vector<Rgba8> palette;
    std::unordered_map<std::uint32_t, std::uint8_t> index;
    std::optional<raster::Cell> key;

    if (e.color_type == 3) {
        bool fits = an.color_depth <= 8 && an.alpha_depth <= 8;
        if (fits && src && src->doc->enc.color_type == 3)
            palette = src->doc->palette; // keep the source's order, drop nothing
        for (std::size_t k = 0; k < palette.size(); ++k) {
            auto& p = palette[k];
            index.emplace(std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 |
                              p[3],
                          static_cast<std::uint8_t>(k));
        }
        for (std::uint64_t i = 0; fits && i < r.cells(); ++i) {
            auto c = r.get(i);
            auto v = rgba8(c);
            if (index.count(v))
                continue;
            if (palette.size() == 256) {
                fits = false;
                break;
            }
            index.emplace(v, static_cast<std::uint8_t>(palette.size()));
            palette.push_back({static_cast<std::uint8_t>(c[0] >> 8), static_cast<std::uint8_t>(c[1] >> 8),
                               static_cast<std::uint8_t>(c[2] >> 8), static_cast<std::uint8_t>(c[3] >> 8)});
        }
        if (fits) {
            int need = palette.size() <= 2 ? 1 : palette.size() <= 4 ? 2 : palette.size() <= 16 ? 4 : 8;
            e.bit_depth = static_cast<std::uint8_t>(std::max<int>(e.bit_depth, need));
        } else {
            palette.clear(), index.clear();
            e.color_type = an.opaque ? 2 : 6;
            e.bit_depth = 8;
        }
    }
    if (e.color_type != 3) {
        if (!an.gray && (e.color_type == 0 || e.color_type == 4))
            e.color_type += 2;
        if (!an.opaque && (e.color_type == 0 || e.color_type == 2)) {
            if (an.key_ok && an.key)
                key = an.key;
            else
                e.color_type += 4;
        }
        const bool alpha = e.color_type & 4;
        int need = std::max(an.color_depth, alpha ? an.alpha_depth : 1);
        if (e.color_type == 0)
            e.bit_depth = static_cast<std::uint8_t>(std::max<int>(e.bit_depth, need));
        else
            e.bit_depth = static_cast<std::uint8_t>(need > 8 || e.bit_depth == 16 ? 16 : 8);
    }
    if (!(e == want))
        warnings.push_back({"encoding-promoted", "/encoding", want.name() + " -> " + e.name()});

    Image img;
    img.enc = e;
    auto put32 = [](std::vector<std::byte>& v, std::uint32_t x) {
        for (int s = 24; s >= 0; s -= 8)
            v.push_back(std::byte((x >> s) & 0xFF));
    };
    put32(img.ihdr, r.width), put32(img.ihdr, r.height);
    for (std::uint8_t b : {e.bit_depth, e.color_type, std::uint8_t{0}, std::uint8_t{0}, e.interlace})
        img.ihdr.push_back(std::byte(b));
    if (e.color_type == 3) {
        std::size_t last_alpha = 0;
        for (std::size_t k = 0; k < palette.size(); ++k) {
            for (int j = 0; j < 3; ++j)
                img.plte.push_back(std::byte(palette[k][j]));
            if (palette[k][3] != 255)
                last_alpha = k + 1;
        }
        for (std::size_t k = 0; k < last_alpha; ++k)
            img.trns.push_back(std::byte(palette[k][3]));
        img.hist.assign(palette.size(), 0);
    } else if (key) {
        int n = e.color_type == 0 ? 1 : 3;
        for (int j = 0; j < n; ++j) {
            auto s = static_cast<std::uint16_t>((*key)[j] >> (16 - e.bit_depth));
            img.trns.push_back(std::byte(s >> 8)), img.trns.push_back(std::byte(s & 0xFF));
        }
    }

    // Scanlines, then filters, per pass.
    const int ch = channels(e.color_type), d = e.bit_depth;
    const std::size_t bpp = std::max(1, ch * d / 8);
    std::vector<std::uint64_t> counts(palette.size());
    std::vector<std::byte> filtered;
    filtered.reserve(filtered_size(e, r.width, r.height));
    const bool reuse = src && !e.interlace && src->doc->enc == e && src->doc->width == r.width &&
                       src->doc->height == r.height && src->filters.size() == r.height;
    const bool none_only = e.color_type == 3 || d < 8;
    for (const Pass& p : e.interlace ? std::span<const Pass>(kAdam7) : std::span<const Pass>(&kWhole, 1)) {
        auto pw = extent(r.width, p.x0, p.dx), ph = extent(r.height, p.y0, p.dy);
        if (!pw || !ph)
            continue;
        const std::size_t rb = row_bytes(pw, e);
        std::vector<std::uint8_t> prev(rb, 0), cur(rb), best(rb), trial(rb);
        for (std::uint32_t py = 0; py < ph; ++py) {
            std::fill(cur.begin(), cur.end(), std::uint8_t{0});
            const std::uint32_t y = p.y0 + py * p.dy;
            for (std::uint32_t px = 0; px < pw; ++px) {
                auto c = r.get(std::uint64_t{y} * r.width + p.x0 + px * p.dx);
                std::uint16_t s[4];
                int n = 0;
                if (e.color_type == 3) {
                    auto k = index.at(rgba8(c));
                    ++counts[k];
                    s[n++] = k;
                } else {
                    s[n++] = c[0];
                    if (e.color_type & 2)
                        s[n++] = c[1], s[n++] = c[2];
                    if (e.color_type & 4)
                        s[n++] = c[3];
                    for (int j = 0; j < n; ++j)
                        s[j] = static_cast<std::uint16_t>(s[j] >> (16 - d));
                }
                for (int j = 0; j < n; ++j) {
                    std::uint64_t bit = (std::uint64_t{px} * ch + j) * d;
                    if (d == 16)
                        cur[bit / 8] = static_cast<std::uint8_t>(s[j] >> 8),
                                  cur[bit / 8 + 1] = static_cast<std::uint8_t>(s[j]);
                    else
                        cur[bit / 8] |= static_cast<std::uint8_t>(s[j] << (8 - d - bit % 8));
                }
            }
            int type = 0;
            if (reuse && rows_equal(r, src->doc->pixels, y) &&
                (y == 0 || rows_equal(r, src->doc->pixels, y - 1))) {
                type = src->filters[y];
            } else if (!none_only) {
                std::uint64_t best_sum = UINT64_MAX;
                for (int t = 0; t < 5; ++t) {
                    filter_row(t, cur.data(), prev.data(), rb, bpp, trial.data());
                    std::uint64_t sum = 0;
                    for (auto v : trial)
                        sum += static_cast<std::uint64_t>(
                            std::abs(static_cast<int>(static_cast<std::int8_t>(v))));
                    if (sum < best_sum)
                        best_sum = sum, type = t;
                }
            }
            filter_row(type, cur.data(), prev.data(), rb, bpp, best.data());
            filtered.push_back(std::byte(type));
            filtered.insert(filtered.end(), reinterpret_cast<std::byte*>(best.data()),
                            reinterpret_cast<std::byte*>(best.data()) + rb);
            std::swap(prev, cur);
        }
    }
    if (!counts.empty()) {
        auto max = *std::max_element(counts.begin(), counts.end());
        for (std::size_t k = 0; k < counts.size(); ++k)
            img.hist[k] = static_cast<std::uint16_t>(
                counts[k] ? std::max<std::uint64_t>(1, max ? counts[k] * 65535 / max : 0) : 0);
    }
    img.params = params ? *params : src ? infer_params(*src) : DeflateParams{};
    img.stream = deflate(filtered, img.params);
    return img;
}

} // namespace semsplinter::png
