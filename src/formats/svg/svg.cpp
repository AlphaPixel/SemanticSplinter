// SVG (docs/formats/svg.md): an xml::Dialect (ids, references, typed attributes, editor state, conflict
// layers) on the xml module's tree, matcher, merge, and splice writer; .svgz adds a gzip layer around it.
#include <semsplinter/emit.hpp>
#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/formats/svg.hpp>
#include <semsplinter/formats/xml.hpp>
#include <semsplinter/sequence.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <optional>
#include <sstream>

namespace semsplinter::svg {

namespace {

constexpr std::string_view kSvgNs = "http://www.w3.org/2000/svg";
constexpr std::string_view kInkNs = "http://www.inkscape.org/namespaces/inkscape";
constexpr std::string_view kSodiNs = "http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd";
constexpr std::string_view kXlinkNs = "http://www.w3.org/1999/xlink";
constexpr std::string_view kAiNs = "http://ns.adobe.com/AdobeIllustrator/10.0/"; // `i:pgf`

using xml::Attr;
using xml::Node;

bool svg_el(const Node& n, std::initializer_list<std::string_view> locals) {
    return n.kind == Node::Kind::element && (n.name.uri == kSvgNs || n.name.uri.empty()) &&
           std::find(locals.begin(), locals.end(), n.name.local) != locals.end();
}

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n");
    return b == std::string_view::npos ? std::string()
                                       : std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
}

// ---- Numbers, colors, transforms, paths: comparison forms (svg.md §4) ----

std::optional<double> number(std::string_view s) {
    std::istringstream is{std::string(s)};
    is.imbue(std::locale::classic());
    double v = 0;
    if (s.empty() || space(s[0]) || !(is >> v) || is.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(v))
        return std::nullopt;
    return v;
}

// Shortest text that reads back as v; rounded to 9 decimals so accumulated relative coordinates compare.
std::string fmt(double v) {
    v = std::round(v * 1e9) / 1e9;
    if (v == 0)
        v = 0;                             // no "-0"
    const bool plain = std::abs(v) < 1e15; // fixed notation: "20", not "2e+01"
    for (int p = plain ? 0 : 1;; ++p) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        if (plain)
            os << std::fixed;
        os.precision(p);
        os << v;
        if (p >= 17 || number(os.str()) == v)
            return os.str();
    }
}

// End of the number starting at i (i itself when there is none): sign, digits, fraction, exponent.
std::size_t scan_number(std::string_view s, std::size_t i) {
    auto digits = [&](std::size_t j) {
        while (j < s.size() && s[j] >= '0' && s[j] <= '9')
            ++j;
        return j;
    };
    std::size_t j = i;
    if (j < s.size() && (s[j] == '+' || s[j] == '-'))
        ++j;
    std::size_t k = digits(j);
    bool any = k > j;
    if (k < s.size() && s[k] == '.') {
        auto f = digits(k + 1);
        any |= f > k + 1;
        k = f;
    }
    if (!any)
        return i;
    if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
        std::size_t e = k + 1;
        if (e < s.size() && (s[e] == '+' || s[e] == '-'))
            ++e;
        if (auto f = digits(e); f > e)
            k = f;
    }
    return k;
}

// Numbers separated by whitespace and commas; nullopt when anything else is there.
std::optional<std::vector<double>> numbers(std::string_view s) {
    std::vector<double> v;
    for (std::size_t i = 0;;) {
        while (i < s.size() && (space(s[i]) || s[i] == ','))
            ++i;
        if (i == s.size())
            return v;
        auto e = scan_number(s, i);
        if (e == i)
            return std::nullopt;
        v.push_back(*number(s.substr(i, e - i)));
        i = e;
    }
}

std::string color_key(std::string_view value) {
    std::string s = trim(value);
    if (s.starts_with("url("))
        return s; // a paint server: ids keep their case
    for (char& c : s)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    static constexpr std::pair<std::string_view, std::string_view> kNamed[] = {
        {"black", "#000000"},   {"white", "#ffffff"},   {"red", "#ff0000"},   {"lime", "#00ff00"},
        {"blue", "#0000ff"},    {"yellow", "#ffff00"},  {"cyan", "#00ffff"},  {"aqua", "#00ffff"},
        {"magenta", "#ff00ff"}, {"fuchsia", "#ff00ff"}, {"gray", "#808080"},  {"grey", "#808080"},
        {"silver", "#c0c0c0"},  {"maroon", "#800000"},  {"olive", "#808000"}, {"green", "#008000"},
        {"purple", "#800080"},  {"teal", "#008080"},    {"navy", "#000080"},  {"orange", "#ffa500"}};
    for (auto [name, hex] : kNamed)
        if (s == name)
            return std::string(hex);
    if (s.size() == 4 && s[0] == '#')
        return std::string{'#', s[1], s[1], s[2], s[2], s[3], s[3]};
    if (s.starts_with("rgb(") && s.ends_with(")"))
        if (auto v = numbers(std::string_view(s).substr(4, s.size() - 5)); v && v->size() == 3) {
            std::string r = "#";
            for (double x : *v) {
                if (x < 0 || x > 255 || x != std::floor(x))
                    return s;
                static constexpr char kHex[] = "0123456789abcdef";
                r += kHex[static_cast<int>(x) / 16], r += kHex[static_cast<int>(x) % 16];
            }
            return r;
        }
    return s;
}

