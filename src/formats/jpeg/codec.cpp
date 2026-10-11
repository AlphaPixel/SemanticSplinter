#include "internal.hpp"

#include <semsplinter/error.hpp>

// clang-format off
#include <cstdio> // before jpeglib.h, which uses FILE
#include <jpeglib.h>
// clang-format on

#include <array>
#include <csetjmp>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4611) // setjmp: guarded() runs only C calls and noexcept code under it
#pragma warning(disable : 4324) // Err is padded for jmp_buf's alignment
#endif

namespace semsplinter::jpeg {

namespace {

static_assert(sizeof(JCOEF) == sizeof(std::int16_t) && sizeof(JBLOCK) == 128);

// libjpeg-turbo reports errors through error_exit, which must not return: we longjmp back to guarded().
struct Err {
    jpeg_error_mgr pub; // first: libjpeg-turbo hands back a pointer to it
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
    char warning[JMSG_LENGTH_MAX];
    int warnings;
};

void on_error(j_common_ptr c) {
    auto* e = reinterpret_cast<Err*>(c->err);
    (*c->err->format_message)(c, e->message);
    std::longjmp(e->jump, 1);
}

void on_message(j_common_ptr c, int level) {
    auto* e = reinterpret_cast<Err*>(c->err);
    if (level < 0 && e->warnings++ == 0) // corrupt-data warnings; trace messages are ignored
        (*c->err->format_message)(c, e->warning);
}

jpeg_error_mgr* setup(Err& e) {
    auto* p = jpeg_std_error(&e.pub);
    p->error_exit = on_error;
    p->emit_message = on_message;
    e.message[0] = e.warning[0] = '\0';
    e.warnings = 0;
    return p;
}

// Runs `f` with a longjmp target for libjpeg-turbo errors. `f` makes only C calls and noexcept moves of
// preallocated memory, so the jump skips no destructor.
template <class F> bool guarded(Err& e, F&& f) {
    if (setjmp(e.jump))
        return false;
    f();
    return true;
}

void source(jpeg_decompress_struct& di, std::span<const std::byte> data) {
    jpeg_mem_src(&di, reinterpret_cast<const unsigned char*>(data.data()),
                 static_cast<unsigned long>(data.size()));
}

struct HuffTable {
    int cls = 0, slot = 0;
    std::array<UINT8, 17> bits{};
    std::array<UINT8, 256> values{};
};

// Ours' Huffman tables and per-component selectors, when one sequential Huffman scan covers every
// component and no slot is defined twice.
bool huffman_tables(const JpegDoc& d, std::vector<HuffTable>* out, std::vector<std::pair<int, int>>* sel) {
    if (d.progressive() || d.arithmetic())
        return false;
    std::array<bool, 8> seen{};
    const Segment* sos = nullptr;
    for (auto& s : d.segs) {
        auto p = d.bytes(s);
        if (s.marker == kSOS) {
            if (sos)
                return false;
            sos = &s;
        } else if (s.marker == kDHT) {
            for (std::size_t at = 0; at < p.size();) {
                if (p.size() - at < 17)
                    return false;
                HuffTable t;
                const auto tc_th = std::to_integer<int>(p[at]);
                t.cls = tc_th >> 4, t.slot = tc_th & 15;
                if (t.cls > 1 || t.slot > 3 || seen[std::size_t(t.cls * 4 + t.slot)])
                    return false;
                seen[std::size_t(t.cls * 4 + t.slot)] = true;
                std::size_t n = 0;
                for (int k = 1; k <= 16; ++k)
                    n += t.bits[std::size_t(k)] = std::to_integer<UINT8>(p[at + std::size_t(k)]);
                if (n > 256 || p.size() - at - 17 < n)
                    return false;
                for (std::size_t k = 0; k < n; ++k)
                    t.values[k] = std::to_integer<UINT8>(p[at + 17 + k]);
                if (out)
                    out->push_back(t);
                at += 17 + n;
            }
        }
    }
    if (!sos)
        return false;
    auto p = d.bytes(*sos);
    const std::size_t ns = p.empty() ? 0 : std::to_integer<std::size_t>(p[0]);
    if (ns != d.comps.size() || p.size() < 1 + 2 * ns + 3)
        return false;
    for (auto& c : d.comps) {
        bool found = false;
        for (std::size_t j = 0; j < ns; ++j)
            if (std::to_integer<int>(p[1 + 2 * j]) == c.id) {
                const auto t = std::to_integer<int>(p[2 + 2 * j]);
                if ((t >> 4) > 3 || (t & 15) > 3 || !seen[std::size_t(t >> 4)] ||
                    !seen[std::size_t(4 + (t & 15))])
                    return false;
                if (sel)
                    sel->emplace_back(t >> 4, t & 15);
                found = true;
            }
        if (!found)
            return false;
    }
    return true;
}

// Ours' scan script from its SOS headers (Q-JPEG-7); empty when a scan names an unknown component.
std::vector<jpeg_scan_info> scan_script(const JpegDoc& d) {
    std::vector<jpeg_scan_info> out;
    for (auto& s : d.segs) {
        if (s.marker != kSOS)
            continue;
        auto p = d.bytes(s);
        const std::size_t ns = p.empty() ? 0 : std::to_integer<std::size_t>(p[0]);
        if (ns < 1 || ns > MAX_COMPS_IN_SCAN || p.size() < 1 + 2 * ns + 3)
            return {};
        jpeg_scan_info si{};
        si.comps_in_scan = static_cast<int>(ns);
        for (std::size_t j = 0; j < ns; ++j) {
            int index = -1;
            for (std::size_t c = 0; c < d.comps.size(); ++c)
                if (d.comps[c].id == std::to_integer<int>(p[1 + 2 * j]))
                    index = static_cast<int>(c);
            if (index < 0)
                return {};
            si.component_index[j] = index;
        }
        si.Ss = std::to_integer<int>(p[1 + 2 * ns]);
        si.Se = std::to_integer<int>(p[2 + 2 * ns]);
        si.Ah = std::to_integer<int>(p[3 + 2 * ns]) >> 4;
        si.Al = std::to_integer<int>(p[3 + 2 * ns]) & 15;
        out.push_back(si);
    }
    return out;
}

std::string colorspace_name(J_COLOR_SPACE cs) {
    switch (cs) {
    case JCS_GRAYSCALE:
        return "gray";
    case JCS_YCbCr:
        return "ycbcr";
    case JCS_RGB:
        return "rgb";
    case JCS_CMYK:
        return "cmyk";
    case JCS_YCCK:
        return "ycck";
    default:
        return "unknown";
    }
}

} // namespace

void read_coefficients(JpegDoc& doc) {
    Err err;
    jpeg_decompress_struct di{};
    di.err = setup(err);
    jvirt_barray_ptr* arrays = nullptr;
    bool ok = guarded(err, [&] {
        jpeg_create_decompress(&di);
        source(di, doc.src.bytes());
        jpeg_read_header(&di, TRUE);
        arrays = jpeg_read_coefficients(&di);
    });
    if (ok && static_cast<std::size_t>(di.num_components) != doc.comps.size()) {
        ok = false;
        std::snprintf(err.message, sizeof err.message, "component count differs from the SOF segment");
    }
    if (ok) {
        doc.colorspace = colorspace_name(di.jpeg_color_space);
        doc.planes.resize(doc.comps.size());
        for (std::size_t c = 0; c < doc.comps.size(); ++c) {
            auto& p = doc.planes[c];
            const auto& ci = di.comp_info[c];
            p.bw = doc.mcus_x * static_cast<std::uint32_t>(doc.comps[c].h);
            p.bh = doc.mcus_y * static_cast<std::uint32_t>(doc.comps[c].v);
            p.real_w = ci.width_in_blocks, p.real_h = ci.height_in_blocks;
            p.coef.resize(std::uint64_t{p.bw} * p.bh * 64);
        }
        ok = guarded(err, [&] {
            for (std::size_t c = 0; c < doc.planes.size(); ++c) {
                auto& p = doc.planes[c];
                for (JDIMENSION by = 0; by < p.bh; ++by) {
                    JBLOCKARRAY rows = (*di.mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(&di),
                                                                     arrays[c], by, 1, FALSE);
                    std::memcpy(p.block(0, by), rows[0], std::size_t{p.bw} * sizeof(JBLOCK));
                }
            }
        });
    }
    jpeg_destroy_decompress(&di);
    if (!ok)
        throw Error(ErrorCategory::invalid_input, std::string("JPEG: ") + err.message);
    if (err.warnings)
        doc.warnings.push_back(
            {"input-warning", "/pixels",
             std::string(err.warning) +
                 (err.warnings > 1 ? " (+" + std::to_string(err.warnings - 1) + " more)" : "")});
}

std::optional<raster::Raster> decode_pixels(const JpegDoc& doc, Limits& budget) {
    const bool wide = doc.precision == 12;
    J_COLOR_SPACE out = JCS_UNKNOWN;
    if (doc.comps.size() == 1)
        out = JCS_GRAYSCALE;
    else if (doc.comps.size() == 3 && (doc.colorspace == "ycbcr" || doc.colorspace == "rgb"))
        out = JCS_RGB;
    else if (doc.comps.size() == 4 && (doc.colorspace == "cmyk" || doc.colorspace == "ycck"))
        out = JCS_CMYK;
    if (out == JCS_UNKNOWN || (doc.precision != 8 && !wide))
        return std::nullopt;
    const std::uint64_t cells = std::uint64_t{doc.width} * doc.height;
    if (cells > budget.max_pixels)
        throw Error(ErrorCategory::limit, "JPEG: " + std::to_string(doc.width) + "x" +
                                              std::to_string(doc.height) + " exceeds --max-pixels");
    budget.charge(cells * (wide ? 8 : 4));
    raster::Raster r(doc.width, doc.height, wide);
    const std::size_t nc = out == JCS_GRAYSCALE ? 1 : out == JCS_RGB ? 3 : 4;
    std::vector<J12SAMPLE> row12(wide ? std::size_t{doc.width} * nc : 0);
    std::vector<JSAMPLE> row8(wide ? 0 : std::size_t{doc.width} * nc);
    const unsigned max = wide ? 4095 : 255;

    Err err;
    jpeg_decompress_struct di{};
    di.err = setup(err);
    bool ok = guarded(err, [&] {
        jpeg_create_decompress(&di);
        source(di, doc.src.bytes());
        jpeg_read_header(&di, TRUE);
        di.out_color_space = out;
        di.dct_method = JDCT_ISLOW;
        di.do_fancy_upsampling = TRUE;
        jpeg_start_decompress(&di);
    });
    if (ok && (di.output_components != static_cast<int>(nc) || di.output_width != doc.width)) {
        ok = false;
        std::snprintf(err.message, sizeof err.message, "unexpected output layout");
    }
    ok = ok && guarded(err, [&] {
             while (di.output_scanline < di.output_height) {
                 const std::uint64_t y = di.output_scanline;
                 if (wide) {
                     J12SAMPROW p = row12.data();
                     jpeg12_read_scanlines(&di, &p, 1);
                 } else {
                     JSAMPROW p = row8.data();
                     jpeg_read_scanlines(&di, &p, 1);
                 }
                 for (std::uint32_t x = 0; x < doc.width; ++x) {
                     std::array<unsigned, 4> s{};
                     for (std::size_t k = 0; k < nc; ++k)
                         s[k] = wide ? static_cast<unsigned>(row12[x * nc + k]) : row8[x * nc + k];
                     if (nc == 1)
                         s[1] = s[2] = s[0];
                     else if (nc == 4) // Adobe CMYK stores inverted values: R = C * K / max
                         for (int k = 0; k < 3; ++k)
                             s[std::size_t(k)] = s[std::size_t(k)] * s[3] / max;
                     raster::Cell cell{};
                     for (int k = 0; k < 3; ++k) {
                         const unsigned v = s[std::size_t(k)];
                         cell[std::size_t(k)] =
                             static_cast<std::uint16_t>(wide ? (v << 4 | v >> 8) : v * 257);
                     }
                     cell[3] = 0xFFFF;
                     r.set(y * doc.width + x, cell);
                 }
             }
             jpeg_finish_decompress(&di);
         });
    jpeg_destroy_decompress(&di);
    if (!ok)
        throw Error(ErrorCategory::invalid_input, std::string("JPEG: pixel decode: ") + err.message);
    return r;
}

bool tables_reusable(const JpegDoc& d) {
    return huffman_tables(d, nullptr, nullptr);
}

std::optional<std::vector<std::byte>> write_coefficients(const JpegDoc& ours,
                                                         const std::vector<Plane>& planes, bool ours_script,
                                                         bool ours_tables, std::string& error) {
    std::vector<jpeg_scan_info> script = ours_script ? scan_script(ours) : std::vector<jpeg_scan_info>{};
    std::vector<HuffTable> tables;
    std::vector<std::pair<int, int>> sel;
    if (ours_tables && !huffman_tables(ours, &tables, &sel))
        ours_tables = false;

    Err derr, cerr;
    jpeg_decompress_struct di{};
    jpeg_compress_struct ci{};
    di.err = setup(derr);
    ci.err = setup(cerr);
    unsigned char* buf = nullptr;
    unsigned long size = 0;
    jvirt_barray_ptr* arrays = nullptr;
    bool ok = guarded(derr, [&] {
        jpeg_create_decompress(&di);
        source(di, ours.src.bytes());
        jpeg_read_header(&di, TRUE);
        arrays = jpeg_read_coefficients(&di);
        for (std::size_t c = 0; c < planes.size(); ++c)
            for (JDIMENSION by = 0; by < planes[c].bh; ++by) {
                JBLOCKARRAY rows = (*di.mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(&di),
                                                                 arrays[c], by, 1, TRUE);
                std::memcpy(rows[0], planes[c].block(0, by), std::size_t{planes[c].bw} * sizeof(JBLOCK));
            }
    });
    ok = ok && guarded(cerr, [&] {
             jpeg_create_compress(&ci);
             jpeg_mem_dest(&ci, &buf, &size);
             jpeg_copy_critical_parameters(&di, &ci);
             ci.write_JFIF_header = FALSE; // every APPn is spliced from the inputs (jpeg.md §7)
             ci.write_Adobe_marker = FALSE;
             ci.restart_interval = ours.restart;
             ci.arith_code = ours.arithmetic() ? TRUE : FALSE;
             if (!script.empty()) {
                 ci.scan_info = script.data();
                 ci.num_scans = static_cast<int>(script.size());
             } else if (ours.progressive()) {
                 jpeg_simple_progression(&ci);
             }
             if (ours_tables) {
                 for (auto& t : tables) {
                     JHUFF_TBL*& p = t.cls ? ci.ac_huff_tbl_ptrs[t.slot] : ci.dc_huff_tbl_ptrs[t.slot];
                     if (!p)
                         p = jpeg_alloc_huff_table(reinterpret_cast<j_common_ptr>(&ci));
                     std::memcpy(p->bits, t.bits.data(), sizeof p->bits);
                     std::memcpy(p->huffval, t.values.data(), sizeof p->huffval);
                     p->sent_table = FALSE;
                 }
                 for (std::size_t c = 0; c < sel.size(); ++c)
                     ci.comp_info[c].dc_tbl_no = sel[c].first, ci.comp_info[c].ac_tbl_no = sel[c].second;
             } else {
                 ci.optimize_coding = TRUE;
             }
             jpeg_write_coefficients(&ci, arrays);
             jpeg_finish_compress(&ci);
         });
    if (!ok)
        error = derr.message[0] ? derr.message : cerr.message;
    jpeg_destroy_compress(&ci);
    jpeg_destroy_decompress(&di);
    std::optional<std::vector<std::byte>> out;
    if (ok)
        out.emplace(reinterpret_cast<const std::byte*>(buf), reinterpret_cast<const std::byte*>(buf) + size);
    std::free(buf);
    return out;
}

std::vector<std::byte> encode(const raster::Raster& r, const EncodeOptions& o) {
    const int nc = o.gray ? 1 : 3;
    std::vector<JSAMPLE> row(std::size_t{r.width} * static_cast<std::size_t>(nc));
    Err err;
    jpeg_compress_struct ci{};
    ci.err = setup(err);
    unsigned char* buf = nullptr;
    unsigned long size = 0;
    bool ok = guarded(err, [&] {
        jpeg_create_compress(&ci);
        jpeg_mem_dest(&ci, &buf, &size);
        ci.image_width = r.width, ci.image_height = r.height;
        ci.input_components = nc;
        ci.in_color_space = o.gray ? JCS_GRAYSCALE : JCS_RGB;
        jpeg_set_defaults(&ci);
        jpeg_set_quality(&ci, o.quality, TRUE);
        if (!o.gray)
            ci.comp_info[0].h_samp_factor = o.h_samp, ci.comp_info[0].v_samp_factor = o.v_samp;
        ci.arith_code = o.arithmetic ? TRUE : FALSE;
        if (o.progressive)
            jpeg_simple_progression(&ci);
        ci.restart_interval = o.restart;
        jpeg_start_compress(&ci, TRUE);
        for (auto& [m, data] : o.markers)
            jpeg_write_marker(&ci, m, reinterpret_cast<const JOCTET*>(data.data()),
                              static_cast<unsigned>(data.size()));
        while (ci.next_scanline < ci.image_height) {
            const std::uint64_t y = ci.next_scanline;
            for (std::uint32_t x = 0; x < r.width; ++x) {
                const auto c = r.get(y * r.width + x);
                for (int k = 0; k < nc; ++k)
                    row[std::size_t(x) * std::size_t(nc) + std::size_t(k)] =
                        static_cast<JSAMPLE>(c[std::size_t(k)] >> 8);
            }
            JSAMPROW p = row.data();
            jpeg_write_scanlines(&ci, &p, 1);
        }
        jpeg_finish_compress(&ci);
    });
    jpeg_destroy_compress(&ci);
    std::vector<std::byte> out;
    if (ok)
        out.assign(reinterpret_cast<const std::byte*>(buf), reinterpret_cast<const std::byte*>(buf) + size);
    std::free(buf);
    if (!ok)
        throw Error(ErrorCategory::internal, std::string("JPEG encode: ") + err.message);
    return out;
}

} // namespace semsplinter::jpeg
