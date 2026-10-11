// SQLite module (docs/formats/sqlite.md, MVP): every read and write goes through the SQLite library on a
// private in-memory copy of the file (sqlite3_deserialize). Diff: header fields, schema objects by
// (type, name) over normalized SQL, rows by primary key or rowid, cells by column. Merge: KeyedMerge per
// level, add-add of SQLite-allocated row ids renumbered (SPEC §2.3), and a writer that applies the result
// to a copy of ours, so pages SQLite does not touch keep their bytes (sqlite.md §7).
#include <semsplinter/error.hpp>
#include <semsplinter/formats/sqlite.hpp>
#include <semsplinter/merge.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" void semsplinter_cksum(unsigned char* page, int n, unsigned char* out); // cksum.c

namespace semsplinter::sqlite {

namespace {

constexpr std::string_view kMagic{"SQLite format 3\0", 16};
constexpr const char* kEllipsis = "\xE2\x80\xA6";

// ---- Values (sqlite.md §3: storage class plus value; REAL compared bitwise) ----

struct Cell {
    int type = SQLITE_NULL;
    std::int64_t i = 0; // INTEGER, or the bits of a REAL
    std::string s;      // TEXT (UTF-8) or BLOB
    auto operator<=>(const Cell&) const = default;
};
using Row = std::vector<Cell>;
using Key = std::vector<Cell>;

std::string hex(std::string_view s) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s)
        o += d[c >> 4], o += d[c & 15];
    return o;
}

// The dump and report form (sqlite.md §5): JSON, with tags where JSON alone is ambiguous.
nlohmann::ordered_json js(const Cell& c) {
    switch (c.type) {
    case SQLITE_INTEGER:
        return c.i;
    case SQLITE_FLOAT: {
        const double d = std::bit_cast<double>(c.i);
        if (std::isfinite(d))
            return d;
        return {{"$real", d > 0 ? "inf" : "-inf"}};
    }
    case SQLITE_TEXT:
        return c.s;
    case SQLITE_BLOB:
        if (c.s.size() <= 64)
            return {{"$blob", hex(c.s)}};
        return {{"$blob", "xxh3:" + hash128(c.s).hex()}, {"len", c.s.size()}};
    default:
        return nullptr;
    }
}

std::string text(const nlohmann::ordered_json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string clip(std::string t) {
    if (t.size() <= 120)
        return t;
    std::size_t n = 120;
    while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80)
        --n;
    return t.substr(0, n) + kEllipsis;
}

void add(Hasher& h, const Cell& c) {
    h.add(static_cast<std::uint64_t>(c.type)).add(static_cast<std::uint64_t>(c.i)).add(c.s);
}

Value cell_value(const Cell& c) {
    Hasher h;
    add(h, c);
    return {clip(text(js(c))), h.digest()};
}

std::string quote_id(std::string_view n) {
    std::string o = "\"";
    for (char c : n)
        o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + '"';
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return o;
}

// ---- Connections ----

[[noreturn]] void fail(sqlite3* db, std::string_view what) {
    const std::string msg = sqlite3_errmsg(db);
    const bool missing = msg.find("no such module") != std::string::npos ||
                         msg.find("no such collation") != std::string::npos ||
                         msg.find("no such function") != std::string::npos;
    throw Error(missing ? ErrorCategory::unsupported : ErrorCategory::invalid_input,
                "sqlite: " + std::string(what.substr(0, 200)) + ": " + msg);
}

class Stmt {
public:
    Stmt(sqlite3* db, std::string_view sql) : db_(db), sql_(sql) {
        if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &s_, nullptr) != SQLITE_OK)
            fail(db, sql);
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int k, const Cell& c) {
        switch (c.type) {
        case SQLITE_INTEGER:
            sqlite3_bind_int64(s_, k, c.i);
            break;
        case SQLITE_FLOAT:
            sqlite3_bind_double(s_, k, std::bit_cast<double>(c.i));
            break;
        case SQLITE_TEXT:
            sqlite3_bind_text(s_, k, c.s.data(), static_cast<int>(c.s.size()), SQLITE_TRANSIENT);
            break;
        case SQLITE_BLOB:
            sqlite3_bind_blob(s_, k, c.s.data(), static_cast<int>(c.s.size()), SQLITE_TRANSIENT);
            break;
        default:
            sqlite3_bind_null(s_, k);
        }
        return *this;
    }
    Stmt& bind(int k, std::string_view t) {
        sqlite3_bind_text(s_, k, t.data(), static_cast<int>(t.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int k, std::int64_t v) {
        sqlite3_bind_int64(s_, k, v);
        return *this;
    }
    // SQLITE_ROW, SQLITE_DONE, or SQLITE_CONSTRAINT; anything else throws.
    int run() {
        const int rc = sqlite3_step(s_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE && (rc & 0xFF) != SQLITE_CONSTRAINT)
            fail(db_, sql_);
        return rc;
    }
    bool step() {
        const int rc = run();
        if ((rc & 0xFF) == SQLITE_CONSTRAINT)
            fail(db_, sql_);
        return rc == SQLITE_ROW;
    }
    void reset() {
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
    }
    std::int64_t i64(int k) const { return sqlite3_column_int64(s_, k); }
    std::string str(int k) const {
        auto* p = reinterpret_cast<const char*>(sqlite3_column_text(s_, k));
        return p ? std::string(p, static_cast<std::size_t>(sqlite3_column_bytes(s_, k))) : std::string();
    }
    Cell cell(int k) const {
        Cell c;
        c.type = sqlite3_column_type(s_, k);
        if (c.type == SQLITE_INTEGER) {
            c.i = sqlite3_column_int64(s_, k);
        } else if (c.type == SQLITE_FLOAT) {
            c.i = std::bit_cast<std::int64_t>(sqlite3_column_double(s_, k));
        } else if (c.type == SQLITE_TEXT) {
            c.s = str(k);
        } else if (c.type == SQLITE_BLOB) {
            auto* p = static_cast<const char*>(sqlite3_column_blob(s_, k));
            if (p)
                c.s.assign(p, static_cast<std::size_t>(sqlite3_column_bytes(s_, k)));
        }
        return c;
    }

private:
    sqlite3* db_;
    std::string sql_;
    sqlite3_stmt* s_ = nullptr;
};

// A connection on a private copy of a database image. Inputs are read-only and defensive (sqlite.md §9);
// the writer's copy is resizable and may write shadow tables. Header bytes 18/19 say WAL mode, which an
// in-memory database cannot open, so the copy says rollback mode; the writer puts ours' bytes back.
class Conn {
public:
    Conn(std::span<const std::byte> image, bool writable) {
        if (sqlite3_open_v2(":memory:", &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
            SQLITE_OK)
            throw Error(ErrorCategory::internal, "sqlite: cannot open a connection");
        sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
        sqlite3_db_config(db_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        if (!writable)
            sqlite3_db_config(db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        const auto n = static_cast<sqlite3_int64>(image.size());
        auto* buf = static_cast<unsigned char*>(sqlite3_malloc64(static_cast<sqlite3_uint64>(n ? n : 1)));
        if (!buf)
            throw Error(ErrorCategory::limit, "sqlite: out of memory");
        std::memcpy(buf, image.data(), image.size());
        if (n > 19 && (buf[18] == 2 || buf[19] == 2))
            buf[18] = buf[19] = 1;
        const unsigned flags = SQLITE_DESERIALIZE_FREEONCLOSE |
                               (writable ? SQLITE_DESERIALIZE_RESIZEABLE : SQLITE_DESERIALIZE_READONLY);
        if (sqlite3_deserialize(db_, "main", buf, n, n, flags) != SQLITE_OK)
            fail(db_, "deserialize");
    }
    ~Conn() { sqlite3_close(db_); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    sqlite3* get() const { return db_; }
    void exec(const std::string& sql) {
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            fail(db_, sql);
    }
    std::string one(const std::string& sql) {
        Stmt s(db_, sql);
        return s.step() ? s.str(0) : std::string();
    }
    bool exists(std::string_view type, std::string_view name) {
        Stmt s(db_, "SELECT 1 FROM sqlite_schema WHERE (?1 = '' OR type = ?1) AND name = ?2");
        s.bind(1, type).bind(2, name);
        return s.step();
    }
    std::vector<std::byte> serialize() {
        sqlite3_int64 n = 0;
        auto* p = sqlite3_serialize(db_, "main", &n, 0);
        if (!p)
            throw Error(ErrorCategory::internal, "sqlite: serialize failed");
        auto* b = reinterpret_cast<const std::byte*>(p);
        std::vector<std::byte> out(b, b + n);
        sqlite3_free(p);
        return out;
    }

private:
    sqlite3* db_ = nullptr;
};

// ---- Schema normalization (sqlite.md §4) ----

// Whitespace and comments collapsed outside string literals, identifier quotes dropped, case folded.
std::string normalize(std::string_view s) {
    std::string o;
    bool gap = false;
    auto tight = [](char c) { return std::strchr("(),;=.", c) != nullptr; };
    auto put = [&](std::string_view tok) {
        if (gap && !o.empty() && !tight(o.back()) && !tight(tok.front()))
            o += ' ';
        gap = false;
        o += tok;
    };
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            gap = true, ++i;
        } else if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            i = std::min(s.find('\n', i), s.size()), gap = true;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const auto e = s.find("*/", i + 2);
            i = e == std::string_view::npos ? s.size() : e + 2, gap = true;
        } else if (c == '\'' || c == '"' || c == '`' || c == '[') {
            const char close = c == '[' ? ']' : c;
            std::string tok;
            std::size_t j = i + 1;
            for (; j < s.size(); ++j) {
                if (s[j] == close) {
                    if (close != ']' && j + 1 < s.size() && s[j + 1] == close) {
                        tok += close, ++j;
                        continue;
                    }
                    break;
                }
                tok += s[j];
            }
            i = j + 1;
            put(c == '\'' ? "'" + tok + "'" : lower(tok));
        } else {
            put(std::string(1, static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c)));
            ++i;
            // Keep a word together so `put` inserts no gaps inside it.
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' ||
                                    static_cast<unsigned char>(s[i]) >= 0x80)) {
                o += static_cast<char>(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
                ++i;
            }
        }
    }
    return o;
}

