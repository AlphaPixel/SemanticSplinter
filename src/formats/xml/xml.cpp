// Generic XML (docs/formats/xml.md): expat tokens into an own span tree (D-26), normalized subtree hashes,
// identity matching, KeyedMerge on attributes, SequenceMerge on children, and a splice writer.
#include "../common/splice.hpp"

#include <semsplinter/error.hpp>
#include <semsplinter/formats/xml.hpp>
#include <semsplinter/sequence.hpp>
#include <semsplinter/text.hpp>

#include <expat.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>

namespace semsplinter::xml {

namespace {

constexpr std::size_t npos = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxDepth = 512, kMaxAttrs = 4096, kMaxName = 64 * 1024; // xml.md §8
constexpr std::size_t kGapCap = 256; // similarity pairing per gap (xml.md §5)
constexpr std::string_view kDsigNs = "http://www.w3.org/2000/09/xmldsig#";

using Kind = Node::Kind;

// ---- Encodings (xml.md §7.1): spans stay in the input encoding; generated text is encoded to match ----

enum class Enc : std::uint8_t { utf8, utf16le, utf16be, latin1, ascii };

std::string_view enc_name(Enc e) {
    static constexpr std::string_view n[] = {"utf-8", "utf-16le", "utf-16be", "iso-8859-1", "us-ascii"};
    return n[static_cast<int>(e)];
}

unsigned width(Enc e) {
    return e == Enc::utf16le || e == Enc::utf16be ? 2 : 1;
}

// BOM, then the "<" of a BOM-less UTF-16 document; else 8-bit (refined by the declaration after parsing).
Enc sniff(std::span<const std::byte> b, std::size_t& bom) {
    auto u = [&](std::size_t i) { return i < b.size() ? std::to_integer<unsigned>(b[i]) : 0x100u; };
    bom = 0;
    if (u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF)
        return bom = 3, Enc::utf8;
    if (u(0) == 0xFF && u(1) == 0xFE)
        return bom = 2, Enc::utf16le;
    if (u(0) == 0xFE && u(1) == 0xFF)
        return bom = 2, Enc::utf16be;
    if (u(0) == 0x3C && u(1) == 0)
        return Enc::utf16le;
    if (u(0) == 0 && u(1) == 0x3C)
        return Enc::utf16be;
    return Enc::utf8;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return r;
}

Enc declared(std::string_view name) {
    auto n = lower(name);
    if (n == "iso-8859-1" || n == "latin1" || n == "latin-1" || n == "iso_8859-1" || n == "l1")
        return Enc::latin1;
    if (n == "us-ascii" || n == "ascii")
        return Enc::ascii;
    return Enc::utf8;
}

std::string decode(std::string_view raw, Enc e) {
    if (e == Enc::utf8)
        return std::string(raw);
    if (e == Enc::utf16le || e == Enc::utf16be)
        return line_utf8(as_bytes(raw), Line{0, raw.size(), Eol::none},
                         e == Enc::utf16le ? Encoding::utf16le : Encoding::utf16be);
    std::string r;
    for (unsigned char c : raw) {
        if (c < 0x80) {
            r += static_cast<char>(c);
        } else {
            r += static_cast<char>(0xC0 | (c >> 6));
            r += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return r;
}

// Characters the target cannot hold become character references (xml.md §7).
std::string encode(std::string_view utf8, Enc e) {
    if (e == Enc::utf8)
        return std::string(utf8);
    if (e == Enc::utf16le || e == Enc::utf16be)
        return encode_utf8_as(utf8, e == Enc::utf16le ? Encoding::utf16le : Encoding::utf16be);
    const char32_t max = e == Enc::latin1 ? 0xFF : 0x7F;
    std::string r;
    for (std::size_t i = 0; i < utf8.size();) {
        auto c = static_cast<unsigned char>(utf8[i]);
        std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (std::size_t k = 1; k < n && i + k < utf8.size(); ++k)
            cp = cp << 6 | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += n;
        if (cp <= max)
            r += static_cast<char>(cp);
        else
            r += "&#" + std::to_string(static_cast<std::uint32_t>(cp)) + ";";
    }
    return r;
}

std::string transcode(std::string raw, Enc from, Enc to) {
    return from == to ? raw : encode(decode(raw, from), to);
}

std::string escape_text(std::string_view s) {
    std::string r;
    for (char c : s)
        r += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    return r;
}

std::string escape_attr(std::string_view s, char quote = '"') {
    std::string r;
    for (char c : s) {
        if (c == '&')
            r += "&amp;";
        else if (c == '<')
            r += "&lt;";
        else if (c == quote)
            r += quote == '"' ? "&quot;" : "&apos;";
        else if (c == '\t' || c == '\n' || c == '\r')
            r += "&#" + std::to_string(static_cast<int>(c)) + ";";
        else
            r += c;
    }
    return r;
}

std::string truncated(std::string s, std::size_t n = 200) {
    if (s.size() <= n)
        return s;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    return s.substr(0, n) + "...";
}

// ---- Options (xml.md §4, §5) ----

struct Opts {
    bool ws_significant = false, ignore_comments = false, ignore_pis = false, strict_prefixes = false,
         strict_cdata = false, attr_lines = false;
};

Opts options(const OptionValues& o) {
    Opts r;
    if (auto f = o.find("xml-ws"); f != o.end()) {
        if (f->second != "formatting" && f->second != "significant")
            throw Error(ErrorCategory::invalid_input,
                        "--xml-ws: expected 'formatting' or 'significant', got '" + f->second + "'");
        r.ws_significant = f->second == "significant";
    }
    r.ignore_comments = o.count("xml-ignore-comments") > 0;
    r.ignore_pis = o.count("xml-ignore-pis") > 0;
    r.strict_prefixes = o.count("xml-strict-prefixes") > 0;
    r.strict_cdata = o.count("xml-strict-cdata") > 0;
    r.attr_lines = o.count("dump-attr-lines") > 0;
    return r;
}

// ---- The document ----

struct Doc final : Document {
    ByteSource src;
    Enc enc = Enc::utf8;
    std::size_t bom = 0;
    std::vector<Node> nodes; // nodes[0]: the document; its start tag is the BOM and XML declaration
    struct Decl {
        bool present = false;
        std::string version, encoding;
        int standalone = -1;
    } decl;
    std::vector<Warning> warnings;

    std::string_view bytes(Span s) const {
        return {reinterpret_cast<const char*>(src.bytes().data()) + s.off, s.len};
    }
    std::string utf8(Span s) const { return decode(bytes(s), enc); }
};

std::string decl_text(const Doc& d) {
    if (!d.decl.present)
        return {};
    std::string s = "version=\"" + d.decl.version + "\"";
    if (!d.decl.encoding.empty())
        s += " encoding=\"" + lower(d.decl.encoding) + "\"";
    if (d.decl.standalone >= 0)
        s += std::string(" standalone=\"") + (d.decl.standalone ? "yes" : "no") + "\"";
    return s;
}

bool ignored(const Node& n, const Opts& o) {
    return (n.kind == Kind::comment && o.ignore_comments) || (n.kind == Kind::pi && o.ignore_pis);
}

bool all_ws(std::string_view s) {
    return s.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// ---- Loading (xml.md §2, §8): expat events into the span tree ----

class Loader {
public:
    Loader(Doc& d, const Dialect& dia, const Opts& opt, Limits& budget)
        : d_(d), dia_(dia), opt_(opt), budget_(budget) {}

    void run() {
        auto bytes = d_.src.bytes();
        if (bytes.size() > 0x7FFFFFFF)
            throw Error(ErrorCategory::limit, "xml: inputs over 2 GiB are not supported");
        d_.enc = sniff(bytes, d_.bom);
        std::unique_ptr<std::remove_pointer_t<XML_Parser>, decltype(&XML_ParserFree)> p(
            XML_ParserCreate(nullptr), &XML_ParserFree);
        if (!p)
            throw Error(ErrorCategory::internal, "xml: cannot create the parser");
        p_ = p.get();
        XML_SetUserData(p_, this);
        // Security (SPEC §9, xml.md §8): no parameter entities, no external entities (no handler loads
        // them), and any entity declaration stops the parse. Billion-laughs protection stays at its defaults.
        XML_SetParamEntityParsing(p_, XML_PARAM_ENTITY_PARSING_NEVER);
        XML_SetElementHandler(
            p_,
            [](void* u, const XML_Char* n, const XML_Char** a) {
                guard(u, [&](Loader& l) { l.start(n, a); });
            },
            [](void* u, const XML_Char*) { guard(u, [](Loader& l) { l.end(); }); });
        XML_SetCharacterDataHandler(p_, [](void* u, const XML_Char* s, int len) {
            guard(u, [&](Loader& l) { l.chars(std::string_view(s, static_cast<std::size_t>(len))); });
        });
        XML_SetCdataSectionHandler(
            p_, [](void* u) { guard(u, [](Loader& l) { l.cdata_start(); }); },
            [](void* u) { guard(u, [](Loader& l) { l.cdata_end(); }); });
        XML_SetCommentHandler(p_, [](void* u, const XML_Char* s) {
            guard(u, [&](Loader& l) { l.leaf(Kind::comment, {}, s); });
        });
        XML_SetProcessingInstructionHandler(p_, [](void* u, const XML_Char* t, const XML_Char* s) {
            guard(u, [&](Loader& l) { l.leaf(Kind::pi, t, s); });
        });
        XML_SetXmlDeclHandler(p_, [](void* u, const XML_Char* v, const XML_Char* e, int sa) {
            guard(u, [&](Loader& l) { l.xml_decl(v, e, sa); });
        });
        XML_SetDoctypeDeclHandler(
            p_,
            [](void* u, const XML_Char*, const XML_Char*, const XML_Char*, int) {
                guard(u, [](Loader& l) { l.doctype_start(); });
            },
            [](void* u) { guard(u, [](Loader& l) { l.doctype_end(); }); });
        XML_SetEntityDeclHandler(p_, [](void* u, const XML_Char* name, int, const XML_Char*, int,
                                        const XML_Char*, const XML_Char*, const XML_Char*, const XML_Char*) {
            guard(u, [&](Loader&) {
                throw Error(ErrorCategory::invalid_input,
                            "xml-entity-declared: the DOCTYPE declares entity '" + std::string(name) +
                                "'; entities are disabled (SPEC §9)");
            });
        });
        XML_SetSkippedEntityHandler(p_, [](void* u, const XML_Char* name, int is_param) {
            guard(u, [&](Loader& l) {
                if (!is_param) // an undeclared reference under an unread external DTD: kept as written
                    l.chars("&" + std::string(name) + ";");
            });
        });

        d_.nodes.emplace_back().kind = Kind::document;
        stack_ = {0};
        auto status = XML_Parse(p_, reinterpret_cast<const char*>(bytes.data()),
                                static_cast<int>(bytes.size()), XML_TRUE);
        if (err_)
            throw *err_;
        if (status != XML_STATUS_OK) {
            auto code = XML_GetErrorCode(p_);
            throw Error(code == XML_ERROR_UNKNOWN_ENCODING ? ErrorCategory::unsupported
                                                           : ErrorCategory::invalid_input,
                        "xml: line " + std::to_string(XML_GetCurrentLineNumber(p_)) + ", column " +
                            std::to_string(XML_GetCurrentColumnNumber(p_)) + ": " + XML_ErrorString(code));
        }
        if (width(d_.enc) == 1 && d_.bom == 0 && d_.decl.present)
            d_.enc = declared(d_.decl.encoding);
        if (d_.decl.version == "1.1")
            d_.warnings.push_back({"xml-1.1", "/#decl", "XML 1.1 parsed as XML 1.0 (xml.md Q-XML-8)"});
        Node& doc = d_.nodes[0];
        const auto size = static_cast<std::uint32_t>(bytes.size());
        doc.start_tag = {0, d_.decl.present ? decl_.end() : static_cast<std::uint32_t>(d_.bom)};
        doc.whole = {0, size};
        doc.end_tag = {size, 0};
        finish(0);
    }

private:
    template <class F> static void guard(void* u, F&& f) {
        auto& l = *static_cast<Loader*>(u);
        if (l.err_)
            return;
        try {
            f(l);
        } catch (const Error& e) {
            l.err_ = e;
        } catch (const std::exception& e) {
            l.err_ = Error(ErrorCategory::internal, std::string("xml: ") + e.what());
        }
        if (l.err_)
            XML_StopParser(l.p_, XML_FALSE);
    }

    Span here() const {
        auto i = XML_GetCurrentByteIndex(p_);
        auto c = XML_GetCurrentByteCount(p_);
        return {static_cast<std::uint32_t>(i < 0 ? 0 : i), static_cast<std::uint32_t>(c < 0 ? 0 : c)};
    }

    std::uint32_t add(Kind k, std::size_t extra = 0) {
        budget_.charge(sizeof(Node) + extra);
        auto i = static_cast<std::uint32_t>(d_.nodes.size());
        auto& n = d_.nodes.emplace_back();
        n.kind = k;
        n.parent = stack_.back();
        d_.nodes[n.parent].children.push_back(i);
        return i;
    }

    std::string lookup(const std::string& prefix) const {
        if (prefix == "xml")
            return std::string(kXmlNs);
        for (auto it = ns_.rbegin(); it != ns_.rend(); ++it)
            if (it->first == prefix)
                return it->second;
        if (prefix.empty())
            return {};
        throw Error(ErrorCategory::invalid_input, "xml: namespace prefix '" + prefix + "' is not bound");
    }

    QName resolve(const std::string& qname, bool attribute) const {
        auto colon = qname.find(':');
        QName q;
        if (colon != std::string::npos)
            q.prefix = qname.substr(0, colon), q.local = qname.substr(colon + 1);
        else
            q.local = qname;
        if (!q.prefix.empty() || !attribute) // unprefixed attributes are in no namespace
            q.uri = lookup(q.prefix);
        return q;
    }

    // The code unit at byte offset i (0 past the end); markup delimiters are ASCII in every encoding.
    char32_t unit(std::uint32_t i) const {
        auto b = d_.src.bytes();
        if (i + width(d_.enc) > b.size())
            return 0;
        if (width(d_.enc) == 1)
            return std::to_integer<unsigned>(b[i]);
        unsigned x = std::to_integer<unsigned>(b[i]), y = std::to_integer<unsigned>(b[i + 1]);
        return d_.enc == Enc::utf16le ? x | y << 8 : y | x << 8;
    }

    // The attribute spans of a start tag: lead whitespace, name through the closing quote, the quote.
    void scan_tag(Node& n, std::size_t count) {
        const unsigned w = width(d_.enc);
        auto at = [&](std::uint32_t i) { return unit(i); };
        auto ws = [](char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
        std::uint32_t i = n.start_tag.off + w;
        const std::uint32_t end = n.start_tag.end();
        while (i < end && !ws(at(i)) && at(i) != '/' && at(i) != '>')
            i += w;
        n.name_end = n.attrs_end = i;
        n.attrs.resize(count);
        std::size_t k = 0;
        for (;;) {
            const std::uint32_t s = i;
            while (i < end && ws(at(i)))
                i += w;
            if (i >= end || at(i) == '/' || at(i) == '>')
                break;
            if (k == count)
                throw Error(ErrorCategory::internal,
                            "xml: start tag holds more attributes than the parser saw");
            const std::uint32_t name = i;
            while (i < end && at(i) != '"' && at(i) != '\'')
                i += w;
            const char32_t q = i < end ? at(i) : '"';
            i += w;
            while (i < end && at(i) != q)
                i += w;
            i += w;
            n.attrs[k].lead_ws = {s, name - s};
            n.attrs[k].raw = {name, i - name};
            n.attrs[k].quote = static_cast<char>(q);
            n.attrs_end = i;
            ++k;
        }
        if (k != count)
            throw Error(ErrorCategory::internal, "xml: start tag holds fewer attributes than the parser saw");
    }

    void start(const XML_Char* name, const XML_Char** atts) {
        open_text_ = npos;
        if (stack_.size() > kMaxDepth)
            throw Error(ErrorCategory::limit, "xml: elements nested deeper than 512 (xml.md §8)");
        const auto spec = static_cast<std::size_t>(XML_GetSpecifiedAttributeCount(p_) / 2);
        if (spec > kMaxAttrs)
            throw Error(ErrorCategory::limit, "xml: more than 4096 attributes on one element (xml.md §8)");
        if (std::strlen(name) > kMaxName)
            throw Error(ErrorCategory::limit, "xml: a name longer than 64 KiB (xml.md §8)");
        const auto k = add(Kind::element);
        Node& n = d_.nodes[k];
        n.qname = name;
        n.start_tag = here();
        scan_tag(n, spec);
        ns_marks_.push_back(ns_.size());
        std::size_t bytes = n.qname.size();
        for (std::size_t i = 0; i < spec; ++i) {
            Attr& a = n.attrs[i];
            a.qname = atts[2 * i];
            a.value = atts[2 * i + 1];
            bytes += a.qname.size() + a.value.size() + sizeof(Attr);
            if (a.qname == "xmlns" || a.qname.starts_with("xmlns:")) {
                a.ns_decl = true;
                a.name = {std::string(kXmlnsNs), a.qname.size() > 5 ? a.qname.substr(6) : "", "xmlns"};
                ns_.emplace_back(a.name.local, a.value);
            }
        }
        budget_.charge(bytes);
        n.name = resolve(n.qname, false);
        std::set<std::pair<std::string, std::string>> seen;
        n.preserve = d_.nodes[n.parent].preserve;
        for (auto& a : n.attrs) {
            if (a.ns_decl)
                continue;
            a.name = resolve(a.qname, true);
            if (!seen.emplace(a.name.uri, a.name.local).second)
                throw Error(ErrorCategory::invalid_input, "xml: duplicate attribute '" + a.qname + "'");
            if (a.name.uri == kXmlNs && a.name.local == "space")
                n.preserve = a.value == "preserve";
        }
        stack_.push_back(k);
    }

    void end() {
        open_text_ = npos;
        const Span et = here();
        const auto k = stack_.back();
        Node& n = d_.nodes[k];
        n.end_tag = et.len && et.off >= n.start_tag.end() ? et : Span{n.start_tag.end(), 0};
        n.whole = {n.start_tag.off, n.end_tag.end() - n.start_tag.off};
        finish(k);
        stack_.pop_back();
        ns_.resize(ns_marks_.back());
        ns_marks_.pop_back();
    }

    void chars(std::string_view s) {
        if (stack_.size() == 1)
            return; // outside the root: whitespace, kept as gaps
        budget_.charge(s.size());
        const Span sp = here();
        if (in_cdata_) {
            d_.nodes[cdata_].text += s;
            return;
        }
        if (open_text_ == npos) {
            open_text_ = add(Kind::text);
            d_.nodes[open_text_].whole = sp;
        }
        Node& t = d_.nodes[open_text_];
        t.whole.len = sp.end() - t.whole.off;
        t.text += s;
    }

    void cdata_start() {
        open_text_ = npos;
        cdata_ = add(Kind::cdata);
        d_.nodes[cdata_].whole.off = here().off;
        in_cdata_ = true;
    }

    void cdata_end() {
        Node& n = d_.nodes[cdata_];
        n.whole.len = here().end() - n.whole.off;
        in_cdata_ = false;
    }

    void leaf(Kind kind, const char* target, const char* data) {
        if (in_doctype_)
            return; // declarations inside the DOCTYPE belong to its raw span
        open_text_ = npos;
        const auto k = add(kind, std::strlen(data));
        Node& n = d_.nodes[k];
        n.whole = here();
        n.text = data;
        if (target)
            n.qname = target;
    }

    void xml_decl(const char* version, const char* encoding, int standalone) {
        d_.decl = {true, version ? version : "", encoding ? encoding : "", standalone};
        decl_ = here();
    }

    // Expat reports the DOCTYPE late; it starts at the first '<' after the previous top-level node.
    void doctype_start() {
        auto& top = d_.nodes[0].children;
        std::uint32_t i = top.empty() ? (d_.decl.present ? decl_.end() : static_cast<std::uint32_t>(d_.bom))
                                      : d_.nodes[top.back()].whole.end();
        while (i < d_.src.size() && unit(i) != '<')
            i += width(d_.enc);
        doctype_off_ = i;
        in_doctype_ = true;
    }

    void doctype_end() {
        in_doctype_ = false;
        const auto k = add(Kind::doctype);
        Node& n = d_.nodes[k];
        n.whole = {doctype_off_, here().end() - doctype_off_};
        n.text = d_.utf8(n.whole);
    }

    Hash128 leaf_hash(const Node& n) const {
        Hasher h;
        switch (n.kind) {
        case Kind::text:
        case Kind::cdata:
            h.add(opt_.strict_cdata && n.kind == Kind::cdata ? "c" : "t").add(n.text);
            break;
        case Kind::comment:
            h.add("!").add(n.text);
            break;
        case Kind::pi:
            h.add("?").add(n.qname).add(n.text);
            break;
        default:
            h.add("D").add(n.text);
            break;
        }
        return h.digest();
    }

    // Formatting whitespace, gaps, the subtree hash, and the free prefixes of a closed element.
    void finish(std::uint32_t k) {
        Node& n = d_.nodes[k];
        auto& nodes = d_.nodes;
        if (n.kind == Kind::element) {
            const bool mixed = std::any_of(n.children.begin(), n.children.end(), [&](std::uint32_t c) {
                return nodes[c].kind == Kind::cdata ||
                       (nodes[c].kind == Kind::text && !all_ws(nodes[c].text));
            });
            if (!mixed && !opt_.ws_significant && !n.preserve && !dia_.significant_whitespace(n))
                std::erase_if(n.children, [&](std::uint32_t c) { return nodes[c].kind == Kind::text; });
        }
        std::uint32_t at = n.start_tag.end();
        for (auto c : n.children) {
            Node& x = nodes[c];
            if (x.kind != Kind::element)
                x.hash = leaf_hash(x);
            x.lead_ws = {at, x.whole.off - at};
            at = x.whole.end();
        }
        n.trail_ws = {at, n.end_tag.off - at};

        Hasher h;
        h.add(n.kind == Kind::document ? "doc" : "e").add(n.name.uri).add(n.name.local);
        if (opt_.strict_prefixes)
            h.add(n.name.prefix);
        std::vector<const Attr*> as;
        for (auto& a : n.attrs)
            if (!a.ns_decl || opt_.strict_prefixes)
                as.push_back(&a);
        std::sort(as.begin(), as.end(), [](const Attr* x, const Attr* y) {
            return std::tie(x->ns_decl, x->name.uri, x->name.local) <
                   std::tie(y->ns_decl, y->name.uri, y->name.local);
        });
        h.add(std::uint64_t{as.size()});
        for (auto* a : as) {
            h.add(a->name.uri).add(a->name.local).add(a->value);
            if (opt_.strict_prefixes)
                h.add(a->name.prefix);
        }
        for (auto c : n.children)
            if (!ignored(nodes[c], opt_))
                h.add(nodes[c].hash.hi).add(nodes[c].hash.lo);
        n.hash = h.digest();

        std::map<std::string, std::string> used;
        auto use = [&](const std::string& prefix, const std::string& uri) {
            if (prefix != "xml")
                used.emplace(prefix, uri);
        };
        if (n.kind == Kind::element) {
            use(n.name.prefix, n.name.uri);
            for (auto& a : n.attrs)
                if (!a.ns_decl && !a.name.prefix.empty())
                    use(a.name.prefix, a.name.uri);
        }
        for (auto c : n.children)
            for (auto& [p, u] : nodes[c].free_ns)
                use(p, u);
        for (auto& a : n.attrs)
            if (a.ns_decl)
                used.erase(a.name.local);
        n.free_ns.assign(used.begin(), used.end());
    }

    Doc& d_;
    const Dialect& dia_;
    const Opts& opt_;
    Limits& budget_;
    XML_Parser p_ = nullptr;
    std::optional<Error> err_;
    std::vector<std::uint32_t> stack_;
    std::vector<std::pair<std::string, std::string>> ns_; // (prefix, uri) declarations in scope
    std::vector<std::size_t> ns_marks_;
    std::size_t open_text_ = npos;
    std::uint32_t cdata_ = 0, doctype_off_ = 0;
    bool in_cdata_ = false, in_doctype_ = false;
    Span decl_;
};

std::unique_ptr<Doc> load_doc(ByteSource src, const Dialect& dia, const Opts& opt, Limits& budget) {
    auto d = std::make_unique<Doc>();
    d->src = std::move(src);
    Loader(*d, dia, opt, budget).run();
    return d;
}

// ---- Identity (xml.md §3) ----

// One side's child list at one level, with identity keys and address steps.
struct Kids {
    const Doc* d = nullptr;
    std::vector<std::uint32_t> n;
    std::vector<std::string> key, step;
    const Node& at(std::size_t i) const { return d->nodes[n[i]]; }
};

Kids kids_of(const Doc& d, std::uint32_t parent, const Opts* filter) {
    Kids k;
    k.d = &d;
    for (auto c : d.nodes[parent].children)
        if (!filter || !ignored(d.nodes[c], *filter))
            k.n.push_back(c);
    return k;
}

const Attr* find_attr(const Node& n, std::string_view uri, std::string_view local) {
    for (auto& a : n.attrs)
        if (!a.ns_decl && a.name.uri == uri && a.name.local == local)
            return &a;
    return nullptr;
}

std::string label(const Node& x) {
    switch (x.kind) {
    case Kind::element:
        return x.qname;
    case Kind::text:
    case Kind::cdata:
        return "text()";
    case Kind::comment:
        return "comment()";
    case Kind::pi:
        return "processing-instruction(" + x.qname + ")";
    default:
        return "#doctype";
    }
}

std::string name_key(const Node& x) {
    return x.name.uri + '\x1f' + x.name.local;
}

// Keys from the dialect's id rules that are unique on every side, and the address steps (xml.md §3.1).
void identify(std::initializer_list<Kids*> sides, const Dialect& dia, const Node& parent) {
    auto rules = dia.id_rules(parent);
    std::vector<char> valid(rules.size(), 1);
    for (std::size_t r = 0; r < rules.size(); ++r)
        for (auto* s : sides) {
            std::set<std::pair<std::string, std::string>> seen;
            for (std::size_t i = 0; i < s->n.size() && valid[r]; ++i)
                if (s->at(i).kind == Kind::element)
                    if (auto* a = find_attr(s->at(i), rules[r].uri, rules[r].local))
                        valid[r] = seen.emplace(name_key(s->at(i)), a->value).second;
        }
    for (auto* s : sides) {
        s->key.assign(s->n.size(), {});
        s->step.assign(s->n.size(), {});
        std::map<std::string, std::size_t> count, pos;
        for (std::size_t i = 0; i < s->n.size(); ++i)
            ++count[label(s->at(i))];
        for (std::size_t i = 0; i < s->n.size(); ++i) {
            const Node& x = s->at(i);
            const auto lab = label(x);
            if (x.kind == Kind::element)
                for (std::size_t r = 0; r < rules.size(); ++r)
                    if (auto* a = valid[r] ? find_attr(x, rules[r].uri, rules[r].local) : nullptr) {
                        s->key[i] = name_key(x) + '\x1f' + std::string(rules[r].uri) + '\x1f' +
                                    std::string(rules[r].local) + '\x1f' + a->value;
                        s->step[i] = lab + "[@" + a->qname + "=" + pointer_escape(a->value) + "]";
                        break;
                    }
            const auto p = ++pos[lab];
            if (s->step[i].empty())
                s->step[i] = count[lab] > 1 ? lab + "[" + std::to_string(p) + "]" : lab;
        }
    }
}

// The text of a text-only element is its value, not its identity (xml.md §3 table), so only mixed content
// contributes words.
std::vector<std::string> features(const Node& n, const Doc& d) {
    std::vector<std::string> f;
    for (auto& a : n.attrs)
        if (!a.ns_decl)
            f.push_back("@" + a.name.uri + "|" + a.name.local + "=" + a.value);
    const bool mixed = std::any_of(n.children.begin(), n.children.end(),
                                   [&](std::uint32_t c) { return d.nodes[c].kind == Kind::element; });
    for (auto c : n.children) {
        const Node& x = d.nodes[c];
        if (x.kind == Kind::element) {
            f.push_back("<" + name_key(x));
        } else if (mixed && (x.kind == Kind::text || x.kind == Kind::cdata)) {
            std::istringstream words(x.text);
            for (std::string w; words >> w;)
                f.push_back("#" + w);
        }
    }
    std::sort(f.begin(), f.end());
    return f;
}

// Dice coefficient over attribute pairs, text words, and child names (xml.md §3.1).
double similarity(const Node& a, const Doc& da, const Node& b, const Doc& db) {
    auto fa = features(a, da), fb = features(b, db);
    if (fa.empty() && fb.empty())
        return 1.0;
    std::size_t common = 0;
    for (std::size_t i = 0, j = 0; i < fa.size() && j < fb.size();) {
        if (fa[i] == fb[j])
            ++common, ++i, ++j;
        else if (fa[i] < fb[j])
            ++i;
        else
            ++j;
    }
    return 2.0 * static_cast<double>(common) / static_cast<double>(fa.size() + fb.size());
}

struct Stats {
    std::uint64_t by_id = 0, by_hash = 0, by_similarity = 0;
    double min_confidence = 1.0;
};

struct Pairing {
    std::vector<std::size_t> x2y; // npos: unmatched
    std::vector<double> conf;     // 1 unless similarity paired it
};

bool textlike(const Node& n) {
    return n.kind == Kind::text || n.kind == Kind::cdata;
}

// Matches X's children to Y's: ids, then subtree hashes, then in the gaps same-name elements: by name when
// it is unique among the siblings on both sides (the address step is then the bare name), else by similarity.
Pairing pair_kids(const Kids& X, const Kids& Y, Stats* st) {
    Pairing p{std::vector<std::size_t>(X.n.size(), npos), std::vector<double>(X.n.size(), 1.0)};
    std::vector<char> taken(Y.n.size(), 0);
    std::map<std::string, std::size_t> names_x, names_y;
    for (std::size_t i = 0; i < X.n.size(); ++i)
        ++names_x[name_key(X.at(i))];
    for (std::size_t j = 0; j < Y.n.size(); ++j)
        ++names_y[name_key(Y.at(j))];
    std::map<std::string_view, std::size_t> ids;
    for (std::size_t j = 0; j < Y.n.size(); ++j)
        if (!Y.key[j].empty())
            ids.emplace(Y.key[j], j);
    for (std::size_t i = 0; i < X.n.size(); ++i)
        if (!X.key[i].empty())
            if (auto f = ids.find(X.key[i]); f != ids.end()) {
                p.x2y[i] = f->second, taken[f->second] = 1;
                if (st)
                    ++st->by_id;
            }
    std::map<Hash128, Token> tokens;
    Token unique = Token{1} << 63;
    auto token = [&](const Kids& k, std::size_t i) {
        return k.key[i].empty() ? tokens.emplace(k.at(i).hash, tokens.size()).first->second : unique++;
    };
    std::vector<Token> tx, ty;
    for (std::size_t i = 0; i < X.n.size(); ++i)
        tx.push_back(token(X, i));
    for (std::size_t j = 0; j < Y.n.size(); ++j)
        ty.push_back(token(Y, j));
    auto m = diff_match(tx, ty, DiffAlgorithm::histogram);
    for (auto [i, j] : m) {
        p.x2y[i] = j, taken[j] = 1;
        if (st)
            ++st->by_hash;
    }
    std::size_t px = 0, py = 0;
    auto gap = [&](std::size_t x1, std::size_t y1) {
        std::vector<std::size_t> xs, ys;
        for (auto i = px; i < x1; ++i)
            if (p.x2y[i] == npos && X.key[i].empty())
                xs.push_back(i);
        for (auto j = py; j < y1; ++j)
            if (!taken[j] && Y.key[j].empty())
                ys.push_back(j);
        // ponytail: larger gaps stay unpaired (erase plus insert); position-order fallback if they matter.
        if (xs.size() > kGapCap || ys.size() > kGapCap)
            return;
        std::size_t j0 = 0;
        for (auto i : xs)
            for (auto k = j0; k < ys.size(); ++k) {
                const Node &a = X.at(i), &b = Y.at(ys[k]);
                if (a.kind != b.kind && !(textlike(a) && textlike(b)))
                    continue;
                double s = 1.0;
                if (a.kind == Kind::element) {
                    if (name_key(a) != name_key(b))
                        continue;
                    if (names_x[name_key(a)] == 1 && names_y[name_key(b)] == 1) {
                        p.x2y[i] = ys[k], taken[ys[k]] = 1, j0 = k + 1;
                        break;
                    }
                    s = similarity(a, *X.d, b, *Y.d);
                    if (s < 0.5)
                        continue;
                    if (st)
                        ++st->by_similarity, st->min_confidence = std::min(st->min_confidence, s);
                }
                p.x2y[i] = ys[k], p.conf[i] = s, taken[ys[k]] = 1, j0 = k + 1;
                break;
            }
    };
    for (auto [i, j] : m) {
        gap(i, j);
        px = i + 1, py = j + 1;
    }
    gap(X.n.size(), Y.n.size());
    return p;
}

// Indices (into `ys`) outside one longest increasing subsequence: the moved elements.
std::vector<char> moved_flags(const std::vector<std::size_t>& ys) {
    std::vector<std::size_t> tails, tail_idx, prev(ys.size(), npos);
    for (std::size_t i = 0; i < ys.size(); ++i) {
        auto pos =
            static_cast<std::size_t>(std::lower_bound(tails.begin(), tails.end(), ys[i]) - tails.begin());
        if (pos == tails.size())
            tails.push_back(ys[i]), tail_idx.push_back(i);
        else
            tails[pos] = ys[i], tail_idx[pos] = i;
        prev[i] = pos ? tail_idx[pos - 1] : npos;
    }
    std::vector<char> moved(ys.size(), 1);
    for (auto i = tail_idx.empty() ? npos : tail_idx.back(); i != npos; i = prev[i])
        moved[i] = 0;
    return moved;
}

// ---- Diff (xml.md §5) ----

std::string kind_name(const Node& n) {
    switch (n.kind) {
    case Kind::element:
        return "element";
    case Kind::text:
    case Kind::cdata:
        return "text";
    case Kind::comment:
        return "comment";
    case Kind::pi:
        return "pi";
    default:
        return "doctype";
    }
}

std::string summary(const Node& n) {
    switch (n.kind) {
    case Kind::element: {
        std::string s = "<" + n.qname;
        for (auto& a : n.attrs)
            s += " " + a.qname + "=\"" + escape_attr(a.value) + "\"";
        return truncated(s + ">", 120);
    }
    case Kind::pi:
        return truncated(n.qname + " " + n.text);
    default:
        return truncated(n.text);
    }
}

Value value_of(const std::string& text, const Hash128& h) {
    return {truncated(text), h};
}

std::uint64_t subtree_nodes(const Doc& d, std::uint32_t n) {
    std::uint64_t count = 0;
    std::vector<std::uint32_t> todo{n};
    while (!todo.empty()) {
        auto k = todo.back();
        todo.pop_back();
        ++count;
        todo.insert(todo.end(), d.nodes[k].children.begin(), d.nodes[k].children.end());
    }
    return count;
}

std::string attr_key(const Attr& a) {
    return a.ns_decl ? "n" + a.name.local : "a" + a.name.uri + '\x1f' + a.name.local;
}

struct Differ {
    const Dialect& dia;
    const Opts& opt;
    Stats* stats = nullptr;

    void attrs(const Doc& A, std::uint32_t a, const Doc& B, std::uint32_t b, const std::string& path,
               Side side, ChangeSet& out, double conf = 1.0) const {
        std::map<std::string, std::pair<const Attr*, const Attr*>> m;
        for (auto& x : A.nodes[a].attrs)
            if (!x.ns_decl || opt.strict_prefixes)
                m[attr_key(x)].first = &x;
        for (auto& y : B.nodes[b].attrs)
            if (!y.ns_decl || opt.strict_prefixes)
                m[attr_key(y)].second = &y;
        for (auto& [k, xy] : m) {
            auto [x, y] = xy;
            if (x && y && x->value == y->value && (!opt.strict_prefixes || x->qname == y->qname))
                continue;
            const Attr* any = x ? x : y;
            auto c = make_change(side,
                                 !x   ? Op::insert
                                 : !y ? Op::erase
                                      : Op::update,
                                 {path + "/@" + any->qname}, any->ns_decl ? "namespace" : "attribute");
            if (x)
                c.before = value_of(x->value, hash128(x->value));
            if (y)
                c.after = value_of(y->value, hash128(y->value));
            if (conf < 1.0)
                c.confidence = conf;
            out.push_back(std::move(c));
        }
    }

    // A matched element pair still to compare. Work lists, not recursion: nesting reaches 512 (xml.md §8).
    struct Task {
        const Doc* A;
        std::uint32_t a;
        const Doc* B;
        std::uint32_t b;
        std::string path;
        double conf; // the lowest pairing confidence on the way down
    };

    void drain(std::vector<Task>& todo, Side side, ChangeSet& out) const {
        while (!todo.empty()) {
            Task t = std::move(todo.back());
            todo.pop_back();
            attrs(*t.A, t.a, *t.B, t.b, t.path, side, out, t.conf);
            Kids X = kids_of(*t.A, t.a, &opt), Y = kids_of(*t.B, t.b, &opt);
            identify({&X, &Y}, dia, t.A->nodes[t.a]);
            level(X, Y, pair_kids(X, Y, stats), t.path, side, out, nullptr, todo, t.conf);
        }
    }

    // Inserts, erases, moves, and updates of one child list; changed element pairs go to `todo`.
    // `skip`: X indices merged elsewhere.
    void level(const Kids& X, const Kids& Y, const Pairing& p, const std::string& path, Side side,
               ChangeSet& out, const std::set<std::size_t>* skip, std::vector<Task>& todo,
               double conf) const {
        auto push = [&](Change c) {
            if (conf < 1.0)
                c.confidence = conf;
            out.push_back(std::move(c));
        };
        std::vector<std::size_t> keyed_x, keyed_y;
        std::vector<char> taken(Y.n.size(), 0);
        for (std::size_t i = 0; i < X.n.size(); ++i)
            if (p.x2y[i] != npos) {
                taken[p.x2y[i]] = 1;
                if (!X.key[i].empty())
                    keyed_x.push_back(i), keyed_y.push_back(p.x2y[i]);
            }
        std::set<std::size_t> moved;
        auto mf = moved_flags(keyed_y);
        for (std::size_t k = 0; k < mf.size(); ++k)
            if (mf[k])
                moved.insert(keyed_x[k]);
        for (std::size_t i = 0; i < X.n.size(); ++i) {
            const Node& x = X.at(i);
            const std::string at = path + "/" + X.step[i];
            const auto j = p.x2y[i];
            if (j == npos) {
                auto c = make_change(side, Op::erase, {at}, kind_name(x));
                c.before = value_of(summary(x), x.hash);
                if (x.kind == Kind::element)
                    c.metrics["subtree_nodes"] = subtree_nodes(*X.d, X.n[i]);
                push(std::move(c));
                continue;
            }
            if (skip && skip->count(i))
                continue;
            const Node& y = Y.at(j);
            if (moved.count(i)) {
                auto c = make_change(side, Op::move, {path + "/" + Y.step[j]}, kind_name(x));
                c.from = Address{at};
                push(std::move(c));
            }
            if (x.hash == y.hash)
                continue;
            if (x.kind == Kind::element) {
                todo.push_back({X.d, X.n[i], Y.d, Y.n[j], at, std::min(conf, p.conf[i])});
            } else {
                auto c = make_change(side, Op::update, {at}, kind_name(x));
                c.before = value_of(summary(x), x.hash);
                c.after = value_of(summary(y), y.hash);
                push(std::move(c));
            }
        }
        for (std::size_t j = 0; j < Y.n.size(); ++j)
            if (!taken[j]) {
                const Node& y = Y.at(j);
                auto c = make_change(side, Op::insert, {path + "/" + Y.step[j]}, kind_name(y));
                c.after = value_of(summary(y), y.hash);
                if (y.kind == Kind::element)
                    c.metrics["subtree_nodes"] = subtree_nodes(*Y.d, Y.n[j]);
                push(std::move(c));
            }
    }

    ChangeSet document(const Doc& A, const Doc& B, Side side) const {
        ChangeSet out;
        auto da = decl_text(A), db = decl_text(B);
        if (da != db) {
            auto c = make_change(side,
                                 da.empty()   ? Op::insert
                                 : db.empty() ? Op::erase
                                              : Op::update,
                                 {"/#decl"}, "declaration");
            if (!da.empty())
                c.before = value_of(da, hash128(da));
            if (!db.empty())
                c.after = value_of(db, hash128(db));
            out.push_back(std::move(c));
        }
        std::vector<Task> todo{{&A, 0, &B, 0, "", 1.0}};
        drain(todo, side, out);
        return out;
    }
};

// ---- Merge (xml.md §6) ----

struct PlanEl;
struct Ref {
    Side side = Side::ours;
    std::uint32_t n = 0;
    const PlanEl* plan = nullptr; // a merged element; null: the source node verbatim
};
struct Slot {
    std::vector<Ref> ours, theirs, base; // without a conflict, `ours` is the content
    int conflict = -1;
};
struct AttrRow {
    std::array<int, 3> at{-1, -1, -1}; // attribute index per side (base, ours, theirs)
    std::optional<Side> take;
    int conflict = -1;
};
// A merged element: ours' tags and whitespace, merged attributes, and merged children.
struct PlanEl {
    std::uint32_t b = 0, o = 0, t = 0;
    std::vector<AttrRow> attrs;
    std::vector<Slot> kids;
};

std::size_t side_index(Side s) {
    return static_cast<std::size_t>(s);
}

struct Merger {
    std::array<const Doc*, 3> d;
    const Dialect& dia;
    const Opts& opt;
    Differ df;
    ChangeSet co, ct;
    struct Pending {
        Address at;
        std::vector<std::string> po, pt; // where ours' and theirs' competing changes live
        std::string reason;
    };
    std::vector<Pending> pend;
    std::deque<PlanEl> plans; // stable addresses

    // An element both sides changed, to merge into `p` (a work list; nesting reaches 512, xml.md §8).
    struct Task {
        std::uint32_t b, o, t;
        std::string path;
        PlanEl* p;
        double conf_o = 1.0, conf_t = 1.0; // pairing confidence on the way down, per side
    };

    const PlanEl* run() {
        std::vector<Task> todo{{0, 0, 0, "", &plans.emplace_back()}};
        while (!todo.empty()) {
            Task k = std::move(todo.back());
            todo.pop_back();
            element(k, todo);
        }
        return &plans.front();
    }

    void element(const Task& task, std::vector<Task>& todo) {
        PlanEl& p = *task.p;
        const auto b = task.b, o = task.o, t = task.t;
        const std::string& path = task.path;
        p.b = b, p.o = o, p.t = t;
        const Node &nb = d[0]->nodes[b], &no = d[1]->nodes[o], &nt = d[2]->nodes[t];

        // Attributes and namespace declarations: KeyedMerge (xml.md §6.1, §6.2).
        using KV = std::pair<std::string, std::string>;
        auto items = [](const Node& n) {
            std::vector<KV> v;
            for (auto& a : n.attrs)
                v.emplace_back(attr_key(a), a.value);
            return v;
        };
        const auto ib = items(nb), io = items(no), it = items(nt);
        for (auto& row : keyed_merge<std::string, std::string>(ib, io, it)) {
            AttrRow r;
            for (auto [k, idx] : {std::pair{0, row.base}, std::pair{1, row.ours}, std::pair{2, row.theirs}})
                if (idx)
                    r.at[static_cast<std::size_t>(k)] = static_cast<int>(*idx);
            r.take = row.outcome.take;
            if (row.outcome.rule == MergeRule::conflict) {
                const Attr& a = row.ours   ? no.attrs[*row.ours]
                                : row.base ? nb.attrs[*row.base]
                                           : nt.attrs[*row.theirs];
                const std::string ap = path + "/@" + a.qname;
                pend.push_back({{ap}, {ap}, {ap}, row.outcome.reason});
                r.conflict = static_cast<int>(pend.size() - 1);
            }
            p.attrs.push_back(r);
        }
        df.attrs(*d[0], b, *d[1], o, path, Side::ours, co, task.conf_o);
        df.attrs(*d[0], b, *d[2], t, path, Side::theirs, ct, task.conf_t);

        // Children: SequenceMerge over matched identities (xml.md §6.3, §6.4).
        Kids kb = kids_of(*d[0], b, nullptr), ko = kids_of(*d[1], o, nullptr),
             kt = kids_of(*d[2], t, nullptr);
        identify({&kb, &ko, &kt}, dia, nb);
        const Pairing po = pair_kids(kb, ko, nullptr), pt = pair_kids(kb, kt, nullptr);
        auto seq = [](const Kids& k) {
            std::vector<SeqItem> v(k.n.size());
            for (std::size_t i = 0; i < k.n.size(); ++i) {
                v[i].content = k.at(i).hash;
                if (!k.key[i].empty())
                    v[i].id = hash128(k.key[i]);
            }
            return v;
        };
        auto sb = seq(kb), so = seq(ko), st = seq(kt);
        for (std::size_t i = 0; i < kb.n.size(); ++i) {
            if (!kb.key[i].empty() || (po.x2y[i] == npos && pt.x2y[i] == npos))
                continue;
            const Hash128 id = Hasher().add("#").add(std::uint64_t{i}).digest();
            sb[i].id = id;
            if (po.x2y[i] != npos)
                so[po.x2y[i]].id = id;
            if (pt.x2y[i] != npos)
                st[pt.x2y[i]].id = id;
        }
        std::set<std::size_t> recursed, low;
        std::map<std::size_t, const PlanEl*> merged; // by ours index
        SeqMergeOptions sopt;
        sopt.insertion_order_insignificant = dia.unordered(no);
        sopt.recurse = [&](std::optional<std::size_t> bi, std::size_t oi, std::size_t ti) {
            if (!bi || kb.at(*bi).kind != Kind::element || ko.at(oi).kind != Kind::element ||
                kt.at(ti).kind != Kind::element)
                return false;
            if (std::min(po.conf[*bi], pt.conf[*bi]) < 0.8) { // xml.md §3.1: no two-sided auto-merge
                low.insert(*bi);
                return false;
            }
            PlanEl* child = &plans.emplace_back();
            todo.push_back({kb.n[*bi], ko.n[oi], kt.n[ti], path + "/" + kb.step[*bi], child,
                            std::min(task.conf_o, po.conf[*bi]), std::min(task.conf_t, pt.conf[*bi])});
            merged[oi] = child;
            recursed.insert(*bi);
            return true;
        };
        auto refs = [](Side s, const Kids& k, const std::vector<std::size_t>& idx) {
            std::vector<Ref> r;
            for (auto i : idx)
                r.push_back({s, k.n[i], nullptr});
            return r;
        };
        for (auto& c : sequence_merge(sb, so, st, sopt)) {
            Slot s;
            switch (c.kind) {
            case SeqKind::stable:
            case SeqKind::ours:
            case SeqKind::identical:
                s.ours = refs(Side::ours, ko, c.ours);
                break;
            case SeqKind::theirs:
                s.ours = refs(Side::theirs, kt, c.theirs);
                break;
            case SeqKind::both_inserted:
                s.ours = refs(Side::ours, ko, c.ours);
                for (auto& r : refs(Side::theirs, kt, c.theirs))
                    s.ours.push_back(r);
                break;
            case SeqKind::recursed:
                s.ours = {Ref{Side::ours, ko.n[c.ours[0]], merged.at(c.ours[0])}};
                break;
            case SeqKind::conflict: {
                s.ours = refs(Side::ours, ko, c.ours);
                s.theirs = refs(Side::theirs, kt, c.theirs);
                s.base = refs(Side::base, kb, c.base);
                Pending pd;
                pd.reason = c.base.size() == 1 && low.count(c.base[0])      ? "low-confidence-match"
                            : c.base.empty() && c.reason == "both-modified" ? "add-add"
                                                                            : c.reason;
                for (auto i : c.base)
                    pd.po.push_back(path + "/" + kb.step[i]);
                pd.pt = pd.po;
                for (auto i : c.ours)
                    pd.po.push_back(path + "/" + ko.step[i]);
                for (auto i : c.theirs)
                    pd.pt.push_back(path + "/" + kt.step[i]);
                pd.at = {!c.base.empty() || !c.ours.empty() ? pd.po[0] : pd.pt[0]};
                pend.push_back(std::move(pd));
                s.conflict = static_cast<int>(pend.size() - 1);
                break;
            }
            }
            p.kids.push_back(std::move(s));
        }
        for (auto [k, pairing, side, out, conf] : {std::tuple{&ko, &po, Side::ours, &co, task.conf_o},
                                                   std::tuple{&kt, &pt, Side::theirs, &ct, task.conf_t}}) {
            std::vector<Differ::Task> pairs;
            df.level(kb, *k, *pairing, path, side, *out, &recursed, pairs, conf);
            df.drain(pairs, side, *out);
        }
    }
};

// ---- Writing (xml.md §7): splices over ours' bytes ----

using Scope = std::map<std::string, std::string>; // prefix -> URI in the output

class Writer {
public:
    Writer(const Merger& m, const std::vector<Conflict>& conflicts, const Dialect& dia)
        : m_(m), cs_(conflicts), dia_(dia), out_(m.d[1]->enc) {}

    // Top down: each merged element's attributes and namespace scope. Bottom up: its bytes. No recursion,
    // since nesting reaches 512 (xml.md §8).
    std::string write(const PlanEl& root) {
        const Scope top{{std::string(), std::string()}};
        std::vector<const PlanEl*> order;
        std::vector<std::pair<const PlanEl*, const Scope*>> stack{{&root, &top}};
        while (!stack.empty()) {
            auto [p, outer] = stack.back();
            stack.pop_back();
            order.push_back(p);
            const Prep& pr = prep_[p] = prepare(*p, *outer);
            for (auto& slot : p->kids)
                if (slot.conflict < 0)
                    for (auto& r : slot.ours)
                        if (r.plan)
                            stack.emplace_back(r.plan, &pr.sc);
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            done_[*it] = render(**it);
        return std::move(done_[&root]);
    }

private:
    struct Prep {
        std::vector<std::pair<Side, const Attr*>> as; // output attributes
        bool same = true;                             // ours' start tag as written
        std::string extra;                            // namespace fix-up declarations (UTF-8)
        Scope sc;                                     // the scope inside the element
    };

    Prep prepare(const PlanEl& p, const Scope& outer) const {
        const Node& on = m_.d[1]->nodes[p.o];
        Prep pr;
        auto& as = pr.as;
        for (auto& r : p.attrs) {
            auto take = r.take;
            if (r.conflict >= 0) {
                auto& choice = cs_[static_cast<std::size_t>(r.conflict)].resolved_as;
                take = take_for(choice == "mark" ? "ours" : choice, r.at[0] >= 0, r.at[1] >= 0, r.at[2] >= 0);
            }
            if (take)
                as.emplace_back(*take,
                                &node(*take, p).attrs[static_cast<std::size_t>(r.at[side_index(*take)])]);
        }
        // Attributes ours lacks follow the surviving ones (xml.md §7).
        std::stable_partition(as.begin(), as.end(), [&](auto& x) {
            return std::any_of(on.attrs.begin(), on.attrs.end(),
                               [&](const Attr& a) { return attr_key(a) == attr_key(*x.second); });
        });
        pr.same = as.size() == on.attrs.size();
        for (std::size_t k = 0; pr.same && k < as.size(); ++k)
            pr.same = as[k].first == Side::ours && as[k].second == &on.attrs[k];
        Scope& sc = pr.sc = outer;
        for (auto& [s, a] : as)
            if (a->ns_decl)
                sc[a->name.local] = a->value;
        auto need = [&](const std::string& prefix, const std::string& uri) {
            auto f = sc.find(prefix);
            if (prefix != "xml" && (f == sc.end() ? std::string() : f->second) != uri) {
                pr.extra += " " + (prefix.empty() ? std::string("xmlns") : "xmlns:" + prefix) + "=\"" +
                            escape_attr(uri) + "\"";
                sc[prefix] = uri;
            }
        };
        if (on.kind != Kind::document) {
            need(on.name.prefix, on.name.uri);
            for (auto& [s, a] : as)
                if (!a->ns_decl && !a->name.prefix.empty())
                    need(a->name.prefix, a->name.uri);
        }
        return pr;
    }

    // ours' bytes of the element with its start tag and child region spliced (SPEC §2.3).
    std::string render(const PlanEl& p) {
        const Doc& O = *m_.d[1];
        const Node& on = O.nodes[p.o];
        const bool is_doc = on.kind == Kind::document;
        const Prep& pr = prep_.at(&p);
        std::string kids;
        bool any = false;
        for (auto& slot : p.kids)
            any |= emit(slot, pr.sc, kids);
        const bool open = !is_doc && on.end_tag.len == 0 && any; // <a/> that gained children
        std::vector<Splice> sp;
        const auto o0 = on.whole.off;
        if (!is_doc && (!pr.same || !pr.extra.empty() || open))
            sp.push_back(
                {on.start_tag.off - o0, on.start_tag.end() - o0, start_tag(on, pr.as, pr.extra, open)});
        if (open)
            kids += encode("</" + on.qname + ">", out_);
        sp.push_back({on.start_tag.end() - o0, on.trail_ws.off - o0, std::move(kids)});
        return apply_splices(O.bytes(on.whole), std::move(sp));
    }

    const Doc& doc(Side s) const { return *m_.d[side_index(s)]; }
    const Node& node(Side s, const PlanEl& p) const {
        return doc(s).nodes[s == Side::base ? p.b : s == Side::ours ? p.o : p.t];
    }
    std::string raw(Side s, Span sp) const {
        return transcode(std::string(doc(s).bytes(sp)), doc(s).enc, out_);
    }
    std::string lead(const Ref& r) const { return raw(r.side, doc(r.side).nodes[r.n].lead_ws); }

    std::string start_tag(const Node& on, const std::vector<std::pair<Side, const Attr*>>& as,
                          const std::string& extra, bool open) const {
        const Doc& O = *m_.d[1];
        std::string s(O.bytes({on.start_tag.off, on.name_end - on.start_tag.off}));
        std::size_t dq = 0, sq = 0;
        for (auto& a : on.attrs)
            ++(a.quote == '\'' ? sq : dq);
        const char most = sq > dq ? '\'' : '"';
        std::string prev;
        for (auto& [side, a] : as) {
            if (side == Side::ours) { // ours' own attribute: its bytes as written
                prev = std::string(O.bytes(a->lead_ws));
                s += prev + std::string(O.bytes(a->raw));
                continue;
            }
            const Attr* mine = nullptr;
            for (auto& x : on.attrs)
                if (attr_key(x) == attr_key(*a))
                    mine = &x;
            std::string ws = mine           ? std::string(O.bytes(mine->lead_ws))
                             : prev.empty() ? encode(" ", out_)
                                            : prev;
            const char q = mine ? mine->quote : most;
            s += ws + encode((mine ? mine->qname : a->qname) + "=" + q + escape_attr(a->value, q) + q, out_);
            prev = ws;
        }
        s += encode(extra, out_);
        std::string tail = O.utf8({on.attrs_end, on.start_tag.end() - on.attrs_end});
        if (open)
            tail.erase(tail.rfind('/'), 1);
        return s + encode(tail, out_);
    }

    // A source node verbatim, with declarations added when the output scope binds its prefixes differently.
    std::string pristine(Side s, std::uint32_t n, const Scope& sc) const {
        const Doc& D = doc(s);
        const Node& x = D.nodes[n];
        std::string extra;
        if (x.kind == Kind::element)
            for (auto& [prefix, uri] : x.free_ns) {
                auto f = sc.find(prefix);
                if ((f == sc.end() ? std::string() : f->second) != uri)
                    extra += " " + (prefix.empty() ? std::string("xmlns") : "xmlns:" + prefix) + "=\"" +
                             escape_attr(uri) + "\"";
            }
        std::string bytes(D.bytes(x.whole));
        if (!extra.empty())
            bytes.insert(x.start_tag.end() - width(D.enc) * (x.end_tag.len ? 1 : 2) - x.whole.off,
                         encode(extra, D.enc));
        return transcode(std::move(bytes), D.enc, out_);
    }

    std::string fragment(const std::vector<Ref>& refs) const {
        std::string s;
        for (std::size_t i = 0; i < refs.size(); ++i) {
            const Doc& D = doc(refs[i].side);
            const Node& x = D.nodes[refs[i].n];
            if (i)
                s += D.utf8(x.lead_ws);
            s += D.utf8(x.whole);
        }
        return s;
    }

    // Attribute conflicts under --conflict=mark: a mark before the owning element (xml.md §6).
    std::string attr_marks(const PlanEl& p, const std::string& ws) const {
        std::string s;
        for (auto& r : p.attrs) {
            if (r.conflict < 0 || cs_[static_cast<std::size_t>(r.conflict)].resolved_as != "mark")
                continue;
            std::string alt;
            if (r.at[2] >= 0) {
                auto& a = m_.d[2]->nodes[p.t].attrs[static_cast<std::size_t>(r.at[2])];
                alt = "@" + a.qname + "=\"" + escape_attr(a.value) + "\"";
            } else {
                auto& a = r.at[1] >= 0 ? m_.d[1]->nodes[p.o].attrs[static_cast<std::size_t>(r.at[1])]
                                       : m_.d[0]->nodes[p.b].attrs[static_cast<std::size_t>(r.at[0])];
                alt = "@" + a.qname + " (deleted)";
            }
            s += encode(dia_.mark(cs_[static_cast<std::size_t>(r.conflict)].id, alt).before, out_) + ws;
        }
        return s;
    }

    bool emit(const Slot& slot, const Scope& sc, std::string& out) {
        const Conflict* c = slot.conflict >= 0 ? &cs_[static_cast<std::size_t>(slot.conflict)] : nullptr;
        const std::vector<Ref>* list = &slot.ours;
        if (c && c->resolved_as == "theirs")
            list = &slot.theirs;
        else if (c && c->resolved_as == "base")
            list = &slot.base;
        Mark mk;
        if (c && c->resolved_as == "mark") {
            mk = dia_.mark(c->id, fragment(slot.theirs));
            mk.before = encode(mk.before, out_), mk.after = encode(mk.after, out_);
            if (list->empty())
                out += (slot.theirs.empty() ? std::string() : lead(slot.theirs[0])) + mk.before + mk.after;
        }
        for (std::size_t i = 0; i < list->size(); ++i) {
            const Ref& r = (*list)[i];
            const std::string ws = lead(r);
            out += ws;
            if (i == 0 && !mk.before.empty())
                out += mk.before + ws;
            if (r.plan)
                out += attr_marks(*r.plan, ws) + std::move(done_.at(r.plan));
            else
                out += pristine(r.side, r.n, sc);
        }
        if (!list->empty())
            out += mk.after;
        return !list->empty() || !mk.before.empty();
    }

    const Merger& m_;
    const std::vector<Conflict>& cs_;
    const Dialect& dia_;
    Enc out_;
    std::map<const PlanEl*, Prep> prep_;
    std::map<const PlanEl*, std::string> done_;
};

// ---- Dump (xml.md §5) ----

struct DumpLine {
    std::string text, at;
};

std::string one_line(std::string s) {
    for (std::size_t i = 0; (i = s.find('\n', i)) != std::string::npos;)
        s.replace(i, 1, "&#10;");
    return s;
}

// One node per line, 2-space indentation, sorted attributes (xml.md §5). An explicit stack: nesting reaches
// 512.
std::vector<DumpLine> dump_lines(const Doc& d, const Dialect& dia, const Opts& o) {
    std::vector<DumpLine> out;
    if (d.decl.present)
        out.push_back({"<?xml " + decl_text(d) + "?>", "/#decl"});
    struct Item {
        std::uint32_t k;
        std::string path;
        std::size_t depth;
        bool close;
    };
    std::vector<Item> stack{{0, "", 0, false}};
    while (!stack.empty()) {
        Item it = std::move(stack.back());
        stack.pop_back();
        const Node& n = d.nodes[it.k];
        const std::string ind(2 * it.depth, ' ');
        if (it.close) {
            out.push_back({ind + "</" + n.qname + ">", it.path});
            continue;
        }
        switch (n.kind) {
        case Kind::document:
        case Kind::element: {
            Kids kids = kids_of(d, it.k, &o);
            identify({&kids}, dia, n);
            const bool el = n.kind == Kind::element;
            if (el) {
                std::vector<const Attr*> ns, as;
                for (auto& a : n.attrs)
                    (a.ns_decl ? ns : as).push_back(&a);
                std::sort(ns.begin(), ns.end(),
                          [](auto* x, auto* y) { return x->name.local < y->name.local; });
                std::sort(as.begin(), as.end(), [](auto* x, auto* y) {
                    return std::tie(x->name.uri, x->name.local) < std::tie(y->name.uri, y->name.local);
                });
                ns.insert(ns.end(), as.begin(), as.end());
                const std::string close = kids.n.empty() ? "/>" : ">";
                if (o.attr_lines && !ns.empty()) {
                    out.push_back({ind + "<" + n.qname, it.path});
                    for (std::size_t i = 0; i < ns.size(); ++i)
                        out.push_back({ind + "    " + ns[i]->qname + "=\"" + escape_attr(ns[i]->value) +
                                           "\"" + (i + 1 == ns.size() ? close : ""),
                                       it.path + "/@" + ns[i]->qname});
                } else {
                    std::string s = ind + "<" + n.qname;
                    for (auto* a : ns)
                        s += " " + a->qname + "=\"" + escape_attr(a->value) + "\"";
                    out.push_back({s + close, it.path});
                }
                if (!kids.n.empty())
                    stack.push_back({it.k, it.path, it.depth, true});
            }
            for (auto i = kids.n.size(); i-- > 0;)
                stack.push_back(
                    {kids.n[i], it.path + "/" + kids.step[i], el ? it.depth + 1 : it.depth, false});
            break;
        }
        case Kind::text:
        case Kind::cdata: {
            std::string t =
                n.kind == Kind::cdata && o.strict_cdata ? "<![CDATA[" + n.text + "]]>" : escape_text(n.text);
            std::size_t s = 0;
            for (std::size_t e; (e = t.find('\n', s)) != std::string::npos; s = e + 1)
                out.push_back({(s ? "" : ind) + t.substr(s, e - s), it.path});
            out.push_back({(s ? "" : ind) + t.substr(s), it.path});
            break;
        }
        case Kind::comment:
            out.push_back({ind + "<!--" + one_line(n.text) + "-->", it.path});
            break;
        case Kind::pi:
            out.push_back(
                {ind + "<?" + n.qname + (n.text.empty() ? "" : " " + one_line(n.text)) + "?>", it.path});
            break;
        case Kind::doctype:
            out.push_back({ind + one_line(n.text), it.path});
            break;
        }
    }
    return out;
}

// Unified and context hunks name the longest common address of their lines (text.md §5.3).
class XmlComparison final : public Comparison {
public:
    XmlComparison(std::unique_ptr<Comparison> inner, std::vector<DumpLine> a, std::vector<DumpLine> b)
        : inner_(std::move(inner)), a_(std::move(a)), b_(std::move(b)) {}
    bool differs() const override { return inner_->differs(); }
    ChangeSet changes() const override { return inner_->changes(); }
    nlohmann::ordered_json stats() const override { return inner_->stats(); }
    std::vector<std::string> render(std::string& out, const RenderRequest& r) const override {
        RenderRequest rr = r;
        rr.options.hunk_address = [this](const Edit& first, const Edit& last) {
            std::optional<std::string> common;
            auto add = [&](const std::vector<DumpLine>& v, std::size_t s, std::size_t e) {
                for (auto i = s; i < e && i < v.size(); ++i) {
                    const std::string& y = v[i].at;
                    if (!common) {
                        common = y;
                        continue;
                    }
                    const std::string& x = *common;
                    std::size_t n = 0;
                    while (n < x.size() && n < y.size() && x[n] == y[n])
                        ++n;
                    if ((n < x.size() && x[n] != '/') || (n < y.size() && y[n] != '/')) {
                        auto cut = n ? x.rfind('/', n - 1) : std::string::npos;
                        n = cut == std::string::npos ? 0 : cut;
                    }
                    common->resize(n);
                }
            };
            add(a_, first.a0, last.a1);
            add(b_, first.b0, last.b1);
            return common && !common->empty() ? *common : std::string("/");
        };
        return inner_->render(out, rr);
    }

private:
    std::unique_ptr<Comparison> inner_;
    std::vector<DumpLine> a_, b_;
};

// ---- The format ----

const Doc& doc(const Document& d) {
    auto* p = dynamic_cast<const Doc*>(&d);
    if (!p)
        throw Error(ErrorCategory::internal, "xml: not an XML document");
    return *p;
}

FormatInfo generic_info() {
    return {
        "xml",
        "XML (generic; elements, attributes, text; byte-exact splice writer)",
        {".xml", ".xsd", ".xsl", ".xslt", ".plist", ".xliff", ".xlf", ".tmx", ".resx", ".csproj", ".vcxproj",
         ".props", ".targets", ".config", ".manifest", ".storyboard", ".xib"},
        {true, true, false, true, false},
        {{"xml-ws", OptionSpec::Arg::required, "whitespace-only text: formatting (default) or significant"},
         {"xml-ignore-comments", OptionSpec::Arg::none, "do not compare comments"},
         {"xml-ignore-pis", OptionSpec::Arg::none, "do not compare processing instructions"},
         {"xml-strict-prefixes", OptionSpec::Arg::none, "compare namespace prefixes and declarations"},
         {"xml-strict-cdata", OptionSpec::Arg::none, "CDATA differs from equal escaped text"},
         {"dump-attr-lines", OptionSpec::Arg::none, "dump: one XML attribute per line"}}};
}

class XmlFormat final : public Format {
public:
    XmlFormat(FormatInfo info, std::shared_ptr<const Dialect> dia)
        : info_(std::move(info)), dia_(std::move(dia)) {}

    const FormatInfo& info() const override { return info_; }

    // xml.md header: a '<' after an optional BOM and whitespace, and a well-formed scan of the head.
    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        std::size_t bom = 0;
        const Enc e = sniff(head, bom);
        const unsigned w = width(e);
        std::size_t i = bom;
        auto unit = [&](std::size_t k) -> unsigned {
            if (k + w > head.size())
                return 0;
            if (w == 1)
                return std::to_integer<unsigned>(head[k]);
            return e == Enc::utf16le
                       ? std::to_integer<unsigned>(head[k]) | std::to_integer<unsigned>(head[k + 1]) << 8
                       : std::to_integer<unsigned>(head[k + 1]) | std::to_integer<unsigned>(head[k]) << 8;
        };
        while (unit(i) == ' ' || unit(i) == '\t' || unit(i) == '\r' || unit(i) == '\n')
            i += w;
        if (unit(i) != '<')
            return {0};
        struct State {
            XML_Parser p;
            std::optional<Node> root;
            bool entity = false;
        } st{XML_ParserCreate(nullptr), {}, false};
        if (!st.p)
            return {0};
        XML_SetUserData(st.p, &st);
        XML_SetParamEntityParsing(st.p, XML_PARAM_ENTITY_PARSING_NEVER);
        XML_SetStartElementHandler(st.p, [](void* u, const XML_Char* name, const XML_Char** atts) {
            auto& s = *static_cast<State*>(u);
            if (s.root)
                return;
            Node& n = s.root.emplace();
            n.qname = name;
            std::string prefix =
                n.qname.find(':') == std::string::npos ? "" : n.qname.substr(0, n.qname.find(':'));
            n.name = {"", n.qname.substr(prefix.empty() ? 0 : prefix.size() + 1), prefix};
            for (int k = 0; atts[k]; k += 2) {
                Attr& a = n.attrs.emplace_back();
                a.qname = atts[k], a.value = atts[k + 1];
                a.ns_decl = a.qname == "xmlns" || a.qname.starts_with("xmlns:");
                if (a.ns_decl)
                    a.name = {std::string(kXmlnsNs), a.qname.size() > 5 ? a.qname.substr(6) : "", "xmlns"};
                else
                    a.name.local = a.qname;
            }
            for (auto& a : n.attrs)
                if (a.ns_decl && a.name.local == prefix)
                    n.name.uri = a.value;
        });
        XML_SetEntityDeclHandler(st.p,
                                 [](void* u, const XML_Char*, int, const XML_Char*, int, const XML_Char*,
                                    const XML_Char*, const XML_Char*, const XML_Char*) {
                                     auto& s = *static_cast<State*>(u);
                                     s.entity = true;
                                     XML_StopParser(s.p, XML_FALSE);
                                 });
        auto status = XML_Parse(st.p, reinterpret_cast<const char*>(head.data()),
                                static_cast<int>(head.size()), XML_FALSE);
        XML_ParserFree(st.p);
        if (status != XML_STATUS_OK || st.entity || !st.root)
            return {0};
        return {std::clamp(dia_->probe(*st.root), 0, 100)};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return load_doc(std::move(src), *dia_, options(o.ctx.format_options), o.ctx.budget);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        const Opts opt = options(o.ctx.format_options);
        auto cs = Differ{*dia_, opt}.document(doc(a), doc(b), Side::theirs);
        sort_canonical(cs);
        return cs;
    }

    std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                        const DiffOptions& o) const override {
        const Opts opt = options(o.ctx.format_options);
        return std::make_unique<XmlComparison>(Format::compare(a, b, o), dump_lines(doc(a), *dia_, opt),
                                               dump_lines(doc(b), *dia_, opt));
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override;

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions& o) const override {
        for (auto& l : dump_lines(doc(d), *dia_, options(o.ctx.format_options)))
            os << l.text << "\n";
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& x = doc(d);
        nlohmann::ordered_json j;
        j["encoding"] = enc_name(x.enc);
        j["bom"] = x.bom > 0;
        j["dialect"] = dia_->name();
        std::uint64_t elements = 0;
        for (auto c : x.nodes[0].children)
            if (x.nodes[c].kind == Kind::element) {
                j["root"] = x.nodes[c].qname;
                elements = subtree_nodes(x, c);
            }
        j["nodes"] = elements;
        auto& warn = j["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : x.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return j;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        // ponytail: options are not passed here, so matching counts use the defaults.
        Stats st;
        const Opts opt;
        Differ{*dia_, opt, &st}.document(doc(a), doc(b), Side::theirs);
        nlohmann::ordered_json j;
        for (auto op : {Op::insert, Op::erase, Op::update, Op::move})
            j[std::string(to_string(op))] =
                std::count_if(cs.begin(), cs.end(), [&](auto& c) { return c.op == op; });
        j["matched_by_id"] = st.by_id;
        j["matched_by_hash"] = st.by_hash;
        j["matched_by_similarity"] = st.by_similarity;
        j["min_confidence"] = st.min_confidence;
        return j;
    }

private:
    FormatInfo info_;
    std::shared_ptr<const Dialect> dia_;
};

MergeResult XmlFormat::merge(const Document& base_d, const Document& ours_d, const Document& theirs_d,
                             const MergeOptions& mo) const {
    const Doc &b = doc(base_d), &o = doc(ours_d), &t = doc(theirs_d);
    const Opts opt = options(mo.ctx.format_options);
    MergeResult res;
    for (auto [d, name] : {std::pair{&b, "base"}, std::pair{&o, "ours"}, std::pair{&t, "theirs"}})
        for (auto w : d->warnings) {
            w.message = std::string(name) + ": " + w.message;
            res.warnings.push_back(std::move(w));
        }
    Merger m{{&b, &o, &t}, *dia_, opt, Differ{*dia_, opt}, {}, {}, {}, {}};
    const PlanEl* root = m.run();
    // The XML declaration comes from ours (xml.md §7).
    for (auto [d, side, cs] : {std::tuple{&o, Side::ours, &m.co}, std::tuple{&t, Side::theirs, &m.ct}})
        if (decl_text(*d) != decl_text(b)) {
            auto c = make_change(side, Op::update, {"/#decl"}, "declaration");
            c.after = value_of(decl_text(*d), hash128(decl_text(*d)));
            cs->push_back(c);
        }
    const bool decl_dropped = decl_text(t) != decl_text(b) && decl_text(t) != decl_text(o);
    if (decl_dropped)
        res.warnings.push_back(
            {"decl-kept-ours", "/#decl", "the XML declaration comes from ours (xml.md §7)"});

    for (auto& pd : m.pend) {
        std::vector<std::string> ids;
        auto collect = [&](const ChangeSet& cs, const std::vector<std::string>& paths) {
            for (auto& c : cs)
                for (auto& p : paths)
                    if (path_within(c.at.path, p) || (c.from && path_within(c.from->path, p))) {
                        ids.push_back(c.id);
                        break;
                    }
        };
        collect(m.co, pd.po);
        collect(m.ct, pd.pt);
        auto c = make_conflict(pd.at, std::move(ids), pd.reason);
        c.choices = {"ours", "theirs", "base", "mark"};
        res.conflicts.push_back(std::move(c));
    }
    {
        std::set<std::string> in_conflict, theirs_keys, ours_keys;
        for (auto& c : res.conflicts)
            in_conflict.insert(c.changes.begin(), c.changes.end());
        auto key = [](const Change& c) {
            return std::string(to_string(c.op)) + c.at.to_string() +
                   (c.after.hash ? c.after.hash->hex() : "");
        };
        for (auto& c : m.co)
            ours_keys.insert(key(c));
        for (auto& c : m.ct)
            theirs_keys.insert(key(c));
        for (auto [cs, other] : {std::pair{&m.co, &theirs_keys}, std::pair{&m.ct, &ours_keys}})
            for (auto& c : *cs)
                if (!in_conflict.count(c.id) &&
                    !(decl_dropped && c.side == Side::theirs && c.kind == "declaration"))
                    res.auto_merged.push_back({c.id, other->count(key(c)) ? "identical" : "independent"});
    }
    apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);

    std::string bytes = Writer(m, res.conflicts, *dia_).write(*root);
    // Validation (xml.md §8): the output must parse.
    Limits budget = mo.ctx.budget.fresh();
    std::unique_ptr<Doc> out;
    try {
        auto* p = reinterpret_cast<const std::byte*>(bytes.data());
        out = load_doc(ByteSource::from_memory({p, p + bytes.size()}, o.src.name()), *dia_, opt, budget);
    } catch (const Error& e) {
        throw Error(ErrorCategory::internal,
                    std::string("XML writer produced an unreadable file: ") + e.what());
    }
    if (bytes != std::string_view(reinterpret_cast<const char*>(o.src.bytes().data()), o.src.size()) &&
        std::any_of(out->nodes.begin(), out->nodes.end(), [](const Node& n) {
            return n.kind == Kind::element && n.name.uri == kDsigNs && n.name.local == "Signature";
        }))
        res.warnings.push_back(
            {"signature-invalidated", "",
             "the merged document changed under an XML signature; re-sign it (xml.md §7.1)"});
    sort_canonical(m.co), sort_canonical(m.ct);
    res.changes_ours = std::move(m.co);
    res.changes_theirs = std::move(m.ct);
    res.document = std::move(out);
    return res;
}

constexpr IdRule kGenericIds[] = {{kXmlNs, "id"}, {"", "id"},   {"", "name"},   {"", "key"},
                                  {"", "uid"},    {"", "guid"}, {"", "Include"}};

} // namespace

int Dialect::probe(const Node&) const {
    return 50;
}

std::span<const IdRule> Dialect::id_rules(const Node&) const {
    return kGenericIds;
}

bool Dialect::unordered(const Node&) const {
    return false;
}

bool Dialect::significant_whitespace(const Node&) const {
    return false;
}

// xml.md §6: theirs' alternative in a comment, "--" escaped as "-\-".
Mark Dialect::mark(std::string_view id, std::string_view theirs) const {
    std::string p(theirs);
    for (std::size_t i; (i = p.find("--")) != std::string::npos;)
        p.replace(i, 2, "-\\-");
    return {"<!-- splinter-conflict " + std::string(id) + " theirs: " + (p.empty() ? "(deleted)" : p) +
                " -->",
            ""};
}

std::unique_ptr<Format> make_format() {
    return make_format(generic_info(), std::make_shared<Dialect>());
}

std::unique_ptr<Format> make_format(FormatInfo info, std::shared_ptr<const Dialect> dialect) {
    return std::make_unique<XmlFormat>(std::move(info),
                                       dialect ? std::move(dialect) : std::make_shared<Dialect>());
}

} // namespace semsplinter::xml
