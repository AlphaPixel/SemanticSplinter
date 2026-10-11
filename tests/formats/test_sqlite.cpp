#include <semsplinter/format.hpp>

#include "../scratch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

extern "C" int sqlite3_register_cksumvfs(const char*); // the vendored cksumvfs.c
extern "C" int sqlite3_unregister_cksumvfs(void);

using namespace semsplinter;
using Bytes = std::vector<std::byte>;

namespace {

void exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "?";
        sqlite3_free(err);
        FAIL(sql << ": " << msg);
    }
}

Bytes serialize(sqlite3* db) {
    sqlite3_int64 n = 0;
    auto* p = sqlite3_serialize(db, "main", &n, 0);
    REQUIRE(p);
    Bytes b(reinterpret_cast<std::byte*>(p), reinterpret_cast<std::byte*>(p) + n);
    sqlite3_free(p);
    return b;
}

// A database image built through the SQLite API.
Bytes make_db(const std::string& sql) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    exec(db, sql);
    auto b = serialize(db);
    sqlite3_close(db);
    return b;
}

// An in-memory connection on a copy of `image` (in rollback mode: memdb has no WAL).
sqlite3* open_image(const Bytes& image) {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    auto* buf = static_cast<unsigned char*>(sqlite3_malloc64(image.size()));
    std::memcpy(buf, image.data(), image.size());
    if (image.size() > 19 && buf[18] == 2)
        buf[18] = buf[19] = 1;
    REQUIRE(sqlite3_deserialize(db, "main", buf, static_cast<sqlite3_int64>(image.size()),
                                static_cast<sqlite3_int64>(image.size()),
                                SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE) == SQLITE_OK);
    return db;
}

// `sql` run on a copy of `image`, as an application editing a branch's database would.
Bytes edit(const Bytes& image, const std::string& sql) {
    sqlite3* db = open_image(image);
    exec(db, sql);
    auto b = serialize(db);
    sqlite3_close(db);
    b[18] = image[18], b[19] = image[19];
    return b;
}

// Rows of a query on an image, one "a|b|c" line each.
std::string query(const Bytes& image, const std::string& sql) {
    sqlite3* db = open_image(image);
    sqlite3_stmt* s = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK);
    std::string out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        for (int k = 0; k < sqlite3_column_count(s); ++k) {
            auto* t = sqlite3_column_text(s, k);
            out += (k ? "|" : "") + std::string(t ? reinterpret_cast<const char*>(t) : "NULL");
        }
        out += '\n';
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return out;
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& f = *reg.find("sqlite");

    std::unique_ptr<Document> load(const Bytes& b, std::string name = "x.db") {
        return f.load(ByteSource::from_memory(b, std::move(name)), {ctx});
    }
    ChangeSet diff(const Bytes& a, const Bytes& b) { return f.diff(*load(a), *load(b), {ctx}); }
    MergeResult merge(const Bytes& b, const Bytes& o, const Bytes& t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return f.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    Bytes bytes(const Document& d) {
        VectorSink s;
        f.save(d, s, {ctx});
        return s.data;
    }
    Bytes merged(const Bytes& b, const Bytes& o, const Bytes& t) {
        auto r = merge(b, o, t);
        CHECK(r.conflicts.empty());
        return bytes(*r.document);
    }
    std::string dump(const Bytes& b) {
        std::ostringstream os;
        f.dump(*load(b), os, {ctx});
        return os.str();
    }
};

const Change* at(const ChangeSet& cs, std::string_view path) {
    for (auto& c : cs)
        if (c.at.to_string() == path)
            return &c;
    return nullptr;
}

bool has_warning(const MergeResult& r, std::string_view code) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [&](const Warning& w) { return w.code == code; });
}

const std::string kItems = "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, price REAL);"
                           "INSERT INTO items VALUES(1, 'Sword', 12.5), (2, 'Shield', 8.0), (3, 'Bow', 7);";

} // namespace

