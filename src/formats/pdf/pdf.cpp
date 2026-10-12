#include "internal.hpp"

#include <semsplinter/emit.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/formats/pdf.hpp>
#include <semsplinter/formats/xml.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/sequence.hpp>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFAnnotationObjectHelper.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace semsplinter::pdf {

namespace {

[[noreturn]] void bad(const std::string& what) {
    throw Error(ErrorCategory::invalid_input, "PDF: " + what);
}
[[noreturn]] void unsupported(const std::string& what) {
    throw Error(ErrorCategory::unsupported, "PDF: " + what);
}

std::string str(std::span<const std::byte> d) {
    return {reinterpret_cast<const char*>(d.data()), d.size()};
}

// Quoted, truncated, control characters escaped.
std::string quote(std::string_view s, std::size_t max = 200) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size() && i < max; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\')
            out += '\\', out += static_cast<char>(c);
        else if (c == '\n')
            out += "\\n";
        else if (c < 0x20 || c == 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", unsigned{c});
            out += buf;
        } else
            out += static_cast<char>(c);
    }
    return out + (s.size() > max ? "\"..." : "\"");
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::fabs(v) < 1e-9 ? 0.0 : v);
    return buf;
}

std::string name_of(QPDFObjectHandle o) {
    return o.isName() ? o.getName() : "";
}

// Human text of a scalar: strings decoded to UTF-8, names without the slash.
std::string text_of(QPDFObjectHandle o) {
    if (o.isString())
        return o.getUTF8Value();
    if (o.isName())
        return o.getName().substr(1);
    return o.unparse();
}

const Format& xml_format() {
    static const auto f = xml::make_format();
    return *f;
}

// ---- Canonical form: object numbers never matter (pdf.md §4) ----

struct Canon {
    const std::map<QPDFObjGen, std::size_t>* pages = nullptr;
    const std::map<QPDFObjGen, std::string>* annots = nullptr;
    std::set<std::string> skip; // keys left out of the top-level dictionary
    std::set<QPDFObjGen> seen;

    std::string operator()(QPDFObjectHandle o) {
        std::string s;
        write(o, s, 0);
        return s;
    }

    void write(QPDFObjectHandle o, std::string& out, int depth) {
        if (depth > 32) {
            out += "...";
            return;
        }
        if (o.isIndirect()) {
            const auto og = o.getObjGen();
            if (pages && pages->count(og)) {
                out += "page:" + std::to_string(pages->at(og) + 1);
                return;
            }
            if (annots && depth > 0 && annots->count(og)) {
                out += "annot:" + annots->at(og);
                return;
            }
            if (!seen.insert(og).second) {
                out += "ref";
                return;
            }
        }
        if (o.isStream()) {
            write(o.getDict(), out, depth + 1);
            out += "stream:" + raw_hash16(o);
        } else if (o.isDictionary()) {
            out += "<<";
            for (auto& k : o.getKeys())
                if (depth > 0 || !skip.count(k)) {
                    out += k + ' ';
                    write(o.getKey(k), out, depth + 1);
                    out += ' ';
                }
            out += ">>";
        } else if (o.isArray()) {
            out += '[';
            for (auto& e : o.getArrayAsVector())
                write(e, out, depth + 1), out += ' ';
            out += ']';
        } else if (o.isString()) {
            const auto v = o.getUTF8Value();
            out += "s" + std::to_string(v.size()) + ":" + v;
        } else {
            out += o.unparse();
        }
    }
};

// ---- Document model ----

struct Annot {
    std::string key, subtype, author, contents, rect, irt; // irt: key of the annotation replied to
    QPDFObjectHandle obj;
    std::string value; // canonical form (hash input)
};

struct Page {
    QPDFObjectHandle obj;
    std::string box; // "612x792" or "x0,y0,x1,y1" when not at the origin
    int rotate = 0;
    PageContent content;
    Hash128 fp; // content fingerprint plus geometry (pdf.md §3.1)
    std::vector<std::string> words;
    std::vector<Annot> annots;
};

struct Field {
    std::string fqn, ft, display;
    std::string value; // canonical /V plus widget /AS states
    QPDFObjectHandle obj;
    bool signature = false;
};

struct PdfDoc final : Document {
    ByteSource src;
    std::shared_ptr<QPDF> q; // after src: destroyed first, src owns the bytes qpdf reads
    bool locked = false;     // encrypted with a user password: opaque (D-41)
    std::string version;
    bool encrypted = false;
    std::vector<std::pair<std::string, QPDFObjectHandle>> info; // key without the slash
    std::vector<std::byte> xmp;
    std::unique_ptr<Document> xmp_doc;
    std::vector<Page> pages;
    std::vector<Field> fields;
    std::vector<std::pair<std::string, std::string>> extra; // /outline, /names, /pagelabels -> canonical
    int revisions = 1;
    std::uint64_t startxref = 0;
    bool xref_stream = false;
    int docmdp = 0; // certification /P, 0 when not certified
    EnvelopeMap map;
    std::vector<Warning> warnings;

    std::size_t signed_count() const {
        return static_cast<std::size_t>(
            std::count_if(fields.begin(), fields.end(), [](const Field& f) { return f.signature; }));
    }
};

const PdfDoc& doc(const Document& d) {
    auto* p = dynamic_cast<const PdfDoc*>(&d);
    if (!p)
        throw Error(ErrorCategory::internal, "pdf: not a PDF document");
    return *p;
}

std::size_t find(std::string_view hay, std::string_view needle, std::size_t from = 0) {
    return hay.find(needle, from);
}

// startxref, the /Prev chain length, and whether the last section is an xref stream.
void read_revisions(PdfDoc& d) {
    const std::string_view s(reinterpret_cast<const char*>(d.src.bytes().data()), d.src.size());
    auto sx = s.rfind("startxref");
    if (sx == std::string_view::npos)
        return; // qpdf reconstructed the xref; the writer refuses this carrier
    auto number_after = [&](std::size_t at) -> std::optional<std::uint64_t> {
        while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at])))
            ++at;
        if (at >= s.size() || !std::isdigit(static_cast<unsigned char>(s[at])))
            return std::nullopt;
        std::uint64_t v = 0;
        while (at < s.size() && std::isdigit(static_cast<unsigned char>(s[at])) && v < (1ull << 50))
            v = v * 10 + static_cast<std::uint64_t>(s[at++] - '0');
        return v;
    };
    auto start = number_after(sx + 9);
    if (!start || *start >= s.size())
        return;
    d.startxref = *start;
    d.xref_stream = s.substr(*start, 4) != "xref";
    std::set<std::uint64_t> seen;
    int n = 0;
    for (std::optional<std::uint64_t> at = start; at && *at < s.size() && seen.insert(*at).second; ++n) {
        const bool table = s.substr(*at, 4) == "xref";
        auto from = table ? find(s, "trailer", *at) : *at;
        auto to = from == std::string_view::npos ? from : find(s, table ? "startxref" : "stream", from);
        if (to == std::string_view::npos)
            break;
        auto dict = s.substr(from, to - from);
        auto p = dict.find("/Prev");
        at = p == std::string_view::npos ? std::nullopt : number_after(from + p + 5);
    }
    d.revisions = std::max(1, n);
}

// Envelope map (pdf.md §7.1): every revision is one unit; object bodies are content, the rest framing.
// ponytail: units per revision, not per page; finer attribution needs per-object units across revisions.
void build_map(PdfDoc& d) {
    const std::string_view s(reinterpret_cast<const char*>(d.src.bytes().data()), d.src.size());
    std::vector<std::uint64_t> ends; // end of each revision (after %%EOF and its EOL)
    for (auto at = s.find("%%EOF"); at != std::string_view::npos; at = s.find("%%EOF", at + 5)) {
        auto e = at + 5;
        while (e < s.size() && (s[e] == '\r' || s[e] == '\n'))
            ++e;
        ends.push_back(e);
    }
    if (ends.empty() || ends.back() < s.size())
        ends.push_back(s.size());
    auto unit = [&](std::uint64_t off) {
        auto k = std::upper_bound(ends.begin(), ends.end(), off) - ends.begin();
        return "/revisions/" + std::to_string(k + 1);
    };
    auto& r = d.map.ranges;
    std::uint64_t pos = 0;
    auto piece = [&](std::uint64_t from, std::uint64_t to, ByteClass cls) {
        while (from < to) { // split at revision boundaries
            auto k =
                static_cast<std::size_t>(std::upper_bound(ends.begin(), ends.end(), from) - ends.begin());
            const std::uint64_t stop = std::min<std::uint64_t>(to, k < ends.size() ? ends[k] : to);
            r.push_back({from, stop - from, cls, unit(from), ""});
            from = stop;
        }
    };
    // Object headers "N G obj"; stream data is skipped to its endstream.
    for (auto at = s.find("obj"); at != std::string_view::npos; at = s.find("obj", at + 3)) {
        if (at + 3 < s.size() && std::isalnum(static_cast<unsigned char>(s[at + 3])))
            continue;
        std::size_t b = at;
        auto skip_space = [&] {
            while (b > 0 && std::isspace(static_cast<unsigned char>(s[b - 1])))
                --b;
        };
        auto skip_digits = [&] {
            auto e = b;
            while (b > 0 && std::isdigit(static_cast<unsigned char>(s[b - 1])))
                --b;
            return e > b;
        };
        skip_space();
        if (!skip_digits())
            continue;
        skip_space();
        if (!skip_digits() || b < pos)
            continue;
        auto end = s.find("endobj", at);
        auto stream = s.find("stream", at);
        if (stream != std::string_view::npos && stream < end) {
            auto es = s.find("endstream", stream + 6);
            end = es == std::string_view::npos ? es : s.find("endobj", es);
        }
        if (end == std::string_view::npos)
            break;
        piece(pos, b, ByteClass::framing);
        piece(b, end + 6, ByteClass::content);
        pos = end + 6, at = end + 3;
    }
    piece(pos, s.size(), ByteClass::framing);
    d.map.validate(d.src.size());
}

