#include <semsplinter/format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
#include <random>
#include <sstream>

using namespace semsplinter;

namespace {

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& json = *reg.find("json");

    std::unique_ptr<Document> load(std::string_view s) {
        auto b = as_bytes(s);
        return json.load(ByteSource::from_memory({b.begin(), b.end()}, "x.json"), {ctx});
    }
    ChangeSet diff(std::string_view a, std::string_view b) { return json.diff(*load(a), *load(b), {ctx}); }
    MergeResult merge(std::string_view b, std::string_view o, std::string_view t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return json.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::string text(const MergeResult& r) {
        VectorSink s;
        json.save(*r.document, s, {ctx});
        return {reinterpret_cast<const char*>(s.data.data()), s.data.size()};
    }
    std::string merged(std::string_view b, std::string_view o, std::string_view t) {
        auto r = merge(b, o, t);
        CHECK(r.conflicts.empty());
        return text(r);
    }
    std::string dump(std::string_view s) {
        std::ostringstream os;
        json.dump(*load(s), os, {ctx});
        return os.str();
    }
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

} // namespace

TEST_CASE("json: parse, round trip, and errors") {
    Fx fx;
    for (std::string_view s :
         {"{\n  \"name\": \"splinter\",\n  \"n\": 1.50,\n  \"s\": \"\\u00e9\\/\",\n  \"big\": "
          "12345678901234567890123\n}\n",
          "\xEF\xBB\xBF[1, 2]\r\n", "{\r\n\t// c\r\n\t\"a\": [1e+02, -0, true, null, {},],\r\n}", "\"x\"",
          " 42 "}) {
        auto d = fx.load(s);
        VectorSink sink;
        fx.json.save(*d, sink, {fx.ctx});
        CHECK(std::string(reinterpret_cast<const char*>(sink.data.data()), sink.data.size()) == s);
        CHECK(fx.merged(s, s, s) == s); // a merge without changes is byte-identical
        CHECK(fx.diff(s, s).empty());
    }
    CHECK(fx.json.inspect(*fx.load("{/* x */ \"a\": 1}"))["dialect"] == "jsonc");
    for (std::string_view bad : {"", "{\"a\":1,,}", "[1 2]", "{\"a\"}", "\"\x01\"", "01", "[1.]", "nul",
                                 "[1]x", "{\"a\":1", "/* open", "\"\\q\""})
        CHECK_THROWS_AS(fx.load(bad), Error);
    auto dup = fx.json.inspect(*fx.load("{\"a\": {\"k\": 1, \"k\": 2}}"));
    CHECK(dup["warnings"][0]["code"] == "invalid-duplicate-key");
    CHECK(dup["warnings"][0]["at"] == "/a/k");
}

TEST_CASE("json: detection order") { // SPEC §11
    Fx fx;
    CHECK(fx.detect("{\"a\": 1}", "x.json") == "json");
    CHECK(fx.detect("{\"a\": 1}", "noext") == "json"); // content probe
    CHECK(fx.detect("[1, 2]", "") == "json");
    CHECK(fx.detect("42", "x.json") == "json");
    CHECK(fx.detect("42", "notes") == "text"); // a bare scalar needs a JSON name
    CHECK(fx.detect("{oops", "x.json") == "text");
    CHECK(fx.detect("[core]\nx = 1\n", "config") == "text");
    CHECK(fx.detect("{\n  // settings\n  \"a\": 1,\n}", "settings.json") == "json");
    std::string big = "[" + std::string(70000, ' ') + "1, {\"a\": \"tru";
    CHECK(fx.detect(big, "") == "json"); // a valid 64 KiB head is enough
}

TEST_CASE("json: diff addresses and normalization") {
    Fx fx;
    CHECK(fx.diff("{\"n\": 1.50, \"e\": 1E+02, \"s\": \"\\u00e9\", \"big\": 12345678901234567890123}",
                  "{\"s\":\"\xC3\xA9\",\"big\":12345678901234567890123,\"e\":1e2,\"n\":1.5}")
              .empty());
    auto cs = fx.diff(
        "{\"big\": 12345678901234567890123, \"x\": 1, \"gone\": 0, \"t\": [\"a\", \"b\"], \"a/b\": 1}",
        "{\"big\": 12345678901234567890124, \"x\": \"1\", \"new\": {}, \"t\": [\"a\", \"c\", \"d\"], "
        "\"a/b\": 2}");
    REQUIRE(at(cs, "/big"));
    CHECK(at(cs, "/big")->after.text == "12345678901234567890124");
    CHECK(at(cs, "/x")->kind == "type");
    CHECK(at(cs, "/gone")->op == Op::erase);
    CHECK(at(cs, "/new")->op == Op::insert);
    CHECK(at(cs, "/t/1")->op == Op::update);
    CHECK(at(cs, "/t/2")->op == Op::insert);
    CHECK(at(cs, "/a~1b")->metrics["delta"] == 1.0);

    const std::string base =
        "{\"items\": [{\"id\": 1, \"v\": \"a\"}, {\"id\": 2, \"v\": \"b\"}, {\"id\": \"x y\", \"v\": 0}]}";
    cs = fx.diff(
        base,
        "{\"items\": [{\"id\": \"x y\", \"v\": 0}, {\"id\": 1, \"v\": \"a\"}, {\"id\": 2, \"v\": \"B\"}]}");
    REQUIRE(cs.size() == 2);
    CHECK(at(cs, "/items/id:2/v")->after.text == "\"B\"");
    CHECK(at(cs, "/items/id:x%20y")->op == Op::move);
    auto a = fx.load(base), b = fx.load("{\"items\": []}");
    CHECK(fx.json.stats(*a, *b, {})["array_keys"]["/items"] == "id");
}

TEST_CASE("json: dump") {
    Fx fx;
    CHECK(fx.dump("{\"name\": \"s\", \"items\": [{\"label\": \"Sword\", \"id\": 7}], \"tags\": [\"rpg\"], "
                  "\"e\": {}}") ==
          "/e = {}\n/items/id:7/id = 7\n/items/id:7/label = \"Sword\"\n/name = \"s\"\n/tags/0 = \"rpg\"\n");
    auto long_line = fx.dump("{\"s\": \"" + std::string(200, 'x') + "\"}");
    CHECK(long_line.starts_with("/s = \"" + std::string(100, 'x') + "\xE2\x80\xA6\" xxh3:"));
}

TEST_CASE("json: merge keeps ours' bytes and follows the surrounding style") {
    Fx fx;
    const std::string base = "{\n  \"a\": 1,\n  \"b\": 2,\n  \"c\": 3\n}\n";
    CHECK(fx.merged(base, "{\n  \"a\": 10,\n  \"b\": 2,\n  \"c\": 3\n}\n",
                    "{\n    \"a\": 1,\n    \"b\": 2,\n    \"c\": 30,\n    \"d\": [1, 2]\n}") ==
          "{\n  \"a\": 10,\n  \"b\": 2,\n  \"c\": 30,\n  \"d\": [1, 2]\n}\n");
    // Deleting the last member drops its comma; the other side's edit still lands.
    CHECK(fx.merged(base, "{\n  \"a\": 1,\n  \"b\": 2\n}\n", "{\"a\": 11, \"b\": 2, \"c\": 3}") ==
          "{\n  \"a\": 11,\n  \"b\": 2\n}\n");
    // Inline container: a value from a multi-line side is written inline.
    CHECK(fx.merged("{\"a\": 1, \"b\": 2}", "{\"a\": 1, \"b\": 3}",
                    "{\n  \"a\": 1,\n  \"b\": 2,\n  \"c\": {\n    \"x\": [1, 2.50]\n  }\n}") ==
          "{\"a\": 1, \"b\": 3, \"c\": {\"x\": [1, 2.50]}}");
    // Multi-line container: a value from a minified side is laid out with ours' indentation and EOL.
    CHECK(fx.merged("{\r\n\t\"a\": 1\r\n}", "{\r\n\t\"a\": 2\r\n}",
                    "{\"a\":1,\"b\":{\"x\":1,\"y\":[true,null]}}") ==
          "{\r\n\t\"a\": 2,\r\n\t\"b\": {\r\n\t\t\"x\": 1,\r\n\t\t\"y\": "
          "[\r\n\t\t\ttrue,\r\n\t\t\tnull\r\n\t\t]\r\n\t}\r\n}");
    // Theirs re-indented multi-line value.
    CHECK(fx.merged("{\n  \"a\": 1,\n  \"o\": {\n    \"k\": 1\n  }\n}",
                    "{\n  \"a\": 2,\n  \"o\": {\n    \"k\": 1\n  }\n}",
                    "{\n    \"a\": 1,\n    \"o\": [\n        3\n    ]\n}") ==
          "{\n  \"a\": 2,\n  \"o\": [\n      3\n  ]\n}");
    // JSONC: comments and the trailing comma survive.
    CHECK(fx.merged("{\n  // the name\n  \"name\": \"x\", // trailing\n  /* block */\n  \"v\": 1,\n}",
                    "{\n  // the name\n  \"name\": \"x\", // trailing\n  /* block */\n  \"v\": 2,\n}",
                    "{\"name\": \"y\", \"v\": 1}") ==
          "{\n  // the name\n  \"name\": \"y\", // trailing\n  /* block */\n  \"v\": 2,\n}");
    // An emptied container of ours that gains a member.
    CHECK(fx.merged("{\"x\": 1}", "{}", "{\"x\": 1, \"y\": 2}") == "{\"y\": 2}");
    // Duplicate names: ours' shadowed member stays before the one that counts.
    CHECK(fx.merged("{\"k\": 1, \"a\": 1, \"k\": 2}", "{\"k\": 1, \"a\": 2, \"k\": 2}",
                    "{\"k\": 3, \"a\": 1}") == "{\"a\": 2, \"k\": 1, \"k\": 3}");
    // Ours reformatted (an encoding-only change), theirs edited: ours' layout, theirs' value.
    CHECK(fx.merged("{\"a\": [1, 2], \"b\": 1.0}", "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": 1.00\n}",
                    "{\"a\": [1, 5], \"b\": 1.0}") == "{\n  \"a\": [\n    1,\n    5\n  ],\n  \"b\": 1.00\n}");
}

