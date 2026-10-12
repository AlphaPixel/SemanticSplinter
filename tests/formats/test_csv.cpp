#include <semsplinter/format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <sstream>

using namespace semsplinter;

namespace {

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& csv = *reg.find("csv");

    std::unique_ptr<Document> load(std::string_view s, std::string name = "x.csv") {
        auto b = as_bytes(s);
        return csv.load(ByteSource::from_memory({b.begin(), b.end()}, std::move(name)), {ctx});
    }
    ChangeSet diff(std::string_view a, std::string_view b) { return csv.diff(*load(a), *load(b), {ctx}); }
    MergeResult merge(std::string_view b, std::string_view o, std::string_view t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return csv.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::string text(const Document& d) {
        VectorSink s;
        csv.save(d, s, {ctx});
        return {reinterpret_cast<const char*>(s.data.data()), s.data.size()};
    }
    std::string merged(std::string_view b, std::string_view o, std::string_view t) {
        auto r = merge(b, o, t);
        CHECK(r.conflicts.empty());
        return text(*r.document);
    }
    std::string dump(std::string_view s) {
        std::ostringstream os;
        csv.dump(*load(s), os, {ctx});
        return os.str();
    }
    nlohmann::ordered_json inspect(std::string_view s) { return csv.inspect(*load(s)); }
    std::string detect(std::string_view s, std::string_view name) {
        return std::string(reg.detect(as_bytes(s), name).info().name);
    }
};

const Change* at(const ChangeSet& cs, std::string_view path) {
    for (auto& c : cs)
        if (c.at.to_string() == path)
            return &c;
    return nullptr;
}

std::string utf16le(std::string_view ascii) {
    std::string o = "\xFF\xFE";
    for (char c : ascii)
        o += c, o += '\0';
    return o;
}

// As Python's csv.writer writes them (QUOTE_MINIMAL, "\r\n" terminators).
constexpr std::string_view kPython = "id,name,notes,damage\r\n"
                                     "sword_01,Sword,\"sharp, heavy\",12\r\n"
                                     "shield_02,Shield,\"says \"\"block\"\"\",0\r\n"
                                     "bow_03,Bow,\"line one\nline two\",7\r\n";

// Excel "CSV UTF-8" in a European locale: BOM, semicolons, CRLF, no final newline.
constexpr std::string_view kExcel = "\xEF\xBB\xBFid;name;price\r\n"
                                    "a1;Apfel;1,5\r\n"
                                    "b2;Birne;2,25\r\n"
                                    "c3;\"Kirsche; rot\";3";

} // namespace

TEST_CASE("csv: tokenize, sniff, and round trip") {
    Fx fx;
    for (std::string_view s :
         {kPython, kExcel, std::string_view("a\tb\tc\n1\t2\t3\n"), std::string_view("a|b\n1|2\n3|4"),
          std::string_view("sep=;\nx;y\n1;2\n"), std::string_view("h1,h2\n\n1,2\n\n\n3\n4,5,6\n\n"),
          std::string_view("only\n"), std::string_view(""), std::string_view("a,b\r1,2\r"),
          std::string_view("\"q\"\"\",\"\"\n,\n")}) {
        auto d = fx.load(s);
        CHECK(fx.text(*d) == s);
        CHECK(fx.merged(s, s, s) == s); // a merge without changes is byte-identical
        CHECK(fx.diff(s, s).empty());
    }
    CHECK(fx.inspect(kPython)["delimiter"] == ",");
    CHECK(fx.inspect(kPython)["rows"] == 3); // the quoted newline stays inside its record
    CHECK(fx.inspect(kExcel)["delimiter"] == ";");
    CHECK(fx.inspect(kExcel)["dialect"] == "utf-8 bom ';' crlf");
    CHECK(fx.inspect("a\tb\tc\n1\t2\t3\n")["delimiter"] == "\t");
    CHECK(fx.inspect("a|b\n1|2\n3|4")["delimiter"] == "|");
    CHECK(fx.inspect("sep=,\nx;y\n1;2\n")["delimiter"] == ",");
    CHECK(fx.inspect("a;b,c;d\n1;2,3;4\n")["delimiter"] == ";"); // more fields wins
    CHECK(fx.inspect("a,b\n1,2\n")["delimiter"] == ",");
    CHECK(fx.inspect("only\nrows\n")["columns"].size() == 1);
    CHECK_THROWS_AS(fx.load("a,\"open\n1,2\n"), Error);

    // UTF-16LE TSV (Excel "Unicode text").
    const auto u = utf16le("id\tname\nx\t1\ny\t2\n");
    CHECK(fx.inspect(u)["delimiter"] == "\t");
    CHECK(fx.text(*fx.merge(u, u, utf16le("id\tname\nx\t1\ny\t3\n")).document) ==
          utf16le("id\tname\nx\t1\ny\t3\n"));
    CHECK(fx.text(
              *fx.merge(u, utf16le("id\tname\nx\t0\ny\t2\n"), utf16le("id\tname\nx\t1\ny\t3\n")).document) ==
          utf16le("id\tname\nx\t0\ny\t3\n"));
}