bool color_property(std::string_view name) {
    return name == "fill" || name == "stroke" || name == "stop-color" || name == "flood-color" ||
           name == "lighting-color" || name == "color";
}

std::string value_key(std::string_view name, std::string_view v) {
    if (color_property(name))
        return color_key(v);
    if (auto x = number(trim(v)))
        return fmt(*x);
    return std::string(v);
}

// The resulting matrix of a transform list (translate(10) == matrix(1,0,0,1,10,0)); nullopt if unparseable.
std::optional<std::string> transform_key(std::string_view s) {
    constexpr double kPi = 3.14159265358979323846;
    std::array<double, 6> m{1, 0, 0, 1, 0, 0};
    for (std::size_t i = 0;;) {
        while (i < s.size() && (space(s[i]) || s[i] == ','))
            ++i;
        if (i == s.size())
            break;
        auto open = s.find('(', i), close = s.find(')', i);
        if (open == std::string_view::npos || close == std::string_view::npos || close < open)
            return std::nullopt;
        const std::string name = trim(s.substr(i, open - i));
        auto a = numbers(s.substr(open + 1, close - open - 1));
        if (!a)
            return std::nullopt;
        const auto n = a->size();
        auto arg = [&](std::size_t k, double def) { return k < n ? (*a)[k] : def; };
        std::array<double, 6> t{1, 0, 0, 1, 0, 0};
        if (name == "matrix" && n == 6) {
            std::copy(a->begin(), a->end(), t.begin());
        } else if (name == "translate" && (n == 1 || n == 2)) {
            t[4] = arg(0, 0), t[5] = arg(1, 0);
        } else if (name == "scale" && (n == 1 || n == 2)) {
            t[0] = arg(0, 1), t[3] = arg(1, arg(0, 1));
        } else if (name == "rotate" && (n == 1 || n == 3)) {
            const double r = arg(0, 0) * kPi / 180, c = std::cos(r), sn = std::sin(r), x = arg(1, 0),
                         y = arg(2, 0);
            t = {c, sn, -sn, c, x - c * x + sn * y, y - sn * x - c * y};
        } else if (name == "skewX" && n == 1) {
            t[2] = std::tan(arg(0, 0) * kPi / 180);
        } else if (name == "skewY" && n == 1) {
            t[1] = std::tan(arg(0, 0) * kPi / 180);
        } else {
            return std::nullopt;
        }
        m = {m[0] * t[0] + m[2] * t[1], m[1] * t[0] + m[3] * t[1],        m[0] * t[2] + m[2] * t[3],
             m[1] * t[2] + m[3] * t[3], m[0] * t[4] + m[2] * t[5] + m[4], m[1] * t[4] + m[3] * t[5] + m[5]};
        i = close + 1;
    }
    std::string r = "matrix(";
    for (std::size_t k = 0; k < 6; ++k)
        r += (k ? "," : "") + fmt(m[k]);
    return r + ")";
}

// One subpath: from an M/m to the next. `text` is its source, a relative leading moveto made absolute so the
// subpath stands alone; `key` is its absolute form with implicit repeats expanded (svg.md §4, §6.6).
struct Subpath {
    std::string text, key;
};