TEST_CASE("json: keyed and unkeyed arrays") {
    Fx fx;
    const std::string base = "{\"items\": [\n  {\"id\": 1, \"v\": \"a\"},\n  {\"id\": 2, \"v\": \"b\"},\n  "
                             "{\"id\": 3, \"v\": \"c\"}\n]}";
    auto r = fx.merge(base,
                      "{\"items\": [\n  {\"id\": 1, \"v\": \"a\"},\n  {\"id\": 2, \"v\": \"B\"},\n  {\"id\": "
                      "3, \"v\": \"c\"}\n]}",
                      "{\"items\": [{\"id\": 3, \"v\": \"c\"}, {\"id\": 1, \"v\": \"a\"}, {\"id\": 2, \"v\": "
                      "\"b\"}, {\"id\": 4, \"v\": \"d\"}]}");
    CHECK(r.conflicts.empty());
    CHECK(fx.text(r) == "{\"items\": [\n  {\"id\": 3, \"v\": \"c\"},\n  {\"id\": 1, \"v\": \"a\"},\n  "
                        "{\"id\": 2, \"v\": \"B\"},\n  "
                        "{\"id\": 4, \"v\": \"d\"}\n]}");
    CHECK(at(r.changes_ours, "/items/id:2/v"));
    CHECK(at(r.changes_theirs, "/items/id:3")->op == Op::move);
    CHECK(at(r.changes_theirs, "/items/id:4")->op == Op::insert);
    CHECK(r.auto_merged.size() == 3);

    // Both sides edit one identified element: recursion, add-add only at differing leaves.
    r = fx.merge(
        base,
        "{\"items\": [{\"id\": 1, \"v\": \"A\"}, {\"id\": 2, \"v\": \"b\"}, {\"id\": 3, \"v\": \"c\"}]}",
        "{\"items\": [{\"id\": 1, \"v\": \"a\", \"w\": 1}, {\"id\": 2, \"v\": \"b\"}, {\"id\": 3, \"v\": "
        "\"c\"}]}");
    CHECK(r.conflicts.empty());
    CHECK(nlohmann::json::parse(fx.text(r))["items"][0] == nlohmann::json{{"id", 1}, {"v", "A"}, {"w", 1}});

    // Unkeyed objects: a 1:1 edit by both sides recurses positionally.
    CHECK(fx.merged("[{\"a\":1,\"b\":1}]", "[{\"a\":2,\"b\":1}]", "[{\"a\":1,\"b\":2}]") ==
          "[{\"a\":2,\"b\":2}]");

    // Both insert at one anchor: add-add over the chunk; --union keeps both, ours first.
    r = fx.merge("[1, 2]", "[1, 2, 3]", "[1, 2, 4]");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "add-add");
    CHECK(r.conflicts[0].at.to_string() == "#range=2,0");
    CHECK(r.conflicts[0].changes.size() == 2);
    CHECK(fx.text(r) == "[1, 2, 3]");
    CHECK(fx.text(fx.merge("[1, 2]", "[1, 2, 3]", "[1, 2, 4]", {Policy::ours, ResolveAll::union_, {}})) ==
          "[1, 2, 3, 4]");
}