TEST_CASE("csv: detection against text") {
    Fx fx;
    CHECK(fx.detect("just one column\n", "list.csv") == "csv"); // the extension decides (SPEC §11)
    CHECK(fx.detect("a\tb\n", "t.tsv") == "csv");
    CHECK(fx.detect(kPython, "export") == "csv");
    CHECK(fx.detect("x,y,z\n1,2,3\n", "noext") == "csv");
    CHECK(fx.detect("id;name\n1;a\n2;b\n3;c\n4;d\n", "noext") == "csv");
    CHECK(fx.detect("Hello, world.\nThis is plain text, with commas.\nAnd lines, of varying, shape.\n",
                    "readme.txt") == "text");
    CHECK(fx.detect("Dear John,\nthanks.\n", "letter") == "text");
    CHECK(fx.detect("int f(int a, int b) {\n    return g(a, b);\n}\n", "f.c") == "text");
    CHECK(fx.detect("{\n  \"a\": 1,\n  \"b\": 2,\n  \"c\": 3,\n  \"d\": 4,\n  \"e\": 5\n}\n", "x") == "json");
    CHECK(fx.detect("a,b\n1,2\n", "two") == "text"); // two records of two fields: too weak
    // Every line with one comma or semicolon, yet not a table.
    CHECK(fx.detect("v1 = f(1);\nv2 = f(2);\nv3 = f(3);\nv4 = f(4);\nv5 = f(5);\n", "code") == "text");
    CHECK(fx.detect("Hi, Ann\nYes, sir\nNo, never\nWell, then\nOh, well\n", "lines") == "text");
}

TEST_CASE("csv: header and key detection") {
    Fx fx;
    CHECK(fx.inspect(kPython)["key"] == nlohmann::json::array({"id"}));
    CHECK(fx.inspect("n,v\n1,a\n2,b\n3,c\n")["row_identity"] ==
          "position"); // a row-number column is position
    CHECK(fx.inspect("lang,word\nen,hello\nde,hallo\n")["header"] == false); // no typed column, no key name
    CHECK(fx.inspect("item,weight\nsword,1.5\nbow,2\n")["header"] == true);
    CHECK(fx.inspect("item,weight\nsword,1.5\nbow,2\n")["key"] == nlohmann::json::array({"item"}));
    CHECK(fx.inspect("a,a\n1,2\n")["header"] == false);
    fx.opts["csv-header"] = "yes";
    CHECK(fx.inspect("lang,word\nen,hello\n")["columns"] == nlohmann::json::array({"lang", "word"}));
    CHECK(fx.inspect("a,a\n1,2\n")["columns"] == nlohmann::json::array({"a", "a#2"}));
}