std::optional<std::vector<Subpath>> subpaths(std::string_view d) {
    std::vector<Subpath> out;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    std::size_t i = 0, chunk = 0, rest = 0; // chunk: where the current subpath starts; rest: after its moveto
    bool rel_move = false, implicit = false;
    auto skip = [&] {
        while (i < d.size() && (space(d[i]) || d[i] == ','))
            ++i;
    };
    auto close_chunk = [&](std::size_t end) {
        if (out.empty())
            return;
        auto& sp = out.back();
        if (!rel_move) {
            sp.text = trim(d.substr(chunk, end - chunk));
            return;
        }
        std::string tail = trim(d.substr(rest, end - rest));
        tail.erase(0, tail.find_first_not_of(", \t\r\n") == std::string::npos
                          ? tail.size()
                          : tail.find_first_not_of(", \t\r\n"));
        sp.text = "M" + fmt(sx) + "," + fmt(sy) + (implicit ? " l " : tail.empty() ? "" : " ") + tail;
    };
    auto arity = [](char c) {
        switch (c) {
        case 'M':
        case 'L':
        case 'T':
            return 2;
        case 'H':
        case 'V':
            return 1;
        case 'C':
            return 6;
        case 'S':
        case 'Q':
            return 4;
        case 'A':
            return 7;
        case 'Z':
            return 0;
        default:
            return -1;
        }
    };
    skip();
    while (i < d.size()) {
        const char c = d[i];
        const char C = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        const int n = arity(C);
        if (n < 0)
            return std::nullopt;
        const bool rel = c != C;
        if (C == 'M') {
            close_chunk(i);
            out.emplace_back();
            chunk = i, rel_move = rel, implicit = false;
        } else if (out.empty()) {
            return std::nullopt; // path data starts with a moveto
        }
        ++i;
        std::string& key = out.back().key;
        if (C == 'Z') {
            cx = sx, cy = sy;
            key += " Z";
            skip();
            continue;
        }
        for (bool first = true;; first = false) {
            std::array<double, 7> a{};
            for (int k = 0; k < n; ++k) {
                skip();
                if (C == 'A' && (k == 3 || k == 4)) { // flags may run together: "a1 1 0 00 1 1"
                    if (i == d.size() || (d[i] != '0' && d[i] != '1'))
                        return std::nullopt;
                    a[static_cast<std::size_t>(k)] = d[i++] - '0';
                    continue;
                }
                auto e = scan_number(d, i);
                if (e == i)
                    return std::nullopt;
                a[static_cast<std::size_t>(k)] = *number(d.substr(i, e - i));
                i = e;
            }
            const double ox = rel ? cx : 0, oy = rel ? cy : 0;
            char K = C == 'M' && !first ? 'L' : C;
            if (C == 'H') {
                a[0] += ox, cx = a[0];
            } else if (C == 'V') {
                a[0] += oy, cy = a[0];
            } else if (C == 'A') {
                a[5] += ox, a[6] += oy, cx = a[5], cy = a[6];
            } else {
                for (int k = 0; k < n; k += 2)
                    a[static_cast<std::size_t>(k)] += ox, a[static_cast<std::size_t>(k) + 1] += oy;
                cx = a[static_cast<std::size_t>(n) - 2], cy = a[static_cast<std::size_t>(n) - 1];
            }
            if (C == 'M' && first) {
                sx = cx, sy = cy, rest = i;
            }
            key += std::string(key.empty() ? "" : " ") + K;
            for (int k = 0; k < n; ++k)
                key += (k ? "," : "") + fmt(a[static_cast<std::size_t>(k)]);
            skip();
            const bool more = i < d.size() && scan_number(d, i) != i;
            if (C == 'M' && first)
                implicit = more;
            if (!more)
                break;
        }
    }
    close_chunk(d.size());
    return out;
}

std::optional<std::string> path_key(std::string_view d) {
    auto sp = subpaths(d);
    if (!sp)
        return std::nullopt;
    std::string k;
    for (auto& s : *sp)
        k += (k.empty() ? "" : " ") + s.key;
    return k;
}

// Subpath-disjoint edits merge (svg.md §6.6); overlapping ones, or unparseable data, conflict.
std::optional<std::string> merge_path(std::string_view b, std::string_view o, std::string_view t) {
    auto sb = subpaths(b), so = subpaths(o), st = subpaths(t);
    if (!sb || !so || !st)
        return std::nullopt;
    auto items = [](const std::vector<Subpath>& v) {
        std::vector<SeqItem> r;
        for (auto& s : v)
            r.push_back({hash128(s.key), std::nullopt});
        return r;
    };
    std::string out;
    for (auto& c : sequence_merge(items(*sb), items(*so), items(*st))) {
        if (c.kind == SeqKind::conflict)
            return std::nullopt;
        const bool th = c.kind == SeqKind::theirs;
        for (auto i : th ? c.theirs : c.ours)
            out += (out.empty() ? "" : " ") + (th ? *st : *so)[i].text;
    }
    return out;
}

// ---- Conflict layers (svg.md §6.1) ----

constexpr std::string_view kTheirsSuffix = "-splinter-theirs";

// Theirs' copy: its ids suffixed, and references to them inside the copy too.
std::string suffixed(std::string_view x) {
    auto id_at = [&](std::size_t i) { // `id="` or `id='` as an attribute (also xml:id)
        return x.compare(i, 3, "id=") == 0 && i > 0 && (space(x[i - 1]) || x[i - 1] == ':') &&
               i + 3 < x.size() && (x[i + 3] == '"' || x[i + 3] == '\'');
    };
    auto name_char = [](char c) {
        return !space(c) && c != ')' && c != '"' && c != '\'' && c != ';' && c != ',' && c != '<' && c != '>';
    };
    std::set<std::string, std::less<>> ids;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (id_at(i))
            if (auto e = x.find(x[i + 3], i + 4); e != std::string_view::npos)
                ids.emplace(x.substr(i + 4, e - i - 4));
    std::string out;
    for (std::size_t i = 0; i < x.size();) {
        if (x[i] == '#') {
            auto j = i + 1;
            while (j < x.size() && name_char(x[j]))
                ++j;
            out += x.substr(i, j - i);
            if (ids.count(x.substr(i + 1, j - i - 1)))
                out += kTheirsSuffix;
            i = j;
        } else if (id_at(i)) {
            auto e = x.find(x[i + 3], i + 4);
            out += x.substr(i, e - i);
            out += kTheirsSuffix;
            i = e;
        } else {
            out += x[i++];
        }
    }
    return out;
}

