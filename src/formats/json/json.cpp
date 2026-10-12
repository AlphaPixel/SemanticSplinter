// JSON module (docs/formats/json.md, MVP): SpanJson parser, tree diff, KeyedMerge/SequenceMerge, span splice.
#include <semsplinter/error.hpp>
#include <semsplinter/formats/json.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/sequence.hpp>

#include "../common/splice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace semsplinter::json {

namespace {

constexpr std::size_t npos = std::string_view::npos;
constexpr int kMaxDepth = 512;
constexpr std::size_t kProbeHead = 64 * 1024;

// ---- SpanJson model and parser (json.md §9) ----

enum class Kind : std::uint8_t { null, boolean, number, string, array, object };

bool composite(Kind k) {
    return k == Kind::array || k == Kind::object;
}

struct Node {
    Kind kind = Kind::null;
    std::size_t begin = 0, end = 0;         // the value's bytes
    std::size_t kbegin = npos, kend = npos; // object member: its name's string token
    std::size_t comma = npos;               // the separator after this item
    std::string key;                        // object member: decoded name
    std::string text;                       // scalar: canonical JSON (json.md §4)
    std::vector<const Node*> kids;          // source order, duplicate names included
    const Node* parent = nullptr;
    bool dups = false; // object with duplicate member names
    Hash128 hash;      // type tag plus canonical value; objects are order-independent

    std::size_t start() const { return kbegin != npos ? kbegin : begin; } // the item's first byte
};

struct Doc final : Document {
    ByteSource src;
    std::string_view s;
    std::deque<Node> nodes; // front() is the root; stable addresses
    bool jsonc = false;     // comments or trailing commas seen
    std::string eol = "\n";
    std::string unit; // indent unit of the first indented line; empty for a single-line file
    std::vector<Warning> warnings;