TEST_CASE("sqlite: load, dump, and an unchanged merge") {
    Fx fx;
    const auto db = make_db(kItems + "CREATE INDEX items_by_name ON items(name); PRAGMA user_version = 7;");
    CHECK(fx.reg.detect(db, "noext").info().name == "sqlite");
    CHECK(fx.bytes(*fx.load(db)) == db);
    CHECK(fx.diff(db, db).empty());
    CHECK(fx.merged(db, db, db) == db);
    CHECK(fx.dump(db) ==
          "sqlite 3 page_size=4096 encoding=UTF-8 user_version=7 application_id=0\n"
          "schema table items CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, price REAL)\n"
          "schema index items_by_name CREATE INDEX items_by_name ON items(name)\n"
          "table items pk=(id) rows=3\n"
          "row items [1] {\"name\":\"Sword\",\"price\":12.5}\n"
          "row items [2] {\"name\":\"Shield\",\"price\":8.0}\n"
          "row items [3] {\"name\":\"Bow\",\"price\":7.0}\n");
    CHECK_THROWS_AS(fx.load(Bytes(db.begin(), db.begin() + 50)), Error);
    const Bytes junk(200, std::byte{'x'});
    CHECK_THROWS_AS(fx.load(junk), Error);
}

TEST_CASE("sqlite: diff of header, schema, rows, and cells") {
    Fx fx;
    const auto a = make_db(kItems);
    const auto b = edit(a, "UPDATE items SET price = 9 WHERE id = 2; DELETE FROM items WHERE id = 3;"
                           "INSERT INTO items VALUES(4, 'Axe', NULL); CREATE INDEX ix ON items(price);"
                           "PRAGMA user_version = 3;");
    const auto cs = fx.diff(a, b);
    REQUIRE(cs.size() == 5);
    CHECK(at(cs, "/header/user_version")->after.text == "3");
    CHECK(at(cs, "/schema/index:ix")->op == Op::insert);
    auto* cell = at(cs, "/tables/items/rows/pk:2/price");
    REQUIRE(cell);
    CHECK(cell->before.text == "8.0");
    CHECK(cell->after.text == "9.0"); // REAL affinity
    CHECK(at(cs, "/tables/items/rows/pk:3")->op == Op::erase);
    CHECK(at(cs, "/tables/items/rows/pk:4")->after.text == R"({"id":4,"name":"Axe","price":null})");
    CHECK(at(cs, "/tables/items/rows/pk:4")->depends_on == std::vector<std::string>{"/schema/table:items"});

    // Spelling of the same schema is not a change (sqlite.md §4).
    const auto x = make_db("CREATE TABLE t(a INTEGER, b TEXT)");
    const auto y = make_db("create   table \"t\" (\n  [a] integer, -- note\n  `b` text )");
    CHECK(fx.diff(x, y).empty());
}

TEST_CASE("sqlite: row identity by composite key, without rowid, and rowid") {
    Fx fx;
    const auto a = make_db("CREATE TABLE loc(lang TEXT, k TEXT, v TEXT, PRIMARY KEY(lang, k)) WITHOUT ROWID;"
                           "INSERT INTO loc VALUES('en', 'menu,start', 'Start'), ('en', 'a/b~c', 'X');"
                           "CREATE TABLE log(msg TEXT); INSERT INTO log VALUES('one'), ('two');");
    const auto b =
        edit(a, "UPDATE loc SET v = 'Go' WHERE k = 'menu,start'; UPDATE loc SET v = 'Y' WHERE lang = 'en' "
                "AND k = 'a/b~c'; UPDATE log SET msg = 'TWO' WHERE rowid = 2;");
    const auto cs = fx.diff(a, b);
    CHECK(at(cs, "/tables/loc/rows/pk:en,menu~3start/v"));
    CHECK(at(cs, "/tables/loc/rows/pk:en,a~1b~0c/v"));
    CHECK(at(cs, "/tables/log/rows/rowid:2/msg"));
    auto in = fx.f.inspect(*fx.load(a));
    CHECK(in["warnings"].size() == 1);
    CHECK(in["warnings"][0]["code"] == "identity-rowid");
    CHECK(in["warnings"][0]["at"] == "/tables/log");
}