// ---- The dialect ----

struct Cfg {
    bool show_editor_state = false, rename_ids = true, path_merge = true;
    bool operator==(const Cfg&) const = default;
};

Cfg config(const OptionValues& o) {
    Cfg c;
    c.show_editor_state = o.count("svg-show-editor-state") > 0;
    if (auto f = o.find("svg-id-collision"); f != o.end()) {
        if (f->second != "rename" && f->second != "conflict")
            throw Error(ErrorCategory::invalid_input,
                        "--svg-id-collision: expected 'rename' or 'conflict', got '" + f->second + "'");
        c.rename_ids = f->second == "rename";
    }
    if (auto f = o.find("svg-path-merge"); f != o.end()) {
        if (f->second == "segments")
            throw Error(ErrorCategory::unsupported,
                        "--svg-path-merge=segments is not implemented yet (svg.md §12)");
        if (f->second != "subpaths" && f->second != "none")
            throw Error(ErrorCategory::invalid_input,
                        "--svg-path-merge: expected 'subpaths' or 'none', got '" + f->second + "'");
        c.path_merge = f->second == "subpaths";
    }
    return c;
}

class SvgDialect final : public xml::Dialect {
public:
    explicit SvgDialect(Cfg c) : cfg_(c) {}

    std::string_view name() const override { return "svg"; }

    int probe(const Node& root) const override {
        if (root.name.local != "svg")
            return 0;
        return root.name.uri == kSvgNs ? 90 : root.name.uri.empty() ? 60 : 0;
    }

    std::span<const xml::IdRule> id_rules(const Node&) const override {
        static constexpr xml::IdRule kIds[] = {{xml::kXmlNs, "id"}, {"", "id"}};
        return kIds;
    }

    // Two sides' additions at one place are both kept, ours first (theirs on top), except where order is the
    // content: text runs and `switch` alternatives.
    bool unordered(const Node& el) const override {
        return !svg_el(el, {"text", "tspan", "textPath", "switch"});
    }

    xml::Mark mark(std::string_view id, std::string_view theirs, const xml::MarkSite& site) const override {
        if (!site.parent ||
            !svg_el(*site.parent, {"svg", "g", "defs", "symbol", "a", "marker", "mask", "pattern"}))
            return Dialect::mark(id, theirs, site); // no `g` allowed here: the comment mark
        const std::string x(id), h(x.substr(x.find('-') + 1, 8));
        return {
            "<g id=\"splinter-conflict-" + h + "\" data-splinter-conflict=\"" + x + "\" xmlns:inkscape=\"" +
                std::string(kInkNs) + "\" inkscape:groupmode=\"layer\" inkscape:label=\"CONFLICT " + x +
                "\"><g id=\"splinter-ours-" + h + "\" inkscape:label=\"ours\" data-splinter-side=\"ours\">",
            "</g><g id=\"splinter-theirs-" + h +
                "\" inkscape:label=\"theirs\" data-splinter-side=\"theirs\" display=\"none\">" +
                suffixed(site.element) + "</g></g>"};
    }

    // Illustrator's namespace entities (svg.md §10, Q-SVG-2): plain text only, at most 64 and 4 KiB.
    bool accept_entity(std::string_view, std::string_view value, std::size_t count,
                       std::size_t bytes) const override {
        return count <= 64 && bytes <= 4096 && value.find_first_of("&<%") == std::string_view::npos;
    }

    xml::AttrKind attr_kind(const Node&, const Attr& a) const override {
        if (a.name.uri == xml::kXmlNs)
            return a.name.local == "id" ? xml::AttrKind::id : xml::AttrKind::opaque;
        if (!a.name.uri.empty())
            return xml::AttrKind::opaque;
        return a.name.local == "id"      ? xml::AttrKind::id
               : a.name.local == "class" ? xml::AttrKind::token_list
               : a.name.local == "style" ? xml::AttrKind::style_list
                                         : xml::AttrKind::opaque;
    }

    xml::TextMode text_mode(const Node& el) const override {
        return svg_el(el, {"text", "tspan", "textPath", "title", "desc"}) ? xml::TextMode::words
                                                                          : xml::TextMode::opaque;
    }