    const Node& root() const { return nodes.front(); }
    std::string_view slice(std::size_t b, std::size_t e) const { return s.substr(b, e - b); }
};

// Logical members: the last occurrence of each name counts (json.md §4). Arrays: the elements.
std::vector<const Node*> members(const Node& n) {
    if (!n.dups)
        return n.kids;
    std::map<std::string_view, std::size_t> last;
    for (std::size_t i = 0; i < n.kids.size(); ++i)
        last[n.kids[i]->key] = i;
    std::vector<const Node*> out;
    for (std::size_t i = 0; i < n.kids.size(); ++i)
        if (last[n.kids[i]->key] == i)
            out.push_back(n.kids[i]);
    return out;
}

const Node* member(const Node& obj, std::string_view name) {
    const Node* r = nullptr;
    for (auto* k : obj.kids)
        if (k->key == name)
            r = k;
    return r;
}

std::string canon_string(const std::string& decoded) {
    return nlohmann::json(decoded).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string decode_canon(const std::string& canon) {
    return nlohmann::json::parse(canon).get<std::string>();
}

void put_utf8(std::string& o, std::uint32_t cp) {
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

class Parser {
public:
    Parser(Doc& d, Limits* budget) : d_(d), s_(d.s), budget_(budget) {}

    void run() {
        if (s_.starts_with("\xEF\xBB\xBF"))
            p_ = 3;
        ws();
        if (p_ >= s_.size())
            fail("empty document");
        value(0, nullptr);
        ws();
        if (p_ < s_.size())
            fail("unexpected data after the value");
        for (const Node* x : dups_) {
            std::string at;
            for (const Node* k = x; k->parent; k = k->parent) {
                const auto& sib = k->parent->kids;
                at.insert(0,
                          "/" + (k->parent->kind == Kind::object
                                     ? pointer_escape(k->key)
                                     : std::to_string(std::find(sib.begin(), sib.end(), k) - sib.begin())));
            }
            d_.warnings.push_back(
                {"invalid-duplicate-key", at, "duplicate member name; the last one counts"});
        }
    }

    bool eof = false; // the error was the end of input (a truncated probe head)

private:
    Doc& d_;
    std::string_view s_;
    Limits* budget_;
    std::size_t p_ = 0;
    std::string buf_;               // hash input
    std::vector<const Node*> dups_; // later occurrences of duplicate member names

    [[noreturn]] void fail(const char* what) {
        eof = p_ >= s_.size();
        throw Error(ErrorCategory::invalid_input,
                    std::string("json: ") + what + " at offset " + std::to_string(p_));
    }
    char peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }

    void ws() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
                d_.jsonc = true;
                p_ = std::min(s_.find('\n', p_), s_.size());
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
                d_.jsonc = true;
                auto e = s_.find("*/", p_ + 2);
                if (e == npos) {
                    p_ = s_.size();
                    fail("unterminated comment");
                }
                p_ = e + 2;
            } else {
                break;
            }
        }
    }

    Node& make(Kind k, const Node* parent) {
        if (budget_)
            budget_->charge(sizeof(Node));
        Node& n = d_.nodes.emplace_back();
        n.kind = k, n.begin = p_, n.parent = parent;
        return n;
    }

    void finish(Node& n) {
        n.end = p_;
        buf_.assign(1, static_cast<char>(n.kind)); // type tag, then the canonical value

        if (n.kind == Kind::array) {
            for (auto* k : n.kids)
                put(k->hash.hi), put(k->hash.lo);
        } else if (n.kind == Kind::object) {
            std::vector<std::pair<std::string_view, Hash128>> ms;
            for (auto* m : members(n))
                ms.emplace_back(m->key, m->hash);
            std::sort(ms.begin(), ms.end());
            for (auto& [k, v] : ms)
                put(k.size()), buf_ += k, put(v.hi), put(v.lo);
        } else {
            buf_ += n.text;
        }
        n.hash = hash128(buf_);
    }

    void put(std::uint64_t v) {
        for (int i = 0; i < 64; i += 8)
            buf_ += static_cast<char>((v >> i) & 0xFF);
    }

    // Duplicate member names: the object's flag now, the warnings once every address is known.
    void find_dups(Node& n) {
        std::vector<std::pair<std::string_view, std::size_t>> keys;
        for (std::size_t i = 0; i < n.kids.size(); ++i)
            keys.emplace_back(n.kids[i]->key, i);
        std::sort(keys.begin(), keys.end());
        for (std::size_t i = 1; i < keys.size(); ++i)
            if (keys[i].first == keys[i - 1].first)
                n.dups = true, dups_.push_back(n.kids[keys[i].second]);
    }

    Node& value(int depth, const Node* parent) {
        if (depth > kMaxDepth)
            fail("nesting deeper than 512");
        const char c = peek();
        if (c == '{' || c == '[')
            return container(depth, parent, c == '{');
        if (c == '"') {
            Node& n = make(Kind::string, parent);
            n.text = canon_string(string_lit());
            finish(n);
            return n;
        }
        if (c == '-' || digit(c)) {
            Node& n = make(Kind::number, parent);
            n.text = number();
            finish(n);
            return n;
        }
        for (auto [lit, k] : {std::pair{std::string_view("true"), Kind::boolean},
                              std::pair{std::string_view("false"), Kind::boolean},
                              std::pair{std::string_view("null"), Kind::null}}) {
            auto rest = s_.substr(p_);
            if (rest.starts_with(lit)) {
                Node& n = make(k, parent);
                p_ += lit.size();
                n.text = lit;
                finish(n);
                return n;
            }
            if (!rest.empty() && lit.starts_with(rest)) { // cut off by the end of input
                p_ = s_.size();
                fail("truncated literal");
            }
        }
        fail("unexpected character");
    }

    Node& container(int depth, const Node* parent, bool obj) {
        Node& n = make(obj ? Kind::object : Kind::array, parent);
        const char close = obj ? '}' : ']';
        ++p_;
        ws();
        while (peek() != close) {
            std::size_t kb = npos, ke = npos;
            std::string key;
            if (obj) {
                if (peek() != '"')
                    fail("expected a member name");
                kb = p_;
                key = string_lit();
                ke = p_;
                ws();
                if (peek() != ':')
                    fail("expected ':'");
                ++p_;
                ws();
            }
            Node& v = value(depth + 1, &n);
            v.kbegin = kb, v.kend = ke, v.key = std::move(key);
            n.kids.push_back(&v);
            ws();
            if (peek() == ',') {
                v.comma = p_++;
                ws();
                if (peek() == close)
                    d_.jsonc = true; // trailing comma
            } else if (peek() != close) {
                fail(obj ? "expected ',' or '}'" : "expected ',' or ']'");
            }
        }
        ++p_;
        if (obj)
            find_dups(n);
        finish(n);
        return n;
    }

    std::uint32_t hex4() {
        if (p_ + 4 > s_.size()) {
            p_ = s_.size();
            fail("truncated \\u escape");
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[p_++];
            v <<= 4;
            if (digit(c))
                v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else
                fail("bad \\u escape");
        }
        return v;
    }

    std::string string_lit() {
        ++p_;
        std::string out;
        for (;;) {
            if (p_ >= s_.size())
                fail("unterminated string");
            const auto c = static_cast<unsigned char>(s_[p_]);
            if (c == '"') {
                ++p_;
                return out;
            }
            if (c < 0x20)
                fail("control character in string");
            ++p_;
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            const char e = peek();
            ++p_;
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(p_).starts_with("\\u")) {
                    p_ += 2;
                    const std::uint32_t lo = hex4();
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        p_ -= 6, cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD; // lone surrogate
                }
                put_utf8(out, cp);
                break;
            }
            default:
                fail("bad escape");
            }
        }
    }

    // RFC 8259 number; returns the canonical text: fraction trailing zeros and exponent '+' and leading
    // zeros dropped, exponent lowercased (json.md §4).
    std::string number() {
        const std::size_t b = p_;
        if (peek() == '-')
            ++p_;
        if (peek() == '0')
            ++p_;
        else if (digit(peek()))
            while (digit(peek()))
                ++p_;
        else
            fail("bad number");
        std::string c(s_.substr(b, p_ - b));
        if (peek() == '.') {
            const std::size_t f = ++p_;
            if (!digit(peek()))
                fail("bad number");
            while (digit(peek()))
                ++p_;
            auto frac = s_.substr(f, p_ - f);
            while (frac.size() > 1 && frac.back() == '0')
                frac.remove_suffix(1);
            c += ".";
            c += frac;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++p_;
            bool neg = false;
            if (peek() == '+' || peek() == '-')
                neg = s_[p_++] == '-';
            const std::size_t e = p_;
            if (!digit(peek()))
                fail("bad number");
            while (digit(peek()))
                ++p_;
            auto exp = s_.substr(e, p_ - e);
            while (exp.size() > 1 && exp.front() == '0')
                exp.remove_prefix(1);
            c += neg ? "e-" : "e";
            c += exp;
        }
        return c;
    }
};

std::unique_ptr<Doc> parse(ByteSource src, Limits* budget) {
    auto d = std::make_unique<Doc>();
    d->src = std::move(src);
    auto b = d->src.bytes();
    d->s = {reinterpret_cast<const char*>(b.data()), b.size()};
    Parser(*d, budget).run();
    if (auto nl = d->s.find('\n'); nl != npos && nl > 0 && d->s[nl - 1] == '\r')
        d->eol = "\r\n";
    for (std::size_t nl = d->s.find('\n'); nl != npos; nl = d->s.find('\n', nl + 1)) {
        auto i = nl + 1;
        while (i < d->s.size() && (d->s[i] == ' ' || d->s[i] == '\t'))
            ++i;
        if (i > nl + 1 && i < d->s.size() && d->s[i] != '\r' && d->s[i] != '\n') {
            d->unit = d->s.substr(nl + 1, i - nl - 1);
            break;
        }
    }
    return d;
}

// ---- Shared helpers ----

// Back off to a UTF-8 character boundary.
std::size_t utf8_cut(std::string_view s, std::size_t n) {
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    return n;
}

constexpr const char* kEllipsis = "\xE2\x80\xA6";