std::string rect_text(QPDFObjectHandle r) {
    if (!r.isArray() || r.getArrayNItems() != 4)
        return "?";
    auto v = r.getArrayAsVector();
    auto n = [&](int i) { return v[i].isNumber() ? v[i].getNumericValue() : 0.0; };
    const double x = std::min(n(0), n(2)), y = std::min(n(1), n(3));
    return num(std::round(x)) + "," + num(std::round(y)) + "," + num(std::round(std::fabs(n(2) - n(0)))) +
           "," + num(std::round(std::fabs(n(3) - n(1))));
}

std::vector<std::string> split_words(const std::vector<std::string>& lines) {
    std::vector<std::string> w;
    for (auto& l : lines) {
        std::istringstream in(l);
        for (std::string t; in >> t;)
            w.push_back(t);
    }
    return w;
}

void load_pages(PdfDoc& d) {
    QPDFPageDocumentHelper pdh(*d.q);
    auto pages = pdh.getAllPages();
    std::map<QPDFObjGen, std::size_t> index;
    for (std::size_t i = 0; i < pages.size(); ++i)
        index[pages[i].getObjectHandle().getObjGen()] = i;
    for (auto& ph : pages) {
        Page p;
        p.obj = ph.getObjectHandle();
        auto mb = ph.getMediaBox();
        if (mb.isArray() && mb.getArrayNItems() == 4) {
            auto v = mb.getArrayAsVector();
            auto n = [&](int i) { return v[i].isNumber() ? v[i].getNumericValue() : 0.0; };
            p.box = n(0) == 0 && n(1) == 0 ? num(n(2)) + "x" + num(n(3))
                                           : num(n(0)) + "," + num(n(1)) + "," + num(n(2)) + "," + num(n(3));
        }
        auto rot = ph.getAttribute("/Rotate", false);
        p.rotate = rot.isInteger() ? rot.getIntValueAsInt() : 0;
        p.content = read_page(ph);
        p.words = split_words(p.content.lines);
        Hasher h;
        h.add(p.content.fingerprint.hex()).add(p.box).add(static_cast<std::uint64_t>(p.rotate));
        p.fp = h.digest();
        d.pages.push_back(std::move(p));
    }
    // Annotations: keys first (replies refer to them), then canonical values.
    std::map<QPDFObjGen, std::string> keys;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        std::map<std::string, int> used;
        for (auto& ah : pages[i].getAnnotations()) {
            auto a = ah.getObjectHandle();
            Annot x;
            x.obj = a;
            x.subtype = name_of(a.getKey("/Subtype"));
            if (x.subtype == "/Popup" ||
                x.subtype == "/Widget") // popups follow their parent; widgets are fields
                continue;
            x.author = a.getKey("/T").isString() ? a.getKey("/T").getUTF8Value() : "";
            x.contents = a.getKey("/Contents").isString() ? a.getKey("/Contents").getUTF8Value() : "";
            x.rect = rect_text(a.getKey("/Rect"));
            auto nm = a.getKey("/NM");
            if (nm.isString() && !nm.getUTF8Value().empty())
                x.key = nm.getUTF8Value();
            else
                x.key = x.subtype.substr(1) + ":" + x.author + ":" + x.rect + ":" +
                        hash128(x.contents).hex().substr(0, 6);
            if (int n = ++used[x.key]; n > 1)
                x.key += "~" + std::to_string(n);
            if (a.isIndirect())
                keys[a.getObjGen()] = x.key;
            d.pages[i].annots.push_back(std::move(x));
        }
    }
    for (auto& p : d.pages)
        for (auto& x : p.annots) {
            auto irt = x.obj.getKey("/IRT");
            if (irt.isIndirect() && keys.count(irt.getObjGen()))
                x.irt = keys[irt.getObjGen()];
            Canon c{&index, &keys, {"/P", "/Popup", "/Parent", "/AP", "/IRT"}, {}};
            x.value = c(x.obj) + "irt:" + x.irt;
        }
}

void load_fields(PdfDoc& d) {
    QPDFAcroFormDocumentHelper afdh(*d.q);
    if (!afdh.hasAcroForm())
        return;
    std::map<std::string, int> used;
    for (auto& ff : afdh.getFormFields()) {
        Field f;
        f.obj = ff.getObjectHandle();
        f.fqn = ff.getFullyQualifiedName();
        if (int n = ++used[f.fqn]; n > 1)
            f.fqn += "~" + std::to_string(n);
        f.ft = ff.getFieldType();
        f.signature = f.ft == "/Sig" && ff.getValue().isDictionary();
        auto v = ff.getValue();
        Canon c;
        f.value = c(v);
        for (auto& w : afdh.getAnnotationsForField(ff))
            f.value += " as:" + w.getAppearanceState();
        f.display = f.signature ? "signed" : v.isNull() ? "null" : quote(text_of(v));
        d.fields.push_back(std::move(f));
    }
}

void load_meta(PdfDoc& d, const Context& ctx) {
    auto trailer = d.q->getTrailer();
    auto info = trailer.getKey("/Info");
    if (info.isDictionary())
        for (auto& k : info.getKeys())
            d.info.emplace_back(k.substr(1), info.getKey(k));
    auto root = d.q->getRoot();
    auto md = root.getKey("/Metadata");
    if (md.isStream()) {
        try {
            auto b = md.getStreamData();
            auto p = reinterpret_cast<const std::byte*>(b->getBuffer());
            d.xmp.assign(p, p + b->getSize());
            Context sub{ctx.registry, ctx.budget, ctx.depth + 1, ctx.format_options};
            d.xmp_doc =
                xml_format().load(ByteSource::from_memory(d.xmp, "metadata.xmp"), LoadOptions{sub, {}});
        } catch (const std::exception& e) {
            d.xmp_doc.reset();
            d.warnings.push_back({"xmp-opaque", "/xmp", std::string("XMP not parsed: ") + e.what()});
        }
    }
    std::map<QPDFObjGen, std::size_t> index;
    for (std::size_t i = 0; i < d.pages.size(); ++i)
        index[d.pages[i].obj.getObjGen()] = i;
    for (auto [key, unit] : {std::pair{"/Outlines", "/outline"}, std::pair{"/Names", "/names"},
                             std::pair{"/PageLabels", "/pagelabels"}}) {
        auto o = root.getKey(key);
        if (!o.isNull()) {
            Canon c{&index, nullptr, {}, {}};
            d.extra.emplace_back(unit, c(o));
        }
    }
    auto perms = root.getKey("/Perms");
    if (perms.isDictionary() && perms.getKey("/DocMDP").isDictionary()) {
        d.docmdp = 2;
        auto refs = perms.getKey("/DocMDP").getKey("/Reference");
        if (refs.isArray())
            for (auto& r : refs.getArrayAsVector())
                if (r.isDictionary() && name_of(r.getKey("/TransformMethod")) == "/DocMDP") {
                    auto p = r.getKey("/TransformParams").isDictionary()
                                 ? r.getKey("/TransformParams").getKey("/P")
                                 : QPDFObjectHandle::newNull();
                    if (p.isInteger())
                        d.docmdp = p.getIntValueAsInt();
                }
    }
}

std::unique_ptr<PdfDoc> load_doc(ByteSource src, const Context& ctx) {
    auto d = std::make_unique<PdfDoc>();
    d->src = std::move(src);
    const std::string_view s(reinterpret_cast<const char*>(d->src.bytes().data()), d->src.size());
    if (s.substr(0, 1024).find("%PDF-") == std::string_view::npos)
        bad("no %PDF- header in the first 1024 bytes");
    ctx.budget.charge(d->src.size()); // the object model; qpdf allocates on demand
    d->q = std::make_shared<QPDF>();
    d->q->setSuppressWarnings(true);
    try {
        d->q->processMemoryFile(d->src.name().empty() ? "input.pdf" : d->src.name().c_str(), s.data(),
                                s.size());
    } catch (const QPDFExc& e) {
        if (e.getErrorCode() != qpdf_e_password)
            bad(e.what());
        d->locked = true, d->encrypted = true;
    } catch (const std::exception& e) {
        bad(e.what());
    }
    auto pos = s.find("%PDF-");
    d->version = std::string(s.substr(pos + 5, 3));
    if (!d->locked) {
        try {
            d->version = d->q->getPDFVersion();
            d->encrypted = d->q->isEncrypted();
            load_pages(*d);
            load_fields(*d);
            load_meta(*d, ctx);
        } catch (const Error&) {
            throw;
        } catch (const std::exception& e) {
            bad(e.what());
        }
        for (auto& w : d->q->getWarnings())
            d->warnings.push_back({"framing-inconsistent", "", w.what()});
    }
    read_revisions(*d);
    build_map(*d);
    return d;
}