    std::optional<std::string> normalize(const Node& el, const Attr& a) const override {
        if (!cfg_.show_editor_state && editor_state(el, a))
            return std::nullopt;
        if (!a.name.uri.empty() || attr_kind(el, a) != xml::AttrKind::opaque)
            return a.name.local == "style" && a.name.uri.empty() ? style_key(a.value)
                                                                 : Dialect::normalize(el, a);
        const std::string& n = a.name.local;
        if (n == "d")
            return path_key(a.value).value_or(a.value);
        if (n == "transform" || n == "gradientTransform" || n == "patternTransform")
            return transform_key(a.value).value_or(a.value);
        if (n == "points" || n == "viewBox")
            if (auto v = numbers(a.value)) {
                std::string s;
                for (double x : *v)
                    s += (s.empty() ? "" : " ") + fmt(x);
                return s;
            }
        return value_key(n, a.value);
    }

    std::optional<std::string> merge_attr(const Node& el, const Attr* base, const Attr& ours,
                                          const Attr& theirs) const override {
        if (cfg_.path_merge && ours.name.uri.empty() && ours.name.local == "d")
            return merge_path(base ? std::string_view(base->value) : std::string_view(), ours.value,
                              theirs.value);
        return Dialect::merge_attr(el, base, ours, theirs);
    }

    // Illustrator's private copy of the document, and paths an Inkscape path effect generates (svg.md §10).
    bool is_derived(const Node& el) const override {
        if (el.name.uri == kAiNs && el.name.local == "pgf")
            return true;
        return std::any_of(el.attrs.begin(), el.attrs.end(), [](const Attr& a) {
            return a.name.uri == kInkNs && a.name.local == "path-effect";
        });
    }

    // `href="#id"`, `url(#id)` in any value or style, `inkscape:path-effect="#a;#b"`, ARIA id lists (svg.md
    // §2). The namedview's current layer is editor state, not a reference.
    std::string rewrite_ids(const Node&, const Attr& a,
                            const std::function<std::string(std::string_view)>& id) const override {
        const std::string& v = a.value;
        const auto& n = a.name;
        if (n.local == "href" && (n.uri.empty() || n.uri == kXlinkNs))
            return v.starts_with('#') ? "#" + id(std::string_view(v).substr(1)) : v;
        if (n.uri == kInkNs && n.local == "path-effect") {
            std::string s;
            for (std::size_t at = 0; at <= v.size();) {
                auto end = std::min(v.find(';', at), v.size());
                std::string part = trim(std::string_view(v).substr(at, end - at));
                s += (at ? ";" : "") +
                     (part.starts_with('#') ? "#" + id(std::string_view(part).substr(1)) : part);
                at = end + 1;
            }
            return s;
        }
        if (n.uri.empty() && (n.local == "aria-labelledby" || n.local == "aria-describedby")) {
            std::string s;
            std::istringstream is(v);
            for (std::string w; is >> w;)
                s += (s.empty() ? "" : " ") + id(w);
            return s;
        }
        std::string s;
        std::size_t at = 0;
        for (std::size_t p; (p = v.find("url(", at)) != std::string::npos;) {
            auto q = v.find_first_not_of(" \t\"'", p + 4);
            if (q == std::string::npos || v[q] != '#') {
                s += v.substr(at, p + 4 - at);
                at = p + 4;
                continue;
            }
            auto e = std::min(v.find_first_of(") \t\"'", q + 1), v.size());
            s += v.substr(at, q + 1 - at) + id(std::string_view(v).substr(q + 1, e - q - 1));
            at = e;
        }
        return s + v.substr(at);
    }

    // SPEC §2.3: a counter-allocated id (`rect1234`) continues after the highest number in use with its
    // prefix; any other id becomes `<id>-t<n>` (Q-SVG-3).
    std::string renumber(std::string_view id, const std::set<std::string>& taken) const override {
        if (!cfg_.rename_ids)
            return {};
        constexpr std::string_view kDigits = "0123456789";
        const auto cut = id.find_last_not_of(kDigits) + 1; // npos + 1 == 0: all digits
        if (cut < id.size() && id.size() - cut <= 15) {
            const std::string_view prefix = id.substr(0, cut);
            std::uint64_t top = 0;
            for (auto& t : taken)
                if (t.size() > prefix.size() && t.size() - prefix.size() <= 15 && t.starts_with(prefix) &&
                    t.find_first_not_of(kDigits, prefix.size()) == std::string::npos)
                    top = std::max<std::uint64_t>(top, std::stoull(t.substr(prefix.size())));
            return std::string(prefix) + std::to_string(top + 1);
        }
        for (int k = 1;; ++k)
            if (auto c = std::string(id) + "-t" + std::to_string(k); !taken.count(c))
                return c;
    }

private:
    // Editor view state (svg.md §4): hidden from diffs and dumps, kept from ours on two-sided changes.
    static bool editor_state(const Node& el, const Attr& a) {
        const std::string& l = a.name.local;
        if (a.name.uri == kInkNs)
            return l == "zoom" || l == "cx" || l == "cy" || l.starts_with("window-") ||
                   l == "current-layer" || l == "pageopacity" || l == "pageshadow" || l == "version" ||
                   l.starts_with("export-") || l == "pagecheckerboard" || l == "deskcolor" ||
                   l == "deskopacity" || l == "showpageshadow";
        if (a.name.uri == kSodiNs)
            return l == "docname";
        if (!a.name.uri.empty())
            return false;
        if (l == "showgrid")
            return el.name.uri == kSodiNs && el.name.local == "namedview";
        if (l == "data-name") // Illustrator's copy of the id
            return std::any_of(el.attrs.begin(), el.attrs.end(), [&](const Attr& x) {
                return x.name.uri.empty() && x.name.local == "id" && x.value == a.value;
            });
        return false;
    }