// Compact canonical JSON, cut a little past `limit`.
void render(const Node& n, std::string& o, std::size_t limit) {
    if (o.size() > limit)
        return;
    if (!composite(n.kind)) {
        o += n.text;
        return;
    }
    o += n.kind == Kind::object ? '{' : '[';
    bool first = true;
    for (auto* m : members(n)) {
        if (!first)
            o += ',';
        first = false;
        if (n.kind == Kind::object)
            o += canon_string(m->key) + ":";
        render(*m, o, limit);
        if (o.size() > limit)
            return;
    }
    o += n.kind == Kind::object ? '}' : ']';
}

Value value_of(const Node* n) {
    if (!n)
        return {};
    std::string t;
    render(*n, t, 120);
    if (t.size() > 120)
        t = t.substr(0, utf8_cut(t, 120)) + kEllipsis;
    return {std::move(t), n->hash};
}

std::optional<double> to_double(const std::string& s) {
    std::istringstream is(s);
    is.imbue(std::locale::classic());
    double v = 0;
    if (!(is >> v) || !std::isfinite(v))
        return std::nullopt;
    return v;
}

Change change(Side side, Op op, std::string path, const char* kind, const Node* before, const Node* after) {
    Change c = make_change(side, op, Address{std::move(path)}, kind);
    if (op != Op::move)
        c.before = value_of(before), c.after = value_of(after);
    if (op == Op::update && before->kind == Kind::number && after->kind == Kind::number) {
        auto x = to_double(before->text), y = to_double(after->text);
        if (x && y) {
            c.metrics["delta"] = *y - *x;
            if (*x != 0)
                c.metrics["rel"] = (*y - *x) / std::fabs(*x);
        }
    }
    return c;
}

// ---- Array identity (json.md §3) ----

constexpr std::array<std::string_view, 8> kKeyNames{"id",   "uuid", "guid", "key",
                                                    "name", "_id",  "@id",  "slug"};

// Every element is an object holding `name` as a scalar, unique within its array.
bool key_valid(std::string_view name, std::initializer_list<const Node*> arrays) {
    for (auto* a : arrays) {
        if (!a)
            continue;
        std::set<std::string_view> seen;
        for (auto* e : a->kids) {
            const Node* v = e->kind == Kind::object ? member(*e, name) : nullptr;
            if (!v || composite(v->kind) || !seen.insert(v->text).second)
                return false;
        }
    }
    return true;
}

std::optional<std::string> detect_key(std::initializer_list<const Node*> arrays) {
    bool any = false;
    for (auto* a : arrays)
        if (a)
            for (auto* e : a->kids) {
                if (e->kind != Kind::object)
                    return std::nullopt;
                any = true;
            }
    if (any)
        for (auto name : kKeyNames)
            if (key_valid(name, arrays))
                return std::string(name);
    return std::nullopt;
}

// Address segment of an identified element: "id:42", "name:Player%20One".
std::string identity(const Node& e, const std::string& key) {
    const Node* v = member(e, key);
    const std::string val = v->kind == Kind::string ? decode_canon(v->text) : v->text;
    std::string o = key + ":";
    for (char c : val) {
        if (c == '/' || c == '~' || c == ':' || c == '%' || c == ' ') {
            static const char* hex = "0123456789ABCDEF";
            o += '%';
            o += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
            o += hex[static_cast<unsigned char>(c) & 0xF];
        } else {
            o += c;
        }
    }
    return o;
}

// The identity key per array address, shared by the merge and both side diffs so addresses agree.
struct Keys {
    std::map<std::string, std::optional<std::string>, std::less<>> cache;

    std::optional<std::string> at(const std::string& path, std::initializer_list<const Node*> arrays) {
        if (auto it = cache.find(path); it != cache.end() && (!it->second || key_valid(*it->second, arrays)))
            return it->second;
        auto k = detect_key(arrays);
        cache.try_emplace(path, k);
        return k;
    }
};

std::vector<SeqItem> seq_items(const Node& arr, const std::optional<std::string>& key) {
    std::vector<SeqItem> v;
    for (auto* e : arr.kids)
        v.push_back({e->hash, key ? std::optional(hash128(identity(*e, *key))) : std::nullopt});
    return v;
}

std::string element_path(const std::string& path, const Node& arr, std::size_t i,
                         const std::optional<std::string>& key) {
    return path + "/" + (key ? identity(*arr.kids[i], *key) : std::to_string(i));
}

// ---- Tree diff (json.md §5) ----

// Unidentified elements: erase/update address the base index, insert the side's index.
struct Differ {
    Keys& keys;
    Side side;
    ChangeSet& out;

    void node(const Node& a, const Node& b, const std::string& path) {
        if (a.hash == b.hash)
            return;
        if (a.kind == Kind::object && b.kind == Kind::object) {
            auto ma = members(a), mb = members(b);
            std::map<std::string_view, const Node*> ib;
            for (auto* m : mb)
                ib[m->key] = m;
            for (auto* m : ma) {
                auto p = path + "/" + pointer_escape(m->key);
                if (auto f = ib.find(m->key); f != ib.end())
                    node(*m, *f->second, p), ib.erase(f);
                else
                    out.push_back(change(side, Op::erase, p, "member", m, nullptr));
            }
            for (auto* m : mb)
                if (ib.count(m->key))
                    out.push_back(
                        change(side, Op::insert, path + "/" + pointer_escape(m->key), "member", nullptr, m));
            return;
        }
        if (a.kind == Kind::array && b.kind == Kind::array)
            return array(a, b, path);
        out.push_back(change(side, Op::update, path, a.kind == b.kind ? "value" : "type", &a, &b));
    }