// ---- Options ----

struct Opts {
    std::set<std::string> layers{"meta", "pages", "text", "annots", "forms"};
    bool noise = false;
    std::string layout = "stable", signatures = "auto";
};

Opts options(const OptionValues& o) {
    Opts r;
    if (auto it = o.find("pdf-layers"); it != o.end()) {
        r.layers.clear();
        std::istringstream in(it->second);
        for (std::string l; std::getline(in, l, ',');) {
            static const std::set<std::string> known{"meta", "pages", "text", "annots", "forms"};
            if (!known.count(l))
                unsupported("--pdf-layers: unknown or post-v1 layer '" + l + "'");
            r.layers.insert(l);
        }
    }
    r.noise = o.count("pdf-show-noise") > 0;
    if (auto it = o.find("layout"); it != o.end())
        r.layout = it->second;
    if (auto it = o.find("signatures"); it != o.end())
        r.signatures = it->second;
    return r;
}

bool noise_key(std::string_view k) {
    return k == "ModDate" || k == "Producer" || k == "CreationDate";
}
bool noise_path(std::string_view p) {
    for (auto n : {"xmp:MetadataDate", "xmp:ModifyDate", "xmpMM:InstanceID", "xmpMM:History"})
        if (p.find(n) != std::string_view::npos)
            return true;
    return false;
}

// ---- Page alignment (pdf.md §3.1) ----

double similarity(const Page& a, const Page& b) {
    if (a.words.empty() && b.words.empty())
        return 1.0;
    std::vector<Token> ta, tb;
    for (auto& w : a.words)
        ta.push_back(hash128(w).lo);
    for (auto& w : b.words)
        tb.push_back(hash128(w).lo);
    const auto m = diff_match(ta, tb, DiffAlgorithm::histogram).size();
    return 2.0 * double(m) / double(ta.size() + tb.size());
}

struct Alignment {
    std::vector<std::optional<std::size_t>> a2b;
    std::vector<std::optional<std::size_t>> b2a;
    bool changed = false; // any page inserted, deleted, or different
};

Alignment align(const PdfDoc& a, const PdfDoc& b) {
    Alignment al;
    al.a2b.resize(a.pages.size()), al.b2a.resize(b.pages.size());
    std::vector<Token> ta, tb;
    for (auto& p : a.pages)
        ta.push_back(p.fp.lo);
    for (auto& p : b.pages)
        tb.push_back(p.fp.lo);
    auto matches = diff_match(ta, tb, DiffAlgorithm::histogram);
    matches.emplace_back(a.pages.size(), b.pages.size()); // sentinel
    std::size_t ai = 0, bi = 0;
    for (auto [ma, mb] : matches) {
        for (; ai < ma; ++ai) // gap: pair by text similarity, in order
            for (std::size_t k = bi; k < mb; ++k)
                if (similarity(a.pages[ai], b.pages[k]) >= 0.5) {
                    al.a2b[ai] = k, al.b2a[k] = ai, bi = k + 1;
                    break;
                }
        if (ma < a.pages.size())
            al.a2b[ma] = mb, al.b2a[mb] = ma;
        ai = ma + 1, bi = mb + 1;
    }
    al.changed = a.pages.size() != b.pages.size();
    for (std::size_t i = 0; i < a.pages.size(); ++i)
        al.changed |= !al.a2b[i] || a.pages[i].fp != b.pages[*al.a2b[i]].fp;
    return al;
}

std::string page_path(std::size_t base_index) {
    return "/pages/p:" + std::to_string(base_index + 1);
}

// ---- Diff ----

void add_change(ChangeSet& out, Side side, Op op, std::string path, std::string kind,
                std::optional<std::string> before, std::optional<std::string> after) {
    auto c = make_change(side, op, {std::move(path)}, std::move(kind));
    if (before)
        c.before = {*before, hash128(*before)};
    if (after)
        c.after = {*after, hash128(*after)};
    out.push_back(std::move(c));
}

// Keyed changes between two (key, canonical, display) lists.
struct KeyedUnit {
    std::string key, value, display;
};
void keyed_changes(const std::vector<KeyedUnit>& a, const std::vector<KeyedUnit>& b,
                   const std::string& prefix, const std::string& kind, Side side, ChangeSet& out) {
    std::map<std::string, const KeyedUnit*> ma, mb;
    for (auto& u : a)
        ma[u.key] = &u;
    for (auto& u : b)
        mb[u.key] = &u;
    for (auto& [k, u] : ma) {
        auto f = mb.find(k);
        if (f == mb.end())
            add_change(out, side, Op::erase, prefix + pointer_escape(k), kind, u->display, std::nullopt);
        else if (f->second->value != u->value) {
            add_change(out, side, Op::update, prefix + pointer_escape(k), kind, u->display,
                       f->second->display);
            out.back().before.hash = hash128(u->value), out.back().after.hash = hash128(f->second->value);
        }
    }
    for (auto& [k, u] : mb)
        if (!ma.count(k))
            add_change(out, side, Op::insert, prefix + pointer_escape(k), kind, std::nullopt, u->display);
}

std::vector<KeyedUnit> info_units(const PdfDoc& d, bool noise) {
    std::vector<KeyedUnit> v;
    for (auto& [k, o] : d.info)
        if (noise || !noise_key(k)) {
            Canon c;
            v.push_back({k, c(o), quote(text_of(o))});
        }
    return v;
}

std::string annot_line(const Annot& a) {
    return a.subtype.substr(1) + " @ " + a.rect + (a.author.empty() ? "" : " by " + quote(a.author)) +
           (a.contents.empty() ? "" : ": " + quote(a.contents));
}

std::vector<KeyedUnit> annot_units(const Page& p) {
    std::vector<KeyedUnit> v;
    for (auto& a : p.annots)
        v.push_back({a.key, a.value, annot_line(a)});
    return v;
}

std::vector<KeyedUnit> field_units(const PdfDoc& d) {
    std::vector<KeyedUnit> v;
    for (auto& f : d.fields)
        v.push_back({f.fqn, f.ft + " " + f.value, f.ft.substr(f.ft.empty() ? 0 : 1) + " = " + f.display});
    return v;
}

std::vector<KeyedUnit> extra_units(const PdfDoc& d) {
    std::vector<KeyedUnit> v;
    for (auto& [k, c] : d.extra)
        v.push_back({k.substr(1), c, "xxh3=" + hash128(c).hex().substr(0, 16)});
    return v;
}

