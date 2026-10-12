#include <semsplinter/error.hpp>
#include <semsplinter/patch.hpp>

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
#include <tuple>

namespace semsplinter {

namespace {

// Git's base85 alphabet (also used by `GIT binary patch`).
constexpr char kB85[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";
constexpr std::size_t kLineBytes = 64;

Error bad(const std::string& what) {
    return Error(ErrorCategory::invalid_input, "patch: " + what);
}

// The codec of payload_codec() (D-7).
std::vector<std::byte> compress(std::span<const std::byte> in) {
    std::vector<std::byte> z(ZSTD_compressBound(in.size()));
    std::size_t zn = ZSTD_compress(z.data(), z.size(), in.data(), in.size(), 19);
    if (ZSTD_isError(zn))
        throw Error(ErrorCategory::internal, "patch: zstd compression failed");
    z.resize(zn);
    return z;
}

std::vector<std::byte> unzstd(std::span<const std::byte> in, std::uint64_t limit) {
    std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> d(ZSTD_createDStream(), ZSTD_freeDStream);
    std::vector<std::byte> out;
    std::array<std::byte, 65536> buf;
    ZSTD_inBuffer zin{in.data(), in.size(), 0};
    for (;;) {
        ZSTD_outBuffer zout{buf.data(), buf.size(), 0};
        std::size_t rc = ZSTD_decompressStream(d.get(), &zout, &zin);
        if (ZSTD_isError(rc))
            throw bad("corrupt payload");
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(zout.pos));
        if (out.size() > limit)
            throw Error(ErrorCategory::limit, "patch: payload exceeds the input limit");
        if (rc == 0 && zin.pos == zin.size)
            return out;
        if (zin.pos == zin.size && zout.pos < zout.size)
            throw bad("corrupt payload"); // truncated frame
    }
}

std::vector<std::byte> inflate_all(std::span<const std::byte> in, std::uint64_t limit) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK)
        throw Error(ErrorCategory::internal, "patch: zlib init failed");
    std::vector<std::byte> out;
    std::array<Bytef, 65536> buf;
    z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    int rc = Z_OK;
    while (rc == Z_OK) {
        z.next_out = buf.data();
        z.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&z, Z_NO_FLUSH);
        auto* b = reinterpret_cast<const std::byte*>(buf.data());
        out.insert(out.end(), b, b + (buf.size() - z.avail_out));
        if (out.size() > limit) {
            inflateEnd(&z);
            throw Error(ErrorCategory::limit, "patch: payload exceeds the input limit");
        }
    }
    inflateEnd(&z);
    if (rc != Z_STREAM_END)
        throw bad("corrupt payload");
    return out;
}

std::string trim_cr(std::string_view s) {
    if (!s.empty() && s.back() == '\r')
        s.remove_suffix(1);
    return std::string(s);
}

std::optional<Hash128> parse_hash(std::string_view s) {
    if (!s.starts_with("xxh3:") || s.size() != 37)
        return std::nullopt;
    Hash128 h;
    for (std::size_t i = 0; i < 32; ++i) {
        char c = s[5 + i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return std::nullopt;
        auto& half = i < 16 ? h.hi : h.lo;
        half = half << 4 | static_cast<std::uint64_t>(v);
    }
    return h;
}

std::uint64_t to_u64(std::string_view s) {
    if (s.empty() || s.size() > 19 ||
        !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw bad("bad number '" + std::string(s) + "'");
    return std::stoull(std::string(s));
}

std::vector<std::uint64_t> numbers(std::string_view s) {
    std::vector<std::uint64_t> v;
    std::size_t at = 0;
    while (at <= s.size()) {
        auto comma = s.find(',', at);
        v.push_back(
            to_u64(s.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at)));
        if (comma == std::string_view::npos)
            break;
        at = comma + 1;
    }
    return v;
}

Address parse_address(std::string_view s) {
    Address a;
    auto hash = s.find('#');
    a.path = std::string(s.substr(0, hash));
    if (hash == std::string_view::npos)
        return a;
    auto r = s.substr(hash + 1);
    auto eq = r.find('=');
    if (eq == std::string_view::npos)
        throw bad("bad address '" + std::string(s) + "'");
    auto kind = r.substr(0, eq);
    auto n = numbers(r.substr(eq + 1));
    if (kind == "rect" && n.size() == 4)
        a.region = Rect{n[0], n[1], n[2], n[3]};
    else if (kind == "range" && n.size() == 2)
        a.region = Range{n[0], n[1]};
    else if (kind == "box" && n.size() == 6)
        a.region = Box{n[0], n[1], n[2], n[3], n[4], n[5]};
    else
        throw bad("bad address '" + std::string(s) + "'");
    return a;
}

Op parse_op(std::string_view s) {
    for (Op o : {Op::insert, Op::erase, Op::update, Op::move})
        if (to_string(o) == s)
            return o;
    throw bad("unknown operation '" + std::string(s) + "'");
}

// A "---" / "+++" header: the name up to a tab, and what follows it.
std::pair<std::string, std::string> header_name(std::string_view line) {
    auto s = line.substr(4);
    auto tab = s.find('\t');
    std::string name(s.substr(0, tab));
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
    return {name, tab == std::string_view::npos ? std::string{} : std::string(s.substr(tab + 1))};
}

std::string range(std::size_t start, std::size_t len) {
    return len == 1 ? std::to_string(start) : std::to_string(start) + "," + std::to_string(len);
}

// GNU patch -l: runs of blanks compare equal, trailing blanks are ignored.
std::string squeeze(std::string_view s) {
    std::string o;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            space = true;
            continue;
        }
        if (space && !o.empty())
            o += ' ';
        space = false;
        o += c;
    }
    return o;
}