    void array(const Node& a, const Node& b, const std::string& path) {
        const auto key = keys.at(path, {&a, &b});
        const auto ia = seq_items(a, key), ib = seq_items(b, key);
        auto chunks = sequence_merge(ia, ia, ib);
        for (auto& c : chunks) {
            if (c.kind == SeqKind::stable && !c.moved)
                continue;
            if (c.moved) { // only this side moves anything: base and ours are the same sequence
                auto& m = out.emplace_back(change(side, Op::move, element_path(path, a, c.base[0], key),
                                                  "element", nullptr, nullptr));
                m.metrics["from"] = c.base[0];
                m.metrics["to"] = c.theirs[0];
                node(*a.kids[c.base[0]], *b.kids[c.theirs[0]], element_path(path, a, c.base[0], key));
                continue;
            }
            std::size_t pairs = std::min(c.base.size(), c.theirs.size()); // positional (no key)
            if (key)
                pairs = c.base.size() == 1 && c.theirs.size() == 1 && ia[c.base[0]].id == ib[c.theirs[0]].id
                            ? 1
                            : 0;
            for (std::size_t i = 0; i < pairs; ++i)
                node(*a.kids[c.base[i]], *b.kids[c.theirs[i]], element_path(path, a, c.base[i], key));
            for (std::size_t i = pairs; i < c.base.size(); ++i)
                out.push_back(change(side, Op::erase, element_path(path, a, c.base[i], key), "element",
                                     a.kids[c.base[i]], nullptr));
            for (std::size_t i = pairs; i < c.theirs.size(); ++i)
                out.push_back(change(side, Op::insert, element_path(path, b, c.theirs[i], key), "element",
                                     nullptr, b.kids[c.theirs[i]]));
        }
    }
};

ChangeSet diff_roots(Keys& keys, const Doc& a, const Doc& b, Side side) {
    ChangeSet cs;
    Differ{keys, side, cs}.node(a.root(), b.root(), "");
    return cs;
}

// ---- Merge plan (json.md §6) ----

struct Plan;

// One output value. `frame` is ours' node at the same identity (its key, spacing, and comments are kept);
// `plan` merges below a frame whose subtree both sides, or only theirs, changed.
struct Item {
    Side side = Side::ours;
    const Node* node = nullptr;
    const Node* frame = nullptr;
    std::shared_ptr<Plan> plan;
};

// One object row or array chunk; a conflict's items depend on its resolution.
struct Slot {
    std::vector<Item> items;
    std::optional<std::size_t> conflict;
    std::map<std::string, std::vector<Item>, std::less<>> alt;
};

struct Plan {
    std::vector<Slot> slots;
};

// Which reported changes a conflict covers.
struct Scope {
    std::string path;
    bool chunk = false; // an array chunk: elements below `path` by identity or index
    std::set<std::string> ids;
    std::set<std::size_t> base, ours, theirs;

    bool covers(const Change& c) const {
        if (!chunk)
            return path_within(c.at.path, path);
        if (!c.at.path.starts_with(path + "/"))
            return false;
        auto rest = std::string_view(c.at.path).substr(path.size() + 1);
        const auto slash = rest.find('/');
        const std::string seg(rest.substr(0, slash));
        if (ids.count(seg))
            return true;
        if (seg.empty() || !std::all_of(seg.begin(), seg.end(), digit))
            return false;
        const std::size_t i = std::stoull(seg);
        if (c.op == Op::insert && slash == npos)
            return (c.side == Side::ours ? ours : theirs).count(i) > 0;
        return base.count(i) > 0;
    }
};

struct Pending {
    Address at;
    std::string reason;
    Scope scope;
    bool sequence = false; // offers "union"
};

bool same_hash(const Node* x, const Node* y) {
    return x->hash == y->hash;
}

struct Merger {
    std::array<const Doc*, 3> docs; // by Side
    Keys& keys;
    std::vector<Pending> pending;

    std::size_t conflict(Address at, std::string reason, Scope scope, bool sequence) {
        pending.push_back({std::move(at), std::move(reason), std::move(scope), sequence});
        return pending.size() - 1;
    }

    // Side `s`'s value at an identity whose ours node is `frame`. When ours left it unchanged and theirs
    // changed a container, descend so that ours' bytes frame theirs' edits.
    Item take(Side s, const Node* b, const Node* frame, const Node* n, const std::string& path) {
        if (frame && n->hash == frame->hash)
            return {Side::ours, frame, frame, nullptr};
        Item it{s, n, frame, nullptr};
        if (s == Side::theirs && frame && b && b->hash == frame->hash && b->kind == frame->kind &&
            frame->kind == n->kind && composite(n->kind) && !frame->kids.empty())
            it.plan = merge(b, *frame, *n, path);
        return it;
    }

    // Both sides changed a value: merge below it, or null when it is not a pair of like containers.
    std::shared_ptr<Plan> merge(const Node* b, const Node& o, const Node& t, const std::string& path) {
        if (o.kind != t.kind || !composite(o.kind) || (b && b->kind != o.kind))
            return nullptr;
        return o.kind == Kind::object ? object(b, o, t, path) : array(b, o, t, path);
    }

    Slot root() {
        const Node &b = docs[0]->root(), &o = docs[1]->root(), &t = docs[2]->root();
        std::shared_ptr<Plan> sub;
        auto r = merge_leaf(
            &b, &o, &t, [](const Node& x, const Node& y) { return x.hash == y.hash; },
            [&](const Node* vb, const Node& vo, const Node& vt) {
                return (sub = merge(vb, vo, vt, "")) != nullptr;
            });
        Slot slot;
        if (r.rule == MergeRule::conflict) {
            slot.conflict = conflict({""}, r.reason, {""}, false);
            slot.alt["ours"] = {take(Side::ours, &b, &o, &o, "")};
            slot.alt["theirs"] = {take(Side::theirs, &b, &o, &t, "")};
            slot.alt["base"] = {take(Side::base, &b, &o, &b, "")};
        } else if (r.rule == MergeRule::recursed) {
            slot.items = {{Side::ours, &o, &o, sub}};
        } else {
            const Side s = r.take.value_or(Side::ours);
            slot.items = {take(s, &b, &o, s == Side::theirs ? &t : &o, "")};
        }
        return slot;
    }

