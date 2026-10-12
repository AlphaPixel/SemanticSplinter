// CSV/TSV module (docs/formats/csv.md, MVP): tokenizer with raw field spans, dialect sniffer, keyed or
// positional rows merged per cell, column add/delete, span-splice writer.
#include <semsplinter/error.hpp>
#include <semsplinter/formats/csv.hpp>
#include <semsplinter/merge.hpp>
#include <semsplinter/sequence.hpp>
#include <semsplinter/text.hpp>

#include "../common/splice.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace semsplinter::csv {

namespace {

constexpr std::size_t npos = std::string_view::npos;
constexpr std::size_t kHead = 64 * 1024;
constexpr std::string_view kDelims = ",\t;|"; // sniff order, which breaks ties (csv.md §2)
constexpr const char* kEllipsis = "\xE2\x80\xA6";

// ---- Tokenizer (csv.md §2, §9) ----

struct Field {
    std::size_t b = 0, e = 0; // raw span, quotes included
    bool quoted = false;
    std::string value;
};

// [pre, b): blank lines before the record; [b, e): its fields; [e, end): its terminator.
struct Record {
    std::size_t pre = 0, b = 0, e = 0, end = 0;
    std::vector<Field> f;
};

struct Tokens {
    std::vector<Record> rec;
    bool open = false; // the input ended inside a quoted field
};

std::size_t eol_len(std::string_view s, std::size_t p) {
    if (p >= s.size())
        return 0;
    if (s[p] == '\r')
        return p + 1 < s.size() && s[p + 1] == '\n' ? 2 : 1;
    return s[p] == '\n' ? 1 : 0;
}

// RFC 4180 with doubled quotes; text after a closing quote joins the field (as Python's csv does).
Tokens tokenize(std::string_view s, std::size_t p, char d, Limits* budget, std::size_t max = npos) {
    Tokens t;
    const char stops[3] = {d, '\r', '\n'};
    const std::string_view stop(stops, 3);
    std::size_t pre = p;
    while (p < s.size() && t.rec.size() < max) {
        if (auto n = eol_len(s, p)) { // blank line
            p += n;
            continue;
        }
        Record r;
        r.pre = pre, r.b = p;
        for (;;) {
            Field f;
            f.b = p;
            if (p < s.size() && s[p] == '"') {
                f.quoted = true;
                ++p;
                for (;;) {
                    auto q = s.find('"', p);
                    if (q == npos) {
                        f.value.append(s.substr(p));
                        p = s.size(), t.open = true;
                        break;
                    }
                    f.value.append(s.substr(p, q - p));
                    p = q + 1;
                    if (p < s.size() && s[p] == '"')
                        f.value += '"', ++p;
                    else
                        break;
                }
            }
            const auto q = std::min(s.find_first_of(stop, p), s.size());
            f.value.append(s.substr(p, q - p));
            p = f.e = q;
            if (budget)
                budget->charge(sizeof(Field) + f.value.size());
            r.f.push_back(std::move(f));
            if (p < s.size() && s[p] == d)
                ++p;
            else
                break;
        }
        r.e = p;
        p += eol_len(s, p);
        r.end = pre = p;
        t.rec.push_back(std::move(r));
    }
    return t;
}

// An Excel `sep=X` first line: the delimiter and the offset after the line.
std::optional<std::pair<char, std::size_t>> sep_line(std::string_view s) {
    if (s.size() < 5 || (s.substr(0, 4) != "sep=" && s.substr(0, 4) != "SEP="))
        return std::nullopt;
    const std::size_t n = eol_len(s, 5);
    if (s.size() > 5 && !n)
        return std::nullopt;
    return std::pair{s[4], 5 + n};
}

struct Sniff {
    char delim = ',';
    std::size_t width = 0, records = 0; // modal field count (0: no delimiter found)
    bool consistent = false;            // every record has the modal count
    bool open = false;
    // Looks like prose or code rather than a table: a column empty in every record (`x = 1;` lines), or
    // most fields after the first starting with a space (`Hello, world`).
    bool texty = false;
};

bool texty(const std::vector<Record>& rec, std::size_t width) {
    std::vector<bool> filled(width, false);
    std::size_t later = 0, spaced = 0;
    for (auto& r : rec)
        for (std::size_t k = 0; k < r.f.size() && k < width; ++k) {
            filled[k] = filled[k] || !r.f[k].value.empty();
            if (k > 0)
                ++later, spaced += !r.f[k].quoted && r.f[k].value.starts_with(' ');
        }
    return std::find(filled.begin(), filled.end(), false) != filled.end() || 2 * spaced > later;
}

// csv.md §2 step 2: score = fraction of records with the modal field count x log2(modal count).
Sniff sniff(std::string_view s, std::size_t p, bool cut) {
    Sniff best;
    double score = 0;
    for (char d : kDelims) {
        auto t = tokenize(s, p, d, nullptr, 1000);
        if (cut && t.rec.size() > 1) // the head may end inside the last record
            t.rec.pop_back(), t.open = false;
        std::map<std::size_t, std::size_t> counts;
        for (auto& r : t.rec)
            ++counts[r.f.size()];
        std::size_t width = 0, n = 0;
        for (auto [w, c] : counts)
            if (c >= n)
                width = w, n = c;
        if (width < 2)
            continue;
        const double sc = static_cast<double>(n) / static_cast<double>(t.rec.size()) * std::log2(width);
        if (sc > score)
            score = sc, best = {d, width, t.rec.size(), n == t.rec.size(), t.open, texty(t.rec, width)};
    }
    return best;
}

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 0;
        if (!n || i + n > s.size())
            return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        i += n;
    }
    return true;
}

// The text after the BOM as UTF-8; UTF-16/32 is transcoded into `buf`. Other bytes pass through
// (Windows-1252 is compared byte for byte).
std::string_view text_of(std::span<const std::byte> b, const EncodingInfo& enc, std::string& buf) {
    if (enc.encoding == Encoding::utf8)
        return {reinterpret_cast<const char*>(b.data()) + enc.bom, b.size() - enc.bom};
    buf = line_utf8(b, Line{enc.bom, b.size() - enc.bom, Eol::none}, enc.encoding);
    return buf;
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return o;
}

