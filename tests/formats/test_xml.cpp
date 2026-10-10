#include "../cli/console.hpp"
#include "../scratch.hpp"

#include <semsplinter/format.hpp>
#include <semsplinter/formats/xml.hpp>
#include <semsplinter/text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>

using namespace semsplinter;

namespace {

std::vector<std::byte> vec(std::string_view s) {
    auto b = as_bytes(s);
    return {b.begin(), b.end()};
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format* fmt = reg.find("xml");

    std::unique_ptr<Document> load(std::string_view s) {
        return fmt->load(ByteSource::from_memory(vec(s), "x.xml"), {ctx});
    }
    ChangeSet diff(std::string_view a, std::string_view b) { return fmt->diff(*load(a), *load(b), {ctx}); }
    MergeResult merge(std::string_view b, std::string_view o, std::string_view t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return fmt->merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::string out(const MergeResult& r) {
        VectorSink s;
        fmt->save(*r.document, s, {ctx});
        return {reinterpret_cast<const char*>(s.data.data()), s.data.size()};
    }
    std::string merged(std::string_view b, std::string_view o, std::string_view t, ConflictOptions c = {}) {
        auto r = merge(b, o, t, std::move(c));
        CHECK(r.conflicts.empty());
        return out(r);
    }
    std::string dump(std::string_view s) {
        std::ostringstream os;
        fmt->dump(*load(s), os, {ctx});
        return os.str();
    }
};

const char* kBase = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- header -->
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="a.cs" />
    <Compile Include="b.cs" />
  </ItemGroup>
</Project>
)";

std::string replace(std::string s, std::string_view from, std::string_view to) {
    auto at = s.find(from);
    REQUIRE(at != std::string::npos);
    return s.replace(at, from.size(), to);
}

std::string utf16le(std::string_view utf8) {
    return "\xFF\xFE" + encode_utf8_as(utf8, Encoding::utf16le);
}

} // namespace

TEST_CASE("xml: untouched input re-emits byte for byte") {
    Fx fx;
    const std::string doc =
        "\xEF\xBB\xBF<?xml version='1.0'?>\r\n<!DOCTYPE r SYSTEM \"r.dtd\">\r\n"
        "<?pi data?>\r\n<r xmlns='urn:d' xmlns:p=\"urn:p\"   a = 'x &amp; y'\r\n   p:b=\"&#x41;\">\r\n"
        "  <p>mixed <b>bold</b> &lt;text&gt; &unknown;</p>\r\n  <![CDATA[ <raw> ]]>\r\n"
        "  <!-- c - d -->\r\n  <e/><e></e>\r\n</r>\r\n<!-- tail -->\r\n";
    CHECK(fx.merged(doc, doc, doc) == doc);
    const std::string edited = replace(doc, "<e/>", "<e x='1'/>");
    CHECK(fx.merged(doc, edited, doc) == edited); // merge(b, o, b) == o
    CHECK(fx.merged(doc, edited, edited) == edited);
}

TEST_CASE("xml: formatting, attribute order, quoting, and references are not differences") {
    Fx fx;
    const char* a = "<r><item id=\"a\" x=\"1\" y=\"2\"/><t>A&amp;B</t><e></e></r>";
    const char* b = "<r>\n  <item y='2'   x='1' id='a'></item>\n  <t>&#65;&#x26;B</t>\n  <e/>\n</r>";
    CHECK(fx.diff(a, b).empty());
    CHECK(fx.dump(a) == fx.dump(b));

    auto cs = fx.diff(a, "<r><item id=\"a\" x=\"5\" y=\"2\"/><t>A&amp;B</t><e/><new/></r>");
    REQUIRE(cs.size() == 2);
    CHECK(cs[0].at.path == "/r/item[@id=a]/@x");
    CHECK(cs[0].op == Op::update);
    CHECK(*cs[0].before.text == "1");
    CHECK(*cs[0].after.text == "5");
    CHECK(cs[1].at.path == "/r/new");
    CHECK(cs[1].op == Op::insert);

    // Whitespace is content under xml:space="preserve" and in mixed content.
    CHECK(fx.diff("<r xml:space='preserve'><a/></r>", "<r xml:space='preserve'> <a/></r>").size() == 1);
    CHECK(fx.diff("<p>x <b/> y</p>", "<p>x <b/>  y</p>").size() == 1);
}