std::string one_line(std::string_view sql) {
    std::string o;
    for (char c : sql)
        o += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : std::string(1, c);
    return o;
}

// ---- Document ----

struct Table {
    std::string name, type; // type: "table", "virtual", or "shadow"
    std::string sql, norm;
    std::vector<std::string> cols; // stored columns (generated and hidden ones are derived, sqlite.md §10)
    std::vector<std::size_t> pk;   // key columns; empty: rows are identified by rowid
    std::string rid;               // how to name the rowid (`rowid`, `_rowid_`, or `oid`)
    bool counter = false;          // the key is a row id SQLite allocates (rowid or INTEGER PRIMARY KEY)
    std::map<Key, Row> rows;
    std::vector<std::string> shadows; // a virtual table's shadow tables
    Hash128 content;

    bool by_rowid() const { return pk.empty(); }
    std::vector<std::string> key_names() const {
        std::vector<std::string> o;
        for (auto k : pk)
            o.push_back(cols[k]);
        return o;
    }
};

bool compatible(const Table& a, const Table& b) {
    return a.type == "table" && b.type == "table" && a.key_names() == b.key_names();
}

struct Obj {
    std::string type, name, tbl, sql, norm;
    std::int64_t order = 0; // rowid in sqlite_schema: creation order
};
using ObjKey = std::pair<std::string, std::string>; // (type, name)

struct Doc final : Document {
    ByteSource src;
    std::uint32_t page_size = 0;
    unsigned reserved = 0;
    bool wal = false;
    bool cksum = false;             // 8 reserved bytes and page 1's cksumvfs checksum verifies
    std::vector<std::uint64_t> bad; // pages whose checksum does not verify (with cksum)
    std::string encoding, version;
    std::int64_t user_version = 0, application_id = 0;
    std::map<ObjKey, Obj> schema;        // index, view, trigger, and table (no internal or shadow objects)
    std::map<std::string, Table> tables; // table, virtual, and shadow tables
    std::map<std::string, std::int64_t> seq; // sqlite_sequence (derived)
    std::set<std::string> referenced;        // tables a foreign key points at
    bool stat1 = false;
    std::vector<Warning> warnings;
    EnvelopeMap map;
};

std::uint32_t be(std::span<const std::byte> b, std::size_t at, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < n; ++k)
        v = v << 8 | std::to_integer<std::uint32_t>(b[at + k]);
    return v;
}

bool page_ok(std::span<const std::byte> page) {
    unsigned char sum[8];
    std::vector<unsigned char> buf(page.size());
    std::memcpy(buf.data(), page.data(), page.size());
    semsplinter_cksum(buf.data(), static_cast<int>(buf.size() - 8), sum);
    return std::memcmp(sum, buf.data() + buf.size() - 8, 8) == 0;
}

void set_cksum(std::span<std::byte> page) {
    auto* p = reinterpret_cast<unsigned char*>(page.data());
    semsplinter_cksum(p, static_cast<int>(page.size() - 8), p + page.size() - 8);
}

// Header fields by class (sqlite.md §7.1); pages after the header are the B-tree encoding of the rows.
EnvelopeMap envelope_of(std::uint64_t size) {
    EnvelopeMap m;
    struct F {
        std::uint64_t at, len;
        ByteClass cls;
        const char* unit;
    };
    static const F fields[] = {{0, 24, ByteClass::framing, "/header"},
                               {24, 4, ByteClass::volatile_, "/header"},
                               {28, 28, ByteClass::framing, "/header"},
                               {56, 4, ByteClass::content, "/header/encoding"},
                               {60, 4, ByteClass::content, "/header/user_version"},
                               {64, 4, ByteClass::framing, "/header"},
                               {68, 4, ByteClass::content, "/header/application_id"},
                               {72, 20, ByteClass::framing, "/header"},
                               {92, 8, ByteClass::volatile_, "/header"}};
    for (auto& f : fields)
        m.ranges.push_back({f.at, f.len, f.cls, f.unit, ""});
    if (size > 100)
        m.ranges.push_back({100, size - 100, ByteClass::encoding, "/pages", "btree"});
    return m;
}

bool reserved_name(std::string_view n) {
    return lower(n.substr(0, 7)) == "sqlite_";
}