std::string extension(std::string_view name) {
    const auto dot = name.rfind('.');
    return dot == npos ? std::string() : lower(name.substr(dot));
}

bool csv_name(std::string_view name) {
    const auto e = extension(name);
    return e == ".csv" || e == ".tsv" || e == ".tab" || e == ".psv";
}

std::optional<double> number(std::string_view v) {
    if (v.empty() || v.size() > 64 || v.front() == ' ' || v.back() == ' ')
        return std::nullopt;
    std::istringstream is{std::string(v)};
    is.imbue(std::locale::classic());
    double x = 0;
    if (!(is >> x) || is.peek() != std::char_traits<char>::eof() || !std::isfinite(x))
        return std::nullopt;
    return x;
}

// Numeric, boolean, or ISO date (csv.md §2 step 3).
bool typed(std::string_view v) {
    const auto l = lower(v);
    if (number(v) || l == "true" || l == "false" || l == "yes" || l == "no")
        return true;
    auto dig = [&](std::size_t i) { return i < v.size() && v[i] >= '0' && v[i] <= '9'; };
    return v.size() >= 10 && dig(0) && dig(1) && dig(2) && dig(3) && v[4] == '-' && dig(5) && dig(6) &&
           v[7] == '-' && dig(8) && dig(9);
}

bool key_name(std::string_view h, bool first) {
    const auto l = lower(h);
    for (std::string_view k : {"id", "key", "uuid", "guid", "name", "code", "string_id", "msgid"})
        if (l == k)
            return true;
    return first && l.size() > 3 && l.ends_with("_id");
}

// Row 0 is a header if its fields are non-empty, unique, and untyped, and some column below it is typed
// (csv.md §2 step 3) or one of them names a key column (`id`, `key`, ...).
bool detect_header(const std::vector<Record>& rec) {
    if (rec.empty())
        return false;
    const auto& h = rec[0].f;
    std::set<std::string_view> seen;
    for (auto& f : h)
        if (f.value.empty() || typed(f.value) || !seen.insert(f.value).second)
            return false;
    for (std::size_t c = 0; c < h.size(); ++c)
        if (key_name(h[c].value, c == 0))
            return true;
    const std::size_t n = std::min<std::size_t>(rec.size(), 1000); // a sample (csv.md §2)
    for (std::size_t c = 0; c < h.size(); ++c) {
        bool any = false, all = true;
        for (std::size_t i = 1; i < n && all; ++i)
            if (c < rec[i].f.size() && !rec[i].f[c].value.empty())
                any = true, all = typed(rec[i].f[c].value);
        if (any && all)
            return true;
    }
    return false;
}

struct Doc final : Document {
    ByteSource src;
    EncodingInfo enc;
    std::string buf; // transcoded UTF-16/32 text
    std::string_view s;
    std::size_t body = 0; // after the `sep=` line
    char delim = ',';
    bool sep = false, header = false, utf8 = true;
    std::string eol = "\n"; // dominant record terminator
    std::vector<Record> rec;

    std::string_view raw(const Field& f) const { return s.substr(f.b, f.e - f.b); }
};

std::unique_ptr<Doc> parse(ByteSource src, Limits* budget, std::optional<char> delim,
                           std::optional<bool> header) {
    auto d = std::make_unique<Doc>();
    d->src = std::move(src);
    auto b = d->src.bytes();
    d->enc = detect_encoding(b);
    d->s = text_of(b, d->enc, d->buf);
    if (budget)
        budget->charge(d->buf.size() + sizeof(Record) * (d->s.size() / 16));
    if (auto sl = sep_line(d->s))
        d->sep = true, d->delim = sl->first, d->body = sl->second;
    if (delim) {
        d->delim = *delim;
    } else if (!d->sep) {
        const auto sn = sniff(d->s.substr(0, d->body + kHead), d->body, d->s.size() > d->body + kHead);
        const auto ext = extension(d->src.name());
        d->delim = sn.width ? sn.delim : ext == ".tsv" || ext == ".tab" ? '\t' : ext == ".psv" ? '|' : ',';
    }
    auto t = tokenize(d->s, d->body, d->delim, budget);
    if (t.open)
        throw Error(ErrorCategory::invalid_input, "csv: unterminated quoted field");
    d->rec = std::move(t.rec);
    std::array<std::size_t, 3> n{}; // LF, CRLF, CR
    for (auto& r : d->rec)
        if (r.end > r.e)
            ++n[r.end - r.e == 2 ? 1 : d->s[r.e] == '\r' ? 2 : 0];
    d->eol = n[1] > n[0] && n[1] >= n[2] ? "\r\n" : n[2] > n[0] && n[2] > n[1] ? "\r" : "\n";
    d->utf8 = d->enc.encoding != Encoding::utf8 || valid_utf8(d->s);
    d->header = header ? *header : detect_header(d->rec);
    return d;
}

// ---- Table view: column names and data rows (csv.md §3) ----

struct Table {
    const Doc* d = nullptr;
    std::size_t first = 0;         // the first data record
    std::vector<std::string> cols; // header names (deduplicated `name#2`), then positions ("4")
    std::size_t named = 0;         // how many come from the header
    std::map<std::string, std::size_t, std::less<>> at;

    std::size_t rows() const { return d->rec.size() - first; }
    const Record& row(std::size_t i) const { return d->rec[first + i]; }
    bool has(std::string_view n) const { return at.find(n) != at.end(); }
    // nullptr: no such field (a short, ragged row, or no such column).
    const Field* field(std::size_t i, std::string_view n) const {
        auto c = at.find(n);
        if (c == at.end())
            return nullptr;
        const auto& f = row(i).f;
        return c->second < f.size() ? &f[c->second] : nullptr;
    }
    const std::string* cell(std::size_t i, std::string_view n) const {
        const Field* f = field(i, n);
        return f ? &f->value : nullptr;
    }
    void add(std::string n) {
        const std::string base = n;
        for (int k = 2; has(n); ++k)
            n = base + "#" + std::to_string(k);
        at.emplace(n, cols.size());
        cols.push_back(std::move(n));
    }
};