TEST_CASE("sqlite: three-way merge of rows and cells") {
    Fx fx;
    const auto base = make_db(kItems + "CREATE TABLE other(k TEXT PRIMARY KEY, v);");
    // Disjoint edits, including two columns of one row.
    const auto ours =
        edit(base, "UPDATE items SET price = 13 WHERE id = 1; INSERT INTO other VALUES('a', 1);");
    const auto theirs =
        edit(base, "UPDATE items SET name = 'Blade' WHERE id = 1; DELETE FROM items WHERE id = 3;"
                   "INSERT INTO items VALUES(10, 'Axe', 4.5);");
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    const auto out = fx.bytes(*r.document);
    CHECK(query(out, "SELECT * FROM items ORDER BY id") == "1|Blade|13.0\n2|Shield|8.0\n10|Axe|4.5\n");
    CHECK(query(out, "SELECT * FROM other") == "a|1\n");
    CHECK(query(out, "PRAGMA integrity_check") == "ok\n");

    // One side unchanged: the other side's bytes as they are.
    CHECK(fx.merged(base, base, theirs) == theirs);
    CHECK(fx.merged(base, ours, base) == ours);
    // Both made the same change: nothing to write, ours' bytes.
    CHECK(fx.merged(base, ours, edit(base, "INSERT INTO other VALUES('a', 1);")) == ours);

    // Same cell: both-modified; the policy side is written, and --resolve picks the other.
    const auto t2 = edit(base, "UPDATE items SET price = 99 WHERE id = 1;");
    r = fx.merge(base, ours, t2);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].at.path == "/tables/items/rows/pk:1/price");
    CHECK(r.conflicts[0].changes.size() == 2);
    CHECK(query(fx.bytes(*r.document), "SELECT price FROM items WHERE id = 1") == "13.0\n");
    ConflictOptions co;
    co.resolve_all = ResolveAll::theirs;
    r = fx.merge(base, ours, t2, co);
    CHECK(query(fx.bytes(*r.document), "SELECT price FROM items WHERE id = 1") == "99.0\n");

    // Row deleted against row edited: modify-delete.
    r = fx.merge(base, ours, edit(base, "DELETE FROM items WHERE id = 1;"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "modify-delete");
    CHECK(query(fx.bytes(*r.document), "SELECT count(*) FROM items WHERE id = 1") == "1\n");
}