TEST_CASE("xml: comments, PIs, and the declaration are compared unless ignored") {
    Fx fx;
    auto cs = fx.diff("<r><!--a--><?p x?></r>", "<?xml version='1.0'?><r><!--b--><?p y?></r>");
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].at.path == "/#decl");
    CHECK(cs[1].at.path == "/r/comment()");
    CHECK(cs[2].at.path == "/r/processing-instruction(p)");
    fx.opts["xml-ignore-comments"] = "";
    fx.opts["xml-ignore-pis"] = "";
    CHECK(fx.diff("<r><!--a--><?p x?></r>", "<r><!--b--><?p y?></r>").empty());
    fx.opts["xml-ws"] = "bogus";
    CHECK_THROWS_AS(fx.load("<r/>"), Error);
}

TEST_CASE("xml: edits in different places merge; untouched bytes stay") {
    Fx fx;
    const std::string ours = replace(kBase, "net8.0", "net9.0");
    const std::string theirs = replace(kBase, "<Compile Include=\"b.cs\" />",
                                       "<Compile Include=\"b.cs\" />\n    <Compile Include=\"c.cs\" />");
    auto r = fx.merge(kBase, ours, theirs);
    CHECK(r.conflicts.empty());
    CHECK(fx.out(r) == replace(ours, "<Compile Include=\"b.cs\" />",
                               "<Compile Include=\"b.cs\" />\n    <Compile Include=\"c.cs\" />"));
    REQUIRE(r.changes_ours.size() == 1);
    CHECK(r.changes_ours[0].at.path == "/Project/PropertyGroup/TargetFramework/text()");
    REQUIRE(r.changes_theirs.size() == 1);
    CHECK(r.changes_theirs[0].at.path == "/Project/ItemGroup/Compile[@Include=c.cs]");
    CHECK(r.auto_merged.size() == 2);
    // A change under an XML signature keeps the signature and warns (xml.md §7.1).
    const char* sig = "<r xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\"><a/><ds:Signature/></r>";
    auto rs = fx.merge(sig, sig, replace(sig, "<a/>", "<a k=\"1\"/>"));
    CHECK(std::any_of(rs.warnings.begin(), rs.warnings.end(),
                      [](auto& w) { return w.code == "signature-invalidated"; }));
    CHECK(fx.merge(sig, sig, sig).warnings.empty());
    // Deletion travels with its indentation.
    const std::string del = replace(kBase, "\n    <Compile Include=\"a.cs\" />", "");
    CHECK(fx.merged(kBase, ours, del) == replace(del, "net8.0", "net9.0"));
}

TEST_CASE("xml: attributes merge by key; the start tag keeps its spelling") {
    Fx fx;
    const char* base = "<r><item id=\"a\" x=\"1\"   y='2' z=\"3\"/></r>";
    const char* ours = "<r><item id=\"a\" x=\"10\"   y='2' z=\"3\"/></r>";
    const char* theirs = "<r><item w=\"4\" z=\"30\" y='2' x=\"1\" id=\"a\"/></r>";
    CHECK(fx.merged(base, ours, theirs) == "<r><item id=\"a\" x=\"10\"   y='2' z=\"30\" w=\"4\"/></r>");

    auto r = fx.merge(base, ours, "<r><item id=\"a\" x=\"2\"   y='2' z=\"3\"/></r>");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/r/item[@id=a]/@x");
    CHECK(r.conflicts[0].reason == "both-modified");
    CHECK(r.conflicts[0].changes.size() == 2);
    CHECK(fx.out(r) == ours);
    CHECK(fx.out(fx.merge(base, ours, "<r><item id=\"a\" x=\"2\"   y='2' z=\"3\"/></r>", {Policy::theirs})) ==
          "<r><item id=\"a\" x=\"2\"   y='2' z=\"3\"/></r>");
    auto m = fx.out(fx.merge(base, ours, "<r><item id=\"a\" x=\"2\"   y='2' z=\"3\"/></r>", {Policy::mark}));
    CHECK(m == "<r><!-- splinter-conflict " + r.conflicts[0].id +
                   " theirs: @x=\"2\" --><item id=\"a\" x=\"10\"   y='2' z=\"3\"/></r>");
}