void load_rows(Conn& c, Table& t, bool has_rowid, Limits* budget, std::vector<Warning>& warnings) {
    std::vector<std::pair<std::int64_t, std::size_t>> pk; // (ordinal, column)
    std::string pk_type;
    {
        Stmt s(c.get(), "SELECT name, type, pk, hidden FROM pragma_table_xinfo(?1)");
        s.bind(1, t.name);
        while (s.step()) {
            if (s.i64(3) != 0)
                continue;
            if (s.i64(2) > 0)
                pk.emplace_back(s.i64(2), t.cols.size()), pk_type = s.str(1);
            t.cols.push_back(s.str(0));
        }
    }
    std::sort(pk.begin(), pk.end());
    for (auto& [n, k] : pk)
        t.pk.push_back(k);
    if (has_rowid) {
        for (const char* r : {"rowid", "_rowid_", "oid"}) {
            if (std::none_of(t.cols.begin(), t.cols.end(),
                             [&](const std::string& n) { return lower(n) == r; })) {
                t.rid = r;
                break;
            }
        }
        if (t.rid.empty())
            throw Error(ErrorCategory::unsupported, "sqlite: table " + t.name + " hides its rowid");
    }
    std::string sql = "SELECT ";
    if (has_rowid)
        sql += t.rid + (t.cols.empty() ? "" : ", ");
    for (std::size_t k = 0; k < t.cols.size(); ++k)
        sql += (k ? ", " : "") + quote_id(t.cols[k]);
    sql += " FROM " + quote_id(t.name);
    std::vector<std::pair<std::int64_t, Row>> all;
    Stmt s(c.get(), sql);
    const int off = has_rowid ? 1 : 0;
    while (s.step()) {
        Row r;
        std::uint64_t bytes = sizeof(Row) + 64;
        for (std::size_t k = 0; k < t.cols.size(); ++k) {
            r.push_back(s.cell(static_cast<int>(k) + off));
            bytes += sizeof(Cell) + r.back().s.size();
        }
        if (budget)
            budget->charge(bytes);
        all.emplace_back(has_rowid ? s.i64(0) : 0, std::move(r));
    }
    // A declared key identifies rows when it is unique and never NULL (rowid tables allow both).
    bool ok = !t.pk.empty();
    for (auto& [rowid, r] : all) {
        if (!ok)
            break;
        Key k;
        for (auto p : t.pk)
            k.push_back(r[p]);
        ok = std::none_of(k.begin(), k.end(), [](const Cell& x) { return x.type == SQLITE_NULL; }) &&
             t.rows.emplace(std::move(k), r).second;
    }
    if (!ok && !t.pk.empty() && !has_rowid)
        throw Error(ErrorCategory::invalid_input, "sqlite: table " + t.name + " has a NULL or repeated key");
    if (!ok) {
        if (!t.pk.empty() && t.type == "table")
            warnings.push_back({"identity-rowid", "/tables/" + pointer_escape(t.name),
                                "the primary key has NULL or repeated values; rows are identified by rowid"});
        else if (t.type == "table")
            warnings.push_back({"identity-rowid", "/tables/" + pointer_escape(t.name),
                                "no primary key; rows are identified by rowid, which VACUUM may renumber"});
        t.pk.clear(), t.rows.clear();
        for (auto& [rowid, r] : all)
            t.rows.emplace(Key{Cell{SQLITE_INTEGER, rowid, {}}}, std::move(r));
    }
    t.counter = has_rowid && (t.pk.empty() || (t.pk.size() == 1 && lower(pk_type) == "integer"));
    Hasher h;
    for (auto& [k, r] : t.rows) {
        for (auto& x : k)
            add(h, x);
        for (auto& x : r)
            add(h, x);
    }
    t.content = h.digest();
}

std::unique_ptr<Doc> parse(ByteSource src, Limits* budget) {
    auto d = std::make_unique<Doc>();
    d->src = std::move(src);
    const auto b = d->src.bytes();
    if (b.size() < 100 || std::memcmp(b.data(), kMagic.data(), kMagic.size()) != 0)
        throw Error(ErrorCategory::invalid_input, "sqlite: not an SQLite 3 database");
    const auto ps = be(b, 16, 2);
    d->page_size = ps == 1 ? 65536 : ps;
    d->reserved = be(b, 20, 1);
    d->wal = be(b, 18, 1) == 2 || be(b, 19, 1) == 2;
    if (d->reserved == 8 && d->page_size >= 512 && b.size() >= d->page_size && b.size() % d->page_size == 0 &&
        page_ok(b.subspan(0, d->page_size))) {
        d->cksum = true;
        for (std::uint64_t p = 1; p < b.size() / d->page_size; ++p)
            if (!page_ok(b.subspan(p * d->page_size, d->page_size)))
                d->bad.push_back(p + 1);
    }
    d->map = envelope_of(b.size());
    if (budget)
        budget->charge(b.size());

    Conn c(b, false);
    if (const auto qc = c.one("PRAGMA quick_check"); qc != "ok")
        throw Error(ErrorCategory::invalid_input, "sqlite: quick_check: " + qc);
    d->version = c.one("SELECT sqlite_version()");
    d->encoding = c.one("PRAGMA encoding");
    d->user_version = std::stoll(c.one("PRAGMA user_version"));
    d->application_id = std::stoll(c.one("PRAGMA application_id"));

    std::map<std::string, std::pair<std::string, bool>> listed; // name -> (type, without rowid)
    {
        Stmt s(c.get(), "SELECT name, type, wr FROM pragma_table_list WHERE schema = 'main'");
        while (s.step())
            listed[s.str(0)] = {s.str(1), s.i64(2) != 0};
    }
    d->stat1 = listed.count("sqlite_stat1") > 0;
    if (listed.count("sqlite_sequence")) {
        Stmt s(c.get(), "SELECT name, seq FROM sqlite_sequence");
        while (s.step())
            d->seq[s.str(0)] = s.i64(1);
    }
    for (auto& [name, tw] : listed) {
        auto& [type, wr] = tw;
        if (reserved_name(name) || (type != "table" && type != "virtual" && type != "shadow"))
            continue;
        Table& t = d->tables[name];
        t.name = name, t.type = type;
        if (type != "virtual")
            load_rows(c, t, !wr, budget, d->warnings);
        Stmt fk(c.get(), "SELECT \"table\" FROM pragma_foreign_key_list(?1)");
        fk.bind(1, name);
        while (fk.step())
            d->referenced.insert(fk.str(0));
    }
    // Each shadow table belongs to the virtual table with the longest name that prefixes it.
    for (auto& [name, t] : d->tables) {
        if (t.type != "shadow")
            continue;
        const Table* owner = nullptr;
        for (auto& [vn, v] : d->tables)
            if (v.type == "virtual" && name.size() > vn.size() + 1 && name.starts_with(vn + "_") &&
                (!owner || vn.size() > owner->name.size()))
                owner = &v;
        if (owner)
            d->tables[owner->name].shadows.push_back(name);
    }
    for (auto& [name, t] : d->tables) {
        if (t.type != "virtual")
            continue;
        Hasher h;
        for (auto& sh : t.shadows)
            h.add(sh).add(d->tables[sh].content.hex());
        t.content = h.digest();
    }
    Stmt s(c.get(), "SELECT type, name, tbl_name, sql, rowid FROM sqlite_schema");
    while (s.step()) {
        Obj o{s.str(0), s.str(1), s.str(2), s.str(3), {}, s.i64(4)};
        auto t = d->tables.find(o.name);
        if (reserved_name(o.name) || o.sql.empty() || (t != d->tables.end() && t->second.type == "shadow"))
            continue;
        o.norm = normalize(o.sql);
        if (t != d->tables.end())
            t->second.sql = o.sql, t->second.norm = o.norm;
        d->schema[{o.type, o.name}] = std::move(o);
    }
    return d;
}

const Doc& cast(const Document& d) {
    auto* p = dynamic_cast<const Doc*>(&d);
    if (!p)
        throw Error(ErrorCategory::internal, "sqlite: foreign document");
    return *p;
}