    std::shared_ptr<Plan> object(const Node* b, const Node& o, const Node& t, const std::string& path) {
        using KV = std::pair<std::string, const Node*>;
        auto list = [](const Node* n) {
            std::vector<KV> v;
            if (n)
                for (auto* m : members(*n))
                    v.emplace_back(m->key, m);
            return v;
        };
        const auto lb = list(b), lo = list(&o), lt = list(&t);
        std::map<std::string, std::shared_ptr<Plan>> sub;
        auto rows = keyed_merge<std::string, const Node*>(
            lb, lo, lt, same_hash,
            [&](const std::string& k, const Node* const* vb, const Node* const& vo, const Node* const& vt) {
                auto p = merge(vb ? *vb : nullptr, *vo, *vt, path + "/" + pointer_escape(k));
                if (p)
                    sub[k] = p;
                return p != nullptr;
            });
        auto plan = std::make_shared<Plan>();
        for (auto& r : rows) {
            const Node* nb = r.base ? lb[*r.base].second : nullptr;
            const Node* no = r.ours ? lo[*r.ours].second : nullptr;
            const Node* nt = r.theirs ? lt[*r.theirs].second : nullptr;
            const std::string p = path + "/" + pointer_escape(r.key);
            Slot slot;
            if (r.outcome.rule == MergeRule::conflict) {
                slot.conflict = conflict({p}, r.outcome.reason, {p}, false);
                for (auto [name, s, n] :
                     {std::tuple{"ours", Side::ours, no}, std::tuple{"theirs", Side::theirs, nt},
                      std::tuple{"base", Side::base, nb}})
                    slot.alt[name] = n ? std::vector<Item>{take(s, nb, no, n, p)} : std::vector<Item>{};
            } else if (r.outcome.rule == MergeRule::recursed) {
                slot.items = {{Side::ours, no, no, sub[r.key]}};
            } else if (r.outcome.take) {
                const Side s = *r.outcome.take;
                slot.items = {take(s, nb, no, s == Side::ours ? no : s == Side::theirs ? nt : nb, p)};
            }
            plan->slots.push_back(std::move(slot));
        }
        return plan;
    }

    std::shared_ptr<Plan> array(const Node* b, const Node& o, const Node& t, const std::string& path) {
        static const Node kEmpty{Kind::array};
        const Node& nb = b ? *b : kEmpty;
        const std::array<const Node*, 3> arr{&nb, &o, &t};
        const auto key = keys.at(path, {b, &o, &t});
        const auto ib = seq_items(nb, key), io = seq_items(o, key), it = seq_items(t, key);
        std::map<std::size_t, std::shared_ptr<Plan>> sub; // by ours index
        SeqMergeOptions opt;
        opt.recurse = [&](std::optional<std::size_t> bi, std::size_t oi, std::size_t ti) {
            auto p = merge(bi ? nb.kids[*bi] : nullptr, *o.kids[oi], *t.kids[ti],
                           bi ? element_path(path, nb, *bi, key) : element_path(path, o, oi, key));
            if (p)
                sub[oi] = p;
            return p != nullptr;
        };
        auto list = [](const SeqChunk& c, Side s) -> const std::vector<std::size_t>& {
            return s == Side::base ? c.base : s == Side::ours ? c.ours : c.theirs;
        };
        // The items of a chunk under `choice`. Equal-length lists pair positionally, so the side's
        // element keeps ours' frame.
        auto items = [&](const SeqChunk& c, std::string_view choice) {
            std::vector<Item> v;
            if (c.kind == SeqKind::stable) {
                for (auto i : c.ours)
                    v.push_back({Side::ours, o.kids[i], o.kids[i], nullptr});
                return v;
            }
            std::array<std::size_t, 3> pos{};
            for (auto [s, i] : picks(c, choice)) {
                const std::size_t k = pos[static_cast<std::size_t>(s)]++;
                if (s == Side::ours) {
                    Item x{Side::ours, o.kids[i], o.kids[i], nullptr};
                    if (c.kind == SeqKind::recursed)
                        x.plan = sub[i];
                    v.push_back(x);
                    continue;
                }
                const bool paired = choice != "union" && list(c, s).size() == c.ours.size();
                const Node* frame = paired ? o.kids[c.ours[k]] : nullptr;
                const Node* base = paired && c.base.size() == c.ours.size() ? nb.kids[c.base[k]] : nullptr;
                v.push_back(take(s, base, frame, arr[static_cast<std::size_t>(s)]->kids[i],
                                 element_path(path, *arr[static_cast<std::size_t>(s)], i, key)));
            }
            return v;
        };
        auto plan = std::make_shared<Plan>();
        std::size_t bpos = 0;
        for (auto& c : sequence_merge(ib, io, it, opt)) {
            if (c.kind == SeqKind::conflict && !key && c.reason == "both-modified" && c.base.size() == 1 &&
                c.ours.size() == 1 && c.theirs.size() == 1)
                if (auto p = merge(nb.kids[c.base[0]], *o.kids[c.ours[0]], *t.kids[c.theirs[0]],
                                   element_path(path, nb, c.base[0], key)))
                    sub[c.ours[0]] = p, c.kind = SeqKind::recursed;
            Slot slot;
            if (c.kind == SeqKind::conflict) {
                Scope sc;
                std::set<std::string> idents;
                if (key)
                    for (Side s : {Side::base, Side::ours, Side::theirs})
                        for (auto i : list(c, s))
                            idents.insert(identity(*arr[static_cast<std::size_t>(s)]->kids[i], *key));
                Address at;
                if (idents.size() == 1) {
                    at.path = sc.path = path + "/" + *idents.begin();
                } else {
                    at = {path, Range{bpos, c.base.size()}};
                    sc.path = path, sc.chunk = true, sc.ids = idents;
                    if (!key) {
                        sc.base = {c.base.begin(), c.base.end()};
                        sc.ours = {c.ours.begin(), c.ours.end()};
                        sc.theirs = {c.theirs.begin(), c.theirs.end()};
                    }
                }
                slot.conflict = conflict(at, c.reason, sc, true);
                for (auto choice : {"ours", "theirs", "base", "union"})
                    slot.alt[choice] = items(c, choice);
            } else {
                slot.items = items(c, "ours");
            }
            if (!c.moved)
                bpos += c.base.size();
            plan->slots.push_back(std::move(slot));
        }
        return plan;
    }
};