    static std::string style_key(std::string_view style) {
        std::vector<std::pair<std::string, std::string>> props;
        for (std::size_t at = 0; at <= style.size();) {
            auto end = std::min(style.find(';', at), style.size());
            auto decl = style.substr(at, end - at);
            if (auto colon = decl.find(':'); colon != std::string_view::npos) {
                std::string k = trim(decl.substr(0, colon)), v = value_key(k, trim(decl.substr(colon + 1)));
                std::erase_if(props, [&](auto& p) { return p.first == k; });
                if (!k.empty())
                    props.emplace_back(std::move(k), std::move(v));
            }
            at = end + 1;
        }
        std::sort(props.begin(), props.end());
        std::string s;
        for (auto& [k, v] : props)
            s += k + ":" + v + ";";
        return s;
    }

    Cfg cfg_;
};

// ---- SVGZ: the gzip layer (svg.md §7.1) ----

struct Member {
    std::uint64_t off = 0, header = 0,
                  data = 0; // offset, header length, deflate length; 8 trailer bytes follow
};

struct Gzip {
    ByteSource file; // the .svgz bytes
    std::vector<Member> members;
    bool intact = true; // every CRC-32, ISIZE, and header CRC matched
    EnvelopeMap map;
};

Error corrupt(const std::string& what) {
    return Error(ErrorCategory::invalid_input, "svgz: " + what);
}

// RFC 1952 members, inflated as one stream up to max_out bytes. `partial` (probing a head): returns what
// decoded before the input ended or broke.
std::vector<std::byte> gunzip(std::span<const std::byte> in, std::uint64_t max_out, bool partial, Gzip* g) {
    std::vector<std::byte> out;
    try {
        auto u8 = [&](std::size_t i) {
            if (i >= in.size())
                throw corrupt("truncated gzip member");
            return std::to_integer<unsigned>(in[i]);
        };
        std::size_t at = 0;
        do {
            const std::size_t start = at;
            if (u8(at) != 0x1F || u8(at + 1) != 0x8B || u8(at + 2) != 8)
                throw corrupt("not a deflate gzip member at offset " + std::to_string(at));
            const unsigned flags = u8(at + 3);
            if (flags & 0xE0)
                throw corrupt("reserved gzip flags set");
            std::size_t p = at + 10;
            if (flags & 4)
                p += 2 + (u8(p) | u8(p + 1) << 8);
            for (unsigned f : {8u, 16u}) // FNAME, FCOMMENT
                if (flags & f)
                    while (u8(p++) != 0) {
                    }
            if (flags & 2) {
                const unsigned want = u8(p) | u8(p + 1) << 8;
                if (g && (crc32(in.subspan(start, p - start)) & 0xFFFF) != want)
                    g->intact = false;
                p += 2;
            }
            u8(p); // deflate data follows
            z_stream z{};
            if (inflateInit2(&z, -15) != Z_OK)
                throw Error(ErrorCategory::internal, "zlib: init failed");
            std::unique_ptr<z_stream, int (*)(z_streamp)> guard(&z, inflateEnd);
            z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data() + p));
            z.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size() - p, std::size_t{1} << 31));
            const std::size_t first = out.size();
            std::array<Bytef, 65536> buf;
            for (int rc = Z_OK; rc != Z_STREAM_END;) {
                z.next_out = buf.data();
                z.avail_out = static_cast<uInt>(buf.size());
                rc = ::inflate(&z, Z_NO_FLUSH);
                auto* b = reinterpret_cast<const std::byte*>(buf.data());
                out.insert(out.end(), b, b + (buf.size() - z.avail_out));
                if (out.size() > max_out)
                    throw Error(ErrorCategory::limit, "svgz: the decompressed document exceeds --max-memory");
                if (rc == Z_BUF_ERROR && z.avail_in == 0)
                    throw corrupt("truncated deflate data");
                if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
                    throw corrupt("bad deflate data");
            }
            const std::size_t data = z.total_in;
            p += data;
            const std::uint32_t crc =
                u8(p) | u8(p + 1) << 8 | u8(p + 2) << 16 | static_cast<std::uint32_t>(u8(p + 3)) << 24;
            const std::uint32_t isize =
                u8(p + 4) | u8(p + 5) << 8 | u8(p + 6) << 16 | static_cast<std::uint32_t>(u8(p + 7)) << 24;
            const auto plain = std::span(out).subspan(first);
            if (g) {
                if (crc32(plain) != crc || static_cast<std::uint32_t>(plain.size()) != isize)
                    g->intact = false;
                g->members.push_back({start, p - data - start, data});
            }
            at = p + 8;
        } while (at < in.size());
    } catch (const Error&) {
        if (!partial)
            throw;
    }
    return out;
}