// ---- Addresses (sqlite.md §3) ----

std::string key_text(const Cell& c) {
    switch (c.type) {
    case SQLITE_INTEGER:
        return std::to_string(c.i);
    case SQLITE_FLOAT:
        return text(js(c));
    case SQLITE_TEXT:
        return c.s;
    case SQLITE_BLOB:
        return "x'" + hex(c.s) + "'";
    default:
        return "NULL";
    }
}

std::string table_path(std::string_view t) {
    return "/tables/" + pointer_escape(t);
}

// "/tables/items/rows/pk:42", "/tables/loc/rows/pk:en,menu.start", "/tables/log/rows/rowid:7".
std::string row_path(const Table& t, const Key& k) {
    std::string o = table_path(t.name) + (t.by_rowid() ? "/rows/rowid:" : "/rows/pk:");
    for (std::size_t j = 0; j < k.size(); ++j) {
        if (j)
            o += ',';
        for (char c : key_text(k[j]))
            o += c == '~'   ? std::string("~0")
                 : c == '/' ? std::string("~1")
                 : c == ',' ? std::string("~3")
                            : std::string(1, c);
    }
    return o;
}

std::string schema_path(const ObjKey& k) {
    return "/schema/" + k.first + ":" + pointer_escape(k.second);
}

nlohmann::ordered_json row_json(const Table& t, const Row& r, bool skip_key) {
    nlohmann::ordered_json o = nlohmann::ordered_json::object();
    for (std::size_t k = 0; k < t.cols.size(); ++k)
        if (!skip_key || std::find(t.pk.begin(), t.pk.end(), k) == t.pk.end())
            o[t.cols[k]] = js(r[k]);
    return o;
}

Value row_value(const Table& t, const Row& r) {
    Hasher h;
    for (auto& c : r)
        add(h, c);
    return {clip(text(row_json(t, r, false))), h.digest()};
}

Value sql_value(const Obj& o) {
    return {clip(one_line(o.sql)), hash128(o.norm)};
}

// ---- Diff (sqlite.md §5) ----

void diff_rows(const Table& a, const Table& b, Side side, ChangeSet& out) {
    std::vector<std::pair<std::size_t, std::size_t>> common;
    for (std::size_t i = 0; i < a.cols.size(); ++i)
        if (auto f = std::find(b.cols.begin(), b.cols.end(), a.cols[i]); f != b.cols.end())
            common.emplace_back(i, static_cast<std::size_t>(f - b.cols.begin()));
    const std::vector<std::string> dep{"/schema/table:" + pointer_escape(a.name)};
    auto row = [&](Op op, const Table& t, const Key& k, const Row& r) {
        Change c = make_change(side, op, {row_path(t, k)}, "row");
        (op == Op::insert ? c.after : c.before) = row_value(t, r);
        c.depends_on = dep;
        out.push_back(std::move(c));
    };
    auto ia = a.rows.begin(), ib = b.rows.begin();
    while (ia != a.rows.end() || ib != b.rows.end()) {
        if (ib == b.rows.end() || (ia != a.rows.end() && ia->first < ib->first)) {
            row(Op::erase, a, ia->first, ia->second), ++ia;
        } else if (ia == a.rows.end() || ib->first < ia->first) {
            row(Op::insert, b, ib->first, ib->second), ++ib;
        } else {
            const std::string p = row_path(a, ia->first);
            for (auto [x, y] : common) {
                if (ia->second[x] == ib->second[y])
                    continue;
                Change c = make_change(side, Op::update, {p + "/" + pointer_escape(a.cols[x])}, "cell");
                c.before = cell_value(ia->second[x]), c.after = cell_value(ib->second[y]);
                c.depends_on = dep;
                out.push_back(std::move(c));
            }
            ++ia, ++ib;
        }
    }
}

ChangeSet diff_docs(const Doc& a, const Doc& b, Side side) {
    ChangeSet out;
    auto header = [&](const char* f, const std::string& x, const std::string& y) {
        if (x == y)
            return;
        auto& c = out.emplace_back(make_change(side, Op::update, {std::string("/header/") + f}, "header"));
        c.before = {x, hash128(x)}, c.after = {y, hash128(y)};
    };
    header("page_size", std::to_string(a.page_size), std::to_string(b.page_size));
    header("encoding", a.encoding, b.encoding);
    header("user_version", std::to_string(a.user_version), std::to_string(b.user_version));
    header("application_id", std::to_string(a.application_id), std::to_string(b.application_id));
    std::set<ObjKey> keys;
    for (auto* d : {&a, &b})
        for (auto& [k, o] : d->schema)
            keys.insert(k);
    for (auto& k : keys) {
        auto x = a.schema.find(k), y = b.schema.find(k);
        if (x == a.schema.end()) {
            out.emplace_back(make_change(side, Op::insert, {schema_path(k)}, "schema")).after =
                sql_value(y->second);
        } else if (y == b.schema.end()) {
            out.emplace_back(make_change(side, Op::erase, {schema_path(k)}, "schema")).before =
                sql_value(x->second);
        } else if (x->second.norm != y->second.norm) {
            auto& c = out.emplace_back(make_change(side, Op::update, {schema_path(k)}, "schema"));
            c.before = sql_value(x->second), c.after = sql_value(y->second);
        }
    }
    for (auto& [name, ta] : a.tables) {
        auto f = b.tables.find(name);
        if (f == b.tables.end())
            continue;
        const Table& tb = f->second;
        if (compatible(ta, tb)) {
            diff_rows(ta, tb, side, out);
        } else if (ta.type == "virtual" && tb.type == "virtual" && ta.content != tb.content) {
            auto& c = out.emplace_back(make_change(side, Op::update, {table_path(name)}, "virtual"));
            c.before.hash = ta.content, c.after.hash = tb.content;
        }
    }
    return out;
}

// ---- Merge (sqlite.md §6) ----

std::size_t ix(Side s) {
    return static_cast<std::size_t>(s);
}

Side side_of(std::string_view choice) {
    return choice == "theirs" ? Side::theirs : choice == "base" ? Side::base : Side::ours;
}

struct Pending {
    Address at;
    std::string reason;
    std::vector<std::string> choices;
    std::vector<std::string> covers; // change addresses at, above, or below these conflict
};

// A unit whose output is one side's value: `take` (nullopt: absent), or a conflict's resolution.
struct Pick {
    std::optional<Side> take;
    std::optional<std::size_t> conflict;
};

struct RowPick {
    Key key;
    std::array<const Row*, 3> r{}; // base, ours, theirs
    Pick whole;                    // the row from one side (or absent)
    std::vector<Pick> cells;       // set when both sides kept the row and changed it: per column
    std::optional<Key> renumbered; // theirs' added row, inserted under this key (SPEC §2.3)
};

struct TablePick {
    std::string name;
    std::array<const Table*, 3> t{};
    enum class Mode : std::uint8_t { keep, rows, whole } mode = Mode::keep;
    Pick whole;
    std::vector<RowPick> rows;
};

struct Merger {
    std::array<const Doc*, 3> d{};
    std::vector<Pending> pending;
    std::vector<Warning> warnings;
    std::array<Pick, 2> header; // user_version, application_id
    std::map<ObjKey, Pick> objs;
    std::vector<TablePick> tables;
    std::set<std::string> renumbered; // theirs' insert addresses