std::string_view body(std::string_view line) {
    if (!line.empty() && line.back() == '\n')
        line.remove_suffix(1);
    return line;
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(std::span<const std::byte> in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 3; ++k)
            v = v << 8 | (i + k < in.size() ? std::to_integer<std::uint32_t>(in[i + k]) : 0u);
        const std::size_t n = std::min<std::size_t>(3, in.size() - i);
        for (std::size_t k = 0; k < 4; ++k)
            out += k <= n ? kB64[(v >> (18 - 6 * k)) & 63] : '=';
    }
    return out;
}

std::vector<std::byte> unbase64(std::string_view s) {
    std::vector<std::byte> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=')
            break;
        auto p = std::strchr(kB64, c);
        if (!c || !p)
            throw bad("bad base64 in payload");
        acc = acc << 6 | static_cast<std::uint32_t>(p - kB64);
        if ((bits += 6) >= 8)
            out.push_back(std::byte((acc >> (bits -= 8)) & 0xFF));
    }
    return out;
}

using json = nlohmann::ordered_json;

// One section of a JSON patch (a §7.1 diff report from --binary --json).
void json_sections(const json& j, std::uint64_t limit, std::vector<PatchSection>& out) {
    if (j.contains("unified")) {
        for (auto& s : parse_patch(j.at("unified").get<std::string>(), limit))
            out.push_back(std::move(s));
        return;
    }
    PatchSection s;
    s.format = j.at("format").get<std::string>();
    for (auto [side, name, hash] :
         {std::tuple{"a", &s.old_name, &s.old_hash}, std::tuple{"b", &s.new_name, &s.new_hash}}) {
        auto& in = j.at("inputs").at(side);
        *name = in.at("path").get<std::string>();
        *hash = parse_hash(in.value("hash", ""));
    }
    s.preamble = {"--- " + s.old_name, "+++ " + s.new_name};
    bool payloads = false;
    for (auto& c : j.at("changes")) {
        UnitHunk u;
        u.op = parse_op(c.at("op").get<std::string>());
        u.at = parse_address(c.at("at").get<std::string>());
        u.id = c.at("id").get<std::string>();
        for (auto [side, text, value] : {std::tuple{"before", &u.before_text, &u.before},
                                         std::tuple{"after", &u.after_text, &u.after}}) {
            if (!c.contains(side))
                continue;
            auto& v = c.at(side);
            if (v.contains("text"))
                *text = v.at("text").get<std::string>();
            if (v.contains("payload"))
                *value = payload_from_json(v.at("payload"), limit), payloads = true;
        }
        s.units.push_back(std::move(u));
    }
    if (!payloads)
        throw bad("a JSON report without payloads is not a patch (write one with --binary --json)");
    if (j.contains("exact")) {
        auto& e = j.at("exact");
        s.exact = ExactBytes{e.at("offset").get<std::uint64_t>(), e.at("removed").get<std::uint64_t>(),
                             payload_from_json(e, limit)};
    }
    out.push_back(std::move(s));
}

} // namespace