TEST_CASE("xml: text conflicts, policies, and comment marks") {
    Fx fx;
    const std::string ours = replace(kBase, ">Exe<", ">Library<"),
                      theirs = replace(kBase, ">Exe<", ">Win--Exe-<");
    auto r = fx.merge(kBase, ours, theirs);
    REQUIRE(r.conflicts.size() == 1);
    auto& c = r.conflicts[0];
    CHECK(c.at.path == "/Project/PropertyGroup/OutputType/text()");
    CHECK(c.reason == "both-modified");
    CHECK(c.changes.size() == 2);
    CHECK(fx.out(r) == ours);
    CHECK(fx.out(fx.merge(kBase, ours, theirs, {Policy::theirs})) == theirs);
    CHECK(fx.out(fx.merge(kBase, ours, theirs, {Policy::base})) == kBase);
    auto marked = fx.merge(kBase, ours, theirs, {Policy::mark});
    CHECK(marked.unresolved_conflicts() == 1);
    CHECK(fx.out(marked) ==
          replace(ours, ">Library<", "><!-- splinter-conflict " + c.id + " theirs: Win-\\-Exe- -->Library<"));
    CHECK(fx.out(fx.merge(kBase, ours, theirs, {Policy::ours, ResolveAll::none, {{c.id, "theirs"}}})) ==
          theirs);
}

TEST_CASE("xml: insertions at one place conflict unless the dialect says order is free") {
    Fx fx;
    const char* base = "<r><a/></r>";
    auto r = fx.merge(base, "<r><a/><b/></r>", "<r><a/><c/></r>");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "add-add");
    CHECK(fx.out(fx.merge(base, "<r><a/><b/></r>", "<r><a/><c/></r>", {Policy::mark})) ==
          "<r><a/><!-- splinter-conflict " + r.conflicts[0].id + " theirs: <c/> --><b/></r>");

    struct Unordered : xml::Dialect {
        bool unordered(const xml::Node&) const override { return true; }
    };
    auto f = xml::make_format({"xmlu", "test", {".xu"}, {}, {}}, std::make_shared<Unordered>());
    auto ld = [&](const char* s) { return f->load(ByteSource::from_memory(vec(s)), {fx.ctx}); };
    auto db = ld(base), dob = ld("<r><a/><b/></r>"), dt = ld("<r><a/><c/></r>");
    auto u = f->merge(*db, *dob, *dt, MergeOptions{fx.ctx, {}, {}});
    CHECK(u.conflicts.empty());
    VectorSink s;
    f->save(*u.document, s, {fx.ctx});
    CHECK(std::string(reinterpret_cast<const char*>(s.data.data()), s.data.size()) == "<r><a/><b/><c/></r>");
}