TEST_CASE("csv: diff") {
    Fx fx;
    const std::string_view a = "id,name,damage\nsword,Sword,12\nbow,Bow,7\n";
    auto cs = fx.diff(a, "id,name,damage\nsword,Sword,15\naxe,Axe,9\n");
    REQUIRE(cs.size() == 3);
    auto* c = at(cs, "/rows/id:sword/damage");
    REQUIRE(c);
    CHECK(c->kind == "cell");
    CHECK(*c->before.text == "12");
    CHECK(*c->after.text == "15");
    CHECK(c->metrics["delta"] == 3.0);
    CHECK(c->metrics["rel"] == 0.25);
    CHECK(at(cs, "/rows/id:bow")->op == Op::erase);
    CHECK(at(cs, "/rows/id:axe")->op == Op::insert);

    cs = fx.diff(a, "id,name,damage,weight\nsword,Sword,12,3\nbow,Bow,7,1\n");
    REQUIRE(cs.size() == 1); // a new column is one change, not one per row
    CHECK(at(cs, "/columns/weight")->op == Op::insert);
    CHECK(at(fx.diff(a, "name,id,damage\nSword,sword,12\nBow,bow,7\n"), "/columns")->kind == "column-order");

    // Keyed rows reordered: no content change.
    CHECK(fx.diff(a, "id,name,damage\nbow,Bow,7\nsword,Sword,12\n").size() == 1);
    CHECK(fx.diff(a, "id,name,damage\nbow,Bow,7\nsword,Sword,12\n")[0].kind == "encoding");

    // Unkeyed rows: positions.
    const std::string_view u = "1,x\n1,x\n2,y\n";
    cs = fx.diff(u, "1,x\n1,x\n2,z\n3,w\n");
    CHECK(at(cs, "/rows/2/2")->kind == "cell");
    CHECK(at(cs, "/rows#range=3,1")->op == Op::insert);
    CHECK(at(fx.diff("a,b,c\n1,2,3\n1,2,3\n", "a,b,c\n1,2,3\n1,2\n"), "/rows/1/c")->kind == "ragged");

    // An Excel re-save (BOM, semicolons, CRLF, requoting) is encoding only.
    cs = fx.diff("id,name,price\na1,Apfel,1.5\n", "\xEF\xBB\xBFid;\"name\";price\r\na1;Apfel;1.5\r\n");
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].kind == "encoding");
    CHECK(cs[0].at.path == "/dialect");
}

TEST_CASE("csv: keyed merge") {
    Fx fx;
    const std::string_view b = "id,name,damage\r\nsword,Sword,12\r\nbow,Bow,7\r\nshield,Shield,0\r\n";
    // Different cells of one row merge.
    CHECK(fx.merged(b, "id,name,damage\r\nsword,Long Sword,12\r\nbow,Bow,7\r\nshield,Shield,0\r\n",
                    "id,name,damage\r\nsword,Sword,15\r\nbow,Bow,7\r\nshield,Shield,0\r\n") ==
          "id,name,damage\r\nsword,Long Sword,15\r\nbow,Bow,7\r\nshield,Shield,0\r\n");
    // Rows added and deleted on both sides; theirs' new row follows its predecessor.
    CHECK(fx.merged(b, "id,name,damage\r\nsword,Sword,12\r\nbow,Bow,7\r\nshield,Shield,0\r\nmace,Mace,5\r\n",
                    "id,name,damage\r\nsword,Sword,12\r\naxe,Axe,9\r\nshield,Shield,0\r\n") ==
          "id,name,damage\r\nsword,Sword,12\r\naxe,Axe,9\r\nshield,Shield,0\r\nmace,Mace,5\r\n");
    // A column added on one side, rows on the other: new rows get an empty cell.
    CHECK(fx.merged("id,en\r\nhi,Hello\r\nbye,Bye\r\n", "id,en,de\r\nhi,Hello,Hallo\r\nbye,Bye,Tschuess\r\n",
                    "id,en\r\nhi,Hello\r\nbye,Bye\r\nyes,Yes\r\n") ==
          "id,en,de\r\nhi,Hello,Hallo\r\nbye,Bye,Tschuess\r\nyes,Yes,\r\n");
    CHECK(fx.merged("id,en\nhi,Hello\n", "id,en\nhi,Hi\n", "id,en,de\nhi,Hello,Hallo\n") ==
          "id,en,de\nhi,Hi,Hallo\n");
    // A column deleted on one side.
    CHECK(fx.merged(b, "id,name\r\nsword,Sword\r\nbow,Bow\r\nshield,Shield\r\n",
                    "id,name,damage\r\nsword,Sword,12\r\nbow,Longbow,7\r\nshield,Shield,0\r\n") ==
          "id,name\r\nsword,Sword\r\nbow,Longbow\r\nshield,Shield\r\n");
    // Both add different columns at the same place: both kept, ours first.
    CHECK(fx.merged("id,a\nx,1\n", "id,a,b\nx,1,2\n", "id,a,c\nx,1,3\n") == "id,a,b,c\nx,1,2,3\n");
    // Theirs' row edit keeps its quoting; an embedded newline survives.
    CHECK(fx.merged(kPython, kPython,
                    "id,name,notes,damage\r\nsword_01,Sword,\"sharp, heavy\",12\r\n"
                    "shield_02,Shield,\"says \"\"block\"\"\",0\r\nbow_03,Bow,\"line one\nline 2\",7\r\n") ==
          "id,name,notes,damage\r\nsword_01,Sword,\"sharp, heavy\",12\r\n"
          "shield_02,Shield,\"says \"\"block\"\"\",0\r\nbow_03,Bow,\"line one\nline 2\",7\r\n");
    // No final newline in ours: a row appended after the last record.
    CHECK(fx.merged("id,v\na,1", "id,v\na,2", "id,v\na,1\nb,3\n") == "id,v\na,2\nb,3");
    // Ragged rows: an untouched short row stays short, a rebuilt one gets the header's width.
    CHECK(fx.merged("a,b\n1,2\n3\n", "a,b\n1,9\n3\n", "a,b\n1,2\n3\n5,6\n") == "a,b\n1,9\n3\n5,6\n");
    CHECK(fx.merged("a,b,c\n1,2,3\n4\n", "a,b,c\n1,2,0\n4\n", "a,b,c\n1,2,3\n7\n") == "a,b,c\n1,2,0\n7,,\n");
    // The `sep=` line and blank lines stay.
    CHECK(fx.merged("sep=;\nid;v\n\na;1\n", "sep=;\nid;v\n\na;2\n", "sep=;\nid;v\n\na;1\nb;3\n") ==
          "sep=;\nid;v\n\na;2\nb;3\n");
}