EnvelopeMap map_of(const std::vector<Member>& members) {
    EnvelopeMap m;
    for (auto& x : members) {
        m.ranges.push_back({x.off, 4, ByteClass::encoding, "/gzip", "gzip"});            // ID, CM, FLG
        m.ranges.push_back({x.off + 4, 4, ByteClass::volatile_, "/gzip/mtime", "gzip"}); // MTIME
        m.ranges.push_back({x.off + 8, x.header - 8, ByteClass::encoding, "/gzip", "gzip"});
        m.ranges.push_back({x.off + x.header, x.data, ByteClass::encoding, "/", "deflate"});
        m.ranges.push_back({x.off + x.header + x.data, 8, ByteClass::integrity, "/gzip", "gzip"});
    }
    return m;
}

// The merged XML in ours' gzip header, deflated at the level ours' XFL names (svg.md §7.1).
Gzip recompress(const Gzip& ours, std::span<const std::byte> plain) {
    const Member& h = ours.members.front();
    const unsigned xfl = std::to_integer<unsigned>(ours.file.bytes()[h.off + 8]);
    const int level = xfl == 2 ? 9 : xfl == 4 ? 1 : Z_DEFAULT_COMPRESSION;
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw Error(ErrorCategory::internal, "zlib: init failed");
    std::unique_ptr<z_stream, int (*)(z_streamp)> guard(&z, deflateEnd);
    std::vector<std::byte> packed(deflateBound(&z, static_cast<uLong>(plain.size())));
    z.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(plain.data()));
    z.avail_in = static_cast<uInt>(plain.size());
    z.next_out = reinterpret_cast<Bytef*>(packed.data());
    z.avail_out = static_cast<uInt>(packed.size());
    if (::deflate(&z, Z_FINISH) != Z_STREAM_END)
        throw Error(ErrorCategory::internal, "zlib: deflate did not finish");
    packed.resize(z.total_out);
    using namespace emit;
    auto bytes_out = emit::emit(group(1, {raw(ours.file, h.off, h.header), emit::bytes(packed),
                                          field(4, Endian::little, constant(semsplinter::crc32(plain))),
                                          field(4, Endian::little, constant(plain.size() & 0xFFFFFFFFu))}));
    Gzip g;
    g.members = {{0, h.header, packed.size()}};
    g.map = map_of(g.members);
    g.file = ByteSource::from_memory(std::move(bytes_out), ours.file.name());
    return g;
}

bool gzip_magic(std::span<const std::byte> b) {
    return b.size() >= 2 && b[0] == std::byte{0x1F} && b[1] == std::byte{0x8B};
}

// ---- The format ----

struct SvgDoc final : Document {
    std::unique_ptr<Document> xml;
    std::optional<Gzip> gz;
};

const SvgDoc& svg(const Document& d) {
    auto* p = dynamic_cast<const SvgDoc*>(&d);
    if (!p)
        throw Error(ErrorCategory::internal, "svg: not an SVG document");
    return *p;
}

std::vector<std::byte> bytes_of(const Format& f, const Document& d, Context& ctx) {
    VectorSink s;
    f.save(d, s, {ctx});
    return std::move(s.data);
}

class SvgFormat final : public Format {
public:
    SvgFormat() : plain_(inner_for(Cfg{})) {}

    const FormatInfo& info() const override { return info_; }

    Probe probe(std::span<const std::byte> head, std::string_view name) const override {
        if (!gzip_magic(head))
            return plain_->probe(head, name);
        auto x = gunzip(head, 64 * 1024, true, nullptr);
        return plain_->probe(x, name);
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        auto d = std::make_unique<SvgDoc>();
        auto f = inner(o.ctx.format_options);
        if (!gzip_magic(src.bytes())) {
            d->xml = f->load(std::move(src), o);
            return d;
        }
        Gzip g;
        Limits& lim = o.ctx.budget;
        auto x = gunzip(src.bytes(), lim.max_memory - std::min(lim.memory_used, lim.max_memory), false, &g);
        lim.charge(x.size());
        g.file = src;
        g.map = map_of(g.members);
        d->xml = f->load(ByteSource::from_memory(std::move(x), src.name()), o);
        d->gz = std::move(g);
        return d;
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions& o) const override {
        return inner(o.ctx.format_options)->diff(*svg(a).xml, *svg(b).xml, o);
    }