TEST_CASE("xml: id'd elements move; similarity pairs elements without ids") {
    Fx fx;
    const char* base = "<r><i id=\"1\"/><i id=\"2\"/><i id=\"3\"/></r>";
    const char* ours = "<r><i id=\"3\"/><i id=\"1\"/><i id=\"2\"/></r>";
    auto cs = fx.diff(base, ours);
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].op == Op::move);
    CHECK(fx.merged(base, ours, "<r><i id=\"1\" v=\"x\"/><i id=\"2\"/><i id=\"3\"/></r>") ==
          "<r><i id=\"3\"/><i id=\"1\" v=\"x\"/><i id=\"2\"/></r>");

    // No ids: same-name elements pair by similarity; >= 0.8 merges two-sided edits, below conflicts.
    const char* b5 = "<r><p a=\"1\" b=\"2\" c=\"3\" d=\"4\" e=\"5\"/><p/></r>";
    CHECK(fx.merged(b5, "<r><p a=\"1\" b=\"2\" c=\"3\" d=\"4\" e=\"6\"/><p/></r>",
                    "<r><p a=\"9\" b=\"2\" c=\"3\" d=\"4\" e=\"5\"/><p/></r>") ==
          "<r><p a=\"9\" b=\"2\" c=\"3\" d=\"4\" e=\"6\"/><p/></r>");
    const char* b4 = "<r><p a=\"1\" b=\"2\" c=\"3\" d=\"4\"/><p/></r>";
    const char* o4 = "<r><p a=\"1\" b=\"2\" c=\"3\" d=\"5\"/><p/></r>";
    auto r = fx.merge(b4, o4, "<r><p a=\"9\" b=\"2\" c=\"3\" d=\"4\"/><p/></r>");
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "low-confidence-match");
    auto d = fx.diff(b4, o4);
    REQUIRE(d.size() == 1);
    CHECK(d[0].at.path == "/r/p[1]/@d");
    CHECK(d[0].confidence == 0.75);

    // A self-closing element of ours that gains theirs' child is opened.
    CHECK(fx.merged("<r><a id=\"x\"/></r>", "<r><a id=\"x\" k=\"1\"/></r>", "<r><a id=\"x\"><c/></a></r>") ==
          "<r><a id=\"x\" k=\"1\"><c/></a></r>");
}

TEST_CASE("xml: namespaces resolve by URI; fragments get the declarations they need") {
    Fx fx;
    CHECK(fx.diff("<r xmlns:a='urn:x'><a:e/></r>", "<r xmlns:b='urn:x'><b:e/></r>").empty());
    fx.opts["xml-strict-prefixes"] = "";
    CHECK(!fx.diff("<r xmlns:a='urn:x'><a:e/></r>", "<r xmlns:b='urn:x'><b:e/></r>").empty());
    fx.opts.clear();
    // Ours drops an unused declaration; theirs starts using it below.
    CHECK(fx.merged("<r xmlns:x=\"urn:x\"><g/></r>", "<r><g/><h/></r>",
                    "<r xmlns:x=\"urn:x\"><g><x:a/></g></r>") ==
          "<r><g xmlns:x=\"urn:x\"><x:a/></g><h/></r>");
    CHECK_THROWS_AS(fx.load("<p:r/>"), Error); // unbound prefix
}

TEST_CASE("xml: UTF-16 and ISO-8859-1 inputs keep their encoding") {
    Fx fx;
    const std::string b = "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<r>\r\n  <a x=\"1\"/>\r\n</r>\r\n";
    const std::string o = replace(b, "x=\"1\"", "x=\"\xC3\xA9\"");
    const std::string t = replace(b, "<a x=\"1\"/>", "<a x=\"1\"/>\r\n  <b>\xE2\x82\xAC</b>");
    const std::string want =
        replace(o, "<a x=\"\xC3\xA9\"/>", "<a x=\"\xC3\xA9\"/>\r\n  <b>\xE2\x82\xAC</b>");
    CHECK(fx.merged(utf16le(b), utf16le(o), utf16le(t)) == utf16le(want));
    CHECK(fx.merged(utf16le(b), utf16le(o), utf16le(o)) == utf16le(o));
    // Theirs in UTF-8: its fragment is transcoded into ours' encoding.
    const std::string t8 = replace(t, "encoding=\"UTF-16\"", "encoding=\"UTF-8\"");
    CHECK(fx.merged(utf16le(b), utf16le(o), t8) == utf16le(want));
    auto r8 = fx.merge(utf16le(b), utf16le(o), t8);
    CHECK(std::any_of(r8.warnings.begin(), r8.warnings.end(),
                      [](auto& w) { return w.code == "decl-kept-ours"; }));
    CHECK(std::none_of(r8.auto_merged.begin(), r8.auto_merged.end(), [&](auto& a) {
        return a.change == r8.changes_theirs.front().id;
    })); // /#decl sorts first
    CHECK(fx.diff(utf16le(o), replace(o, "encoding=\"UTF-16\"", "encoding=\"UTF-8\"")).size() ==
          1); // decl only

    // ISO-8859-1: a character it cannot hold becomes a character reference.
    const std::string lb = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><r><a>caf\xE9</a></r>";
    const std::string lo = replace(lb, "<a>", "<a k=\"1\">");
    const std::string lt = "<?xml version=\"1.0\"?><r><a>caf\xC3\xA9</a><b>\xE2\x82\xAC</b></r>";
    CHECK(fx.merged(lb, lo, lt) == replace(lo, "</a>", "</a><b>&#8364;</b>"));
}