json payload_json(std::span<const std::byte> raw) {
    return {{"codec", "zstd"}, {"encoding", "base64"}, {"bytes", base64(compress(raw))}};
}

std::vector<std::byte> payload_from_json(const json& p, std::uint64_t limit) {
    const auto codec = p.at("codec").get<std::string>();
    if (p.at("encoding").get<std::string>() != "base64" || (codec != "zstd" && codec != "zlib"))
        throw Error(ErrorCategory::unsupported,
                    "patch: payload codec '" + codec + "' is not supported (D-7)");
    auto z = unbase64(p.at("bytes").get<std::string>());
    return codec == "zstd" ? unzstd(z, limit) : inflate_all(z, limit);
}

void add_patch(json& report, const PatchSection& s) {
    if (s.format.empty()) {
        report["unified"] = write_section(s);
        return;
    }
    auto& changes = report["changes"];
    for (std::size_t k = 0; k < s.units.size() && k < changes.size(); ++k)
        for (auto [side, v] :
             {std::pair{"before", &s.units[k].before}, std::pair{"after", &s.units[k].after}})
            if (*v)
                changes[k][side]["payload"] = payload_json(**v);
    if (s.exact) {
        json e = {{"offset", s.exact->offset}, {"removed", s.exact->removed}};
        e.update(payload_json(s.exact->bytes));
        report["exact"] = std::move(e);
    }
}

std::string encode_payload(std::span<const std::byte> raw) {
    auto z = compress(raw);
    std::string out;
    for (std::size_t at = 0; at < z.size(); at += kLineBytes) {
        std::size_t end = std::min(z.size(), at + kLineBytes);
        for (std::size_t i = at; i < end; i += 4) {
            std::uint32_t v = 0;
            for (std::size_t k = 0; k < 4; ++k)
                v = v << 8 | (i + k < end ? std::to_integer<std::uint32_t>(z[i + k]) : 0u);
            char five[5];
            for (int k = 4; k >= 0; --k)
                five[k] = kB85[v % 85], v /= 85;
            out.append(five, 5);
        }
        out += '\n';
    }
    return std::to_string(z.size()) + "\n" + out;
}

std::string_view payload_codec() noexcept {
    return "zstd+base85";
}

std::vector<std::byte> decode_payload(std::string_view lines, std::size_t n, std::uint64_t limit,
                                      std::string_view codec) {
    if (codec != "zlib+base85" && codec != payload_codec())
        throw Error(ErrorCategory::unsupported,
                    "patch: payload codec '" + std::string(codec) + "' is not supported (D-7)");
    std::array<int, 256> val;
    val.fill(-1);
    for (int i = 0; i < 85; ++i)
        val[static_cast<unsigned char>(kB85[i])] = i;
    std::vector<std::byte> z;
    std::uint64_t acc = 0;
    int k = 0;
    for (char c : lines) {
        if (c == '\n' || c == '\r')
            continue;
        int v = val[static_cast<unsigned char>(c)];
        if (v < 0)
            throw bad("bad base85 character in payload");
        acc = acc * 85 + static_cast<std::uint64_t>(v);
        if (++k == 5) {
            if (acc > 0xFFFFFFFFu)
                throw bad("bad base85 group in payload");
            for (int s = 24; s >= 0; s -= 8)
                z.push_back(std::byte((acc >> s) & 0xFF));
            acc = 0, k = 0;
        }
    }
    if (k || z.size() < n || z.size() - n >= 4)
        throw bad("payload length does not match");
    z.resize(n);
    if (codec == "zstd+base85")
        return unzstd(z, limit);
    return inflate_all(z, limit);
}

void PatchSection::reverse() {
    std::swap(old_name, new_name);
    std::swap(old_hash, new_hash);
    exact.reset(); // ponytail: forward only; store the base's removed bytes too if -R must stay byte-exact
    for (auto& u : units) {
        std::swap(u.before, u.after);
        std::swap(u.before_text, u.after_text);
        if (u.op == Op::insert)
            u.op = Op::erase;
        else if (u.op == Op::erase)
            u.op = Op::insert;
    }
    for (auto& h : hunks) {
        std::swap(h.old_start, h.new_start);
        std::swap(h.old_len, h.new_len);
        std::swap(h.no_eol_old, h.no_eol_new);
        for (auto& l : h.lines)
            if (!l.empty() && (l[0] == '-' || l[0] == '+'))
                l[0] = l[0] == '-' ? '+' : '-';
    }
}