TEST_CASE("json: conflicts") {
    Fx fx;
    const std::string base = "{\"a\": 1, \"b\": {\"c\": 1}}";
    auto r = fx.merge(base, "{\"a\": 2, \"b\": {\"c\": 1}}", "{\"a\": 3, \"b\": {\"c\": 1}}");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/a");
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].changes == std::vector<std::string>{r.changes_ours[0].id, r.changes_theirs[0].id});
    CHECK(fx.text(r) == "{\"a\": 2, \"b\": {\"c\": 1}}");
    CHECK(fx.text(fx.merge(base, "{\"a\": 2, \"b\": {\"c\": 1}}", "{\"a\": 3, \"b\": {\"c\": 1}}",
                           {Policy::theirs})) == "{\"a\": 3, \"b\": {\"c\": 1}}");
    // modify-delete, and a type change against an edit.
    r = fx.merge(base, "{\"a\": 1}", "{\"a\": 1, \"b\": {\"c\": 2}}");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "modify-delete");
    CHECK(r.conflicts[0].at.path == "/b");
    CHECK(fx.text(r) == "{\"a\": 1}");
    r = fx.merge(base, "{\"a\": 1, \"b\": [1]}", "{\"a\": 1, \"b\": {\"c\": 2}}");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "both-modified");
    // --conflict=mark has no marking in JSON: ours plus a warning.
    r = fx.merge(base, "{\"a\": 2, \"b\": {\"c\": 1}}", "{\"a\": 3, \"b\": {\"c\": 1}}", {Policy::mark});
    CHECK(r.warnings.at(0).code == "mark-unsupported");
}