TEST_CASE("sqlite: add-add of counter ids is renumbered (SPEC 2.3)") {
    Fx fx;
    const auto base = make_db(kItems);
    const auto ours = edit(base, "INSERT INTO items(name) VALUES('Mace');");    // id 4
    const auto theirs = edit(base, "INSERT INTO items(name) VALUES('Spear');"); // id 4 too
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    CHECK(has_warning(r, "renumbered"));
    CHECK(std::any_of(r.auto_merged.begin(), r.auto_merged.end(),
                      [](const AutoMerge& a) { return a.rule == "renumbered"; }));
    CHECK(query(fx.bytes(*r.document), "SELECT id, name FROM items WHERE id > 3") == "4|Mace\n5|Spear\n");
    // Identical additions merge as one row.
    CHECK(query(fx.merged(base, ours, edit(base, "INSERT INTO items(name) VALUES('Mace');")),
                "SELECT count(*) FROM items") == "4\n");

    // A foreign key points at the table: no renumbering, the differing cells conflict.
    const auto fb = make_db(kItems + "CREATE TABLE inv(item INTEGER REFERENCES items(id));");
    r = fx.merge(fb, edit(fb, "INSERT INTO items(name) VALUES('Mace');"),
                 edit(fb, "INSERT INTO items(name) VALUES('Spear');"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "add-add");
    CHECK(r.conflicts[0].at.path == "/tables/items/rows/pk:4/name");
    CHECK(r.conflicts[0].choices == std::vector<std::string>{"ours", "theirs"});

    // Text keys are not counters: add-add conflicts per differing column.
    const auto tb = make_db("CREATE TABLE t(k TEXT PRIMARY KEY, a, b);");
    r = fx.merge(tb, edit(tb, "INSERT INTO t VALUES('x', 1, 2);"),
                 edit(tb, "INSERT INTO t VALUES('x', 1, 3);"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/tables/t/rows/pk:x/b");
}

TEST_CASE("sqlite: schema merges") {
    Fx fx;
    const auto base = make_db(kItems + "CREATE TABLE gone(x); INSERT INTO gone VALUES(1);"
                                       "CREATE VIEW cheap AS SELECT * FROM items WHERE price < 10;");
    const auto ours = edit(base, "UPDATE items SET price = 1 WHERE id = 1;");
    const auto theirs =
        edit(base, "CREATE TABLE added(a INTEGER PRIMARY KEY, b); INSERT INTO added VALUES(1, 'x');"
                   "CREATE INDEX added_b ON added(b); DROP TABLE gone; DROP VIEW cheap;"
                   "CREATE VIEW cheap AS SELECT * FROM items WHERE price < 5; PRAGMA user_version = 2;");
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    const auto out = fx.bytes(*r.document);
    CHECK(query(out, "SELECT * FROM added") == "1|x\n");
    CHECK(query(out, "SELECT name FROM sqlite_schema WHERE type = 'index'") == "added_b\n");
    CHECK(query(out, "SELECT count(*) FROM sqlite_schema WHERE name = 'gone'") == "0\n");
    CHECK(query(out, "SELECT sql FROM sqlite_schema WHERE name = 'cheap'") ==
          "CREATE VIEW cheap AS SELECT * FROM items WHERE price < 5\n");
    CHECK(query(out, "PRAGMA user_version") == "2\n");
    CHECK(query(out, "SELECT price FROM items WHERE id = 1") == "1.0\n");

    // Table dropped against rows edited: modify-delete on the table.
    r = fx.merge(base, edit(base, "UPDATE gone SET x = 2;"), edit(base, "DROP TABLE gone;"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/tables/gone");
    CHECK(r.conflicts[0].reason == "modify-delete");
    // A table whose schema only theirs changed is taken whole from theirs.
    const auto t3 = edit(base, "ALTER TABLE gone ADD COLUMN y DEFAULT 5;");
    CHECK(query(fx.merged(base, ours, t3), "SELECT * FROM gone") == "1|5\n");
}

TEST_CASE("sqlite: triggers do not fire while merging") {
    Fx fx;
    const auto base = make_db(kItems + "CREATE TABLE log(msg TEXT);"
                                       "CREATE TRIGGER items_ins AFTER INSERT ON items BEGIN "
                                       "INSERT INTO log VALUES('added ' || new.name); END;");
    const auto ours = edit(base, "UPDATE items SET price = 1 WHERE id = 1;");
    const auto theirs = edit(base, "INSERT INTO items VALUES(7, 'Club', 2);");
    const auto out = fx.merged(base, ours, theirs);
    CHECK(query(out, "SELECT msg FROM log") == "added Club\n"); // theirs' log row, not a second one
    CHECK(query(out, "SELECT name FROM sqlite_schema WHERE type = 'trigger'") == "items_ins\n");
}

TEST_CASE("sqlite: untouched pages keep ours' bytes") {
    Fx fx;
    std::string sql =
        "CREATE TABLE big(id INTEGER PRIMARY KEY, t TEXT); CREATE TABLE small(id INTEGER PRIMARY KEY, v);"
        "INSERT INTO small VALUES(1, 'a'), (2, 'b');";
    for (int i = 0; i < 400; ++i)
        sql += "INSERT INTO big VALUES(" + std::to_string(i) + ", '" + std::string(100, 'x') + "');";
    const auto base = make_db(sql);
    const auto ours = edit(base, "UPDATE small SET v = 'A' WHERE id = 1;");
    const auto out = fx.merged(base, ours, edit(base, "UPDATE small SET v = 'B' WHERE id = 2;"));
    REQUIRE(out.size() == ours.size());
    int changed = 0;
    for (std::size_t p = 0; p < out.size(); p += 4096)
        changed += !std::equal(out.begin() + p, out.begin() + p + 4096, ours.begin() + p);
    CHECK(out.size() / 4096 > 10);
    CHECK(changed <= 2); // page 1 (header counters) and the leaf of `small`
    CHECK(query(out, "SELECT v FROM small ORDER BY id") == "A\nB\n");
}

TEST_CASE("sqlite: constraint violations keep ours' row") {
    Fx fx;
    const auto base =
        make_db("CREATE TABLE u(id INTEGER PRIMARY KEY, slug TEXT UNIQUE); INSERT INTO u VALUES(1, 'a');"
                "CREATE TABLE other(x);");
    auto r = fx.merge(base, edit(base, "INSERT INTO u VALUES(2, 'same'); INSERT INTO other VALUES(1);"),
                      edit(base, "INSERT INTO u VALUES(3, 'same');"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "constraint");
    CHECK(r.conflicts[0].at.path == "/tables/u/rows/pk:3");
    CHECK(query(fx.bytes(*r.document), "SELECT id FROM u ORDER BY id") == "1\n2\n");
}

TEST_CASE("sqlite: FTS5 tables merge through their shadow tables") {
    Fx fx;
    const auto base = make_db(kItems + "CREATE VIRTUAL TABLE docs USING fts5(body);"
                                       "INSERT INTO docs VALUES('the quick fox');");
    const auto ours = edit(base, "UPDATE items SET price = 1 WHERE id = 1;");
    const auto theirs = edit(base, "INSERT INTO docs VALUES('a lazy dog');");
    CHECK(at(fx.diff(base, theirs), "/tables/docs")->kind == "virtual");
    const auto out = fx.merged(base, ours, theirs);
    CHECK(query(out, "SELECT body FROM docs WHERE docs MATCH 'lazy'") == "a lazy dog\n");
    CHECK(query(out, "SELECT price FROM items WHERE id = 1") == "1.0\n");
    CHECK(query(edit(out, "INSERT INTO docs(docs) VALUES('integrity-check');"), "SELECT 1") == "1\n");
    // Both sides changed the index: one conflict for the whole virtual table.
    auto r = fx.merge(base, edit(base, "INSERT INTO docs VALUES('x');"), theirs);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/tables/docs");
}

TEST_CASE("sqlite: WAL-mode files, companions, and cksumvfs checksums") {
    Fx fx;
    const auto dir = scratch_path("sqlite");
    std::filesystem::create_directories(dir);
    const auto file = dir / "w.db";
    auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::string s((std::istreambuf_iterator<char>(in)), {});
        return Bytes(reinterpret_cast<const std::byte*>(s.data()),
                     reinterpret_cast<const std::byte*>(s.data()) + s.size());
    };
    {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
        exec(db, "PRAGMA journal_mode = WAL;" + kItems);
        sqlite3_close(db); // checkpoints and removes the WAL
    }
    const auto base = read(file);
    REQUIRE(std::to_integer<int>(base[18]) == 2);
    const auto ours = edit(base, "UPDATE items SET price = 1 WHERE id = 1;");
    auto out = fx.merged(base, ours, edit(base, "UPDATE items SET price = 2 WHERE id = 2;"));
    CHECK(std::to_integer<int>(out[18]) == 2);
    CHECK(std::to_integer<int>(out[19]) == 2);
    CHECK(query(out, "SELECT price FROM items ORDER BY id") == "1.0\n2.0\n7.0\n");

    // A non-empty WAL next to the input is refused; companions are not databases.
    const auto wal = dir / "w.db-wal";
    std::ofstream(wal, std::ios::binary) << "\x37\x7f\x06\x82 pretend frames";
    auto u8 = file.u8string();
    CHECK_THROWS_AS(fx.load(base, std::string(u8.begin(), u8.end())), Error);
    CHECK(fx.reg.detect(read(wal), "w.db-wal").info().name == "sqlite");
    CHECK_THROWS_AS(fx.load(read(wal), "w.db-wal"), Error);

    // A database with cksumvfs checksums, written by the real VFS.
    const auto ck = dir / "ck.db";
    REQUIRE(sqlite3_register_cksumvfs(nullptr) == SQLITE_OK);
    {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(ck.string().c_str(), &db) == SQLITE_OK);
        exec(db, kItems);
        int n = 8;
        sqlite3_file_control(db, nullptr, SQLITE_FCNTL_RESERVE_BYTES, &n);
        exec(db, "VACUUM");
        sqlite3_close(db);
    }
    const auto cb = read(ck);
    REQUIRE(std::to_integer<int>(cb[20]) == 8);
    CHECK(fx.f.inspect(*fx.load(cb))["checksums"] == "cksumvfs");
    // Each branch edits its copy through the VFS, which keeps the checksums.
    auto vfs_edit = [&](const std::string& sql) {
        const auto p = dir / "edit.db";
        std::ofstream(p, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(cb.data()), static_cast<std::streamsize>(cb.size()));
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(p.string().c_str(), &db) == SQLITE_OK);
        exec(db, sql);
        sqlite3_close(db);
        return read(p);
    };
    const auto co = vfs_edit("UPDATE items SET price = 1 WHERE id = 1;");
    CHECK(fx.f.inspect(*fx.load(co))["checksums"] == "cksumvfs");
    out = fx.merged(cb, co, vfs_edit("INSERT INTO items VALUES(9, 'Pike', 3);"));
    CHECK(fx.f.inspect(*fx.load(out))["checksums"] == "cksumvfs");
    CHECK(fx.f.inspect(*fx.load(out))["integrity"] == "ok");
    // The real VFS reads every page of the merge result without a checksum fault.
    std::ofstream(dir / "out.db", std::ios::binary)
        .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open((dir / "out.db").string().c_str(), &db) == SQLITE_OK);
        sqlite3_stmt* s = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, "SELECT group_concat(name) FROM items", -1, &s, nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(s) == SQLITE_ROW);
        CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, 0))) ==
              "Sword,Shield,Bow,Pike");
        sqlite3_finalize(s);
        sqlite3_close(db);
    }
    sqlite3_unregister_cksumvfs();
    std::filesystem::remove_all(dir);
}