TEST_CASE("xml: entities and resource limits are refused") {
    Fx fx;
    auto fails = [&](const std::string& s, ErrorCategory cat) {
        try {
            fx.load(s);
        } catch (const Error& e) {
            CHECK(e.category() == cat);
            return true;
        }
        return false;
    };
    CHECK(fails("<!DOCTYPE r [<!ENTITY a \"x\">]><r>&a;</r>", ErrorCategory::invalid_input));
    CHECK(fails("<!DOCTYPE r [<!ENTITY a \"x\"><!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">]><r>&b;</r>",
                ErrorCategory::invalid_input));
    CHECK(fails("<!DOCTYPE r [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><r>&x;</r>",
                ErrorCategory::invalid_input));
    CHECK(fails("<!DOCTYPE r [<!ENTITY % p \"x\">]><r/>", ErrorCategory::invalid_input));
    CHECK(fails("<r>&undeclared;</r>", ErrorCategory::invalid_input));
    CHECK(fails("<r><a></r>", ErrorCategory::invalid_input));
    std::string deep;
    for (int i = 0; i < 600; ++i)
        deep += "<e>";
    CHECK(fails(deep, ErrorCategory::limit));
    std::string wide = "<r";
    for (int i = 0; i < 5000; ++i)
        wide += " a" + std::to_string(i) + "=\"\"";
    CHECK(fails(wide + "/>", ErrorCategory::limit));
    Limits tiny;
    tiny.max_memory = 1000;
    Context small{fx.reg, tiny, 0, fx.opts};
    CHECK_THROWS_AS(
        fx.fmt->load(ByteSource::from_memory(vec("<r>" + std::string(2000, 'x') + "</r>")), {small}), Error);
    // An external DTD is never read.
    CHECK(fx.dump("<!DOCTYPE r SYSTEM \"file:///nonexistent.dtd\"><r/>") ==
          "<!DOCTYPE r SYSTEM \"file:///nonexistent.dtd\">\n<r/>\n");
}

TEST_CASE("xml: deep merges recurse without trouble") {
    Fx fx;
    std::string open, close;
    for (int i = 0; i < 511; ++i) // the deepest nesting the loader accepts
        open += "<e id=\"" + std::to_string(i) + "\">", close += "</e>";
    const std::string b = open + "<leaf a=\"1\" b=\"1\"/>" + close;
    const std::string o = replace(b, "a=\"1\"", "a=\"2\""), t = replace(b, "b=\"1\"", "b=\"2\"");
    const std::string dumped = fx.dump(o);
    CHECK(std::count(dumped.begin(), dumped.end(), '\n') == 2 * 511 + 1);
    CHECK(fx.diff(b, o).size() == 1);
    CHECK(fx.merged(b, o, t) == replace(o, "b=\"1\"", "b=\"2\""));
}

TEST_CASE("xml: probe and detection") {
    Fx fx;
    auto detect = [&](std::string_view s, std::string_view name) {
        return std::string(fx.reg.detect(as_bytes(s), name).info().name);
    };
    CHECK(detect("<?xml version='1.0'?><a/>", "x") == "xml");
    CHECK(detect("  <!-- c --><a b='1'><c/></a>", "notes.txt") == "xml");
    CHECK(detect(utf16le("<a/>"), "x") == "xml");
    CHECK(detect("<html><br></html>", "x.xml") == "text");
    CHECK(detect("plain text", "x") == "text");
    CHECK(detect("<!DOCTYPE r [<!ENTITY a \"x\">]><r/>", "x") == "text");
}