// Random trees with disjoint edits on each side: the merge equals both edits applied (json.md §11).
TEST_CASE("json: generated disjoint merges") {
    Fx fx;
    std::mt19937 rng(20261010);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    int next_id = 1000;
    std::function<nlohmann::ordered_json(int)> gen = [&](int depth) -> nlohmann::ordered_json {
        switch (depth > 2 ? pick(4) : pick(7)) {
        case 0:
            return pick(100) - 50;
        case 1:
            return std::vector<std::string>{"x", "a\"b", "\xC3\xA9", "line\nbreak",
                                            ""}[static_cast<std::size_t>(pick(5))];
        case 2:
            return pick(2) == 0;
        case 3:
            return nullptr;
        case 4: {
            nlohmann::ordered_json o = nlohmann::ordered_json::object();
            for (int i = pick(4); i > 0; --i)
                o["k" + std::to_string(pick(6))] = gen(depth + 1);
            return o;
        }
        case 5: {
            nlohmann::ordered_json a = nlohmann::ordered_json::array();
            for (int i = pick(4); i > 0; --i)
                a.push_back(gen(depth + 1));
            return a;
        }
        default: {
            nlohmann::ordered_json a = nlohmann::ordered_json::array();
            for (int i = pick(4); i > 0; --i)
                a.push_back({{"id", next_id++}, {"v", gen(depth + 1)}});
            return a;
        }
        }
    };
    std::function<void(nlohmann::ordered_json&, int)> mutate = [&](nlohmann::ordered_json& j, int depth) {
        if (j.is_object() && !j.empty() && pick(3)) {
            auto it = j.begin();
            std::advance(it, pick(static_cast<int>(j.size())));
            if (it.key() != "id")
                return mutate(it.value(), depth + 1);
        }
        if (j.is_array() && !j.empty() && pick(3)) {
            auto i = static_cast<std::size_t>(pick(static_cast<int>(j.size())));
            if (pick(4) == 0)
                return void(j.erase(i));
            return mutate(j[i], depth + 1);
        }
        if (j.is_array() && !j.empty() && j[0].is_object() && j[0].contains("id"))
            return void(j.push_back({{"id", next_id++}, {"v", 1}}));
        auto old = j;
        while (j == old)
            j = gen(depth);
    };
    for (int round = 0; round < 200; ++round) {
        nlohmann::ordered_json base = nlohmann::ordered_json::object();
        for (int k = 0; k < 6; ++k)
            base["m" + std::to_string(k)] = gen(1);
        auto ours = base, theirs = base;
        mutate(ours["m" + std::to_string(2 * pick(3))], 1);
        mutate(theirs["m" + std::to_string(2 * pick(3) + 1)], 1);
        auto expected = ours;
        for (int k = 1; k < 6; k += 2)
            expected["m" + std::to_string(k)] = theirs["m" + std::to_string(k)];
        const int indents[] = {-1, 2, 4};
        auto sb = base.dump(indents[pick(3)]), so = ours.dump(indents[pick(3)]),
             st = theirs.dump(indents[pick(3)]);
        INFO(sb << "\n" << so << "\n" << st);
        auto r = fx.merge(sb, so, st);
        CHECK(r.conflicts.empty());
        CHECK(nlohmann::json::parse(fx.text(r)) == nlohmann::json::parse(expected.dump()));
        CHECK(nlohmann::json::parse(fx.text(fx.merge(sb, sb, st))) == nlohmann::json::parse(st));
        CHECK(fx.text(fx.merge(sb, so, so)) == so);
        CHECK(fx.text(fx.merge(sb, so, sb)) == so);
    }
}

