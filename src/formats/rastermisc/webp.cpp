#include "common.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/formats/webp.hpp>

#include <webp/decode.h>
#include <webp/encode.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace semsplinter::webp {

namespace {

using namespace rastermisc;
constexpr std::string_view kName = "WebP";

struct Chunk {
    std::string fourcc;
    std::uint64_t at = 0; // header
    std::uint32_t size = 0;
    std::uint64_t data() const { return at + 8; }
    std::uint64_t end() const { return at + 8 + size + (size & 1); }
};

struct WFrame {
    std::uint64_t at = 0, end = 0; // the image chunks (ALPH + VP8, or VP8L) with headers and pads
    bool lossy = false;
    std::uint32_t x = 0, y = 0, duration = 0;
    std::uint8_t flags = 0; // ANMF: bit 1 no-blend, bit 0 dispose to background
};

struct WebpDoc final : Doc {
    bool extended = false, animated = false;
    std::uint32_t cw = 0, ch = 0; // canvas
    std::vector<WFrame> wf;
    std::map<std::string, Chunk> chunks; // keys -> their chunk
    std::uint32_t background = 0;
    std::uint16_t loop = 0;
};

const WebpDoc& wdoc(const Doc& d) {
    return static_cast<const WebpDoc&>(d);
}

std::uint32_t u24(const Reader& r, std::uint64_t at) {
    return static_cast<std::uint32_t>(r.u(at, 3));
}

// Chunks from `at` to `end`. Sizes must stay inside; a missing final pad byte is tolerated.
std::vector<Chunk> chunks_of(const Reader& r, std::uint64_t at, std::uint64_t end) {
    std::vector<Chunk> v;
    while (at + 8 <= end) {
        Chunk c;
        c.at = at;
        auto f = r.sub(at, 4);
        c.fourcc.assign(reinterpret_cast<const char*>(f.data()), 4);
        c.size = r.le32(at + 4);
        if (c.data() + c.size > end)
            bad(kName, c.fourcc + " chunk runs past its container");
        v.push_back(c);
        at = std::min<std::uint64_t>(c.end(), end);
    }
    return v;
}

// libwebp's encoder output for one image, as its image chunks (ALPH + VP8, or VP8L).
std::vector<std::byte> encode_image(const Raster& r, bool lossless) {
    std::vector<std::uint8_t> rgba(r.cells() * 4);
    for (std::uint64_t i = 0; i < r.cells(); ++i) {
        auto c = rgba8(r.get(i));
        std::memcpy(rgba.data() + 4 * i, c.data(), 4);
    }
    WebPConfig config;
    WebPPicture pic;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&pic))
        throw Error(ErrorCategory::internal, "WebP: libwebp version mismatch");
    if (lossless) { // Q-RAST-5: fixed, deterministic per libwebp version
        config.lossless = 1, config.exact = 1, config.method = 6, config.quality = 100;
    } else { // ponytail: fixed quality 90 for --reencode; infer the source quality if users ask
        config.quality = 90, config.method = 4;
    }
    pic.use_argb = 1;
    pic.width = static_cast<int>(r.width), pic.height = static_cast<int>(r.height);
    WebPMemoryWriter wr;
    WebPMemoryWriterInit(&wr);
    pic.writer = WebPMemoryWrite, pic.custom_ptr = &wr;
    const bool ok =
        WebPPictureImportRGBA(&pic, rgba.data(), static_cast<int>(r.width * 4)) && WebPEncode(&config, &pic);
    WebPPictureFree(&pic);
    std::vector<std::byte> riff(reinterpret_cast<const std::byte*>(wr.mem),
                                reinterpret_cast<const std::byte*>(wr.mem) + wr.size);
    WebPMemoryWriterClear(&wr);
    if (!ok)
        throw Error(ErrorCategory::internal, "WebP: encoding failed");
    const Reader rr{riff, kName};
    std::vector<std::byte> out;
    for (auto& c : chunks_of(rr, 12, riff.size()))
        if (c.fourcc == "ALPH" || c.fourcc == "VP8 " || c.fourcc == "VP8L")
            out.insert(out.end(), riff.begin() + static_cast<std::ptrdiff_t>(c.at),
                       riff.begin() + static_cast<std::ptrdiff_t>(c.end()));
    return out;
}

struct OutImg {
    std::vector<emit::Seg> chunks; // image chunks
    std::uint32_t x = 0, y = 0, w = 0, h = 0, duration = 0;
    std::uint8_t flags = 0;
    bool alpha = false, lossy = false;
};