Table view(const Doc& d, bool header) {
    Table t;
    t.d = &d;
    if (header && !d.rec.empty()) {
        t.first = 1;
        for (auto& f : d.rec[0].f)
            t.add(f.value.empty() ? std::to_string(t.cols.size() + 1) : f.value);
        t.named = t.cols.size();
    }
    std::size_t width = 0;
    for (std::size_t i = t.first; i < d.rec.size(); ++i)
        width = std::max(width, d.rec[i].f.size());
    while (t.cols.size() < width)
        t.add(std::to_string(t.cols.size() + 1));
    return t;
}

bool same(const std::string* x, const std::string* y) {
    return x == y || (x && y && *x == *y);
}

// ---- Row identity (csv.md §3) ----

// Percent-encodes the characters that address segments and composite keys reserve.
std::string esc(std::string_view v) {
    std::string o;
    for (char c : v) {
        if (c == '/' || c == '~' || c == ':' || c == '%' || c == ' ' || c == ',' || c == '#') {
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

using Key = std::vector<std::string>; // key column names; empty: rows are identified by position

// Each row's identity under `key`, or nullopt when a key column is missing or a key empty or repeated.
std::optional<std::vector<std::string>> row_keys(const Table& t, const Key& key) {
    for (auto& k : key)
        if (!t.has(k))
            return std::nullopt;
    std::vector<std::string> out(t.rows());
    std::set<std::string_view> seen;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        for (std::size_t j = 0; j < key.size(); ++j) {
            const std::string* v = t.cell(i, key[j]);
            if (!v || v->empty())
                return std::nullopt;
            out[i] += (j ? "," : "") + esc(*v);
        }
        if (!seen.insert(out[i]).second)
            return std::nullopt;
    }
    return out;
}

// "id:sword_01", "id,lang:sword_01,de".
std::string segment(const Key& key, const std::string& id) {
    std::string o;
    for (auto& k : key)
        o += (o.empty() ? "" : ",") + esc(k);
    return o + ":" + id;
}

bool sequential(const Table& t, const std::string& col) {
    std::optional<double> prev;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        const std::string* v = t.cell(i, col);
        auto x = v ? number(*v) : std::nullopt;
        if (!x || (prev ? *x != *prev + 1 : *x != 0 && *x != 1))
            return false;
        prev = x;
    }
    return t.rows() > 0;
}

// csv.md §3 key selection: --csv-key, then a key-named header column, then a unique first column that is
// not a row number. `ts[0]` names the columns (ours, or the new side of a diff).
Key pick_key(const OptionValues& opt, std::initializer_list<const Table*> ts) {
    auto ok = [&](const Key& k) {
        return std::all_of(ts.begin(), ts.end(), [&](const Table* t) { return row_keys(*t, k).has_value(); });
    };
    const Table& f = **ts.begin();
    if (auto o = opt.find("csv-key"); o != opt.end()) {
        Key k;
        for (std::size_t p = 0; p <= o->second.size();) {
            auto c = std::min(o->second.find(',', p), o->second.size());
            std::string n = o->second.substr(p, c - p);
            p = c + 1;
            if (!n.empty() &&
                std::all_of(n.begin(), n.end(), [](char ch) { return ch >= '0' && ch <= '9'; }) &&
                !f.has(n)) {
                const std::size_t i = std::stoull(n);
                if (i >= 1 && i <= f.cols.size())
                    n = f.cols[i - 1];
            }
            k.push_back(n);
        }
        if (!ok(k))
            throw Error(ErrorCategory::invalid_input,
                        "csv: --csv-key=" + o->second +
                            ": a key column is missing, or a key empty or repeated");
        return k;
    }
    for (std::size_t c = 0; c < f.named; ++c)
        if (key_name(f.cols[c], c == 0) && ok({f.cols[c]}))
            return {f.cols[c]};
    if (!f.cols.empty() && ok({f.cols[0]}) &&
        std::none_of(ts.begin(), ts.end(), [&](const Table* t) { return sequential(*t, f.cols[0]); }))
        return {f.cols[0]};
    return {};
}

// ---- Values and canonical text ----

std::size_t utf8_cut(std::string_view s, std::size_t n) {
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    return n;
}

std::string clip(std::string t) {
    return t.size() > 120 ? t.substr(0, utf8_cut(t, 120)) + kEllipsis : t;
}

std::string quote(std::string_view v, char d, bool always = false) {
    const char special[4] = {d, '"', '\r', '\n'};
    if (!always && v.find_first_of(std::string_view(special, 4)) == npos)
        return std::string(v);
    std::string o = "\"";
    for (char c : v)
        o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + '"';
}

// Comma-separated, minimally quoted (the dump and report form).
std::string canonical(const Table& t, std::size_t i) {
    std::string o;
    for (std::size_t k = 0; k < t.row(i).f.size(); ++k)
        o += (k ? "," : "") + quote(t.row(i).f[k].value, ',');
    return o;
}

// A number as is, other text as a JSON string, so reports and dumps stay one line per value.
std::string shown(const std::string& v) {
    return number(v) ? v : nlohmann::json(v).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

Value cell_value(const std::string* v) {
    return v ? Value{clip(shown(*v)), hash128(*v)} : Value{};
}

Value row_value(const Table& t, std::size_t i) {
    const Record& r = t.row(i);
    std::string text;
    for (char c : canonical(t, i))
        text += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : std::string(1, c);
    return {clip(text), hash128(t.d->s.substr(r.b, r.e - r.b))};
}

// Row content over `cols` only, so a column added on one side does not change every row (csv.md §5).
std::vector<SeqItem> seq_items(const Table& t, const std::vector<std::string>& cols) {
    std::vector<SeqItem> v;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        Hasher h;
        for (auto& c : cols) {
            const std::string* x = t.cell(i, c);
            h.add(x ? 1 : 0);
            if (x)
                h.add(*x);
        }
        v.push_back({h.digest(), std::nullopt});
    }
    return v;
}

// At least half of the shared cells are equal (csv.md §5 step 3).
bool similar(const Table& a, std::size_t i, const Table& b, std::size_t j) {
    std::size_t n = 0, eq = 0;
    for (auto& c : a.cols)
        if (b.has(c))
            ++n, eq += same(a.cell(i, c), b.cell(j, c));
    return 2 * eq >= n;
}

// ---- Diff (csv.md §5) ----

struct Differ {
    const Table &a, &b;
    const Key& key;
    Side side;
    ChangeSet& out;
    std::vector<std::string> common; // columns of both, in a's order

    void run() {
        std::vector<std::string> order;
        for (auto& c : a.cols)
            if (b.has(c))
                common.push_back(c);
            else
                out.push_back(make_change(side, Op::erase, {"/columns/" + pointer_escape(c)}, "column"));
        for (auto& c : b.cols)
            if (a.has(c))
                order.push_back(c);
            else
                out.push_back(make_change(side, Op::insert, {"/columns/" + pointer_escape(c)}, "column"));
        if (order != common) {
            auto& m = out.emplace_back(make_change(side, Op::update, {"/columns"}, "column-order"));
            auto join = [](const std::vector<std::string>& v) {
                std::string o;
                for (auto& x : v)
                    o += (o.empty() ? "" : ",") + quote(x, ',');
                return o;
            };
            m.before.text = clip(join(common)), m.after.text = clip(join(order));
        }
        if (key.empty())
            unkeyed();
        else
            keyed();
    }

    void cells(std::size_t i, std::size_t j, const std::string& path) {
        for (auto& c : common) {
            const std::string *x = a.cell(i, c), *y = b.cell(j, c);
            if (same(x, y))
                continue;
            Change ch =
                make_change(side, Op::update, {path + "/" + pointer_escape(c)}, x && y ? "cell" : "ragged");
            ch.before = cell_value(x), ch.after = cell_value(y);
            if (auto p = x ? number(*x) : std::nullopt, q = y ? number(*y) : std::nullopt; p && q) {
                ch.metrics["delta"] = *q - *p;
                if (*p != 0)
                    ch.metrics["rel"] = (*q - *p) / std::fabs(*p);
            }
            out.push_back(std::move(ch));
        }
    }

    void row(Op op, Address at, const Table& t, std::size_t i) {
        Change c = make_change(side, op, std::move(at), "row");
        (op == Op::insert ? c.after : c.before) = row_value(t, i);
        out.push_back(std::move(c));
    }

    void keyed() {
        const auto ka = *row_keys(a, key), kb = *row_keys(b, key);
        std::map<std::string_view, std::size_t> ib;
        for (std::size_t j = 0; j < kb.size(); ++j)
            ib[kb[j]] = j;
        for (std::size_t i = 0; i < ka.size(); ++i) {
            const std::string p = "/rows/" + segment(key, ka[i]);
            if (auto f = ib.find(ka[i]); f != ib.end())
                cells(i, f->second, p), ib.erase(f);
            else
                row(Op::erase, {p}, a, i);
        }
        for (std::size_t j = 0; j < kb.size(); ++j)
            if (ib.count(kb[j]))
                row(Op::insert, {"/rows/" + segment(key, kb[j])}, b, j);
    }

    // Erase and update address the row's index in a, insert its index in b.
    void unkeyed() {
        const auto ia = seq_items(a, common), ib = seq_items(b, common);
        for (auto& c : sequence_merge(ia, ia, ib)) {
            if (c.kind == SeqKind::stable)
                continue;
            // Pair rows of the gap in order by similarity: an update instead of erase plus insert.
            std::size_t k = 0;
            for (auto i : c.base) {
                auto m = k;
                while (m < c.theirs.size() && !similar(a, i, b, c.theirs[m]))
                    ++m;
                if (m == c.theirs.size()) {
                    row(Op::erase, {"/rows", Range{i, 1}}, a, i);
                    continue;
                }
                for (; k < m; ++k)
                    row(Op::insert, {"/rows", Range{c.theirs[k], 1}}, b, c.theirs[k]);
                cells(i, c.theirs[k++], "/rows/" + std::to_string(i));
            }
            for (; k < c.theirs.size(); ++k)
                row(Op::insert, {"/rows", Range{c.theirs[k], 1}}, b, c.theirs[k]);
        }
    }
};

ChangeSet diff_tables(const Table& a, const Table& b, const Key& key, Side side) {
    ChangeSet cs;
    Differ{a, b, key, side, cs, {}}.run();
    return cs;
}

std::string dialect_text(const Doc& d) {
    std::string o = d.enc.encoding != Encoding::utf8 ? std::string(to_string(d.enc.encoding))
                    : d.utf8                         ? "utf-8"
                                                     : "windows-1252";
    if (d.enc.bom)
        o += " bom";
    o += d.delim == '\t' ? " tab" : std::string(" '") + d.delim + "'";
    o += d.eol == "\r\n" ? " crlf" : d.eol == "\r" ? " cr" : " lf";
    if (d.sep)
        o += " sep-line";
    return o;
}

// ---- Merge plan (csv.md §6) ----

// Which side's field fills one output cell; a conflicted cell takes the side its resolution names.
struct Pick {
    Side side = Side::ours;
    std::optional<std::size_t> conflict;
};

// One output row: its record on each side, and one pick per merged column.
struct Item {
    std::array<std::optional<std::size_t>, 3> row;
    std::vector<Pick> cells;
};

struct Slot {
    std::vector<Item> items;
    std::optional<std::size_t> conflict;
    std::map<std::string, std::vector<Item>, std::less<>> alt; // per choice
};

struct Pending {
    Address at;
    std::string reason;
    std::function<bool(const Change&)> covers;
    bool sequence = false; // offers "union"
};

Side side_of(std::string_view choice) {
    return choice == "theirs" ? Side::theirs : choice == "base" ? Side::base : Side::ours;
}

std::size_t ix(Side s) {
    return static_cast<std::size_t>(s);
}

struct Merger {
    std::array<const Table*, 3> t; // base, ours, theirs
    Key key;
    bool header = false;
    std::vector<std::string> okeys; // ours' row identities (keyed tables)
    std::vector<Slot> slots;
    std::vector<std::string> cols;                // merged columns
    std::vector<std::optional<std::size_t>> kept; // a column one side deleted: kept per its conflict
    std::vector<Pending> pending;

    const Table& tb(Side s) const { return *t[ix(s)]; }

    std::size_t conflict(Address at, std::string reason, std::function<bool(const Change&)> covers,
                         bool sequence = false) {
        pending.push_back({std::move(at), std::move(reason), std::move(covers), sequence});
        return pending.size() - 1;
    }

    static Item single(Side s, std::size_t i) {
        Item it;
        it.row[ix(s)] = i;
        return it;
    }

    // Rows equal over the columns both tables have.
    bool rows_equal(Side x, std::size_t i, Side y, std::size_t j) const {
        const Table &a = tb(x), &b = tb(y);
        for (auto& c : a.cols)
            if (b.has(c) && !same(a.cell(i, c), b.cell(j, c)))
                return false;
        return true;
    }

    void keyed() {
        struct Ref {
            Side side;
            std::size_t i;
        };
        using KV = std::pair<std::string, Ref>;
        std::array<std::vector<KV>, 3> l;
        for (Side s : {Side::base, Side::ours, Side::theirs}) {
            const auto ks = *row_keys(tb(s), key);
            for (std::size_t i = 0; i < ks.size(); ++i)
                l[ix(s)].emplace_back(ks[i], Ref{s, i});
            if (s == Side::ours)
                okeys = ks;
        }
        auto rows = keyed_merge<std::string, Ref>(
            l[0], l[1], l[2],
            [&](const Ref& x, const Ref& y) { return rows_equal(x.side, x.i, y.side, y.i); },
            [](const std::string&, const Ref*, const Ref&, const Ref&) { return true; });
        for (auto& r : rows) {
            const std::string path = "/rows/" + segment(key, r.key);
            const std::array<std::optional<std::size_t>, 3> at{r.base, r.ours, r.theirs};
            Slot s;
            if (r.outcome.rule == MergeRule::conflict) { // modify-delete
                s.conflict = conflict({path}, r.outcome.reason, [path](const Change& c) {
                    return !c.at.region && path_within(c.at.path, path);
                });
                for (auto [name, side] : {std::pair{"ours", Side::ours}, std::pair{"theirs", Side::theirs},
                                          std::pair{"base", Side::base}})
                    s.alt[name] =
                        at[ix(side)] ? std::vector{single(side, *at[ix(side)])} : std::vector<Item>{};
            } else if (r.ours && r.theirs) {
                s.items = {Item{at, {}}};
            } else if (r.outcome.take) {
                s.items = {single(*r.outcome.take, *at[ix(*r.outcome.take)])};
            }
            slots.push_back(std::move(s));
        }
    }

    void unkeyed() {
        std::vector<std::string> common;
        for (auto& c : tb(Side::ours).cols)
            if (tb(Side::base).has(c) && tb(Side::theirs).has(c))
                common.push_back(c);
        const auto ib = seq_items(tb(Side::base), common), io = seq_items(tb(Side::ours), common),
                   it = seq_items(tb(Side::theirs), common);
        std::uint64_t bpos = 0;
        for (auto& c : sequence_merge(ib, io, it)) {
            Slot s;
            // Equal runs pair row by row and merge per cell; a conflicting run only when the rows are
            // similar.
            bool pair =
                c.ours.size() == c.theirs.size() && (c.base.size() == c.ours.size() || c.base.empty());
            for (std::size_t k = 0; pair && c.kind == SeqKind::conflict && k < c.ours.size(); ++k)
                pair = c.base.empty() ? similar(tb(Side::ours), c.ours[k], tb(Side::theirs), c.theirs[k])
                                      : similar(tb(Side::base), c.base[k], tb(Side::ours), c.ours[k]) &&
                                            similar(tb(Side::base), c.base[k], tb(Side::theirs), c.theirs[k]);
            if (pair) {
                for (std::size_t k = 0; k < c.ours.size(); ++k)
                    s.items.push_back(Item{
                        {c.base.empty() ? std::nullopt : std::optional(c.base[k]), c.ours[k], c.theirs[k]},
                        {}});
            } else if (c.kind == SeqKind::conflict) {
                const std::set<std::size_t> bs(c.base.begin(), c.base.end()),
                    os(c.ours.begin(), c.ours.end()), ts(c.theirs.begin(), c.theirs.end());
                s.conflict = conflict(
                    {"/rows", Range{bpos, c.base.size()}}, c.reason,
                    [=](const Change& ch) {
                        if (ch.at.region) {
                            auto& r = std::get<Range>(*ch.at.region);
                            return ch.op == Op::insert ? (ch.side == Side::ours ? os : ts).count(r.start) > 0
                                                       : bs.count(r.start) > 0;
                        }
                        auto rest =
                            std::string_view(ch.at.path).substr(std::min<std::size_t>(ch.at.path.size(), 6));
                        if (!ch.at.path.starts_with("/rows/"))
                            return false;
                        rest = rest.substr(0, rest.find('/'));
                        return !rest.empty() && rest.find_first_not_of("0123456789") == npos &&
                               bs.count(std::stoull(std::string(rest))) > 0;
                    },
                    true);
                for (auto choice : {"ours", "theirs", "base", "union"}) {
                    auto& v = s.alt[choice];
                    for (auto [side, i] : picks(c, choice))
                        v.push_back(single(side, i));
                }
            } else {
                for (auto [side, i] : picks(c))
                    s.items.push_back(single(side, i));
            }
            bpos += c.base.size();
            slots.push_back(std::move(s));
        }
    }

    template <class F> void each_item(F&& f) {
        for (auto& s : slots) {
            for (auto& it : s.items)
                f(it);
            for (auto& [name, v] : s.alt)
                for (auto& it : v)
                    f(it);
        }
    }

    // Side `s` kept column `n` that the other side deleted, and changed a cell in it.
    bool column_edited(const std::string& n, Side s) {
        const Table &b = tb(Side::base), &x = tb(s);
        bool edited = false;
        each_item([&](const Item& it) {
            if (!it.row[ix(s)])
                return;
            const std::string* v = x.cell(*it.row[ix(s)], n);
            if (it.row[0] ? !same(b.cell(*it.row[0], n), v) : v && !v->empty())
                edited = true;
        });
        return edited;
    }

    // Membership per column. Order: ours' (theirs' when only theirs reordered), each column of the other
    // side after its predecessor there, behind the first side's own additions (Q-CSV-2: ours first).
    void columns() {
        const Table &b = tb(Side::base), &o = tb(Side::ours), &th = tb(Side::theirs);
        auto reordered = [&](const Table& x) {
            std::vector<std::string> p, q;
            for (auto& c : x.cols)
                if (b.has(c))
                    p.push_back(c);
            for (auto& c : b.cols)
                if (x.has(c))
                    q.push_back(c);
            return p != q;
        };
        const bool swap = !reordered(o) && reordered(th);
        const Table &first = swap ? th : o, &second = swap ? o : th;
        std::vector<std::string> order = first.cols;
        std::size_t pos = 0;
        for (auto& n : second.cols) {
            if (auto f = std::find(order.begin(), order.end(), n); f != order.end()) {
                pos = static_cast<std::size_t>(f - order.begin()) + 1;
                continue;
            }
            while (pos < order.size() && !second.has(order[pos]))
                ++pos;
            order.insert(order.begin() + static_cast<std::ptrdiff_t>(pos++), n);
        }
        for (auto& n : order) {
            const bool in_b = b.has(n), in_o = o.has(n), in_t = th.has(n);
            if ((in_o && in_t) || (!in_b && (in_o || in_t))) {
                cols.push_back(n), kept.push_back(std::nullopt);
            } else if (in_b && (in_o || in_t) && column_edited(n, in_o ? Side::ours : Side::theirs)) {
                const std::string p = "/columns/" + pointer_escape(n), tail = "/" + pointer_escape(n);
                cols.push_back(n);
                kept.push_back(conflict({p}, "modify-delete", [p, tail](const Change& c) {
                    return c.at.path == p || (c.at.path.starts_with("/rows/") && c.at.path.ends_with(tail));
                }));
            }
        }
    }

    Pick cell(const Item& it, const std::string& n) {
        const Table &b = tb(Side::base), &o = tb(Side::ours), &th = tb(Side::theirs);
        const std::string* vb = it.row[0] && b.has(n) ? b.cell(*it.row[0], n) : nullptr;
        // A side without the column left it as in base.
        const std::string* vo = o.has(n) ? o.cell(*it.row[1], n) : vb;
        const std::string* vt = th.has(n) ? th.cell(*it.row[2], n) : vb;
        if (same(vo, vt))
            return {o.has(n) ? Side::ours : th.has(n) ? Side::theirs : Side::base};
        if (same(vb, vo))
            return {Side::theirs};
        if (same(vb, vt))
            return {Side::ours};
        const std::string p = "/rows/" +
                              (key.empty() ? std::to_string(it.row[0] ? *it.row[0] : *it.row[1])
                                           : segment(key, okeys[*it.row[1]])) +
                              "/" + pointer_escape(n);
        return {Side::ours, conflict({p}, it.row[0] ? "both-modified" : "add-add", [p](const Change& c) {
                    return !c.at.region && path_within(p, c.at.path);
                })};
    }

    void picks_for_all() {
        each_item([&](Item& it) {
            for (auto& n : cols)
                if (it.row[1] && it.row[2])
                    it.cells.push_back(cell(it, n));
                else // a row from one side: its fields, empty where it lacks the column (csv.md §6)
                    it.cells.push_back({it.row[1] ? Side::ours : it.row[2] ? Side::theirs : Side::base});
        });
    }
};

// ---- Span-splice writer (csv.md §7) ----

struct Writer {
    const Merger& m;
    const std::vector<Conflict>& conflicts;

    Side resolve(const Pick& p) const {
        return p.conflict ? side_of(conflicts[*p.conflict].resolved_as) : p.side;
    }

    std::string write() const {
        const Table& o = m.tb(Side::ours);
        const Doc& od = *o.d;
        const char d = od.delim;
        std::vector<std::size_t> live; // merged columns in the output
        std::vector<std::string> names;
        for (std::size_t c = 0; c < m.cols.size(); ++c)
            if (!m.kept[c] || m.tb(side_of(conflicts[*m.kept[c]].resolved_as)).has(m.cols[c]))
                live.push_back(c), names.push_back(m.cols[c]);
        const bool same_cols = names == o.cols;
        // A column whose every field ours quotes keeps being quoted (csv.md §7).
        std::vector<bool> always(m.cols.size(), false);
        for (auto c : live) {
            auto f = o.at.find(m.cols[c]);
            if (f == o.at.end() || !o.rows())
                continue;
            bool all = true;
            for (std::size_t i = 0; i < o.rows() && all; ++i)
                all = f->second < o.row(i).f.size() && o.row(i).f[f->second].quoted;
            always[c] = all;
        }

        std::vector<Splice> ins, rep;
        std::size_t named = 0; // header fields written
        if (m.header) {
            std::string h;
            bool all_quoted = o.first == 1;
            for (std::size_t k = 0; all_quoted && k < od.rec[0].f.size(); ++k)
                all_quoted = od.rec[0].f[k].quoted;
            for (auto c : live) {
                const std::string& n = m.cols[c];
                const Table& src = o.has(n)                    ? o
                                   : m.tb(Side::theirs).has(n) ? m.tb(Side::theirs)
                                                               : m.tb(Side::base);
                const std::size_t k = src.at.at(n);
                if (k >= src.named)
                    continue; // a positional column of ragged rows
                const Field& f = src.d->rec[0].f[k];
                h += (named++ ? std::string(1, d) : std::string()) +
                     (src.d->delim == d && (f.quoted || !all_quoted) ? std::string(src.d->raw(f))
                                                                     : quote(f.value, d, all_quoted));
            }
            if (!o.first && named)
                ins.push_back({od.body, od.body, h + od.eol});
            else if (o.first && !same_cols)
                rep.push_back({od.rec[0].b, od.rec[0].e, h});
        }

        // Insertions go after the last ours record placed so far; `open`: that record has no terminator.
        std::size_t pos = o.first ? od.rec[0].end : od.body;
        bool open = o.first && od.rec[0].e == od.rec[0].end;
        std::vector<bool> keep(o.rows(), false);
        for (auto& s : m.slots)
            for (auto& it : s.conflict ? s.alt.at(conflicts[*s.conflict].resolved_as) : s.items) {
                if (it.row[1]) { // ours' record, kept or rebuilt in place
                    const std::size_t i = *it.row[1];
                    const Record& r = o.row(i);
                    keep[i] = true;
                    bool untouched = same_cols;
                    for (auto c : live)
                        untouched = untouched && resolve(it.cells[c]) == Side::ours;
                    if (!untouched)
                        rep.push_back({r.b, r.e, record(it, live, named, always)});
                    pos = r.end, open = r.e == r.end;
                } else {
                    const std::string text = record(it, live, named, always);
                    ins.push_back({pos, pos, open ? od.eol + text : text + od.eol});
                }
            }
        for (std::size_t i = 0; i < o.rows(); ++i)
            if (!keep[i])
                rep.push_back({o.row(i).pre, o.row(i).end, ""});
        if (ins.empty() && rep.empty())
            return std::string(od.s);
        ins.insert(ins.end(), rep.begin(),
                   rep.end()); // insertions first: they precede a deletion at their offset
        return apply_splices(od.s, std::move(ins));
    }

    // A rebuilt record: each field's raw span when its side shares ours' delimiter, else minimally quoted.
    // Ragged rows stay short: trailing missing fields are left out, down to the header's width.
    std::string record(const Item& it, const std::vector<std::size_t>& live, std::size_t named,
                       const std::vector<bool>& always) const {
        const char d = m.tb(Side::ours).d->delim;
        std::vector<std::string> f;
        std::size_t last = 0;
        for (auto c : live) {
            const Side s = resolve(it.cells[c]);
            const Table& x = m.tb(s);
            const Field* fl = it.row[ix(s)] ? x.field(*it.row[ix(s)], m.cols[c]) : nullptr;
            if (!fl) {
                f.emplace_back(always[c] ? "\"\"" : "");
                continue;
            }
            f.push_back(x.d->delim == d && (!always[c] || fl->quoted) ? std::string(x.d->raw(*fl))
                                                                      : quote(fl->value, d, always[c]));
            last = f.size();
        }
        f.resize(std::max(last, std::min(named, f.size())));
        std::string o;
        for (std::size_t k = 0; k < f.size(); ++k)
            o += (k ? std::string(1, d) : std::string()) + f[k];
        return o;
    }
};

// ---- Format ----

const Doc& cast(const Document& d) {
    auto* c = dynamic_cast<const Doc*>(&d);
    if (!c)
        throw Error(ErrorCategory::internal, "csv: foreign document");
    return *c;
}

std::optional<bool> header_option(const OptionValues& o) {
    auto h = o.find("csv-header");
    if (h == o.end() || h->second == "auto")
        return std::nullopt;
    if (h->second == "yes")
        return true;
    if (h->second == "no")
        return false;
    throw Error(ErrorCategory::invalid_input, "--csv-header: expected yes, no, or auto");
}

class CsvFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{
            "csv",
            "CSV/TSV tables (rows by key column, cells merged)",
            {".csv", ".tsv", ".tab", ".psv"},
            {true, true, false, true, false},
            {{"csv-key", OptionSpec::Arg::required, "key column(s) by name or 1-based index (id or id,lang)"},
             {"csv-header", OptionSpec::Arg::required, "first row is a header: auto (default), yes, or no"}}};
        return i;
    }

    // Extension: 60. Content: every record has the same field count >= 2 under one dialect, 70 for at
    // least 5 records, 40 for 2 to 4 records of 3 or more fields; a `sep=` line: 70 (csv.md header).
    Probe probe(std::span<const std::byte> head, std::string_view name) const override {
        if (looks_binary(head))
            return {0};
        std::string buf;
        const auto s = text_of(head, detect_encoding(head), buf);
        const bool cut = head.size() >= kHead;
        int content = 0;
        if (sep_line(s)) {
            content = 70;
        } else {
            const auto sn = sniff(s, 0, cut);
            if (sn.width >= 2 && sn.consistent && !sn.open && !sn.texty)
                content = sn.records >= 5 ? 70 : sn.records >= 2 && sn.width >= 3 ? 40 : 0;
        }
        return {csv_name(name) ? std::max(60, content) : content};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        return parse(std::move(src), &o.ctx.budget, std::nullopt, header_option(o.ctx.format_options));
    }

    static bool header(std::initializer_list<const Doc*> ds) {
        return std::any_of(ds.begin(), ds.end(), [](const Doc* d) { return d->header; });
    }

    ChangeSet diff(const Document& da, const Document& db, const DiffOptions& o) const override {
        const Doc &a = cast(da), &b = cast(db);
        const bool h = header({&a, &b});
        const Table ta = view(a, h), tb = view(b, h);
        auto cs = diff_tables(ta, tb, pick_key(o.ctx.format_options, {&tb, &ta}), Side::theirs);
        // Dialect and quoting are encoding (csv.md §7.1).
        if (dialect_text(a) != dialect_text(b) ||
            (cs.empty() && !std::ranges::equal(a.src.bytes(), b.src.bytes()))) {
            auto& c = cs.emplace_back(make_change(Side::theirs, Op::update, {"/dialect"}, "encoding"));
            c.before.text = dialect_text(a), c.after.text = dialect_text(b);
        }
        sort_canonical(cs);
        return cs;
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override {
        const std::array<const Doc*, 3> docs{&cast(base), &cast(ours), &cast(theirs)};
        const bool h = header({docs[0], docs[1], docs[2]});
        const std::array<Table, 3> tables{view(*docs[0], h), view(*docs[1], h), view(*docs[2], h)};
        MergeResult res;
        Merger m;
        m.t = {&tables[0], &tables[1], &tables[2]};
        m.key = pick_key(mo.ctx.format_options, {&tables[1], &tables[0], &tables[2]});
        m.header = h;
        if (docs[0]->delim != docs[1]->delim || docs[2]->delim != docs[1]->delim)
            res.warnings.push_back(
                {"dialect-mismatch", "/dialect", "inputs use different delimiters; wrote ours'"});
        if (m.key.empty())
            m.unkeyed();
        else
            m.keyed();
        m.columns();
        m.picks_for_all();

        auto& co = res.changes_ours;
        auto& ct = res.changes_theirs;
        co = diff_tables(tables[0], tables[1], m.key, Side::ours);
        ct = diff_tables(tables[0], tables[2], m.key, Side::theirs);
        std::set<std::string> in_conflict;
        for (auto& p : m.pending) {
            std::vector<std::string> ids;
            for (auto* cs : {&co, &ct})
                for (auto& c : *cs)
                    if (p.covers(c))
                        ids.push_back(c.id), in_conflict.insert(c.id);
            auto c = make_conflict(p.at, std::move(ids), p.reason);
            if (p.sequence)
                c.choices.push_back("union");
            res.conflicts.push_back(std::move(c));
        }
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

        // An unchanged side yields the other side's file as is (theirs only in ours' dialect).
        auto bytes_of = [&](std::size_t k) { return docs[k]->src.bytes(); };
        const Doc* whole = nullptr;
        if (std::ranges::equal(bytes_of(0), bytes_of(1)) && dialect_text(*docs[1]) == dialect_text(*docs[2]))
            whole = docs[2];
        else if (std::ranges::equal(bytes_of(0), bytes_of(2)) || std::ranges::equal(bytes_of(1), bytes_of(2)))
            whole = docs[1];
        std::vector<std::byte> out;
        if (whole) {
            auto b = whole->src.bytes();
            out.assign(b.begin(), b.end());
        } else {
            const Doc& od = *docs[1];
            std::string text = Writer{m, res.conflicts}.write();
            auto src = od.src.bytes();
            out.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(od.enc.bom));
            if (od.enc.encoding != Encoding::utf8)
                text = encode_utf8_as(text, od.enc.encoding);
            auto* p = reinterpret_cast<const std::byte*>(text.data());
            out.insert(out.end(), p, p + text.size());
        }
        const Doc& like = whole ? *whole : *docs[1];
        try {
            res.document =
                parse(ByteSource::from_memory(std::move(out), "merged.csv"), nullptr, like.delim, h);
        } catch (const Error& e) {
            throw Error(ErrorCategory::internal,
                        std::string("CSV writer produced an unparsable file: ") + e.what());
        }
        sort_canonical(co), sort_canonical(ct);
        return res;
    }

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(cast(d).src.bytes());
    }

    // `#columns` then, for keyed tables, one line per cell (`[id:x] col = value`), else one canonical
    // CSV line per row (csv.md §5).
    void dump(const Document& doc, std::ostream& os, const DumpOptions& o) const override {
        const Doc& d = cast(doc);
        const Table t = view(d, d.header);
        const Key key = pick_key(o.ctx.format_options, {&t});
        // One dump line, which spans several when a quoted value holds line breaks.
        auto line = [&](const std::string& s, std::string at) {
            os << s << '\n';
            if (o.line_at)
                o.line_at->insert(o.line_at->end(),
                                  static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n')) + 1,
                                  Address{std::move(at)});
        };
        std::string cols = "#columns ";
        for (std::size_t c = 0; c < t.cols.size(); ++c)
            cols += (c ? "," : "") + quote(t.cols[c], ',');
        line(cols, "/columns");
        if (key.empty()) {
            for (std::size_t i = 0; i < t.rows(); ++i)
                line(canonical(t, i), "/rows/" + std::to_string(i));
            return;
        }
        const auto ks = *row_keys(t, key);
        for (std::size_t i = 0; i < t.rows(); ++i)
            for (auto& c : t.cols) {
                const std::string* v = t.cell(i, c);
                if (!v || std::find(key.begin(), key.end(), c) != key.end())
                    continue;
                line('[' + segment(key, ks[i]) + "] " + c + " = " + shown(*v),
                     "/rows/" + segment(key, ks[i]) + "/" + pointer_escape(c));
            }
    }

    nlohmann::ordered_json inspect(const Document& doc) const override {
        const Doc& d = cast(doc);
        const Table t = view(d, d.header);
        nlohmann::ordered_json o{{"dialect", dialect_text(d)},
                                 {"delimiter", std::string(1, d.delim)},
                                 {"header", d.header},
                                 {"columns", t.cols},
                                 {"rows", t.rows()}};
        const OptionValues none;
        const Key key = pick_key(none, {&t});
        if (key.empty())
            o["row_identity"] = "position";
        else
            o["key"] = key;
        return o;
    }

    // stats.format: counts per op, cells changed, and the row identity (csv.md §3, §5).
    nlohmann::ordered_json stats(const Document& a, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json o;
        for (auto op : {Op::insert, Op::erase, Op::update, Op::move})
            o[std::string(to_string(op))] =
                std::count_if(cs.begin(), cs.end(), [&](const Change& c) { return c.op == op; });
        o["cells_changed"] = std::count_if(
            cs.begin(), cs.end(), [](const Change& c) { return c.kind == "cell" || c.kind == "ragged"; });
        const Doc &da = cast(a), &db = cast(b);
        const bool h = header({&da, &db});
        const Table ta = view(da, h), tb = view(db, h);
        const OptionValues none;
        const Key key = pick_key(none, {&tb, &ta});
        if (key.empty())
            o["row_identity"] = "position";
        else
            o["row_identity"] = "key", o["key"] = key;
        return o;
    }
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<CsvFormat>();
}

} // namespace semsplinter::csv