// ---- Span-splice writer (json.md §7) ----

std::string line_indent(std::string_view s, std::size_t pos) {
    const auto nl = pos == 0 ? npos : s.rfind('\n', pos - 1);
    const std::size_t b = nl == npos ? 0 : nl + 1;
    std::size_t e = b;
    while (e < pos && (s[e] == ' ' || s[e] == '\t'))
        ++e;
    return std::string(s.substr(b, e - b));
}

// Re-indents the lines after the first from `from` to `to`, with `eol` line ends.
std::string reindent(std::string_view text, std::string_view from, std::string_view to,
                     std::string_view eol) {
    std::string out;
    for (bool first = true;; first = false) {
        const auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        if (!first && line.starts_with(from)) {
            out += to;
            line.remove_prefix(from.size());
        }
        if (nl == npos)
            return out += line;
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        out += line;
        out += eol;
        text.remove_prefix(nl + 1);
    }
}

bool all_space(std::string_view s) {
    return s.find_first_not_of(" \t\r\n") == npos;
}

// Text before each item: after the opening bracket or the previous comma.
std::string_view pre_of(const Doc& d, const Node& c, std::size_t k) {
    const std::size_t from = k == 0 ? c.begin + 1 : c.kids[k - 1]->comma + 1;
    return d.slice(from, c.kids[k]->start());
}

bool multiline(const Doc& d, const Node& c) {
    for (std::size_t k = 0; k < c.kids.size(); ++k)
        if (pre_of(d, c, k).find('\n') != npos)
            return true;
    return false;
}

// Container layout from source tokens: keys, strings, and numbers keep their spelling.
std::string layout(const Doc& d, const Node& n, bool multi, const std::string& indent,
                   const std::string& unit, std::string_view colon, std::string_view eol) {
    if (!composite(n.kind) || n.kids.empty())
        return std::string(d.slice(n.begin, n.end));
    std::string o(1, n.kind == Kind::object ? '{' : '[');
    bool first = true;
    for (auto* m : members(n)) {
        if (!first)
            o += ',';
        if (multi)
            o += std::string(eol) + indent + unit;
        else if (!first)
            o += ' ';
        first = false;
        if (n.kind == Kind::object)
            o += std::string(d.slice(m->kbegin, m->kend)) + std::string(colon);
        o += layout(d, *m, multi, indent + unit, unit, colon, eol);
    }
    if (multi)
        o += std::string(eol) + indent;
    o += n.kind == Kind::object ? '}' : ']';
    return o;
}

struct Emitter {
    std::array<const Doc*, 3> docs;
    const std::vector<Conflict>& conflicts;
    mutable std::map<const Node*, bool> multi_cache; // side containers, asked once per inserted item

    bool side_multiline(const Doc& d, const Node& c) const {
        auto [it, fresh] = multi_cache.try_emplace(&c, false);
        if (fresh)
            it->second = multiline(d, c);
        return it->second;
    }
    const Doc& doc(Side s) const { return *docs[static_cast<std::size_t>(s)]; }
    const Doc& ours() const { return *docs[1]; }

    std::vector<Item> resolve(const Plan& p) const {
        std::vector<Item> v;
        for (auto& s : p.slots) {
            const auto& src = s.conflict ? s.alt.at(conflicts[*s.conflict].resolved_as) : s.items;
            v.insert(v.end(), src.begin(), src.end());
        }
        return v;
    }

    // A value from another side, placed in a container of ours with the given style. A composite value
    // takes the layout of `like` (a composite it replaces, or a sibling) when there is one; otherwise it
    // is laid out anew only when its source container's style differs from ours.
    std::string value_text(Side s, const Node& n, bool multi, const std::string& indent,
                           const std::string& unit, std::string_view colon, std::optional<bool> like) const {
        const Doc& sd = doc(s);
        const auto text = sd.slice(n.begin, n.end);
        const bool has_nl = text.find('\n') != npos;
        if (composite(n.kind) && !n.kids.empty()) {
            const bool want = like                                                 ? *like
                              : n.parent && side_multiline(sd, *n.parent) != multi ? multi
                                                                                   : has_nl;
            if (want != has_nl)
                return layout(sd, n, want, indent, unit, colon, ours().eol);
        }
        if (has_nl)
            return reindent(text, line_indent(sd.s, n.start()), indent, ours().eol);
        return std::string(text);
    }

    std::string body(const Item& it, bool multi, const std::string& indent, const std::string& unit,
                     std::string_view colon, std::optional<bool> like) const {
        const Doc& o = ours();
        if (const Node* f = it.frame) {
            std::string s(f->kbegin != npos ? o.slice(f->kbegin, f->begin) : std::string_view{});
            if (it.plan)
                return s + container(*f, *it.plan);
            if (it.side == Side::ours)
                return s + std::string(o.slice(f->begin, f->end));
            if (composite(f->kind) && !f->kids.empty())
                like = o.slice(f->begin, f->end).find('\n') != npos;
            return s + value_text(it.side, *it.node, multi, line_indent(o.s, f->start()), unit, colon, like);
        }
        const Node& n = *it.node;
        std::string s = n.kbegin != npos
                            ? std::string(doc(it.side).slice(n.kbegin, n.kend)) + std::string(colon)
                            : std::string();
        return s + value_text(it.side, n, multi, indent, unit, colon, like);
    }