struct Out {
    bool animated = false;
    std::uint32_t cw = 0, ch = 0, background = 0;
    std::uint16_t loop = 0;
    std::vector<emit::Seg> icc, exif, xmp, unknown; // whole chunks
    std::vector<OutImg> frames;
};

emit::Seg chunk(std::string_view fourcc, std::vector<emit::Seg> data, emit::GroupId& ids) {
    const auto g = ids++;
    return emit::group(ids++, {emit::bytes(fourcc), emit::field(4, emit::Endian::little, emit::size_of(g)),
                               emit::group(g, std::move(data)), emit::align(2)});
}

std::vector<std::byte> u24le(std::uint32_t v) {
    std::vector<std::byte> o;
    put_le(o, v, 3);
    return o;
}

// The RIFF container (raster-misc.md §7.1): RIFF size, chunk sizes and pads are Emitter fields; VP8X flags
// and canvas follow the chunks written.
std::vector<std::byte> assemble(Out o) {
    emit::GroupId ids = 10;
    std::vector<emit::Seg> body{emit::bytes(std::string_view("WEBP"))};
    const bool alpha = std::any_of(o.frames.begin(), o.frames.end(), [](const OutImg& f) { return f.alpha; });
    const bool extended = o.animated || !o.icc.empty() || !o.exif.empty() || !o.xmp.empty() ||
                          !o.unknown.empty() || (alpha && o.frames[0].lossy);
    if (extended) {
        const std::uint32_t cw = o.animated ? o.cw : o.frames[0].w, ch = o.animated ? o.ch : o.frames[0].h;
        std::vector<std::byte> x{std::byte((o.icc.empty() ? 0 : 0x20) | (alpha ? 0x10 : 0) |
                                           (o.exif.empty() ? 0 : 0x08) | (o.xmp.empty() ? 0 : 0x04) |
                                           (o.animated ? 0x02 : 0)),
                                 std::byte{0}, std::byte{0}, std::byte{0}};
        auto a = u24le(cw - 1), b = u24le(ch - 1);
        x.insert(x.end(), a.begin(), a.end()), x.insert(x.end(), b.begin(), b.end());
        body.push_back(chunk("VP8X", {emit::bytes(x)}, ids));
        for (auto& s : o.icc)
            body.push_back(std::move(s));
        if (o.animated) {
            std::vector<std::byte> anim;
            put_le(anim, o.background, 4), put_le(anim, o.loop, 2);
            body.push_back(chunk("ANIM", {emit::bytes(anim)}, ids));
        }
    }
    for (auto& f : o.frames) {
        if (!o.animated) {
            for (auto& s : f.chunks)
                body.push_back(std::move(s));
            continue;
        }
        std::vector<std::byte> h;
        for (auto v : {f.x / 2, f.y / 2, f.w - 1, f.h - 1, f.duration}) {
            auto b = u24le(v);
            h.insert(h.end(), b.begin(), b.end());
        }
        h.push_back(std::byte(f.flags));
        std::vector<emit::Seg> data{emit::bytes(h)};
        for (auto& s : f.chunks)
            data.push_back(std::move(s));
        body.push_back(chunk("ANMF", std::move(data), ids));
    }
    for (auto* v : {&o.exif, &o.xmp, &o.unknown})
        for (auto& s : *v)
            body.push_back(std::move(s));
    const emit::GroupId b = 1;
    return emit::emit(emit::group(2, {emit::bytes(std::string_view("RIFF")),
                                      emit::field(4, emit::Endian::little, emit::size_of(b)),
                                      emit::group(b, std::move(body))}));
}