std::vector<PatchSection> parse_patch(std::string_view text, std::uint64_t payload_limit) {
    if (auto first = text.find_first_not_of(" \t\r\n");
        first != std::string_view::npos && (text[first] == '{' || text[first] == '[')) {
        std::vector<PatchSection> out;
        try {
            auto j = json::parse(text);
            for (auto& r : j.is_array() ? j : json::array({j}))
                json_sections(r, payload_limit, out);
        } catch (const json::exception& e) {
            throw bad(std::string("bad JSON patch: ") + e.what());
        }
        return out;
    }
    std::vector<std::string> lines;
    for (std::size_t at = 0; at < text.size();) {
        auto nl = text.find('\n', at);
        lines.emplace_back(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        at = nl == std::string_view::npos ? text.size() : nl + 1;
    }
    std::vector<PatchSection> out;
    std::vector<std::string> preamble;
    auto starts = [&](std::size_t i, std::string_view p) {
        return i < lines.size() && lines[i].starts_with(p);
    };
    std::size_t i = 0;
    while (i < lines.size()) {
        const bool envelope = trim_cr(lines[i]) == "splinter-patch 1";
        const std::size_t h = envelope ? i + 1 : i;
        if (!(starts(h, "--- ") && starts(h + 1, "+++ ") &&
              (envelope ? starts(h + 2, "format ") : starts(h + 2, "@@ -")))) {
            if (envelope)
                throw bad("line " + std::to_string(i + 1) +
                          ": expected ---, +++, and format after splinter-patch");
            preamble.push_back(trim_cr(lines[i++]));
            continue;
        }
        PatchSection s;
        s.line = h + 1;
        s.preamble = std::move(preamble);
        preamble.clear();
        s.preamble.push_back(trim_cr(lines[h]));
        s.preamble.push_back(trim_cr(lines[h + 1]));
        auto [on, ot] = header_name(trim_cr(lines[h]));
        auto [nn, nt] = header_name(trim_cr(lines[h + 1]));
        s.old_name = on, s.new_name = nn;
        i = h + 2;
        if (envelope) {
            s.old_hash = parse_hash(ot), s.new_hash = parse_hash(nt);
            s.format = trim_cr(lines[i++]).substr(7);
            if (starts(i, "exact ")) {
                // exact OFFSET REMOVED CODEC N
                std::istringstream f(trim_cr(lines[i]).substr(6));
                std::string off, removed, codec, n;
                if (!(f >> off >> removed >> codec >> n))
                    throw bad("line " + std::to_string(i + 1) + ": bad exact line");
                auto size = static_cast<std::size_t>(to_u64(n));
                std::string b85;
                for (++i; b85.size() < (size + 3) / 4 * 5 && i < lines.size();)
                    b85 += trim_cr(lines[i++]);
                s.exact =
                    ExactBytes{to_u64(off), to_u64(removed), decode_payload(b85, size, payload_limit, codec)};
            }
            while (starts(i, "@@ ")) {
                auto hdr = trim_cr(lines[i]);
                if (!hdr.ends_with(" @@"))
                    throw bad("line " + std::to_string(i + 1) + ": bad hunk header");
                auto inner = std::string_view(hdr).substr(3, hdr.size() - 6);
                auto sp1 = inner.find(' '), sp2 = inner.rfind(' ');
                if (sp1 == std::string_view::npos || sp2 <= sp1)
                    throw bad("line " + std::to_string(i + 1) + ": bad hunk header");
                UnitHunk u;
                u.op = parse_op(inner.substr(0, sp1));
                u.at = parse_address(inner.substr(sp1 + 1, sp2 - sp1 - 1));
                u.id = std::string(inner.substr(sp2 + 1));
                ++i;
                while (i < lines.size()) {
                    auto l = trim_cr(lines[i]);
                    if (l.starts_with("- ")) {
                        u.before_text = l.substr(2), ++i;
                    } else if (l.starts_with("+ ")) {
                        u.after_text = l.substr(2), ++i;
                    } else if (l.starts_with("#")) {
                        ++i;
                    } else if (l.starts_with("payload ")) {
                        // payload before|after zlib+base85 N
                        auto f = l.substr(8);
                        auto a = f.find(' '), b = f.rfind(' ');
                        if (a == std::string::npos || b <= a)
                            throw bad("line " + std::to_string(i + 1) + ": bad payload line");
                        auto which = f.substr(0, a), codec = f.substr(a + 1, b - a - 1);
                        auto n = static_cast<std::size_t>(to_u64(f.substr(b + 1)));
                        if (which != "before" && which != "after")
                            throw bad("line " + std::to_string(i + 1) + ": bad payload line");
                        std::size_t want = (n + 3) / 4 * 5;
                        std::string b85;
                        ++i;
                        while (b85.size() < want && i < lines.size())
                            b85 += trim_cr(lines[i++]);
                        (which == "before" ? u.before : u.after) =
                            decode_payload(b85, n, payload_limit, codec);
                    } else {
                        break;
                    }
                }
                s.units.push_back(std::move(u));
            }
            if (s.units.empty())
                throw bad("line " + std::to_string(h + 1) + ": envelope section without hunks");
        } else {
            while (starts(i, "@@ -")) {
                auto hdr = trim_cr(lines[i]);
                auto close = hdr.find(" @@", 4);
                auto plus = hdr.find(" +", 4);
                if (close == std::string::npos || plus == std::string::npos || plus > close)
                    throw bad("line " + std::to_string(i + 1) + ": malformed hunk header");
                TextHunk t;
                auto o = numbers(std::string_view(hdr).substr(4, plus - 4));
                auto n = numbers(std::string_view(hdr).substr(plus + 2, close - plus - 2));
                if (o.empty() || o.size() > 2 || n.empty() || n.size() > 2)
                    throw bad("line " + std::to_string(i + 1) + ": malformed hunk header");
                t.old_start = o[0], t.old_len = o.size() > 1 ? o[1] : 1;
                t.new_start = n[0], t.new_len = n.size() > 1 ? n[1] : 1;
                t.tail = hdr.substr(close + 3);
                ++i;
                std::size_t olds = 0, news = 0;
                while (olds < t.old_len || news < t.new_len) {
                    if (i >= lines.size())
                        throw bad("unexpected end of file in hunk");
                    std::string l = lines[i];
                    if (l.empty() || l == "\r")
                        l = " " + l; // GNU accepts an empty line as blank context
                    char k = l[0];
                    if (k == '\\') {
                        ++i;
                        continue;
                    }
                    if (k != ' ' && k != '-' && k != '+')
                        throw bad("line " + std::to_string(i + 1) + ": malformed patch");
                    olds += k != '+', news += k != '-';
                    if (olds > t.old_len || news > t.new_len)
                        throw bad("line " + std::to_string(i + 1) + ": hunk is longer than its header says");
                    t.lines.push_back(std::move(l));
                    ++i;
                    if (starts(i, "\\")) {
                        t.no_eol_old |= k != '+';
                        t.no_eol_new |= k != '-';
                        ++i;
                    }
                }
                s.hunks.push_back(std::move(t));
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::string write_section(const PatchSection& s) {
    std::string o;
    if (!s.format.empty()) {
        o += "splinter-patch 1\n";
        o += "--- " + s.old_name + (s.old_hash ? "\txxh3:" + s.old_hash->hex() : "") + "\n";
        o += "+++ " + s.new_name + (s.new_hash ? "\txxh3:" + s.new_hash->hex() : "") + "\n";
        o += "format " + s.format + "\n";
        if (s.exact)
            o += "exact " + std::to_string(s.exact->offset) + " " + std::to_string(s.exact->removed) + " " +
                 std::string(payload_codec()) + " " + encode_payload(s.exact->bytes);
        for (auto& u : s.units) {
            o += "@@ " + std::string(to_string(u.op)) + " " + u.at.to_string() + " " + u.id + " @@\n";
            if (u.before_text)
                o += "- " + *u.before_text + "\n";
            if (u.after_text)
                o += "+ " + *u.after_text + "\n";
            for (auto [which, v] : {std::pair{"before", &u.before}, std::pair{"after", &u.after}})
                if (*v)
                    o += std::string("payload ") + which + " " + std::string(payload_codec()) + " " +
                         encode_payload(**v);
        }
        return o;
    }
    o += "--- " + s.old_name + "\n+++ " + s.new_name + "\n";
    for (auto& h : s.hunks) {
        o += "@@ -" + range(h.old_start, h.old_len) + " +" + range(h.new_start, h.new_len) + " @@" + h.tail +
             "\n";
        // The markers follow the last line of each side.
        std::size_t last_old = h.lines.size(), last_new = h.lines.size();
        for (std::size_t k = 0; k < h.lines.size(); ++k) {
            if (h.lines[k][0] != '+')
                last_old = k;
            if (h.lines[k][0] != '-')
                last_new = k;
        }
        for (std::size_t k = 0; k < h.lines.size(); ++k) {
            o += h.lines[k] + "\n";
            if ((h.no_eol_old && k == last_old) || (h.no_eol_new && k == last_new && k != last_old))
                o += "\\ No newline at end of file\n";
        }
    }
    return o;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> v;
    for (std::size_t at = 0; at < s.size();) {
        auto nl = s.find('\n', at);
        auto end = nl == std::string_view::npos ? s.size() : nl + 1;
        v.emplace_back(s.substr(at, end - at));
        at = end;
    }
    return v;
}

namespace {

struct Pattern {
    std::vector<std::string_view> old;  // the ' ' and '-' lines, without their marker
    std::size_t prefix = 0, suffix = 0; // leading / trailing context
    std::size_t first = 0;              // GNU pch_first: old_start, or old_start + 1 for an empty range
};

Pattern pattern(const TextHunk& h) {
    Pattern p;
    for (auto& l : h.lines)
        if (l[0] != '+')
            p.old.push_back(std::string_view(l).substr(1));
    while (p.prefix < h.lines.size() && h.lines[p.prefix][0] == ' ')
        ++p.prefix;
    while (p.suffix < h.lines.size() && h.lines[h.lines.size() - 1 - p.suffix][0] == ' ')
        ++p.suffix;
    if (p.prefix == h.lines.size()) // context only
        p.suffix = 0;
    p.first = h.old_len ? h.old_start : h.old_start + 1;
    return p;
}

class Locator {
public:
    Locator(const std::vector<std::string>& file, bool ws) : file_(file), ws_(ws) {}

    bool same(std::string_view a, std::string_view b) const {
        a = body(a), b = body(b);
        return ws_ ? squeeze(a) == squeeze(b) : a == b;
    }

    // patch_match: pattern lines [pf, n - sf) against the file from line `base` (1-based).
    bool match(const Pattern& p, std::int64_t base, std::size_t pf, std::size_t sf) const {
        for (std::size_t i = pf; i + sf < p.old.size(); ++i) {
            auto at = base + static_cast<std::int64_t>(i) - 1;
            if (at < 0 || at >= static_cast<std::int64_t>(file_.size()) ||
                !same(file_[static_cast<std::size_t>(at)], p.old[i]))
                return false;
        }
        return true;
    }

    // locate_hunk: the 1-based line where the pattern starts, or 0.
    std::int64_t locate(const Pattern& p, std::int64_t fuzz, std::int64_t& in_offset,
                        std::int64_t frozen) const {
        const auto n = static_cast<std::int64_t>(file_.size());
        const auto pat = static_cast<std::int64_t>(p.old.size());
        const auto pre = static_cast<std::int64_t>(p.prefix), suf = static_cast<std::int64_t>(p.suffix);
        const std::int64_t first_guess = static_cast<std::int64_t>(p.first) + in_offset;
        const std::int64_t context = std::max(pre, suf);
        std::int64_t pf = fuzz + pre - context, sf = fuzz + suf - context;
        const std::int64_t max_where = n - (pat - sf) + 1;
        const std::int64_t min_where = frozen + 1 - (pre - pf);
        const std::int64_t max_pos = max_where - first_guess;
        std::int64_t max_neg = first_guess - min_where;
        const std::int64_t max_offset = std::max(max_pos, max_neg);
        if (!pat)
            return first_guess;
        if (first_guess <= max_neg)
            max_neg = first_guess - 1;
        if (pf < 0 && p.first <= 1) {
            if (sf < 0 && (pat != n || pre < frozen))
                return 0;
            std::int64_t off = 1 - first_guess;
            if (frozen <= pre && off <= max_pos &&
                match(p, first_guess + off, 0, static_cast<std::size_t>(std::max<std::int64_t>(sf, 0)))) {
                in_offset += off;
                return first_guess + off;
            }
            return 0;
        }
        pf = std::max<std::int64_t>(pf, 0);
        if (sf < 0) {
            std::int64_t off = first_guess - (n - pat + 1);
            if (off <= max_neg && match(p, first_guess - off, static_cast<std::size_t>(pf), 0)) {
                in_offset -= off;
                return first_guess - off;
            }
            return 0;
        }
        for (std::int64_t off = 0; off <= max_offset; ++off) {
            if (off <= max_pos &&
                match(p, first_guess + off, static_cast<std::size_t>(pf), static_cast<std::size_t>(sf))) {
                in_offset += off;
                return first_guess + off;
            }
            if (off > 0 && off <= max_neg &&
                match(p, first_guess - off, static_cast<std::size_t>(pf), static_cast<std::size_t>(sf))) {
                in_offset -= off;
                return first_guess - off;
            }
        }
        return 0;
    }

private:
    const std::vector<std::string>& file_;
    bool ws_;
};

} // namespace

TextApplyResult apply_text(std::vector<std::string> file, PatchSection& s, const TextApplyOptions& o) {
    TextApplyResult r;
    Locator loc(file, o.ignore_whitespace);
    std::int64_t in_offset = 0, out_offset = 0, frozen = 0; // frozen: input lines already copied
    std::vector<std::string>& out = r.lines;
    for (std::size_t k = 0; k < s.hunks.size(); ++k) {
        const std::size_t number = k + 1;
        if (r.skipped) {
            r.failed.push_back(k);
            continue;
        }
        Pattern p = pattern(s.hunks[k]);
        const std::int64_t context = static_cast<std::int64_t>(std::max(p.prefix, p.suffix));
        const std::int64_t maxfuzz = std::min(static_cast<std::int64_t>(o.fuzz), context);
        std::int64_t where = 0, fuzz = 0;
        do {
            where = loc.locate(p, fuzz, in_offset, frozen);
            if (k == 0 && !where && o.reversed != TextApplyOptions::Reversed::ignore && !o.reverse_given) {
                s.reverse();
                Pattern q = pattern(s.hunks[k]);
                std::int64_t probe = in_offset;
                std::int64_t w = loc.locate(q, fuzz, probe, frozen);
                if (w && o.reversed == TextApplyOptions::Reversed::assume) {
                    r.messages += "Reversed (or previously applied) patch detected!  Assuming -R.\n";
                    r.reversed = true;
                    p = q, where = w, in_offset = probe;
                } else {
                    s.reverse();
                    if (w) {
                        r.messages += "Reversed (or previously applied) patch detected!  Skipping patch.\n";
                        r.skipped = true;
                    }
                }
            }
        } while (!r.skipped && !where && ++fuzz <= maxfuzz);
        if (!where || fuzz || in_offset)
            r.mismatch = true;
        const std::int64_t newwhere = (where ? where : static_cast<std::int64_t>(p.first)) + out_offset;
        if (r.skipped || !where) {
            r.failed.push_back(k);
            if (!r.skipped)
                r.messages +=
                    "Hunk #" + std::to_string(number) + " FAILED at " + std::to_string(newwhere) + ".\n";
            continue;
        }
        // apply_hunk: copy up to the hunk, then context from the input, additions from the patch.
        const auto& h = s.hunks[k];
        for (; frozen < where - 1; ++frozen)
            out.push_back(file[static_cast<std::size_t>(frozen)]);
        std::size_t last_new = h.lines.size();
        for (std::size_t j = 0; j < h.lines.size(); ++j)
            if (h.lines[j][0] != '-')
                last_new = j;
        for (std::size_t j = 0; j < h.lines.size(); ++j) {
            char kind = h.lines[j][0];
            if (kind == ' ') {
                out.push_back(file[static_cast<std::size_t>(frozen++)]);
            } else if (kind == '-') {
                ++frozen;
            } else {
                out.push_back(h.lines[j].substr(1) + (h.no_eol_new && j == last_new ? "" : "\n"));
            }
        }
        out_offset += static_cast<std::int64_t>(h.new_len) - static_cast<std::int64_t>(h.old_len);
        if (fuzz || in_offset) {
            r.messages += "Hunk #" + std::to_string(number) + " succeeded at " + std::to_string(newwhere);
            if (fuzz)
                r.messages += " with fuzz " + std::to_string(fuzz);
            if (in_offset)
                r.messages +=
                    " (offset " + std::to_string(in_offset) + (in_offset == 1 ? " line)" : " lines)");
            r.messages += ".\n";
        }
    }
    if (r.skipped)
        return r;
    for (; frozen < static_cast<std::int64_t>(file.size()); ++frozen)
        out.push_back(file[static_cast<std::size_t>(frozen)]);
    return r;
}

} // namespace semsplinter