TEST_CASE("csv: conflicts") {
    Fx fx;
    const std::string_view b = "id,name,damage\nsword,Sword,12\nbow,Bow,7\n";
    const std::string_view o = "id,name,damage\nsword,Sword,13\nbow,Bow,7\n";
    const std::string_view t = "id,name,damage\nsword,Sword,14\nbow,Bow,7\n";
    auto r = fx.merge(b, o, t);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/rows/id:sword/damage");
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].changes.size() == 2);
    CHECK(fx.text(*r.document) == o);
    ConflictOptions theirs;
    theirs.resolutions[r.conflicts[0].id] = "theirs";
    CHECK(fx.text(*fx.merge(b, o, t, theirs).document) == t);

    // Deleted on one side, edited on the other.
    r = fx.merge(b, "id,name,damage\nsword,Sword,12\n", "id,name,damage\nsword,Sword,12\nbow,Bow,8\n");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/rows/id:bow");
    CHECK(r.conflicts[0].reason == "modify-delete");
    CHECK(fx.text(*r.document) == "id,name,damage\nsword,Sword,12\n");
    theirs.resolutions = {{r.conflicts[0].id, "theirs"}};
    CHECK(fx.text(*fx.merge(b, "id,name,damage\nsword,Sword,12\n",
                            "id,name,damage\nsword,Sword,12\nbow,Bow,8\n", theirs)
                       .document) == "id,name,damage\nsword,Sword,12\nbow,Bow,8\n");

    // A column deleted on one side, a cell in it edited on the other.
    r = fx.merge(b, "id,name\nsword,Sword\nbow,Bow\n", "id,name,damage\nsword,Sword,99\nbow,Bow,7\n");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/columns/damage");
    CHECK(fx.text(*r.document) == "id,name\nsword,Sword\nbow,Bow\n");
    theirs.resolutions = {{r.conflicts[0].id, "theirs"}};
    CHECK(fx.text(*fx.merge(b, "id,name\nsword,Sword\nbow,Bow\n",
                            "id,name,damage\nsword,Sword,99\nbow,Bow,7\n", theirs)
                       .document) == "id,name,damage\nsword,Sword,99\nbow,Bow,7\n");

    // --conflict=mark falls back to ours.
    ConflictOptions mark;
    mark.policy = Policy::mark;
    r = fx.merge(b, o, t, mark);
    CHECK(fx.text(*r.document) == o);
    CHECK(r.warnings.at(0).code == "mark-unsupported");
}

TEST_CASE("csv: unkeyed merge") {
    Fx fx;
    const std::string_view b = "a,1\na,1\nb,2\nc,3\n";
    // Disjoint row edits and inserts merge by position.
    CHECK(fx.merged(b, "a,1\na,1\nb,20\nc,3\n", "a,1\na,1\nb,2\nc,3\nd,4\n") == "a,1\na,1\nb,20\nc,3\nd,4\n");
    // Both edit one row in different cells.
    CHECK(fx.merged(b, "a,1\na,1\nB,2\nc,3\n", "a,1\na,1\nb,22\nc,3\n") == "a,1\na,1\nB,22\nc,3\n");
    // Both replace one row with unrelated rows: a range conflict offering union.
    auto r = fx.merge(b, "a,1\na,1\nx,9\nc,3\n", "a,1\na,1\ny,8\nc,3\n");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.to_string() == "/rows#range=2,1");
    CHECK(r.conflicts[0].changes.size() == 4);
    ConflictOptions u;
    u.resolve_all = ResolveAll::union_;
    CHECK(fx.text(*fx.merge(b, "a,1\na,1\nx,9\nc,3\n", "a,1\na,1\ny,8\nc,3\n", u).document) ==
          "a,1\na,1\nx,9\ny,8\nc,3\n");
}