    // Rebuilds ours' container `c`: kept items bring their leading text (indentation, comments), the
    // text up to their comma, and a same-line comment after it; commas are re-derived.
    std::string container(const Node& c, const Plan& p) const {
        const Doc& o = ours();
        auto items = resolve(p);
        const auto& kids = c.kids;
        if (c.dups) { // shadowed duplicates stay just before the member that overrides them (json.md §6)
            std::vector<Item> v;
            for (auto& it : items) {
                if (it.frame)
                    for (auto* k = kids.data(); *k != it.frame; ++k)
                        if ((*k)->key == it.frame->key)
                            v.push_back({Side::ours, *k, *k, nullptr});
                v.push_back(it);
            }
            items = std::move(v);
        }
        const std::size_t n = kids.size();
        const char open = o.s[c.begin], close = o.s[c.end - 1];
        if (items.empty())
            return n == 0 ? std::string(o.slice(c.begin, c.end)) : std::string{open, close};
        struct Piece {
            std::string_view pre, post, trail;
        };
        auto split_trail = [](std::string_view& seg) {
            const auto nl = seg.find('\n');
            if (nl == npos)
                return std::string_view{};
            auto t = seg.substr(0, nl);
            if (t.ends_with('\r'))
                t.remove_suffix(1);
            seg.remove_prefix(t.size());
            return t;
        };
        std::vector<Piece> pc(n);
        std::map<const Node*, std::size_t> index;
        for (std::size_t k = 0; k < n; ++k) {
            index[kids[k]] = k;
            pc[k].pre = pre_of(o, c, k);
            if (k > 0)
                pc[k - 1].trail = split_trail(pc[k].pre);
            if (kids[k]->comma != npos)
                pc[k].post = o.slice(kids[k]->end, kids[k]->comma);
        }
        const bool trailing = n && kids.back()->comma != npos;
        const bool multi = n ? multiline(o, c) : !o.unit.empty();
        const std::string cind = line_indent(o.s, c.begin);
        std::string unit = o.unit.empty() ? "  " : o.unit;
        std::string fin, pre_first, pre_next;
        std::string colon = ": ";
        if (n) {
            auto f = o.slice(trailing ? kids.back()->comma + 1 : kids.back()->end, c.end - 1);
            pc[n - 1].trail = split_trail(f);
            fin = f;
            if (c.kind == Kind::object)
                colon = o.slice(kids[0]->kend, kids[0]->begin);
            // The line break and indentation of a pre, without the comments before it.
            auto ws_tail = [](std::string_view pre) {
                auto nl = pre.rfind('\n');
                if (nl > 0 && pre[nl - 1] == '\r')
                    --nl;
                return std::string(pre.substr(nl));
            };
            if (multi) {
                std::size_t ref = n - 1; // the last item that starts on its own line
                while (pc[ref].pre.find('\n') == npos)
                    --ref;
                pre_next = ws_tail(pc[ref].pre);
                pre_first = pc[0].pre.find('\n') != npos ? ws_tail(pc[0].pre) : pre_next;
                const auto ind = pre_next.substr(pre_next.find('\n') + 1);
                if (ind.starts_with(cind) && ind.size() > cind.size())
                    unit = ind.substr(cind.size());
            } else {
                pre_first = all_space(pc[0].pre) ? std::string(pc[0].pre) : "";
                pre_next = n > 1 && all_space(pc[1].pre) ? std::string(pc[1].pre) : " ";
            }
        } else if (multi) {
            pre_first = pre_next = o.eol + cind + unit;
            fin = o.eol + cind;
        } else {
            pre_next = " ";
        }
        if (colon.find_first_of("/\n") != npos)
            colon = ": ";
        std::optional<bool> like; // the layout of ours' first non-empty composite member
        for (auto* k : kids)
            if (composite(k->kind) && !k->kids.empty()) {
                like = o.slice(k->begin, k->end).find('\n') != npos;
                break;
            }
        std::string out(1, open);
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Item& it = items[i];
            const Piece* own = nullptr;
            if (it.frame)
                if (auto f = index.find(it.frame); f != index.end())
                    own = &pc[f->second];
            const std::string pre = own && own->pre.find('\n') != npos ? std::string(own->pre)
                                    : i == 0                           ? pre_first
                                                                       : pre_next;
            const auto nl = pre.rfind('\n');
            out += pre;
            out += body(it, multi, nl == npos ? cind : pre.substr(nl + 1), unit, colon, like);
            if (own)
                out += own->post;
            if (i + 1 < items.size() || trailing)
                out += ',';
            if (own)
                out += own->trail;
        }
        return out + fin + close;
    }

    std::string emit(const Slot& root) const {
        Plan p;
        p.slots.push_back(root);
        const auto items = resolve(p);
        const Doc& o = ours();
        const Node& r = o.root();
        if (items.size() == 1 && items[0].side == Side::ours && items[0].node == &r && !items[0].plan)
            return std::string(o.s);
        return apply_splices(o.s,
                             {{r.begin, r.end, body(items.at(0), false, "", o.unit, ": ", std::nullopt)}});
    }
};

// ---- Dump (json.md §5) ----

void dump_node(const Node& n, std::string& path, std::ostream& os, std::vector<Address>* at) {
    if (at && (!composite(n.kind) || n.kids.empty()))
        at->push_back({path});
    if (!composite(n.kind)) {
        os << path << " = ";
        if (n.kind == Kind::string && n.text.size() > 120)
            os << n.text.substr(0, utf8_cut(n.text, 101)) << kEllipsis << "\" xxh3:" << n.hash.hex();
        else
            os << n.text;
        os << '\n';
        return;
    }
    if (n.kids.empty()) {
        os << path << (n.kind == Kind::object ? " = {}\n" : " = []\n");
        return;
    }
    std::vector<std::pair<std::string, const Node*>> segs;
    const auto key = n.kind == Kind::array ? detect_key({&n}) : std::nullopt;
    for (auto* m : members(n))
        segs.emplace_back(n.kind == Kind::object ? pointer_escape(m->key)
                          : key                  ? identity(*m, *key)
                                                 : std::to_string(segs.size()),
                          m);
    if (n.kind == Kind::object || key)
        std::sort(segs.begin(), segs.end());
    for (auto& [seg, m] : segs) {
        const auto len = path.size();
        path += "/" + seg;
        dump_node(*m, path, os, at);
        path.resize(len);
    }
}

// ---- Format ----