    std::size_t conflict(std::string path, std::string reason, std::vector<std::string> covers,
                         std::vector<std::string> choices = {"ours", "theirs", "base"}) {
        pending.push_back({{std::move(path)}, std::move(reason), std::move(choices), std::move(covers)});
        return pending.size() - 1;
    }

    template <class V, class Eq>
    Pick leaf(const std::array<const V*, 3>& v, Eq eq, const std::string& path,
              std::vector<std::string> covers, bool unsupported = false) {
        const auto o = merge_leaf(v[0], v[1], v[2], eq, [](const V*, const V&, const V&) { return false; });
        if (o.rule == MergeRule::conflict)
            return {std::nullopt, conflict(path, o.reason, std::move(covers))};
        if (unsupported && o.take == Side::theirs && !eq(*v[1], *v[2]))
            return {Side::ours, conflict(path, "unsupported", std::move(covers), {"ours"})};
        return {o.take, std::nullopt};
    }

    void run() {
        const std::array ps{d[0]->page_size, d[1]->page_size, d[2]->page_size};
        const std::array en{d[0]->encoding, d[1]->encoding, d[2]->encoding};
        const std::array uv{d[0]->user_version, d[1]->user_version, d[2]->user_version};
        const std::array ai{d[0]->application_id, d[1]->application_id, d[2]->application_id};
        std::equal_to<> eq;
        leaf(std::array{&ps[0], &ps[1], &ps[2]}, eq, "/header/page_size", {"/header/page_size"}, true);
        leaf(std::array{&en[0], &en[1], &en[2]}, eq, "/header/encoding", {"/header/encoding"}, true);
        header[0] =
            leaf(std::array{&uv[0], &uv[1], &uv[2]}, eq, "/header/user_version", {"/header/user_version"});
        header[1] = leaf(std::array{&ai[0], &ai[1], &ai[2]}, eq, "/header/application_id",
                         {"/header/application_id"});

        std::set<ObjKey> keys;
        for (auto* x : d)
            for (auto& [k, o] : x->schema)
                if (k.first != "table")
                    keys.insert(k);
        for (auto& k : keys) {
            std::array<const Obj*, 3> v{};
            for (std::size_t s = 0; s < 3; ++s)
                if (auto f = d[s]->schema.find(k); f != d[s]->schema.end())
                    v[s] = &f->second;
            const std::string p = schema_path(k);
            objs[k] = leaf(v, [](const Obj& x, const Obj& y) { return x.norm == y.norm; }, p, {p});
        }

        std::set<std::string> names;
        for (auto* x : d)
            for (auto& [n, t] : x->tables)
                if (t.type != "shadow")
                    names.insert(n);
        for (auto& n : names)
            table(n);
    }

    void table(const std::string& name) {
        TablePick tp;
        tp.name = name;
        for (std::size_t s = 0; s < 3; ++s)
            if (auto f = d[s]->tables.find(name); f != d[s]->tables.end() && f->second.type != "shadow")
                tp.t[s] = &f->second;
        auto eq = [](const Table& x, const Table& y) {
            return x.type == y.type && x.norm == y.norm && x.content == y.content;
        };
        auto rows_ok = [](const Table* x, const Table* y) {
            return x && y && x->norm == y->norm && compatible(*x, *y);
        };
        const auto o =
            merge_leaf(tp.t[0], tp.t[1], tp.t[2], eq,
                       [&](const Table*, const Table& x, const Table& y) { return rows_ok(&x, &y); });
        const std::string p = table_path(name);
        if (o.rule == MergeRule::recursed || (o.rule == MergeRule::theirs && rows_ok(tp.t[1], tp.t[2]))) {
            tp.mode = TablePick::Mode::rows;
            rows(tp);
        } else if (o.rule == MergeRule::theirs) {
            tp.mode = TablePick::Mode::whole, tp.whole.take = o.take;
        } else if (o.rule == MergeRule::conflict) {
            tp.mode = TablePick::Mode::whole;
            tp.whole.conflict = conflict(p, o.reason, {p, "/schema/table:" + pointer_escape(name)});
        }
        tables.push_back(std::move(tp));
    }

    bool may_renumber(const TablePick& tp) const {
        return tp.t[1]->counter && tp.t[2]->counter && std::none_of(d.begin(), d.end(), [&](const Doc* x) {
                   return x->referenced.count(tp.name) > 0;
               });
    }

    void rows(TablePick& tp) {
        const Table* base =
            tp.t[0] && tp.t[0]->norm == tp.t[1]->norm && compatible(*tp.t[0], *tp.t[1]) ? tp.t[0] : nullptr;
        using KV = std::pair<Key, const Row*>;
        auto list = [](const Table* t) {
            std::vector<KV> v;
            if (t)
                for (auto& [k, r] : t->rows)
                    v.emplace_back(k, &r);
            return v;
        };
        const auto lb = list(base), lo = list(tp.t[1]), lt = list(tp.t[2]);
        auto merged = keyed_merge<Key, const Row*>(
            lb, lo, lt, [](const Row* x, const Row* y) { return *x == *y; },
            [](const Key&, const Row* const*, const Row* const&, const Row* const&) { return true; });
        std::int64_t next = 0; // the next free row id, once one is needed
        const Table& ot = *tp.t[1];
        for (auto& m : merged) {
            RowPick rp;
            rp.key = m.key;
            rp.r = {m.base ? lb[*m.base].second : nullptr, m.ours ? lo[*m.ours].second : nullptr,
                    m.theirs ? lt[*m.theirs].second : nullptr};
            const std::string p = row_path(ot, m.key);
            if (m.outcome.rule == MergeRule::conflict) {
                rp.whole.conflict = conflict(p, m.outcome.reason, {p});
            } else if (m.outcome.rule != MergeRule::recursed) {
                rp.whole.take = m.outcome.take;
            } else if (!rp.r[0] && may_renumber(tp)) {
                if (!next)
                    for (auto* t : tp.t)
                        if (t && !t->rows.empty())
                            next = std::max(next, t->rows.rbegin()->first[0].i + 1);
                rp.whole.take = Side::ours;
                rp.renumbered = Key{Cell{SQLITE_INTEGER, next++, {}}};
                renumbered.insert(p);
                warnings.push_back({"renumbered", p,
                                    "theirs' added row is written as " + row_path(ot, *rp.renumbered) +
                                        " (ours added the same id)"});
            } else {
                for (std::size_t k = 0; k < ot.cols.size(); ++k) {
                    std::array<const Cell*, 3> v{rp.r[0] ? &(*rp.r[0])[k] : nullptr, &(*rp.r[1])[k],
                                                 &(*rp.r[2])[k]};
                    const std::string cp = p + "/" + pointer_escape(ot.cols[k]);
                    const auto o = merge_leaf(v[0], v[1], v[2], std::equal_to<>{},
                                              [](const Cell*, const Cell&, const Cell&) { return false; });
                    if (o.rule == MergeRule::conflict)
                        rp.cells.push_back(
                            {std::nullopt,
                             conflict(cp, o.reason, {cp},
                                      rp.r[0] ? std::vector<std::string>{"ours", "theirs", "base"}
                                              : std::vector<std::string>{"ours", "theirs"})});
                    else
                        rp.cells.push_back({o.take, std::nullopt});
                }
            }
            tp.rows.push_back(std::move(rp));
        }
    }
};

// ---- Writer (sqlite.md §7): apply the merge to a copy of ours through SQLite ----

struct RowOp {
    enum class Kind : std::uint8_t { insert, update, erase } kind;
    Key key;
    Row values;                   // insert: every column; update: the new values of `set`
    std::vector<std::size_t> set; // update: changed columns
    std::string at;
};