    std::unique_ptr<Comparison> compare(const Document& a, const Document& b,
                                        const DiffOptions& o) const override {
        return inner(o.ctx.format_options)->compare(*svg(a).xml, *svg(b).xml, o);
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override {
        const SvgDoc& o = svg(ours);
        auto f = inner(mo.ctx.format_options);
        MergeResult r = f->merge(*svg(base).xml, *o.xml, *svg(theirs).xml, mo);
        for (auto [d, name] :
             {std::pair{&svg(base), "base"}, std::pair{&o, "ours"}, std::pair{&svg(theirs), "theirs"}})
            if (d->gz && !d->gz->intact)
                r.warnings.push_back(
                    {"integrity-mismatch-input", "/gzip",
                     std::string(name) + ": a gzip CRC-32, ISIZE, or header CRC does not match"});
        // Illustrator reopens from its private copy, which no longer matches (svg.md §10).
        auto out_nodes = xml::nodes(*r.document);
        if (!r.changes_ours.empty() && !r.changes_theirs.empty())
            for (auto& n : out_nodes)
                if (n.kind == Node::Kind::element && n.name.uri == kAiNs && n.name.local == "pgf") {
                    r.warnings.push_back(
                        {"derived-stale", "",
                         "Illustrator private data present; Illustrator may ignore merged SVG content"});
                    break;
                }
        auto out = std::make_unique<SvgDoc>();
        out->xml = std::move(r.document);
        if (o.gz) {
            auto merged = bytes_of(*f, *out->xml, mo.ctx);
            if (merged == bytes_of(*f, *o.xml, mo.ctx)) {
                out->gz = o.gz; // unchanged: ours' bytes verbatim
            } else {
                out->gz = recompress(*o.gz, merged);
                r.warnings.push_back(
                    {"encoding-reencoded", "/gzip", "svgz: the merged document was recompressed"});
                if (o.gz->members.size() > 1)
                    r.warnings.push_back(
                        {"gzip-members-joined", "/gzip", "svgz: ours' gzip members became one"});
            }
        }
        r.document = std::move(out);
        return r;
    }

    void save(const Document& d, ByteSink& sink, const SaveOptions& o) const override {
        const SvgDoc& s = svg(d);
        if (s.gz)
            sink.write(s.gz->file.bytes());
        else
            plain_->save(*s.xml, sink, o);
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions& o) const override {
        inner(o.ctx.format_options)->dump(*svg(d).xml, os, o);
    }

    const EnvelopeMap* envelope(const Document& d) const override {
        const SvgDoc& s = svg(d);
        return s.gz ? &s.gz->map : nullptr;
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        const SvgDoc& s = svg(d);
        auto j = plain_->inspect(*s.xml);
        if (s.gz) {
            j["container"] = "gzip";
            j["gzip_members"] = s.gz->members.size();
            j["integrity"] = s.gz->intact
                                 ? nlohmann::ordered_json("ok")
                                 : nlohmann::ordered_json{{"status", "mismatch"},
                                                          {"at", nlohmann::ordered_json::array({"/gzip"})}};
            if (!s.gz->intact)
                j["warnings"].push_back({{"code", "integrity-mismatch-input"},
                                         {"at", "/gzip"},
                                         {"message", "a gzip CRC-32, ISIZE, or header CRC does not match"}});
        }
        return j;
    }

    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        return plain_->stats(*svg(a).xml, *svg(b).xml, cs);
    }

private:
    std::unique_ptr<Format> inner_for(Cfg c) const {
        return xml::make_format(info_, std::make_shared<SvgDialect>(c));
    }
    // The xml format with this invocation's dialect settings.
    std::shared_ptr<const Format> inner(const OptionValues& o) const {
        auto c = config(o);
        if (c == Cfg{})
            return {plain_.get(), [](const Format*) {}};
        return inner_for(c);
    }

    FormatInfo info_{
        "svg",
        "SVG and SVGZ (XML dialect: ids and references, styles, path data, editor state)",
        {".svg", ".svgz"},
        {true, true, false, true, false},
        {{"svg-show-editor-state", OptionSpec::Arg::none,
          "SVG: show editor view state (Inkscape zoom, window, ...) in diffs and dumps"},
         {"svg-id-collision", OptionSpec::Arg::required,
          "SVG: ids both sides added for different elements: rename (default) or conflict", true},
         {"svg-path-merge", OptionSpec::Arg::required,
          "SVG: path data both sides changed: subpaths (default) or none", true}}};
    std::unique_ptr<Format> plain_;
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<SvgFormat>();
}

} // namespace semsplinter::svg