TEST_CASE("xml: dump is canonical and line-oriented") {
    Fx fx;
    const char* d =
        "<?xml version='1.0'?><r xmlns:b='urn:b' z='1' a='&lt;'><!--x\ny--><t>line1\nline2</t><b:e/></r>";
    CHECK(fx.dump(d) ==
          "<?xml version=\"1.0\"?>\n<r xmlns:b=\"urn:b\" a=\"&lt;\" z=\"1\">\n  <!--x&#10;y-->\n"
          "  <t>\n    line1\nline2\n  </t>\n  <b:e/>\n</r>\n");
    const char* dt =
        "<?xml version='1.0'?>\n<!--c-->\n<!DOCTYPE r [\n<!ELEMENT r ANY>\n<!-- in subset -->\n]>\n<r/>";
    CHECK(fx.dump(dt) == "<?xml version=\"1.0\"?>\n<!--c-->\n<!DOCTYPE r [&#10;<!ELEMENT r ANY>&#10;"
                         "<!-- in subset -->&#10;]>\n<r/>\n");
    auto cs = fx.diff(dt, replace(dt, "ANY", "EMPTY"));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path == "/#doctype");
    fx.opts["dump-attr-lines"] = "";
    CHECK(fx.dump("<r a='1' b='2'/>") == "<r\n    a=\"1\"\n    b=\"2\"/>\n");
}

namespace {

// Random trees and edit scripts for the xml.md §11 properties.
struct El {
    std::string name, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<El> kids;
};

struct Gen {
    std::mt19937 rng;
    int ids = 0;
    int pick(int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); }
    std::string word() { return std::string(1, "abc"[pick(3)]) + std::to_string(pick(5)); }
    El tree(int depth) {
        El e{std::string(1, "pqr"[pick(3)]), {}, {}, {}};
        if (pick(2))
            e.attrs.emplace_back("id", "i" + std::to_string(ids++));
        if (pick(2))
            e.attrs.emplace_back("x", word());
        if (depth < 3 && pick(4))
            for (int k = pick(4); k-- > 0;)
                e.kids.push_back(tree(depth + 1));
        else if (pick(2))
            e.text = word() + " " + word();
        return e;
    }
    void all(El& e, std::vector<El*>& v) {
        v.push_back(&e);
        for (auto& k : e.kids)
            all(k, v);
    }
    void edit(El& root) {
        std::vector<El*> v;
        all(root, v);
        El& e = *v[static_cast<std::size_t>(pick(static_cast<int>(v.size())))];
        switch (pick(4)) {
        case 0: {
            auto name = "y" + std::to_string(pick(1000));
            if (std::none_of(e.attrs.begin(), e.attrs.end(), [&](auto& a) { return a.first == name; }))
                e.attrs.emplace_back(name, word());
            break;
        }
        case 1:
            if (!e.text.empty())
                e.text = word();
            else
                e.kids.insert(e.kids.begin() + pick(static_cast<int>(e.kids.size()) + 1), tree(3));
            break;
        case 2:
            if (!e.kids.empty())
                e.kids.erase(e.kids.begin() + pick(static_cast<int>(e.kids.size())));
            break;
        default:
            for (auto& [k, val] : e.attrs)
                if (k == "x")
                    val = word();
            break;
        }
    }
};

// `style` varies indentation, quotes, and empty-element spelling without changing content.
void write(const El& e, int depth, int style, std::string& out) {
    const char q = style & 1 ? '\'' : '"';
    const std::string ind = style & 2 ? "\n" + std::string(static_cast<std::size_t>(2 * depth), ' ') : "";
    out += ind + "<" + e.name;
    for (auto& [k, v] : e.attrs)
        out += std::string(style & 4 ? "  " : " ") + k + "=" + q + v + q;
    if (e.kids.empty() && e.text.empty()) {
        out += style & 8 ? "></" + e.name + ">" : "/>";
        return;
    }
    out += ">" + e.text;
    for (auto& k : e.kids)
        write(k, depth + 1, style, out);
    out += (e.kids.empty() ? "" : ind) + "</" + e.name + ">";
}

std::string xml_of(const El& e, int style) {
    std::string s;
    write(e, 0, style, s);
    return s + "\n";
}