struct Writer {
    const Merger& m;
    const std::vector<Conflict>& conflicts;
    std::vector<Conflict> extra; // constraint failures
    std::vector<Warning> warnings;

    std::optional<Side> pick(const Pick& p) const {
        if (!p.conflict)
            return p.take;
        return side_of(conflicts[*p.conflict].resolved_as);
    }

    // The resolved output row (nullopt: absent).
    std::optional<Row> final_row(const RowPick& rp) const {
        if (!rp.cells.empty()) {
            Row r = *rp.r[1];
            for (std::size_t k = 0; k < r.size(); ++k) {
                const auto s = pick(rp.cells[k]).value_or(Side::ours);
                if (rp.r[ix(s)])
                    r[k] = (*rp.r[ix(s)])[k];
            }
            return r;
        }
        const auto s = pick(rp.whole);
        if (!s || !rp.r[ix(*s)])
            return std::nullopt;
        return *rp.r[ix(*s)];
    }

    std::vector<RowOp> row_ops(const TablePick& tp) const {
        std::vector<RowOp> ops;
        const Table& t = *tp.t[1];
        for (auto& rp : tp.rows) {
            const auto f = final_row(rp);
            const std::string at = row_path(t, rp.key);
            if (!rp.r[1] && f) {
                ops.push_back({RowOp::Kind::insert, rp.key, *f, {}, at});
            } else if (rp.r[1] && !f) {
                ops.push_back({RowOp::Kind::erase, rp.key, {}, {}, at});
            } else if (rp.r[1] && f && *f != *rp.r[1]) {
                RowOp op{RowOp::Kind::update, rp.key, {}, {}, at};
                for (std::size_t k = 0; k < f->size(); ++k)
                    if ((*f)[k] != (*rp.r[1])[k])
                        op.set.push_back(k), op.values.push_back((*f)[k]);
                ops.push_back(std::move(op));
            }
            if (rp.renumbered) {
                Row r = *rp.r[2];
                if (!t.by_rowid())
                    r[t.pk[0]] = (*rp.renumbered)[0];
                ops.push_back(
                    {RowOp::Kind::insert, *rp.renumbered, std::move(r), {}, row_path(t, *rp.renumbered)});
            }
        }
        return ops;
    }

    static std::string where(const Table& t) {
        if (t.by_rowid())
            return " WHERE " + t.rid + " = ?";
        std::string w;
        for (auto k : t.pk)
            w += (w.empty() ? " WHERE " : " AND ") + quote_id(t.cols[k]) + " = ?";
        return w;
    }

    static std::string insert_sql(const Table& t) {
        std::string c, v;
        if (t.by_rowid() && !t.rid.empty())
            c = t.rid, v = "?";
        for (auto& n : t.cols)
            c += (c.empty() ? "" : ", ") + quote_id(n), v += v.empty() ? "?" : ", ?";
        return "INSERT INTO " + quote_id(t.name) + "(" + c + ") VALUES(" + v + ")";
    }

    // Inserts every row of `t` (a whole table taken from one side).
    static void fill(Conn& c, const Table& t) {
        Stmt s(c.get(), insert_sql(t));
        for (auto& [k, r] : t.rows) {
            int n = 1;
            if (t.by_rowid() && !t.rid.empty())
                s.bind(n++, k[0]);
            for (auto& x : r)
                s.bind(n++, x);
            s.step();
            s.reset();
        }
    }

    void apply_rows(Conn& c, const Table& t, const std::vector<RowOp>& ops) {
        for (auto& op : ops) {
            std::string sql;
            if (op.kind == RowOp::Kind::insert) {
                sql = insert_sql(t);
            } else if (op.kind == RowOp::Kind::erase) {
                sql = "DELETE FROM " + quote_id(t.name) + where(t);
            } else {
                sql = "UPDATE " + quote_id(t.name) + " SET ";
                for (std::size_t j = 0; j < op.set.size(); ++j)
                    sql += (j ? ", " : "") + quote_id(t.cols[op.set[j]]) + " = ?";
                sql += where(t);
            }
            Stmt s(c.get(), sql);
            int n = 1;
            if (op.kind == RowOp::Kind::insert) {
                if (t.by_rowid())
                    s.bind(n++, op.key[0]);
                for (auto& x : op.values)
                    s.bind(n++, x);
            } else {
                for (auto& x : op.values)
                    s.bind(n++, x);
                for (auto& x : op.key)
                    s.bind(n++, x);
            }
            // A violated UNIQUE, CHECK, or NOT NULL constraint keeps ours' row (sqlite.md §6).
            if ((s.run() & 0xFF) == SQLITE_CONSTRAINT) {
                auto cf = make_conflict({op.at}, {}, "constraint");
                cf.choices = {"ours"};
                extra.push_back(std::move(cf));
                warnings.push_back({"constraint", op.at, sqlite3_errmsg(c.get())});
            }
        }
    }

    // Replaces the table `name` with side `t` (nullptr: drops it).
    void whole(Conn& c, const std::string& name, const Table* ours, const Doc* side, const Table* t) {
        const bool keep_vt =
            ours && t && t->type == "virtual" && ours->type == "virtual" && ours->norm == t->norm;
        if (ours && !keep_vt)
            c.exec("DROP TABLE " + quote_id(name));
        if (!t)
            return;
        if (!keep_vt)
            c.exec(t->sql);
        if (t->type != "virtual") {
            fill(c, *t);
            return;
        }
        // A virtual table's index is its shadow tables: take theirs whole, never row by row (sqlite.md §3).
        for (auto& sh : t->shadows) {
            c.exec("DELETE FROM " + quote_id(sh));
            fill(c, side->tables.at(sh));
        }
    }