// The changes of `b` against `a`, addressed by a's page indices (a is base in a merge).
ChangeSet diff_docs(const PdfDoc& a, const PdfDoc& b, const Opts& opt, Side side, Context& ctx) {
    ChangeSet out;
    if (a.locked || b.locked) {
        if (a.src.size() != b.src.size() || hash128(a.src.bytes()) != hash128(b.src.bytes())) {
            add_change(out, side, Op::update, "/encrypted", "encrypted",
                       "bytes=" + std::to_string(a.src.size()), "bytes=" + std::to_string(b.src.size()));
            out.back().before.hash = hash128(a.src.bytes()), out.back().after.hash = hash128(b.src.bytes());
        }
        return out;
    }
    if (opt.layers.count("meta")) {
        keyed_changes(info_units(a, opt.noise), info_units(b, opt.noise), "/info/", "metadata", side, out);
        if (a.xmp_doc && b.xmp_doc) {
            DiffOptions xo{ctx, {}};
            for (auto c : xml_format().diff(*a.xmp_doc, *b.xmp_doc, xo)) {
                if (!opt.noise && noise_path(c.at.path))
                    continue;
                c.at.path = "/xmp" + c.at.path;
                c.id = make_change_id(side, c.op, c.at), c.side = side, c.kind = "metadata";
                out.push_back(std::move(c));
            }
        } else if (a.xmp != b.xmp) {
            add_change(out, side,
                       a.xmp.empty()   ? Op::insert
                       : b.xmp.empty() ? Op::erase
                                       : Op::update,
                       "/xmp", "metadata",
                       a.xmp.empty() ? std::nullopt : std::optional(str(a.xmp).substr(0, 200)),
                       b.xmp.empty() ? std::nullopt : std::optional(str(b.xmp).substr(0, 200)));
        }
        keyed_changes(extra_units(a), extra_units(b), "/", "structure", side, out);
    }
    const Alignment al = align(a, b);
    std::map<Hash128, std::size_t> erased; // fingerprint -> index in out, for moves
    for (std::size_t i = 0; i < a.pages.size(); ++i) {
        const Page& pa = a.pages[i];
        if (!al.a2b[i]) {
            if (opt.layers.count("pages")) {
                add_change(out, side, Op::erase, page_path(i), "page", pa.box, std::nullopt);
                erased.emplace(pa.fp, out.size() - 1);
            }
            continue;
        }
        const Page& pb = b.pages[*al.a2b[i]];
        const auto path = page_path(i);
        if (pa.fp != pb.fp) {
            const bool geometry = pa.box != pb.box || pa.rotate != pb.rotate;
            if (opt.layers.count("pages") && geometry)
                add_change(out, side, Op::update, path, "geometry",
                           pa.box + " rotate=" + std::to_string(pa.rotate),
                           pb.box + " rotate=" + std::to_string(pb.rotate));
            const bool text = pa.words != pb.words;
            if (opt.layers.count("text") && text) {
                std::vector<Token> ta, tb;
                for (auto& w : pa.words)
                    ta.push_back(hash128(w).lo);
                for (auto& w : pb.words)
                    tb.push_back(hash128(w).lo);
                auto m = diff_match(ta, tb, DiffAlgorithm::histogram);
                std::vector<bool> ka(ta.size()), kb(tb.size());
                for (auto [x, y] : m)
                    ka[x] = kb[y] = true;
                auto changed = [](const std::vector<std::string>& w, const std::vector<bool>& keep) {
                    std::string s;
                    for (std::size_t k = 0; k < w.size(); ++k)
                        if (!keep[k])
                            s += (s.empty() ? "" : " ") + w[k];
                    return s;
                };
                add_change(out, side, Op::update, path + "/text", "text", changed(pa.words, ka),
                           changed(pb.words, kb));
                out.back().before.hash =
                    hash128(std::string(pa.content.lines.empty() ? "" : pa.content.lines[0]));
                out.back().metrics["words_inserted"] = tb.size() - m.size();
                out.back().metrics["words_deleted"] = ta.size() - m.size();
                if (al.a2b[i] && similarity(pa, pb) < 1.0)
                    out.back().confidence = similarity(pa, pb);
            }
            if (opt.layers.count("pages")) {
                std::multiset<std::string> ia(pa.content.images.begin(), pa.content.images.end()),
                    ib(pb.content.images.begin(), pb.content.images.end());
                for (auto& im : ia)
                    if (!ib.count(im))
                        add_change(out, side, Op::erase, path + "/images/" + im.substr(im.find(':') + 1, 6),
                                   "image", im, std::nullopt);
                for (auto& im : ib)
                    if (!ia.count(im))
                        add_change(out, side, Op::insert, path + "/images/" + im.substr(im.find(':') + 1, 6),
                                   "image", std::nullopt, im);
                if (!geometry && !text && ia == ib)
                    add_change(out, side, Op::update, path + "/content", "content",
                               "xxh3=" + pa.content.fingerprint.hex().substr(0, 16),
                               "xxh3=" + pb.content.fingerprint.hex().substr(0, 16));
            }
        }
        if (opt.layers.count("annots"))
            keyed_changes(annot_units(pa), annot_units(pb), path + "/annots/", "annotation", side, out);
    }
    if (opt.layers.count("pages"))
        for (std::size_t k = 0; k < b.pages.size(); ++k)
            if (!al.b2a[k]) {
                const auto path =
                    "/pages/p:new:" + std::string(to_string(side)) + ":" + std::to_string(k + 1);
                auto mv = erased.find(b.pages[k].fp);
                if (mv != erased.end()) { // the same page elsewhere: a move
                    auto& c = out[mv->second];
                    auto from = c.at;
                    c = make_change(side, Op::move, {path}, "page");
                    c.from = from;
                    erased.erase(mv);
                } else {
                    add_change(out, side, Op::insert, path, "page", std::nullopt, b.pages[k].box);
                }
            }
    if (opt.layers.count("forms"))
        keyed_changes(field_units(a), field_units(b), "/fields/", "field", side, out);
    sort_canonical(out);
    return out;
}

// ---- Writing (pdf.md §7) ----

// Copies objects of a foreign document into `dst`. References to pages become the mapped carrier page
// (or null); everything else indirect goes through copyForeignObject.
struct Importer {
    QPDF& dst;
    std::map<QPDFObjGen, QPDFObjectHandle> pages; // foreign page -> carrier page

    QPDFObjectHandle operator()(QPDFObjectHandle o) {
        if (o.isIndirect()) {
            if (o.isPageObject()) {
                auto f = pages.find(o.getObjGen());
                return f == pages.end() ? QPDFObjectHandle::newNull() : f->second;
            }
            return dst.copyForeignObject(o);
        }
        if (o.isArray()) {
            auto a = QPDFObjectHandle::newArray();
            for (auto& e : o.getArrayAsVector())
                a.appendItem((*this)(e));
            return a;
        }
        if (o.isDictionary()) {
            auto d = QPDFObjectHandle::newDictionary();
            for (auto& k : o.getKeys())
                d.replaceKey(k, (*this)(o.getKey(k)));
            return d;
        }
        return QPDFObjectHandle::parse(o.unparse());
    }
};

std::string serialize(QPDFObjectHandle o) {
    const auto og = o.getObjGen();
    std::string s = std::to_string(og.getObj()) + " " + std::to_string(og.getGen()) + " obj\n";
    if (o.isStream()) {
        auto raw = o.getRawStreamData();
        auto dict = o.getDict().shallowCopy();
        dict.replaceKey("/Length", QPDFObjectHandle::newInteger(static_cast<long long>(raw->getSize())));
        s += dict.unparse() + "\nstream\n";
        s.append(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize());
        s += "\nendstream";
    } else {
        s += o.unparseResolved();
    }
    return s + "\nendobj\n";
}

// Objects of `dst` that differ from the carrier's pristine copy, plus new ones and `touched` streams.
std::vector<QPDFObjectHandle> dirty_objects(const PdfDoc& carrier, QPDF& dst,
                                            const std::set<QPDFObjGen>& touched) {
    std::vector<QPDFObjectHandle> out;
    for (auto& o : dst.getAllObjects()) {
        const auto og = o.getObjGen();
        auto orig = carrier.q->getObject(og);
        bool dirty = touched.count(og) > 0 || orig.isNull() != o.isNull();
        if (!dirty && !o.isNull())
            dirty = o.isStream() ? !orig.isStream() || o.getDict().unparse() != orig.getDict().unparse()
                                 : orig.isStream() || o.unparseResolved() != orig.unparseResolved();
        if (dirty && !o.isNull())
            out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const QPDFObjectHandle& x, const QPDFObjectHandle& y) {
        return x.getObjGen() < y.getObjGen();
    });
    return out;
}

// The trailer keys carried into the new section (everything except framing and /ID).
QPDFObjectHandle carried_trailer(QPDF& dst) {
    auto t = dst.getTrailer();
    auto out = QPDFObjectHandle::newDictionary();
    static const std::set<std::string> framing{"/Prev",  "/Size",   "/XRefStm", "/Type", "/W",
                                               "/Index", "/Length", "/Filter",  "/ID",   "/DecodeParms"};
    for (auto& k : t.getKeys())
        if (!framing.count(k))
            out.replaceKey(k, t.getKey(k));
    return out;
}