// Benchmark for #38, hidden from CI: run `semsplinter-tests "[.perf]"` in Release.
TEST_CASE("json: merge of a 100k-record keyed array", "[.perf]") {
    Fx fx;
    const std::size_t n = 100000;
    std::vector<std::size_t> b(n);
    for (std::size_t i = 0; i < n; ++i)
        b[i] = i;
    auto o = b, t = b;
    std::rotate(o.begin() + 100, o.begin() + 101, o.begin() + 5000); // ours moves one record
    o.erase(o.begin() + 20100, o.begin() + 20110);                   // and deletes ten
    t.insert(t.begin() + 70000, n + 1);                              // theirs inserts one
    // Each side also edits one record in a thousand.
    auto text = [](const std::vector<std::size_t>& ids, std::size_t edit) {
        std::string s = "{\"items\": [\n";
        for (std::size_t k = 0; k < ids.size(); ++k) {
            auto id = std::to_string(ids[k]);
            s += (k ? ",\n  {\"id\": " : "  {\"id\": ") + id + ", \"name\": \"record " + id +
                 "\", \"tags\": [\"alpha\", \"beta\"], \"v\": \"" + (ids[k] % 1000 == edit ? "edited" : "v") +
                 "\"}";
        }
        return s + "\n]}\n";
    };
    auto sb = text(b, 1000), so = text(o, 3), st = text(t, 7);
    auto t0 = std::chrono::steady_clock::now();
    auto db = fx.load(sb), dob = fx.load(so), dt = fx.load(st);
    auto t1 = std::chrono::steady_clock::now();
    auto r = fx.json.merge(*db, *dob, *dt, MergeOptions{fx.ctx, {}, {}});
    auto t2 = std::chrono::steady_clock::now();
    CHECK(r.conflicts.empty());
    auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    WARN(sb.size() << " bytes; load " << ms(t1 - t0) << " ms, merge " << ms(t2 - t1) << " ms");
}