    std::vector<std::byte> write() {
        const Doc& od = *m.d[1];
        // What changes relative to ours.
        std::array<std::optional<std::int64_t>, 2> hdr;
        const std::array<std::int64_t, 3> uv{m.d[0]->user_version, od.user_version, m.d[2]->user_version};
        const std::array<std::int64_t, 3> ai{m.d[0]->application_id, od.application_id,
                                             m.d[2]->application_id};
        for (std::size_t j = 0; j < 2; ++j) {
            const auto& v = j ? ai : uv;
            const auto s = pick(m.header[j]).value_or(Side::ours);
            if (v[ix(s)] != v[1])
                hdr[j] = v[ix(s)];
        }
        std::map<ObjKey, const Obj*> out_objs; // index, view, trigger in the output
        std::vector<ObjKey> drops;
        for (auto& [k, p] : m.objs) {
            const auto s = pick(p);
            const Obj* o = nullptr;
            if (s)
                if (auto f = m.d[ix(*s)]->schema.find(k); f != m.d[ix(*s)]->schema.end())
                    o = &f->second;
            const auto mine = od.schema.find(k);
            if (o)
                out_objs[k] = o;
            if (mine != od.schema.end() && (!o || o->norm != mine->second.norm))
                drops.push_back(k);
        }
        struct Act {
            const TablePick* tp;
            std::vector<RowOp> ops;
            const Doc* side = nullptr;
            const Table* t = nullptr;
            bool whole = false;
        };
        std::vector<Act> acts;
        for (auto& tp : m.tables) {
            if (tp.mode == TablePick::Mode::rows) {
                if (auto ops = row_ops(tp); !ops.empty())
                    acts.push_back({&tp, std::move(ops)});
            } else if (tp.mode == TablePick::Mode::whole) {
                const auto s = pick(tp.whole);
                const Table* t = s ? tp.t[ix(*s)] : nullptr;
                if (s != Side::ours && (t || tp.t[1]))
                    acts.push_back({&tp, {}, s ? m.d[ix(*s)] : nullptr, t, true});
            }
        }
        const bool dml = !acts.empty();
        if (!dml && drops.empty() && !hdr[0] && !hdr[1] &&
            std::all_of(out_objs.begin(), out_objs.end(),
                        [&](auto& kv) { return od.schema.count(kv.first) > 0; })) {
            auto b = od.src.bytes();
            return {b.begin(), b.end()};
        }

        Conn c(od.src.bytes(), true);
        c.exec("PRAGMA foreign_keys = OFF");
        c.exec("BEGIN IMMEDIATE");
        // Triggers must not fire on the merged rows (Q-SQLITE-5): drop them all, recreate the output's.
        for (auto& [k, o] : od.schema)
            if (k.first == "trigger" && (dml || std::find(drops.begin(), drops.end(), k) != drops.end()))
                c.exec("DROP TRIGGER " + quote_id(k.second));
        for (auto& k : drops)
            if (k.first != "trigger")
                c.exec("DROP " + std::string(k.first == "index" ? "INDEX " : "VIEW ") + quote_id(k.second));
        for (auto& a : acts) {
            const Table* ours = a.tp->t[1];
            if (a.whole)
                whole(c, a.tp->name, ours, a.side, a.t);
            else
                apply_rows(c, *ours, a.ops);
        }
        // Output objects missing from the database, in creation order: indexes, views, triggers.
        for (const char* type : {"index", "view", "trigger"}) {
            std::vector<const Obj*> todo;
            for (auto& [k, o] : out_objs)
                if (k.first == type && !c.exists(type, k.second))
                    todo.push_back(o);
            std::sort(todo.begin(), todo.end(),
                      [](const Obj* x, const Obj* y) { return x->order < y->order; });
            for (auto* o : todo) {
                if (std::string_view(type) != "view" && !c.exists("", o->tbl)) {
                    warnings.push_back({"dependency-dropped", schema_path({o->type, o->name}),
                                        "not created: its table " + o->tbl + " is not in the result"});
                    continue;
                }
                c.exec(o->sql);
            }
        }
        if (hdr[0])
            c.exec("PRAGMA user_version = " + std::to_string(*hdr[0]));
        if (hdr[1])
            c.exec("PRAGMA application_id = " + std::to_string(*hdr[1]));
        // sqlite_sequence is derived: max(ours, theirs, max(rowid) in the result) (sqlite.md §3).
        if (c.exists("table", "sqlite_sequence"))
            for (auto& [name, v] : m.d[2]->seq) {
                if (!c.exists("table", name))
                    continue;
                Stmt u(c.get(), "UPDATE sqlite_sequence SET seq = ?1 WHERE name = ?2 AND seq < ?1");
                u.bind(1, v).bind(2, name).step();
                Stmt i(c.get(), "INSERT INTO sqlite_sequence(name, seq) SELECT ?2, ?1 WHERE NOT EXISTS "
                                "(SELECT 1 FROM sqlite_sequence WHERE name = ?2)");
                i.bind(1, v).bind(2, name).step();
            }
        c.exec("COMMIT");
        if (dml && od.stat1)
            warnings.push_back(
                {"derived-stale", "/tables/sqlite_stat1", "kept ours' statistics; run ANALYZE"});
        if (const auto ic = c.one("PRAGMA integrity_check"); ic != "ok")
            throw Error(ErrorCategory::internal, "sqlite: merged database fails integrity_check: " + ic);
        {
            Stmt fk(c.get(), "PRAGMA foreign_key_check");
            while (fk.step())
                warnings.push_back({"foreign-key", table_path(fk.str(0)),
                                    "row " + fk.str(1) + " references a missing row in " + fk.str(2)});
        }
        auto out = c.serialize();
        const auto src = od.src.bytes();
        out[18] = src[18], out[19] = src[19];
        // Keep cksumvfs checksums valid on every page that changed (Q-SQLITE-7).
        if (od.cksum && out.size() % od.page_size == 0)
            for (std::size_t p = 0; p < out.size(); p += od.page_size) {
                std::span<std::byte> page(out.data() + p, od.page_size);
                if (p + od.page_size > src.size() || !std::ranges::equal(page, src.subspan(p, od.page_size)))
                    set_cksum(page);
            }
        return out;
    }
};

// ---- Format ----