// An incremental update (pdf.md §7): the carrier's bytes verbatim, the changed and new objects, a new
// xref section of the carrier's kind, and a trailer with /Prev. Laid out by the Emitter.
std::vector<std::byte> write_incremental(const PdfDoc& carrier, QPDF& dst,
                                         const std::set<QPDFObjGen>& touched) {
    auto objs = dirty_objects(carrier, dst, touched);
    auto trailer = carried_trailer(dst);
    auto old_trailer = carried_trailer(*carrier.q);
    if (objs.empty() && trailer.unparse() == old_trailer.unparse())
        return {carrier.src.bytes().begin(), carrier.src.bytes().end()};

    using namespace emit;
    std::vector<Seg> segs{raw(carrier.src, 0, carrier.src.size())};
    const auto cb = carrier.src.bytes();
    if (!cb.empty() && cb.back() != std::byte{'\n'} && cb.back() != std::byte{'\r'})
        segs.push_back(bytes("\n"));
    Hasher id;
    id.add(carrier.src.size()).raw(cb.subspan(cb.size() - std::min<std::size_t>(cb.size(), 4096)));
    int size = 0;
    if (auto s = dst.getTrailer().getKey("/Size"); s.isInteger())
        size = s.getIntValueAsInt();
    std::vector<std::pair<int, GroupId>> entries; // object number -> group (generation kept below)
    std::map<int, int> gens;
    GroupId next = 1;
    std::uint64_t appended = 0;
    for (auto& o : objs) {
        auto s = serialize(o);
        id.add(s);
        appended += s.size();
        const auto og = o.getObjGen();
        entries.emplace_back(og.getObj(), next);
        gens[og.getObj()] = og.getGen();
        size = std::max(size, og.getObj() + 1);
        segs.push_back(group(next++, {bytes(s)}));
    }
    const GroupId xref = next++;
    // /ID: [0] kept (it feeds encryption keys), [1] deterministic (pdf.md §7).
    auto ids = dst.getTrailer().getKey("/ID");
    std::string id_text;
    if (ids.isArray() && ids.getArrayNItems() == 2)
        id_text = " /ID [" + ids.getArrayItem(0).unparse() + " <" + id.digest().hex() + ">]";
    const std::string prev = " /Prev " + std::to_string(carrier.startxref);
    auto body = trailer.unparse(); // "<< ... >>"
    body = body.substr(2, body.size() - 4);

    auto runs = [&] { // consecutive object numbers -> (first, count, entries)
        std::vector<std::pair<int, std::vector<std::pair<int, GroupId>>>> r;
        for (auto& e : entries) {
            if (r.empty() || r.back().first + static_cast<int>(r.back().second.size()) != e.first)
                r.push_back({e.first, {}});
            r.back().second.push_back(e);
        }
        return r;
    };
    std::vector<Seg> xs;
    if (!carrier.xref_stream) {
        xs.push_back(bytes("xref\n"));
        for (auto& [first, run] : runs()) {
            xs.push_back(bytes(std::to_string(first) + " " + std::to_string(run.size()) + "\n"));
            for (auto& [n, g] : run) {
                Seg off = varfield(VarEncoding::decimal, offset_of(g));
                off.width = 10;
                char gen[16];
                std::snprintf(gen, sizeof gen, " %05d n\r\n", gens[n]);
                xs.push_back(std::move(off));
                xs.push_back(bytes(gen));
            }
        }
        xs.push_back(bytes("trailer\n<< /Size " + std::to_string(size) + prev + id_text + body + " >>\n"));
    } else { // an xref stream holding itself (Q-PDF-8)
        const int self = size++;
        entries.emplace_back(self, xref);
        gens[self] = 0;
        const unsigned w = carrier.src.size() + appended + 4096 < (1ull << 32) ? 4 : 8;
        std::string index;
        std::size_t rows = 0;
        for (auto& [first, run] : runs())
            index += std::to_string(first) + " " + std::to_string(run.size()) + " ", rows += run.size();
        index.pop_back();
        xs.push_back(bytes(std::to_string(self) + " 0 obj\n<< /Type /XRef /Size " + std::to_string(size) +
                           " /W [1 " + std::to_string(w) + " 2] /Index [" + index + "]" + prev + id_text +
                           body + " /Length " + std::to_string(rows * (3 + w)) + " >>\nstream\n"));
        for (auto& [first, run] : runs())
            for (auto& [n, g] : run) {
                xs.push_back(field(1, Endian::big, constant(1)));
                xs.push_back(field(w, Endian::big, offset_of(g)));
                xs.push_back(field(2, Endian::big, constant(static_cast<std::uint64_t>(gens[n]))));
            }
        xs.push_back(bytes("\nendstream\nendobj\n"));
    }
    segs.push_back(group(xref, std::move(xs)));
    segs.push_back(bytes("startxref\n"));
    segs.push_back(varfield(VarEncoding::decimal, offset_of(xref)));
    segs.push_back(bytes("\n%%EOF\n"));
    return emit::emit(group(0, std::move(segs)));
}

// A full rewrite by QPDFWriter (--layout=compact, pdf.md §7).
std::vector<std::byte> write_full(QPDF& q, bool object_streams) {
    QPDFWriter w(q);
    w.setOutputMemory();
    if (!q.isEncrypted())
        w.setDeterministicID(true);
    if (object_streams)
        w.setObjectStreamMode(qpdf_o_generate);
    w.write();
    auto b = w.getBufferSharedPointer();
    auto p = reinterpret_cast<const std::byte*>(b->getBuffer());
    return {p, p + b->getSize()};
}

// A fresh, mutable qpdf object model of a document's bytes.
std::shared_ptr<QPDF> reopen(const PdfDoc& d) {
    auto q = std::make_shared<QPDF>();
    q->setSuppressWarnings(true);
    q->processMemoryFile("carrier.pdf", reinterpret_cast<const char*>(d.src.bytes().data()), d.src.size());
    return q;
}

// Signature values removed: /V of signature fields and the catalog's /Perms (pdf.md §7.1 strip).
void strip_signatures(const PdfDoc& carrier, QPDF& dst, std::vector<Warning>& warnings) {
    for (auto& f : carrier.fields)
        if (f.signature) {
            dst.getObject(f.obj.getObjGen()).removeKey("/V");
            warnings.push_back({"signature-stripped", "/fields/" + pointer_escape(f.fqn),
                                "signature value removed (--signatures)"});
        }
    dst.getRoot().removeKey("/Perms");
    auto af = dst.getRoot().getKey("/AcroForm");
    if (af.isDictionary())
        af.removeKey("/SigFlags");
}

// ---- Format ----

class PdfFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "pdf",
            "PDF (metadata, pages, text, annotations, forms; merges written as incremental updates)",
            {".pdf", ".ai"},
            {true, true, false, true, false},
            {{"pdf-layers", OptionSpec::Arg::required,
              "diff layers: meta,pages,text,annots,forms (default all)"},
             {"pdf-show-noise", OptionSpec::Arg::none, "report /ModDate, /Producer, and XMP dates and ids"}}};
        return i;
    }

    Probe probe(std::span<const std::byte> head, std::string_view) const override {
        const std::string_view s(reinterpret_cast<const char*>(head.data()),
                                 std::min<std::size_t>(head.size(), 1024));
        auto at = s.find("%PDF-");
        if (at == std::string_view::npos || at + 8 > s.size())
            return {0};
        const auto v = s.substr(at + 5, 3);
        const bool known = (v[0] == '1' && v[1] == '.' && v[2] >= '0' && v[2] <= '7') || v == "2.0";
        if (!known)
            return {0};
        return {at == 0 ? 95 : 70};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return load_doc(std::move(src), o.ctx);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        return diff_docs(doc(a), doc(b), options(o.ctx.format_options), Side::theirs, o.ctx);
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& o) const override;

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(doc(d).src.bytes());
    }

    void dump(const Document& dd, std::ostream& os, const DumpOptions& o) const override {
        const PdfDoc& d = doc(dd);
        const Opts opt = options(o.ctx.format_options);
        os << "pdf " << d.version << " pages=" << d.pages.size() << " revisions=" << d.revisions
           << " signed=" << d.signed_count() << " encrypted=" << (d.encrypted ? "yes" : "no") << "\n";
        if (d.locked) {
            os << "locked bytes=" << d.src.size() << " xxh3=" << hash128(d.src.bytes()).hex().substr(0, 16)
               << "\n";
            return;
        }
        std::string info;
        for (auto& [k, v] : d.info)
            if (opt.noise || !noise_key(k))
                info += " " + k + "=" + quote(text_of(v));
        if (!info.empty())
            os << "info" << info << "\n";
        if (d.xmp_doc) {
            std::ostringstream x;
            xml_format().dump(*d.xmp_doc, x, o);
            std::istringstream in(x.str());
            for (std::string l; std::getline(in, l);)
                if (opt.noise || !noise_path(l))
                    os << "xmp " << l << "\n";
        } else if (!d.xmp.empty()) {
            os << "xmp bytes=" << d.xmp.size() << " xxh3=" << hash128(d.xmp).hex().substr(0, 16) << "\n";
        }
        for (std::size_t i = 0; i < d.pages.size(); ++i) {
            auto& p = d.pages[i];
            os << "== page " << i + 1 << " " << p.box << " rotate=" << p.rotate << "\n";
            for (auto& l : p.content.lines)
                os << l << "\n";
            for (auto& im : p.content.images)
                os << "[image " << im << "]\n";
            if (!p.annots.empty()) {
                os << "== page " << i + 1 << " annots\n";
                for (auto& a : p.annots)
                    os << annot_line(a) << "\n";
            }
        }
        if (!d.fields.empty()) {
            os << "== fields\n";
            for (auto& f : d.fields)
                os << f.fqn << " (" << f.ft.substr(f.ft.empty() ? 0 : 1) << ") = " << f.display << "\n";
        }
    }

    const EnvelopeMap* envelope(const Document& d) const override { return &doc(d).map; }

    nlohmann::ordered_json inspect(const Document& dd) const override {
        const PdfDoc& d = doc(dd);
        nlohmann::ordered_json r;
        r["version"] = d.version;
        r["pages"] = d.pages.size();
        r["revisions"] = d.revisions;
        r["xref"] = d.xref_stream ? "stream" : "table";
        r["encrypted"] = d.locked ? "password" : d.encrypted ? "permissions-only" : "no";
        r["integrity"] = "ok"; // PDF has no integrity fields (pdf.md §7.1)
        auto& sigs = r["signatures"] = nlohmann::ordered_json::array();
        for (auto& f : d.fields)
            if (f.signature)
                sigs.push_back({{"field", f.fqn}, {"status", "no-crypto"}});
        if (d.docmdp)
            r["certification"] = d.docmdp;
        auto& units = r["units"] = nlohmann::ordered_json::array();
        for (std::size_t i = 0; i < d.pages.size(); ++i)
            units.push_back({{"at", page_path(i)}, {"kind", "page"}, {"annots", d.pages[i].annots.size()}});
        for (auto& f : d.fields)
            units.push_back(
                {{"at", "/fields/" + pointer_escape(f.fqn)}, {"kind", "field"}, {"value", f.display}});
        auto& warn = r["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : d.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return r;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json j;
        j["pages"] = {doc(a).pages.size(), doc(b).pages.size()};
        std::uint64_t ins = 0, del = 0;
        for (auto& c : cs)
            if (c.kind == "text")
                ins += c.metrics.value("words_inserted", std::uint64_t{0}),
                    del += c.metrics.value("words_deleted", std::uint64_t{0});
        j["words_inserted"] = ins, j["words_deleted"] = del;
        return j;
    }
};