struct Scratch {
    std::filesystem::path dir;
    explicit Scratch(const std::string& name) {
        dir = scratch_path("test-" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string file(const std::string& name, std::string_view content) const {
        std::ofstream(dir / name, std::ios::binary)
            .write(content.data(), static_cast<std::streamsize>(content.size()));
        return splinter::cli::from_path(dir / name);
    }
};

} // namespace

TEST_CASE("xml: properties over random trees and edits") {
    Fx fx;
    for (unsigned seed = 1; seed <= 150; ++seed) {
        INFO("seed " << seed);
        Gen g{std::mt19937(seed)};
        El base = g.tree(0), ours = base, theirs = base;
        for (int k = g.pick(3) + 1; k-- > 0;)
            g.edit(ours);
        for (int k = g.pick(3) + 1; k-- > 0;)
            g.edit(theirs);
        const int sb = g.pick(16), so = g.pick(16), st = g.pick(16);
        const auto b = xml_of(base, sb), o = xml_of(ours, so), t = xml_of(theirs, st);
        CHECK(fx.diff(b, xml_of(base, so)).empty()); // reformatting is no change
        CHECK(fx.merged(b, b, b) == b);
        CHECK(fx.merged(b, o, b) == o);
        CHECK(fx.merged(b, o, o) == o);
        CHECK(fx.diff(fx.merged(b, b, t), xml_of(theirs, sb)).empty());
        for (auto policy : {Policy::ours, Policy::theirs, Policy::base, Policy::mark})
            CHECK_NOTHROW(fx.merge(b, o, t, {policy})); // the writer validates its output by parsing it
    }
}

TEST_CASE("cli: xml diff, unified dump diff, and merge") {
    Scratch s("xml");
    const std::string ours = replace(kBase, "net8.0", "net9.0");
    const std::string theirs = replace(kBase, "<Compile Include=\"b.cs\" />",
                                       "<Compile Include=\"b.cs\" />\n    <Compile Include=\"c.cs\" />");
    auto b = s.file("base.csproj", kBase), o = s.file("ours.csproj", ours),
         t = s.file("theirs.csproj", theirs);

    CaptureConsole d;
    CHECK(run_cli(d, {b, o}) == 1);
    CHECK(d.out_text == "~ /Project/PropertyGroup/TargetFramework/text() net8.0 -> net9.0\n");
    CaptureConsole u;
    CHECK(run_cli(u, {"-u", "--label", "a", "--label", "b", b, o}) == 1);
    CHECK(u.out_text.find(" @@ /Project/PropertyGroup/TargetFramework/text()\n") != std::string::npos);
    CHECK(u.out_text.find("-      net8.0\n+      net9.0\n") != std::string::npos);
    CaptureConsole same;
    CHECK(
        run_cli(same, {b, s.file("reformatted.csproj", "<?xml version='1.0' encoding='UTF-8'?><!-- header -->"
                                                       "<Project Sdk='Microsoft.NET.Sdk'><PropertyGroup>"
                                                       "<OutputType>Exe</OutputType><TargetFramework>net8.0"
                                                       "</TargetFramework></PropertyGroup><ItemGroup>"
                                                       "<Compile Include='a.cs'/><Compile Include='b.cs'/>"
                                                       "</ItemGroup></Project>")}) == 0);

    CaptureConsole m;
    CHECK(run_cli(m, {"-m", o, b, t}) == 0);
    CHECK(m.out_text == replace(ours, "<Compile Include=\"b.cs\" />",
                                "<Compile Include=\"b.cs\" />\n    <Compile Include=\"c.cs\" />"));

    auto t2 = s.file("t2.csproj", replace(kBase, "net8.0", "net7.0"));
    CaptureConsole j;
    CHECK(run_cli(j, {"-m", "--conflict=mark", "--json=-", o, b, t2}) == 1);
    auto at = j.out_text.find("\n{");
    REQUIRE(at != std::string::npos);
    auto rep = nlohmann::json::parse(j.out_text.substr(at + 1));
    CHECK(rep["format"] == "xml");
    REQUIRE(rep["conflicts"].size() == 1);
    CHECK(rep["conflicts"][0]["resolved_as"] == "mark");
    CHECK(j.out_text.find("<!-- splinter-conflict") != std::string::npos);
}