bool companion(std::span<const std::byte> head, std::string_view name) {
    static const unsigned char journal[8] = {0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    const bool wal = head.size() >= 4 && (be(head, 0, 4) == 0x377f0682 || be(head, 0, 4) == 0x377f0683);
    const bool jr = head.size() >= 8 && std::memcmp(head.data(), journal, 8) == 0;
    return (wal && name.ends_with("-wal")) || (jr && name.ends_with("-journal")) || name.ends_with("-shm");
}

class SqliteFormat final : public Format {
public:
    const FormatInfo& info() const override {
        static const FormatInfo i{"sqlite",
                                  "SQLite databases (rows by primary key, cells merged)",
                                  {".db", ".sqlite", ".sqlite3", ".db3", ".gpkg", ".mbtiles"},
                                  {true, true, false, true, false},
                                  {}};
        return i;
    }

    // The 16-byte magic (100); WAL, rollback-journal, and -shm companions (90) so load() refuses them.
    Probe probe(std::span<const std::byte> head, std::string_view name) const override {
        if (head.size() >= kMagic.size() && std::memcmp(head.data(), kMagic.data(), kMagic.size()) == 0)
            return {100};
        return {companion(head, name) ? 90 : 0};
    }

    std::unique_ptr<Document> load(ByteSource src, const LoadOptions& o) const override {
        auto head = src.bytes().first(std::min<std::size_t>(src.size(), 16));
        if (companion(head, src.name()))
            throw Error(ErrorCategory::invalid_input,
                        "sqlite: " + src.name() +
                            " is a WAL, journal, or shared-memory file, not a database");
        // Committed pages may still sit in the WAL next to the input (sqlite.md §10).
        if (!src.name().empty()) {
            std::error_code ec;
            const std::string n = src.name() + "-wal"; // ByteSource names are UTF-8
            const std::filesystem::path wal(std::u8string(n.begin(), n.end()));
            if (std::filesystem::is_regular_file(wal, ec) && std::filesystem::file_size(wal, ec) > 0)
                throw Error(ErrorCategory::invalid_input,
                            "sqlite: " + n + " exists; checkpoint the database first");
        }
        return parse(std::move(src), &o.ctx.budget);
    }

    ChangeSet diff(const Document& a, const Document& b, const DiffOptions&) const override {
        auto cs = diff_docs(cast(a), cast(b), Side::theirs);
        sort_canonical(cs);
        return cs;
    }

    MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                      const MergeOptions& mo) const override {
        Merger m;
        m.d = {&cast(base), &cast(ours), &cast(theirs)};
        m.run();
        MergeResult res;
        auto& co = res.changes_ours;
        auto& ct = res.changes_theirs;
        co = diff_docs(*m.d[0], *m.d[1], Side::ours);
        ct = diff_docs(*m.d[0], *m.d[2], Side::theirs);
        std::set<std::string> in_conflict;
        auto covered = [](const Change& c, const Pending& p) {
            return std::any_of(p.covers.begin(), p.covers.end(), [&](const std::string& a) {
                return path_within(c.at.path, a) || path_within(a, c.at.path);
            });
        };
        for (auto& p : m.pending) {
            std::vector<std::string> ids;
            for (auto* cs : {&co, &ct})
                for (auto& c : *cs)
                    if (covered(c, p))
                        ids.push_back(c.id), in_conflict.insert(c.id);
            auto c = make_conflict(p.at, std::move(ids), p.reason);
            c.choices = p.choices;
            res.conflicts.push_back(std::move(c));
        }
        apply_conflict_policy(res.conflicts, mo.conflict, res.warnings);
        for (auto& c : res.conflicts) // a policy naming a side this conflict cannot write keeps ours
            if (std::find(c.choices.begin(), c.choices.end(), c.resolved_as) == c.choices.end())
                c.resolved_as = "ours", c.resolved = false;
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
                    res.auto_merged.push_back({c.id, cs == &ct && m.renumbered.count(c.at.path) ? "renumbered"
                                                     : other->count(sig(c))                     ? "identical"
                                                                            : "independent"});
        res.warnings.insert(res.warnings.end(), m.warnings.begin(), m.warnings.end());

        // An unchanged side yields the other side's file as is.
        auto bytes = [&](std::size_t k) { return m.d[k]->src.bytes(); };
        std::vector<std::byte> out;
        if (std::ranges::equal(bytes(0), bytes(1))) {
            out.assign(bytes(2).begin(), bytes(2).end());
        } else if (std::ranges::equal(bytes(0), bytes(2)) || std::ranges::equal(bytes(1), bytes(2))) {
            out.assign(bytes(1).begin(), bytes(1).end());
        } else {
            Writer w{m, res.conflicts, {}, {}};
            out = w.write();
            res.conflicts.insert(res.conflicts.end(), w.extra.begin(), w.extra.end());
            res.warnings.insert(res.warnings.end(), w.warnings.begin(), w.warnings.end());
        }
        try {
            res.document = parse(ByteSource::from_memory(std::move(out), "merged.sqlite"), nullptr);
        } catch (const Error& e) {
            throw Error(ErrorCategory::internal,
                        std::string("SQLite writer produced an unreadable file: ") + e.what());
        }
        sort_canonical(co), sort_canonical(ct);
        return res;
    }

    void save(const Document& d, ByteSink& sink, const SaveOptions&) const override {
        sink.write(cast(d).src.bytes());
    }

    // sqlite.md §5: header, schema, then per table its identity and one line per row, sorted by key.
    void dump(const Document& doc, std::ostream& os, const DumpOptions&) const override {
        const Doc& d = cast(doc);
        os << "sqlite 3 page_size=" << d.page_size << " encoding=" << d.encoding
           << " user_version=" << d.user_version << " application_id=" << d.application_id << '\n';
        for (const char* type : {"table", "index", "view", "trigger"})
            for (auto& [k, o] : d.schema)
                if (k.first == type)
                    os << "schema " << type << ' ' << o.name << ' ' << one_line(o.sql) << '\n';
        for (auto& [name, t] : d.tables) {
            if (t.type == "shadow")
                continue;
            if (t.type == "virtual") {
                os << "table " << name << " virtual content=xxh3:" << t.content.hex() << '\n';
                continue;
            }
            os << "table " << name << " pk=(";
            const auto names = t.by_rowid() ? std::vector<std::string>{"rowid"} : t.key_names();
            for (std::size_t j = 0; j < names.size(); ++j)
                os << (j ? "," : "") << names[j];
            os << ") rows=" << t.rows.size() << '\n';
            for (auto& [k, r] : t.rows) {
                nlohmann::ordered_json key = nlohmann::ordered_json::array();
                for (auto& x : k)
                    key.push_back(js(x));
                os << "row " << name << ' ' << text(key) << ' ' << text(row_json(t, r, !t.by_rowid()))
                   << '\n';
            }
        }
    }

    const EnvelopeMap* envelope(const Document& d) const override { return &cast(d).map; }

    nlohmann::ordered_json inspect(const Document& doc) const override {
        const Doc& d = cast(doc);
        nlohmann::ordered_json j;
        j["sqlite_version"] = d.version;
        j["page_size"] = d.page_size;
        j["encoding"] = d.encoding;
        j["journal"] = d.wal ? "wal" : "rollback";
        j["user_version"] = d.user_version;
        j["application_id"] = d.application_id;
        j["checksums"] = d.cksum ? "cksumvfs" : "none";
        if (d.bad.empty()) {
            j["integrity"] = "ok";
        } else {
            nlohmann::ordered_json at = nlohmann::ordered_json::array();
            for (auto p : d.bad)
                at.push_back("/pages/" + std::to_string(p));
            j["integrity"] = {{"status", "mismatch"}, {"at", at}};
        }
        auto& tables = j["tables"] = nlohmann::ordered_json::array();
        for (auto& [name, t] : d.tables) {
            if (t.type == "shadow")
                continue;
            nlohmann::ordered_json e{{"name", name}};
            if (t.type == "virtual")
                e["identity"] = "virtual";
            else
                e["identity"] = t.by_rowid() ? "rowid" : "pk", e["key"] = t.key_names(),
                e["rows"] = t.rows.size();
            tables.push_back(std::move(e));
        }
        auto& warn = j["warnings"] = nlohmann::ordered_json::array();
        for (auto& w : d.warnings)
            warn.push_back({{"code", w.code}, {"at", w.at}, {"message", w.message}});
        return j;
    }

    // stats.format: per table rows_inserted, rows_deleted, rows_updated, cells_changed, identity; totals.
    nlohmann::ordered_json stats(const Document&, const Document& b, const ChangeSet& cs) const override {
        nlohmann::ordered_json o;
        o["sqlite_version"] = cast(b).version;
        auto& tables = o["tables"] = nlohmann::ordered_json::object();
        std::map<std::string, std::set<std::string>> updated;
        std::array<std::uint64_t, 4> total{};
        for (auto& c : cs) {
            if (!c.at.path.starts_with("/tables/") || (c.kind != "row" && c.kind != "cell"))
                continue;
            const auto rest = c.at.path.substr(8);
            const std::string name = rest.substr(0, rest.find('/'));
            auto& t = tables[name];
            if (t.is_null())
                t = {{"rows_inserted", 0}, {"rows_deleted", 0}, {"rows_updated", 0}, {"cells_changed", 0}};
            if (c.kind == "row") {
                const char* f = c.op == Op::insert ? "rows_inserted" : "rows_deleted";
                t[f] = t[f].get<std::uint64_t>() + 1, ++total[c.op == Op::insert ? 0 : 1];
            } else {
                t["cells_changed"] = t["cells_changed"].get<std::uint64_t>() + 1, ++total[3];
                if (updated[name].insert(c.at.path.substr(0, c.at.path.rfind('/'))).second)
                    t["rows_updated"] = t["rows_updated"].get<std::uint64_t>() + 1, ++total[2];
            }
        }
        for (auto& [name, t] : cast(b).tables)
            if (tables.contains(name))
                tables[name]["identity"] = t.type == "virtual" ? "virtual" : t.by_rowid() ? "rowid" : "pk";
        o["rows_inserted"] = total[0], o["rows_deleted"] = total[1], o["rows_updated"] = total[2],
        o["cells_changed"] = total[3];
        return o;
    }
};

} // namespace

std::unique_ptr<Format> make_format() {
    return std::make_unique<SqliteFormat>();
}

} // namespace semsplinter::sqlite