TEST_CASE("csv: dialects across sides") {
    Fx fx;
    // Theirs re-saved with commas and LF and edited one value: written in ours' dialect.
    auto r =
        fx.merge(kExcel, kExcel, "id,name,price\na1,Apfel,\"1,75\"\nb2,Birne,\"2,25\"\nc3,Kirsche; rot,3\n");
    CHECK(r.conflicts.empty());
    CHECK(r.warnings.at(0).code == "dialect-mismatch");
    CHECK(fx.text(*r.document) ==
          "\xEF\xBB\xBFid;name;price\r\na1;Apfel;1,75\r\nb2;Birne;2,25\r\nc3;\"Kirsche; rot\";3");
    // A field holding ours' delimiter is quoted.
    CHECK(fx.merged("id,v\na,1\n", "id;v\na;1\n", "id,v\na,\"x;y\"\n") == "id;v\na;\"x;y\"\n");
    // A column ours quotes throughout stays quoted.
    CHECK(fx.merged("\"id\",\"v\"\n\"a\",\"1\"\n", "\"id\",\"v\"\n\"a\",\"1\"\n\"b\",\"2\"\n",
                    "id,v\na,3\n") == "\"id\",\"v\"\n\"a\",\"3\"\n\"b\",\"2\"\n");
}

TEST_CASE("csv: unified hunks name cells and rows") {
    Fx fx;
    auto unified = [&](std::string_view a, std::string_view b) {
        auto da = fx.load(a), db = fx.load(b);
        DiffOptions o{fx.ctx, {}};
        RenderRequest r{OutputStyle::unified, {}, {"a", "b"}, {"a", "b"}, {"a", "b"}};
        r.options.annotate = true;
        std::string out;
        fx.csv.compare(*da, *db, o)->render(out, r);
        return out;
    };
    auto keyed =
        unified("id,name,damage\nsword,Sword,12\nbow,Bow,7\n", "id,name,damage\nsword,Sword,12\nbow,Bow,9\n");
    CHECK(keyed.find("@@ -2,4 +2,4 @@ /rows/id:bow/damage\n# ~ /rows/id:bow/damage 7 -> 9") !=
          std::string::npos);
    // A quoted line break makes two dump lines with one address.
    auto rows = unified("1,x\n2,\"y\nz\"\n", "1,x\n2,\"y\nw\"\n");
    CHECK(rows.find(" @@ /rows/1\n") != std::string::npos);
}

TEST_CASE("csv: key option, dump, and stats") {
    Fx fx;
    CHECK(fx.dump("id,name,damage\nsword,Sword,12\nbow,\"Bow, long\",7\n") ==
          "#columns id,name,damage\n[id:sword] name = \"Sword\"\n[id:sword] damage = 12\n"
          "[id:bow] name = \"Bow, long\"\n[id:bow] damage = 7\n");
    CHECK(fx.dump("1,x\n2,\"y\nz\"\n") == "#columns 1,2\n1,x\n2,\"y\nz\"\n");

    fx.opts["csv-key"] = "id,lang";
    const std::string_view b = "id,lang,text\nhi,en,Hello\nhi,de,Hallo\n";
    auto cs = fx.diff(b, "id,lang,text\nhi,en,Hello\nhi,de,Guten Tag\n");
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/rows/id,lang:hi,de/text");
    fx.opts["csv-key"] = "1";
    CHECK_THROWS_AS(fx.diff(b, b), Error); // "id" repeats
    fx.opts["csv-key"] = "nope";
    CHECK_THROWS_AS(fx.diff(b, b), Error);
    fx.opts.clear();

    const std::string_view a = "id,v\nx,1\n";
    const std::string_view c = "id,v\nx,2\ny,3\n";
    auto st = fx.csv.stats(*fx.load(a), *fx.load(c), fx.diff(a, c));
    CHECK(st["insert"] == 1);
    CHECK(st["cells_changed"] == 1);
    CHECK(st["row_identity"] == "key");
}