class WebpCodec final : public Codec {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "webp",
            "WebP (lossless, lossy, animated)",
            {".webp"},
            {true, true, false, true, true},
            {{"reencode", OptionSpec::Arg::none,
              "merge lossy WebP frames in pixel space and re-encode them (lossy)", true}}};
        return i;
    }

    int probe(std::span<const std::byte> head, std::string_view) const override {
        return head.size() >= 16 && std::memcmp(head.data(), "RIFF", 4) == 0 &&
                       std::memcmp(head.data() + 8, "WEBP", 4) == 0
                   ? 100
                   : 0;
    }

    std::unique_ptr<Doc> load(ByteSource src, Limits& budget, const OptionValues&) const override {
        auto d = std::make_unique<WebpDoc>();
        d->src = std::move(src);
        const Reader r{d->src.bytes(), kName};
        const auto size = r.d.size();
        if (std::memcmp(r.sub(0, 4).data(), "RIFF", 4) != 0 ||
            std::memcmp(r.sub(8, 4).data(), "WEBP", 4) != 0)
            bad(kName, "missing RIFF/WEBP header");
        const std::uint64_t riff = r.le32(4);
        const std::uint64_t end = size;                        // the RIFF size is advisory (Q-RAST-9)
        if (riff + 8 != size && riff + 8 + (riff & 1) != size) // Q-RAST-9: advisory
            d->warnings.push_back({"framing-inconsistent", "/riff",
                                   "RIFF size " + std::to_string(riff) + " differs from the file size " +
                                       std::to_string(size)});
        auto& m = d->map.ranges;
        auto add = [&](std::uint64_t a, std::uint64_t len, ByteClass c, std::string unit,
                       std::string layer = {}) {
            if (len)
                m.push_back({a, len, c, std::move(unit), std::move(layer)});
        };
        add(0, 12, ByteClass::framing, "/riff");
        auto header = [&](const Chunk& c, const std::string& unit) {
            add(c.at, 8, ByteClass::framing, unit);
            if (c.size & 1 && c.data() + c.size < size)
                add(c.data() + c.size, 1, ByteClass::framing, unit);
        };
        std::map<std::string, int> seen;
        auto image_chunks = [&](const std::vector<Chunk>& cs, std::size_t from, WFrame& f,
                                const std::string& unit) {
            bool found = false;
            for (std::size_t k = from; k < cs.size() && !found; ++k) {
                auto& c = cs[k];
                if (c.fourcc != "ALPH" && c.fourcc != "VP8 " && c.fourcc != "VP8L")
                    continue;
                if (!f.end)
                    f.at = c.at;
                f.end = std::min<std::uint64_t>(c.end(), size);
                header(c, unit + "/pixels");
                add(c.data(), c.size, ByteClass::encoding, unit + "/pixels",
                    c.fourcc == "ALPH"   ? "alph"
                    : c.fourcc == "VP8 " ? "vp8"
                                         : "vp8l");
                if (c.fourcc != "ALPH")
                    found = true, f.lossy = c.fourcc == "VP8 ";
            }
            if (!found)
                bad(kName, "no VP8 or VP8L image data");
        };
        auto cs = chunks_of(r, 12, end);
        if (cs.empty())
            bad(kName, "no chunks");
        std::size_t k = 0;
        if (cs[0].fourcc == "VP8X") {
            if (cs[0].size < 10)
                bad(kName, "short VP8X chunk");
            d->extended = true;
            d->animated = r.u8(cs[0].data()) & 0x02;
            d->cw = u24(r, cs[0].data() + 4) + 1, d->ch = u24(r, cs[0].data() + 7) + 1;
            header(cs[0], "/canvas");
            add(cs[0].data(), cs[0].size, ByteClass::derived, "/canvas");
            k = 1;
        }
        bool image_done = false;
        for (; k < cs.size(); ++k) {
            auto& c = cs[k];
            auto data = r.sub(c.data(), c.size);
            auto key = [&](const std::string& path, std::string kind = "metadata") {
                d->chunks[path] = c;
                d->keys.push_back({path, hash128(data), bytes_text(data), std::move(kind)});
                header(c, path);
                add(c.data(), c.size, ByteClass::content, path);
            };
            if (c.fourcc == "ICCP") {
                key("/icc", "color");
            } else if (c.fourcc == "EXIF") {
                key("/exif");
            } else if (c.fourcc == "XMP ") {
                key("/xmp");
            } else if (c.fourcc == "ANIM" && c.size >= 6) {
                d->background = r.le32(c.data()), d->loop = r.le16(c.data() + 4);
                char buf[16];
                std::snprintf(buf, sizeof buf, "%08x", d->background);
                d->keys.push_back({"/animation/background", hash128(r.sub(c.data(), 4)), buf, "animation"});
                d->keys.push_back({"/animation/loop", hash128(r.sub(c.data() + 4, 2)),
                                   d->loop ? std::to_string(d->loop) : "forever", "animation"});
                header(c, "/animation");
                add(c.data(), c.size, ByteClass::content, "/animation");
            } else if (c.fourcc == "ANMF" && c.size >= 16) {
                WFrame f;
                f.x = u24(r, c.data()) * 2, f.y = u24(r, c.data() + 3) * 2,
                f.duration = u24(r, c.data() + 12);
                f.flags = r.u8(c.data() + 15);
                const std::string unit = "/frames/" + std::to_string(d->wf.size());
                header(c, unit);
                add(c.data(), 16, ByteClass::content, unit);
                auto inner = chunks_of(r, c.data() + 16, c.data() + c.size);
                image_chunks(inner, 0, f, unit);
                d->wf.push_back(f);
            } else if ((c.fourcc == "ALPH" || c.fourcc == "VP8 " || c.fourcc == "VP8L") && !image_done &&
                       !d->animated) {
                WFrame f;
                std::vector<Chunk> rest(cs.begin() + static_cast<std::ptrdiff_t>(k), cs.end());
                image_chunks(rest, 0, f, "");
                while (k + 1 < cs.size() && cs[k].fourcc == "ALPH")
                    ++k;
                d->wf.push_back(f);
                image_done = true;
            } else {
                const std::string path =
                    "/chunks/" + pointer_escape(c.fourcc) + ":" + std::to_string(seen[c.fourcc]++);
                key(path);
            }
        }
        if (d->wf.empty())
            bad(kName, "no image");
        d->sequence = d->animated;
        for (std::size_t i = 0; i < d->wf.size(); ++i) {
            auto& wf = d->wf[i];
            auto payload = r.sub(wf.at, wf.end - wf.at);
            Frame f;
            f.coded = hash128(payload);
            f.lossy = wf.lossy;
            WebPDecoderConfig config;
            if (!WebPInitDecoderConfig(&config))
                throw Error(ErrorCategory::internal, "WebP: libwebp version mismatch");
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
            if (WebPGetFeatures(bytes, payload.size(), &config.input) != VP8_STATUS_OK)
                bad(kName, "frame " + std::to_string(i) + ": bad bitstream header");
            f.pixels = new_raster(static_cast<std::uint32_t>(config.input.width),
                                  static_cast<std::uint32_t>(config.input.height), false, budget);
            config.output.colorspace = MODE_RGBA;
            if (WebPDecode(bytes, payload.size(), &config) != VP8_STATUS_OK)
                bad(kName, "frame " + std::to_string(i) + ": libwebp cannot decode it");
            const auto& o = config.output.u.RGBA;
            for (std::uint32_t y = 0; y < f.pixels.height; ++y)
                for (std::uint32_t x = 0; x < f.pixels.width; ++x) {
                    const std::uint8_t* p =
                        o.rgba + std::size_t{y} * static_cast<std::size_t>(o.stride) + 4u * x;
                    f.pixels.set(std::uint64_t{y} * f.pixels.width + x, cell({p[0], p[1], p[2], p[3]}));
                }
            WebPFreeDecBuffer(&config.output);
            f.encoding = wf.lossy ? "lossy" : "lossless";
            if (d->animated) {
                if (wf.x + f.pixels.width > d->cw || wf.y + f.pixels.height > d->ch)
                    bad(kName, "frame " + std::to_string(i) + " lies outside the canvas");
                f.keys.push_back({"/rect", hash128(std::to_string(wf.x) + "," + std::to_string(wf.y)),
                                  std::to_string(wf.x) + "," + std::to_string(wf.y)});
                f.keys.push_back({"/duration", hash128(std::to_string(wf.duration)),
                                  std::to_string(wf.duration) + "ms", "animation"});
                f.keys.push_back({"/blend", hash128(std::string_view(wf.flags & 2 ? "0" : "1")),
                                  wf.flags & 2 ? "no-blend" : "blend", "animation"});
                f.keys.push_back({"/dispose", hash128(std::string_view(wf.flags & 1 ? "1" : "0")),
                                  wf.flags & 1 ? "background" : "none", "animation"});
            }
            d->frames.push_back(std::move(f));
        }
        if (d->animated)
            d->keys.push_back({"/canvas", hash128(std::to_string(d->cw) + "x" + std::to_string(d->ch)),
                               std::to_string(d->cw) + "x" + std::to_string(d->ch)});
        d->encoding = {{"container", d->extended ? "extended" : "simple"}};
        std::sort(m.begin(), m.end(),
                  [](const EnvRange& a, const EnvRange& b) { return a.offset < b.offset; });
        d->map.validate(size);
        return d;
    }

    std::vector<std::byte> write(Plan& p, std::vector<Warning>&) const override {
        Out o;
        o.animated = std::any_of(p.docs.begin(), p.docs.end(), [](const Doc* d) { return d->sequence; });
        emit::GroupId ids = 1000;
        for (auto& [k, s] : p.keys) {
            const WebpDoc& d = wdoc(p.doc(s));
            if (k == "/canvas")
                o.cw = d.cw, o.ch = d.ch;
            else if (k == "/animation/background")
                o.background = d.background;
            else if (k == "/animation/loop")
                o.loop = d.loop;
            else if (auto c = d.chunks.find(k); c != d.chunks.end()) {
                auto seg = emit::raw(d.src, c->second.at,
                                     std::min<std::uint64_t>(c->second.end(), d.src.size()) - c->second.at);
                std::vector<emit::Seg> one;
                one.push_back(std::move(seg));
                if ((c->second.size & 1) && c->second.end() > d.src.size())
                    one.push_back(emit::bytes(std::vector<std::byte>{std::byte{0}})); // a missing pad byte
                auto& dst = k == "/icc" ? o.icc : k == "/exif" ? o.exif : k == "/xmp" ? o.xmp : o.unknown;
                dst.push_back(emit::group(ids++, std::move(one)));
            }
        }
        for (std::size_t i = 0; i < p.frames.size(); ++i) {
            const OutFrame& of = p.frames[i];
            OutImg f;
            f.w = of.pixels.width, f.h = of.pixels.height;
            for (std::uint64_t c = 0; c < of.pixels.cells() && !f.alpha; ++c)
                f.alpha = of.pixels.get(c)[3] != 0xFFFF;
            for (auto& [k, s] : of.keys) {
                auto& wf = wdoc(p.doc(s)).wf[*of.src[static_cast<std::size_t>(s)]];
                if (k == "/rect")
                    f.x = wf.x, f.y = wf.y;
                else if (k == "/duration")
                    f.duration = wf.duration;
                else if (k == "/blend")
                    f.flags = static_cast<std::uint8_t>((f.flags & 1) | (wf.flags & 2));
                else if (k == "/dispose")
                    f.flags = static_cast<std::uint8_t>((f.flags & 2) | (wf.flags & 1));
            }
            // Unchanged frames keep their bitstream bytes; others are re-encoded as their source was coded.
            if (of.same) {
                const WebpDoc& sd = wdoc(p.doc(*of.same));
                const WFrame& wf = sd.wf[*of.src[static_cast<std::size_t>(*of.same)]];
                f.lossy = wf.lossy;
                f.chunks.push_back(emit::raw(sd.src, wf.at, wf.end - wf.at));
                if ((wf.end - wf.at) % 2) // a final chunk without its pad byte
                    f.chunks.push_back(emit::bytes(std::vector<std::byte>{std::byte{0}}));
            } else {
                const Side bs = of.src[static_cast<std::size_t>(p.enc)] ? p.enc : Side::ours;
                f.lossy = of.src[static_cast<std::size_t>(bs)] &&
                          wdoc(p.doc(bs)).wf[*of.src[static_cast<std::size_t>(bs)]].lossy;
                f.chunks.push_back(emit::bytes(encode_image(of.pixels, !f.lossy)));
                if (f.lossy)
                    p.approximate.push_back(i);
            }
            o.frames.push_back(std::move(f));
        }
        return assemble(std::move(o));
    }
};

} // namespace

std::vector<std::byte> write(const std::vector<FrameSpec>& frames, int loop) {
    Out o;
    o.animated = frames.size() > 1 || loop >= 0;
    o.loop = static_cast<std::uint16_t>(std::max(loop, 0));
    for (auto& f : frames) {
        OutImg x;
        x.x = f.x, x.y = f.y, x.w = f.pixels.width, x.h = f.pixels.height, x.duration = f.duration,
        x.lossy = !f.lossless;
        for (std::uint64_t c = 0; c < f.pixels.cells() && !x.alpha; ++c)
            x.alpha = f.pixels.get(c)[3] != 0xFFFF;
        x.chunks.push_back(emit::bytes(encode_image(f.pixels, f.lossless)));
        o.cw = std::max(o.cw, f.x + f.pixels.width), o.ch = std::max(o.ch, f.y + f.pixels.height);
        o.frames.push_back(std::move(x));
    }
    return assemble(std::move(o));
}

std::unique_ptr<Format> make_format() {
    return rastermisc::make_format(std::make_unique<WebpCodec>());
}

} // namespace semsplinter::webp