// ---- Merge (pdf.md §6, carrier logic D-46) ----

struct Row {
    enum Kind : std::uint8_t { info, annot, field, extra } kind;
    std::string at, key;
    std::size_t page = 0;                        // annots: base page index
    std::array<std::optional<std::size_t>, 3> i; // unit index per side (by Side)
    LeafOutcome outcome;
    std::optional<std::size_t> conflict;
};

MergeResult PdfFormat::merge(const Document& base_d, const Document& ours_d, const Document& theirs_d,
                             const MergeOptions& mo) const {
    const std::array<const PdfDoc*, 3> docs{&doc(base_d), &doc(ours_d), &doc(theirs_d)};
    auto D = [&](Side s) -> const PdfDoc& { return *docs[static_cast<std::size_t>(s)]; };
    const Opts opt = options(mo.ctx.format_options);
    if (opt.signatures == "resign")
        unsupported("--signatures=resign needs --sign-command (not implemented)");
    if (opt.layout != "stable" && opt.layout != "compact")
        unsupported("--layout=" + opt.layout);
    Opts all = opt;
    all.layers = {"meta", "pages", "text", "annots", "forms"}; // merge decisions need every layer
    MergeResult res;
    for (Side s : {Side::base, Side::ours, Side::theirs})
        for (auto w : D(s).warnings) {
            w.message = std::string(to_string(s)) + ": " + w.message;
            res.warnings.push_back(std::move(w));
        }
    auto& co = res.changes_ours;
    auto& ct = res.changes_theirs;
    co = diff_docs(D(Side::base), D(Side::ours), all, Side::ours, mo.ctx);
    ct = diff_docs(D(Side::base), D(Side::theirs), all, Side::theirs, mo.ctx);
    auto ids_at = [&](const std::string& path, bool below = false) {
        std::vector<std::string> ids;
        for (auto* cs : {&co, &ct})
            for (auto& c : *cs)
                if (c.at.path == path || (below && path_within(c.at.path, path)))
                    ids.push_back(c.id);
        return ids;
    };
    std::vector<Warning> sub_warnings; // policy warnings of partial calls; resolve-unmatched is redone below
    auto policy = [&](std::vector<Conflict>& cs) {
        apply_conflict_policy(cs, mo.conflict, sub_warnings);
        std::erase_if(sub_warnings, [](const Warning& w) { return w.code == "resolve-unmatched"; });
    };
    auto side_of = [](const std::string& choice) {
        return choice == "theirs" ? Side::theirs : choice == "base" ? Side::base : Side::ours;
    };
    auto finish = [&](std::unique_ptr<PdfDoc> out) {
        std::set<std::string> ids;
        for (auto& c : res.conflicts)
            ids.insert(c.id);
        for (auto& [id, choice] : mo.conflict.resolutions)
            if (!ids.count(id))
                res.warnings.push_back({"resolve-unmatched", {}, "no conflict with id " + id});
        res.warnings.insert(res.warnings.end(), sub_warnings.begin(), sub_warnings.end());
        std::set<std::string> conflicted;
        for (auto& c : res.conflicts)
            conflicted.insert(c.changes.begin(), c.changes.end());
        for (auto* cs : {&co, &ct})
            for (auto& c : *cs)
                if (!conflicted.count(c.id))
                    res.auto_merged.push_back({c.id, "independent"});
        res.document = std::move(out);
        return std::move(res);
    };

    // Encrypted with a password: the whole file is one opaque leaf (D-41).
    if (D(Side::base).locked || D(Side::ours).locked || D(Side::theirs).locked) {
        const Hash128 hb = hash128(D(Side::base).src.bytes()), ho = hash128(D(Side::ours).src.bytes()),
                      ht = hash128(D(Side::theirs).src.bytes());
        auto lo = opaque_leaf(&hb, &ho, &ht);
        Side take = lo.take.value_or(Side::ours);
        if (lo.rule == MergeRule::conflict) {
            res.conflicts.push_back(make_conflict({"/encrypted"}, ids_at("/encrypted"), lo.reason));
            policy(res.conflicts);
            take = side_of(res.conflicts[0].resolved_as);
        }
        Limits fresh = mo.ctx.budget.fresh();
        Context c{mo.ctx.registry, fresh, mo.ctx.depth, mo.ctx.format_options};
        return finish(load_doc(D(take).src, c));
    }

    // 1. The carrier: the side that changed page content (pdf.md §6 step 1). Both: conflict at /pages,
    // since page-content merge is post-v1 (D-46).
    const Alignment ao = align(D(Side::base), D(Side::ours)), at = align(D(Side::base), D(Side::theirs));
    Side C = Side::ours;
    auto same_bytes = [&](Side x, Side y) {
        auto a = D(x).src.bytes(), b = D(y).src.bytes();
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
    };
    if ((at.changed && !ao.changed) || (!ao.changed && same_bytes(Side::ours, Side::base)))
        C = Side::theirs; // an untouched ours: theirs' bytes need no update section
    if (ao.changed && at.changed) {
        bool same = D(Side::ours).pages.size() == D(Side::theirs).pages.size();
        for (std::size_t k = 0; same && k < D(Side::ours).pages.size(); ++k)
            same = D(Side::ours).pages[k].fp == D(Side::theirs).pages[k].fp;
        if (!same) {
            std::vector<std::string> ids;
            for (auto* cs : {&co, &ct})
                for (auto& c : *cs)
                    if (c.at.path.starts_with("/pages/") && c.kind != "annotation")
                        ids.push_back(c.id);
            std::vector<Conflict> pc{make_conflict({"/pages"}, ids, "both-modified")};
            pc[0].choices = {"ours", "theirs", "base"};
            policy(pc);
            C = side_of(pc[0].resolved_as);
            res.warnings.push_back({"content-not-merged", "/pages",
                                    "both sides changed page content; page-content merge is post-v1, " +
                                        pc[0].resolved_as + "' pages are kept"});
            res.conflicts.push_back(std::move(pc[0]));
        }
    }
    const PdfDoc& car = D(C);
    auto b2 = [&](Side s, std::size_t b) -> std::optional<std::size_t> {
        return s == Side::base ? std::optional(b) : (s == Side::ours ? ao : at).a2b[b];
    };
    // The page holding a side's annotations for base page b. A side that replaced or deleted the page
    // counts as leaving its annotations alone: they went with the page content (carrier logic, step 1).
    auto apage = [&](Side s, std::size_t b) -> const Page& {
        auto p = b2(s, b);
        return p ? D(s).pages[*p] : D(Side::base).pages[b];
    };

    // 2-5. Keyed rows: info keys, annotations per base page, form fields, carrier-only structures.
    std::vector<Row> rows;
    auto add_rows = [&](Row::Kind kind, const std::string& prefix, std::size_t page,
                        const std::array<std::vector<KeyedUnit>, 3>& lists) {
        std::array<std::vector<std::pair<std::string, std::string>>, 3> kv;
        std::array<std::map<std::string, std::size_t>, 3> idx;
        for (std::size_t s = 0; s < 3; ++s)
            for (std::size_t k = 0; k < lists[s].size(); ++k)
                kv[s].emplace_back(lists[s][k].key, lists[s][k].value), idx[s][lists[s][k].key] = k;
        for (auto& r : keyed_merge<std::string, std::string>(kv[0], kv[1], kv[2])) {
            if (r.outcome.rule == MergeRule::unchanged)
                continue;
            Row row{kind, prefix + pointer_escape(r.key), r.key, page, {}, r.outcome, {}};
            for (std::size_t s = 0; s < 3; ++s)
                if (idx[s].count(r.key))
                    row.i[s] = idx[s][r.key];
            rows.push_back(std::move(row));
        }
    };
    add_rows(Row::info, "/info/", 0,
             {info_units(D(Side::base), false), info_units(D(Side::ours), false),
              info_units(D(Side::theirs), false)});
    for (std::size_t b = 0; b < D(Side::base).pages.size(); ++b) {
        std::array<std::vector<KeyedUnit>, 3> l;
        for (Side s : {Side::base, Side::ours, Side::theirs})
            l[static_cast<std::size_t>(s)] = annot_units(apage(s, b));
        add_rows(Row::annot, page_path(b) + "/annots/", b, l);
    }
    add_rows(Row::field, "/fields/", 0,
             {field_units(D(Side::base)), field_units(D(Side::ours)), field_units(D(Side::theirs))});
    add_rows(Row::extra, "/", 0,
             {extra_units(D(Side::base)), extra_units(D(Side::ours)), extra_units(D(Side::theirs))});

    auto value = [&](const Row& r, Side s) -> std::optional<std::string> {
        auto i = r.i[static_cast<std::size_t>(s)];
        if (!i)
            return std::nullopt;
        const PdfDoc& d = D(s);
        switch (r.kind) {
        case Row::info:
            return info_units(d, false)[*i].value;
        case Row::annot:
            return apage(s, r.page).annots[*i].value;
        case Row::field:
            return d.fields[*i].ft + " " + d.fields[*i].value;
        default:
            return d.extra[*i].second;
        }
    };
    // Does writing side s's state (nullopt: absent) change the carrier's unit?
    auto needs = [&](const Row& r, std::optional<Side> s) {
        auto want = s ? value(r, *s) : std::nullopt;
        return want != value(r, C);
    };
    // Can it be written onto the carrier?
    auto transferable = [&](const Row& r, std::optional<Side> s) {
        auto ci = r.i[static_cast<std::size_t>(C)];
        switch (r.kind) {
        case Row::info:
            return true;
        case Row::annot: { // deletions apply; additions need an unchanged anchor page (pdf.md §6 step 3)
            if (!s)
                return true;
            auto cp = b2(C, r.page), sp = b2(*s, r.page);
            return cp && sp && car.pages[*cp].fp == D(*s).pages[*sp].fp;
        }
        case Row::field: { // values only; structure follows the carrier (§6 step 4)
            auto si = s ? r.i[static_cast<std::size_t>(*s)] : std::nullopt;
            return ci && si && car.fields[*ci].ft == D(*s).fields[*si].ft && !car.fields[*ci].signature &&
                   !D(*s).fields[*si].signature;
        }
        default:
            return false; // outline, names, page labels: the carrier's (§6 step 5)
        }
    };
    std::vector<Conflict> rc;
    for (auto& r : rows) {
        auto ids = ids_at(r.at);
        if (r.outcome.rule == MergeRule::conflict) {
            r.conflict = rc.size();
            rc.push_back(make_conflict({r.at}, ids, r.outcome.reason));
        } else if (needs(r, r.outcome.take) && !transferable(r, r.outcome.take)) {
            r.conflict = rc.size();
            rc.push_back(make_conflict({r.at}, ids,
                                       r.kind == Row::annot   ? "dependency"
                                       : r.kind == Row::field ? "structure"
                                                              : "carrier"));
        }
    }
    policy(rc);
    for (auto& r : rows)
        if (r.conflict)
            r.conflict = *r.conflict + res.conflicts.size();
    res.conflicts.insert(res.conflicts.end(), rc.begin(), rc.end());

    // XMP through the xml module (§6 step 2).
    std::optional<std::vector<std::byte>> xmp;
    {
        auto &xb = D(Side::base), &xo = D(Side::ours), &xt = D(Side::theirs);
        // Only a non-carrier side's non-noise XMP change needs a merge; noise stays the carrier's (§6 step
        // 2).
        bool any = false;
        for (auto* cs : {&co, &ct})
            for (auto& c : *cs)
                any |= c.side != C && path_within(c.at.path, "/xmp");
        if (any && xb.xmp_doc && xo.xmp_doc && xt.xmp_doc) {
            auto xr = xml_format().merge(*xb.xmp_doc, *xo.xmp_doc, *xt.xmp_doc, mo);
            VectorSink sink;
            Limits fresh = mo.ctx.budget.fresh();
            Context c{mo.ctx.registry, fresh, mo.ctx.depth + 1, mo.ctx.format_options};
            xml_format().save(*xr.document, sink, SaveOptions{c});
            xmp = std::move(sink.data);
            for (auto x : xr.conflicts) {
                if (noise_path(x.at.path)) // both sides saved: dates and instance ids are noise (§4)
                    continue;
                x.at.path = "/xmp" + x.at.path;
                res.conflicts.push_back(std::move(x));
            }
            for (auto& w : xr.warnings)
                if (w.code != "resolve-unmatched")
                    res.warnings.push_back({w.code, w.at.empty() ? "/xmp" : "/xmp" + w.at, w.message});
            // the xml merge's changes are reported by diff_docs already
        } else if (any) { // one side without (parseable) XMP: an opaque leaf
            const Hash128 hb = hash128(xb.xmp), ho = hash128(xo.xmp), ht = hash128(xt.xmp);
            auto lo = opaque_leaf(&hb, &ho, &ht);
            Side take = lo.take.value_or(Side::ours);
            if (lo.rule == MergeRule::conflict) {
                std::vector<Conflict> xc{make_conflict({"/xmp"}, ids_at("/xmp", true), lo.reason)};
                policy(xc);
                take = side_of(xc[0].resolved_as);
                res.conflicts.push_back(std::move(xc[0]));
            }
            xmp = D(take).xmp;
        }
        if (xmp && *xmp == car.xmp)
            xmp.reset();
    }

    // Apply onto a fresh copy of the carrier.
    auto dst = reopen(car);
    std::set<QPDFObjGen> touched;
    std::vector<std::string> edits; // addresses of the units written (for DocMDP)
    auto take_of = [&](const Row& r) -> std::optional<Side> {
        if (!r.conflict)
            return r.outcome.take;
        auto& c = res.conflicts[*r.conflict];
        return take_for(c.resolved_as, r.i[0].has_value(), r.i[1].has_value(), r.i[2].has_value());
    };
    std::array<std::unique_ptr<Importer>, 3> importers;
    auto importer = [&](Side s) -> Importer& {
        auto& imp = importers[static_cast<std::size_t>(s)];
        if (!imp) {
            imp = std::make_unique<Importer>(Importer{*dst, {}});
            const auto& al = s == Side::ours ? ao : at;
            for (std::size_t b = 0; b < D(Side::base).pages.size(); ++b) {
                auto sp = s == Side::base ? std::optional(b) : al.a2b[b];
                auto cp = b2(C, b);
                if (sp && cp)
                    imp->pages[D(s).pages[*sp].obj.getObjGen()] =
                        dst->getObject(car.pages[*cp].obj.getObjGen());
            }
        }
        return *imp;
    };
    auto src_unit = [&](const Row& r, Side s) -> QPDFObjectHandle {
        auto i = *r.i[static_cast<std::size_t>(s)];
        return r.kind == Row::annot ? apage(s, r.page).annots[i].obj : D(s).fields[i].obj;
    };

    // Info keys.
    for (auto& r : rows) {
        if (r.kind != Row::info)
            continue;
        auto s = take_of(r);
        if (!needs(r, s))
            continue;
        auto trailer = dst->getTrailer();
        auto infod = trailer.getKey("/Info");
        if (!infod.isDictionary()) {
            infod = dst->makeIndirectObject(QPDFObjectHandle::newDictionary());
            trailer.replaceKey("/Info", infod);
        }
        if (!s)
            infod.removeKey("/" + r.key);
        else
            for (auto& [k, v] : D(*s).info)
                if (k == r.key)
                    infod.replaceKey("/" + k, importer(*s)(v));
        edits.push_back(r.at);
    }

    // Annotations, per carrier page; replies are re-linked by key once all are in place.
    struct Placed {
        QPDFObjectHandle obj;
        std::string irt;
    };
    std::map<std::size_t, std::map<std::string, Placed>> placed; // carrier page -> key -> annotation
    std::set<std::size_t> pages_touched;
    for (auto& r : rows) {
        if (r.kind != Row::annot)
            continue;
        auto s = take_of(r);
        const bool has_c = r.i[static_cast<std::size_t>(C)].has_value();
        if (!needs(r, s))
            continue;
        if (!transferable(r, s)) {
            res.warnings.push_back(
                {"conflict-unapplied", r.at, "annotation anchor changed; the carrier's is kept"});
            continue;
        }
        auto cp = b2(C, r.page);
        if (!cp)
            continue;
        auto page = dst->getObject(car.pages[*cp].obj.getObjGen());
        auto annots = page.getKey("/Annots");
        auto list = annots.isArray() ? annots.getArrayAsVector() : std::vector<QPDFObjectHandle>{};
        QPDFObjectHandle existing;
        if (has_c)
            existing =
                dst->getObject(car.pages[*cp].annots[*r.i[static_cast<std::size_t>(C)]].obj.getObjGen());
        if (!s) { // delete the carrier's annotation and its popup
            auto popup = existing.getKey("/Popup");
            std::erase_if(list, [&](QPDFObjectHandle& a) {
                return a.isSameObjectAs(existing) || (popup.isIndirect() && a.isSameObjectAs(popup));
            });
        } else {
            const Annot& src = apage(*s, r.page).annots[*r.i[static_cast<std::size_t>(*s)]];
            auto obj = src.obj; // a copy of the handle: qpdf 11's getters are non-const (#143)
            auto& imp = importer(*s);
            auto dict = QPDFObjectHandle::newDictionary();
            for (auto& k : obj.getKeys())
                if (k != "/P" && k != "/Popup" && k != "/IRT" && k != "/Parent" && (k != "/AP" || !has_c))
                    dict.replaceKey(k, imp(obj.getKey(k)));
            dict.replaceKey("/P", page);
            if (has_c) { // modified: keep the carrier's object and popup; /AP is stale (pdf.md §6 step 3)
                if (existing.getKey("/Popup").isIndirect())
                    dict.replaceKey("/Popup", existing.getKey("/Popup"));
                dst->replaceObject(existing.getObjGen(), dict);
                existing = dst->getObject(existing.getObjGen());
                if (obj.hasKey("/AP"))
                    res.warnings.push_back(
                        {"derived-stale", r.at, "appearance stream removed; viewers regenerate it"});
            } else {
                existing = dst->makeIndirectObject(dict);
                list.push_back(existing);
                auto popup = obj.getKey("/Popup");
                if (popup.isDictionary()) {
                    auto pd = QPDFObjectHandle::newDictionary();
                    for (auto& k : popup.getKeys())
                        if (k != "/P" && k != "/Parent")
                            pd.replaceKey(k, imp(popup.getKey(k)));
                    pd.replaceKey("/P", page), pd.replaceKey("/Parent", existing);
                    auto pobj = dst->makeIndirectObject(pd);
                    existing.replaceKey("/Popup", pobj);
                    list.push_back(pobj);
                }
            }
            placed[*cp][r.key] = {existing, src.irt};
        }
        page.replaceKey("/Annots", QPDFObjectHandle::newArray(list));
        pages_touched.insert(*cp);
        edits.push_back(r.at);
    }
    for (auto& [cp, by_key] : placed)
        for (auto& [key, p] : by_key) {
            if (p.irt.empty())
                continue;
            QPDFObjectHandle parent;
            if (auto f = by_key.find(p.irt); f != by_key.end())
                parent = f->second.obj;
            else
                for (auto& a : car.pages[cp].annots)
                    if (a.key == p.irt)
                        parent = dst->getObject(a.obj.getObjGen());
            if (parent.isIndirect())
                p.obj.replaceKey("/IRT", parent);
            else
                res.warnings.push_back({"reply-orphaned", page_path(cp) + "/annots/" + pointer_escape(key),
                                        "the annotation replied to is not in the result; /IRT dropped"});
        }

    // Form field values (pdf.md §6 step 4).
    const bool pdf2 = car.version >= "2.0";
    for (auto& r : rows) {
        if (r.kind != Row::field)
            continue;
        auto s = take_of(r);
        if (!needs(r, s))
            continue;
        if (!transferable(r, s)) {
            res.warnings.push_back({"conflict-unapplied", r.at, "field structure follows the carrier"});
            continue;
        }
        auto field = dst->getObject(car.fields[*r.i[static_cast<std::size_t>(C)]].obj.getObjGen());
        QPDFFormFieldObjectHelper ff(field);
        auto v = src_unit(r, *s).getKey("/V"); // transferable: s has the field
        ff.setV(v.isNull() ? QPDFObjectHandle::newNull() : importer(*s)(v), !pdf2);
        if (ff.isText() || ff.isChoice()) {
            QPDFAcroFormDocumentHelper afdh(*dst);
            for (auto& w : afdh.getAnnotationsForField(ff)) {
                try {
                    ff.generateAppearance(w);
                } catch (const std::exception&) {
                    res.warnings.push_back({"derived-stale", r.at, "appearance stream not regenerated"});
                }
            }
        }
        if (!pdf2)
            res.warnings.push_back({"need-appearances", r.at, "/NeedAppearances set for viewers"});
        edits.push_back(r.at);
    }

    // XMP.
    if (xmp) {
        auto root = dst->getRoot();
        auto md = root.getKey("/Metadata");
        const std::string data = str(*xmp);
        if (md.isStream()) {
            md.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
            md.getDict().removeKey("/Filter"), md.getDict().removeKey("/DecodeParms");
            touched.insert(md.getObjGen());
        } else {
            auto s = dst->newStream(data);
            s.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
            s.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
            root.replaceKey("/Metadata", s);
        }
        edits.push_back("/xmp");
    }

    // 6. Signatures and the write mode (pdf.md §6 step 6, §7.1).
    const bool changed = !edits.empty();
    std::unique_ptr<PdfDoc> out;
    Limits fresh = mo.ctx.budget.fresh(); // the result is a new document (#51)
    Context oc{mo.ctx.registry, fresh, mo.ctx.depth, mo.ctx.format_options};
    const bool signed_ = car.signed_count() > 0;
    // An incremental update needs a sound xref chain to append to and, in v1, an unencrypted carrier.
    const bool repaired = std::any_of(car.warnings.begin(), car.warnings.end(),
                                      [](const Warning& w) { return w.code == "framing-inconsistent"; });
    const bool incremental = opt.layout == "stable" && !car.encrypted && car.startxref > 0 && !repaired;
    std::vector<std::byte> bytes;
    if (!changed) {
        bytes.assign(car.src.bytes().begin(), car.src.bytes().end());
    } else if (incremental) {
        if (signed_ && opt.signatures == "strip")
            strip_signatures(car, *dst, res.warnings);
        else
            for (auto& f : car.fields)
                if (f.signature)
                    res.warnings.push_back({"signature-not-covering-changes",
                                            "/fields/" + pointer_escape(f.fqn),
                                            "the signature (no-crypto) covers an earlier revision only"});
        bytes = write_incremental(car, *dst, touched);
    } else {
        if (opt.layout == "stable")
            res.warnings.push_back({"layout-rewritten", "",
                                    car.encrypted
                                        ? "encrypted carrier: written as a full rewrite (qpdf)"
                                        : "carrier xref repaired by qpdf: written as a full rewrite"});
        if (signed_ && opt.layout == "stable" && opt.signatures == "auto") {
            // A signed document is never silently unsigned (§6 step 6): conflict, the policy side is written.
            std::vector<std::string> ids;
            for (auto* cs : {&co, &ct})
                for (auto& c : *cs)
                    ids.push_back(c.id);
            std::vector<Conflict> sc{make_conflict({"/signatures"}, ids, "signature")};
            policy(sc);
            const Side s = side_of(sc[0].resolved_as);
            res.conflicts.push_back(std::move(sc[0]));
            res.warnings.insert(res.warnings.end(), sub_warnings.begin(), sub_warnings.end());
            sub_warnings.clear();
            return finish(load_doc(D(s).src, oc));
        }
        if (signed_ && opt.signatures != "keep")
            strip_signatures(car, *dst, res.warnings);
        else
            for (auto& f : car.fields)
                if (f.signature)
                    res.warnings.push_back({"signature-invalidated", "/fields/" + pointer_escape(f.fqn),
                                            "signature kept; the rewrite invalidates it"});
        bytes = write_full(*dst, car.xref_stream);
    }
    if (changed && car.docmdp) { // pdf.md §7.1 DocMDP
        const bool annots = std::any_of(edits.begin(), edits.end(), [](const std::string& e) {
            return e.find("/annots/") != std::string::npos;
        });
        if (car.docmdp == 1 || (car.docmdp == 2 && annots))
            res.warnings.push_back({"certification-violated", "/signatures",
                                    "the merged changes exceed the certification's DocMDP permissions (P=" +
                                        std::to_string(car.docmdp) + ")"});
    }
    out = load_doc(ByteSource::from_memory(std::move(bytes), "merged.pdf"), oc); // validation (§8)
    if (incremental && changed &&
        (out->src.size() < car.src.size() ||
         std::memcmp(out->src.bytes().data(), car.src.bytes().data(), car.src.size()) != 0))
        throw Error(ErrorCategory::internal, "PDF writer: the carrier is not a prefix of the output");
    if (!out->warnings.empty())
        throw Error(ErrorCategory::internal,
                    "PDF writer: the output does not reload cleanly: " + out->warnings[0].message);
    if (C != Side::ours)
        res.warnings.push_back(
            {"carrier", "", std::string(to_string(C)) + " is the carrier (its page content)"});
    return finish(std::move(out));
}

} // namespace

std::vector<std::byte> rewrite(std::span<const std::byte> file, bool object_streams,
                               const char* user_password) {
    QPDF q;
    q.setSuppressWarnings(true);
    q.processMemoryFile("fixture.pdf", reinterpret_cast<const char*>(file.data()), file.size());
    QPDFWriter w(q);
    w.setOutputMemory();
    w.setStaticID(true);
    if (object_streams)
        w.setObjectStreamMode(qpdf_o_generate);
    if (user_password) {
        w.setStaticAesIV(true);
        w.setR6EncryptionParameters(user_password, "owner", true, true, true, true, true, true, qpdf_r3p_full,
                                    true);
    }
    w.write();
    auto b = w.getBufferSharedPointer();
    auto p = reinterpret_cast<const std::byte*>(b->getBuffer());
    return {p, p + b->getSize()};
}

std::unique_ptr<Format> make_format() {
    return std::make_unique<PdfFormat>();
}

} // namespace semsplinter::pdf