bool json_name(std::string_view name) {
    auto dot = name.rfind('.');
    if (dot == npos)
        return false;
    std::string ext(name.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return ext == ".json" || ext == ".jsonc" || ext == ".geojson" || ext == ".webmanifest" ||
           ext == ".code-workspace";
}

const Doc& cast(const Document& d) {
    auto* j = dynamic_cast<const Doc*>(&d);
    if (!j)
        throw Error(ErrorCategory::internal, "json: foreign document");
    return *j;
}

class JsonFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{"json",
                                  "JSON and JSONC (order- and format-preserving)",
                                  {".json", ".jsonc", ".geojson", ".webmanifest", ".code-workspace"},
                                  {true, true, false, true, false},
                                  {}};
        return i;
    }

    // `{`/`[` that tokenizes (60), or `{"key":` (80); a bare scalar only under a JSON name (json.md header).
    Probe probe(std::span<const std::byte> head, std::string_view name) const override {
        Doc d;
        d.s = {reinterpret_cast<const char*>(head.data()), head.size()};
        Parser p(d, nullptr);
        try {
            p.run();
        } catch (const Error&) {
            if (!p.eof || head.size() < kProbeHead || d.nodes.empty())
                return {0};
        }
        const Node& r = d.root();
        if (!composite(r.kind))
            return {json_name(name) ? 10 : 0};
        const Node* obj = r.kind == Kind::object ? &r : r.kids.empty() ? nullptr : r.kids[0];
        return {obj && obj->kind == Kind::object && !obj->kids.empty() ? 80 : 60};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return parse(std::move(src), &o.ctx.budget);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions&) const override {
        Keys keys;
        auto cs = diff_roots(keys, cast(a), cast(b), Side::theirs);
        sort_canonical(cs);
        return cs;
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override {
        const std::array<const Doc*, 3> docs{&cast(base), &cast(ours), &cast(theirs)};
        MergeResult res;
        for (std::size_t k = 0; k < 3; ++k)
            for (auto w : docs[k]->warnings) {
                w.message = std::string(k == 0 ? "base" : k == 1 ? "ours" : "theirs") + ": " + w.message;
                res.warnings.push_back(std::move(w));
            }
        Keys keys;
        Merger m{docs, keys, {}};
        const Slot root = m.root();
        auto& co = res.changes_ours;
        auto& ct = res.changes_theirs;
        co = diff_roots(keys, *docs[0], *docs[1], Side::ours);
        ct = diff_roots(keys, *docs[0], *docs[2], Side::theirs);
        // Every change a scope covers lies at or below its path: look them up by prefix.
        std::vector<const Change*> by_path;
        for (auto* cs : {&co, &ct})
            for (auto& c : *cs)
                by_path.push_back(&c);
        std::sort(by_path.begin(), by_path.end(),
                  [](const Change* x, const Change* y) { return x->at.path < y->at.path; });
        std::set<std::string> in_conflict;
        for (auto& pc : m.pending) {
            std::array<std::vector<std::string>, 2> ids; // ours, then theirs
            auto it = std::lower_bound(by_path.begin(), by_path.end(), pc.scope.path,
                                       [](const Change* c, const std::string& p) { return c->at.path < p; });
            for (; it != by_path.end() && (*it)->at.path.starts_with(pc.scope.path); ++it)
                if (pc.scope.covers(**it))
                    ids[(*it)->side == Side::ours ? 0 : 1].push_back((*it)->id),
                        in_conflict.insert((*it)->id);
            ids[0].insert(ids[0].end(), ids[1].begin(), ids[1].end());
            auto c = make_conflict(pc.at, std::move(ids[0]), pc.reason);
            if (pc.sequence)
                c.choices.push_back("union");
            res.conflicts.push_back(std::move(c));
        }
        // A change the other side also made (same op, address, and result) merged as "identical".
        auto sig = [](const Change& c) {
            return std::string(to_string(c.op)) + " " + c.at.to_string() + " " +
                   (c.after.hash ? c.after.hash->hex() : std::string());
        };
        std::array<std::set<std::string>, 2> made;
        for (auto& c : co)
            made[0].insert(sig(c));
        for (auto& c : ct)
            made[1].insert(sig(c));
        for (auto [cs, other] : {std::pair{&co, &made[1]}, std::pair{&ct, &made[0]}})
            for (auto& c : *cs)
                if (!in_conflict.count(c.id))
                    res.auto_merged.push_back({c.id, other->count(sig(c)) ? "identical" : "independent"});
        apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);

        const std::string text = Emitter{docs, res.conflicts}.emit(root);
        auto* p = reinterpret_cast<const std::byte*>(text.data());
        try {
            res.document = parse(ByteSource::from_memory({p, p + text.size()}, "merged.json"), nullptr);
        } catch (const Error& e) {
            throw Error(ErrorCategory::internal,
                        std::string("JSON writer produced an unparsable file: ") + e.what());
        }
        sort_canonical(co), sort_canonical(ct);
        return res;
    }

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(cast(d).src.bytes());
    }

    void dump(const Document& d, std::ostream& os, const DumpOptions& o) const override {
        std::string path;
        dump_node(cast(d).root(), path, os, o.line_at);
    }

    nlohmann::ordered_json inspect(const Document& d) const override {
        auto& j = cast(d);
        nlohmann::ordered_json o{{"dialect", j.jsonc ? "jsonc" : "json"}, {"nodes", j.nodes.size()}};
        if (!j.warnings.empty()) {
            auto& w = o["warnings"] = nlohmann::ordered_json::array();
            for (auto& x : j.warnings)
                w.push_back({{"code", x.code}, {"at", x.at}, {"message", x.message}});
        }
        return o;
    }

    // stats.format: counts per op and the identity keys chosen for arrays (json.md §3).
    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json o;
        for (auto op : {Op::insert, Op::erase, Op::update, Op::move})
            o[std::string(to_string(op))] =
                std::count_if(cs.begin(), cs.end(), [&](const Change& c) { return c.op == op; });
        Keys keys;
        diff_roots(keys, cast(a), cast(b), Side::theirs);
        auto& k = o["array_keys"] = nlohmann::ordered_json::object();
        for (auto& [path, key] : keys.cache)
            if (key)
                k[path] = *key;
        return o;
    }
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<JsonFormat>();
}

} // namespace semsplinter::json
