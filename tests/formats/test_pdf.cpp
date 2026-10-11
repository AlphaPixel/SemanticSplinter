#include "../cli/console.hpp"
#include "../scratch.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/error.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/formats/pdf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace semsplinter;

namespace {

// ---- Hand-written fixtures (pdf.md §11) ----

std::string stream(const std::string& data, const std::string& dict = "") {
    return "<< /Length " + std::to_string(data.size()) + dict + " >>\nstream\n" + data + "\nendstream";
}

// Objects 1..n, a classic xref table, and a trailer.
std::string pdf_file(const std::vector<std::string>& objs, const std::string& trailer,
                     const char* version = "1.7") {
    std::string s = std::string("%PDF-") + version + "\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> off;
    for (std::size_t i = 0; i < objs.size(); ++i) {
        off.push_back(s.size());
        s += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const auto x = s.size();
    s += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f\r\n";
    for (auto o : off) {
        char e[32];
        std::snprintf(e, sizeof e, "%010zu 00000 n\r\n", o);
        s += e;
    }
    s += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " " + trailer + " >>\nstartxref\n" +
         std::to_string(x) + "\n%%EOF\n";
    return s;
}

std::uint64_t last_startxref(const std::string& f) {
    return std::stoull(f.substr(f.rfind("startxref") + 10));
}

// An incremental update appended by hand (a classic xref section).
std::string append_update(const std::string& f, const std::map<int, std::string>& objs, int size,
                          const std::string& trailer) {
    std::string s = f;
    std::map<int, std::size_t> off;
    for (auto& [n, body] : objs) {
        off[n] = s.size();
        s += std::to_string(n) + " 0 obj\n" + body + "\nendobj\n";
    }
    const auto x = s.size();
    s += "xref\n";
    for (auto& [n, o] : off) {
        char e[48];
        std::snprintf(e, sizeof e, "%d 1\n%010zu 00000 n\r\n", n, o);
        s += e;
    }
    s += "trailer\n<< /Size " + std::to_string(size) + " /Prev " + std::to_string(last_startxref(f)) + " " +
         trailer + " >>\nstartxref\n" + std::to_string(x) + "\n%%EOF\n";
    return s;
}

struct Fix {
    std::string title = "Doc", author = "Alice", version = "1.7";
    std::vector<std::string> pages = {"(Hello World) Tj", "(Second page) Tj 0 -14 Td (more text) Tj"};
    std::vector<std::vector<std::string>> annots = {{"/Subtype /Text /Rect [72 650 92 670] /Contents (Typo?) "
                                                     "/T (Bob) /NM (a1)"},
                                                    {}};
    std::string field = "(Alice)";
    std::string xmp;
    bool sig = false;
    std::string sig_extra, extra_root;
};

// 1 catalog, 2 pages, 3 font, 4 info, 5 text field (widget on page 1), 6 AcroForm, [7 XMP], [8 sig field,
// 9 sig value], then per page: page, content, annotations.
std::string make_pdf(const Fix& f) {
    std::vector<std::string> o(6);
    const int xmp = f.xmp.empty() ? 0 : static_cast<int>(o.size()) + 1;
    if (xmp)
        o.push_back(stream(f.xmp, " /Type /Metadata /Subtype /XML"));
    int sig = 0;
    if (f.sig) {
        sig = static_cast<int>(o.size()) + 1;
        o.push_back("");
        o.push_back(
            "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 10 20 30] "
            "/Contents <00000000>" +
            f.sig_extra + " >>");
    }
    std::string kids;
    std::vector<int> page_ids;
    for (std::size_t p = 0; p < f.pages.size(); ++p) {
        const int id = static_cast<int>(o.size()) + 1;
        page_ids.push_back(id);
        std::string annots = p == 0 ? "5 0 R " : "";
        if (p == 0 && sig)
            annots += std::to_string(sig) + " 0 R ";
        const auto& list = p < f.annots.size() ? f.annots[p] : std::vector<std::string>{};
        for (std::size_t a = 0; a < list.size(); ++a)
            annots += std::to_string(id + 2 + static_cast<int>(a)) + " 0 R ";
        o.push_back(
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> "
            "/Contents " +
            std::to_string(id + 1) + " 0 R /Annots [" + annots + "] >>");
        o.push_back(stream("BT /F1 12 Tf 72 700 Td " + f.pages[p] + " ET"));
        for (auto& a : list)
            o.push_back("<< /Type /Annot " + a + " /P " + std::to_string(id) + " 0 R >>");
        kids += std::to_string(id) + " 0 R ";
    }
    o[0] = "<< /Type /Catalog /Pages 2 0 R /AcroForm 6 0 R" +
           (xmp ? " /Metadata " + std::to_string(xmp) + " 0 R" : std::string()) + f.extra_root + " >>";
    o[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(f.pages.size()) + " >>";
    o[2] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    o[3] = "<< /Title (" + f.title + ") /Author (" + f.author + ") /Producer (fixture) >>";
    o[4] = "<< /FT /Tx /T (name) /V " + f.field +
           " /DA (/Helv 12 Tf 0 g) /Type /Annot /Subtype /Widget "
           "/Rect [72 600 272 620] /P " +
           std::to_string(page_ids[0]) + " 0 R >>";
    o[5] = "<< /Fields [5 0 R" + (sig ? " " + std::to_string(sig) + " 0 R" : std::string()) +
           "] /DR << /Font << /Helv 3 0 R >> >> /DA (/Helv 12 Tf 0 g)" + (sig ? " /SigFlags 3" : "") + " >>";
    if (sig)
        o[static_cast<std::size_t>(sig - 1)] = "<< /FT /Sig /T (approval) /V " + std::to_string(sig + 1) +
                                               " 0 R /Type /Annot /Subtype /Widget /Rect [0 0 0 0] /P " +
                                               std::to_string(page_ids[0]) + " 0 R >>";
    return pdf_file(o,
                    "/Root 1 0 R /Info 4 0 R /ID [<0123456789abcdef0123456789abcdef> "
                    "<0123456789abcdef0123456789abcdef>]",
                    f.version.c_str());
}

std::vector<std::byte> bytes(std::string_view s) {
    auto b = as_bytes(s);
    return {b.begin(), b.end()};
}
std::string text(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format& pdf = *reg.find("pdf");

    std::unique_ptr<Document> load(const std::string& b) {
        return pdf.load(ByteSource::from_memory(bytes(b), "x.pdf"), {ctx});
    }
    ChangeSet diff(const std::string& a, const std::string& b) { return pdf.diff(*load(a), *load(b), {ctx}); }
    MergeResult merge(const std::string& b, const std::string& o, const std::string& t,
                      ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return pdf.merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::string out(const MergeResult& r) {
        VectorSink s;
        pdf.save(*r.document, s, {ctx});
        return text(s.data);
    }
    std::string dump(const std::string& b) {
        std::ostringstream os;
        pdf.dump(*load(b), os, {ctx});
        return os.str();
    }
};

const Change* at(const ChangeSet& cs, std::string_view path) {
    for (auto& c : cs)
        if (c.at.path == path)
            return &c;
    return nullptr;
}
bool warned(const MergeResult& r, std::string_view code) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [&](const Warning& w) { return w.code == code; });
}
std::size_t count(const std::string& s, std::string_view needle) {
    std::size_t n = 0;
    for (auto p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1))
        ++n;
    return n;
}

const std::string kNote2 =
    "/Subtype /Text /Rect [100 500 120 520] /Contents (Second note) /T (Carol) /NM (a2)";
const std::string kNote3 = "/Subtype /Highlight /Rect [72 690 160 712] /Contents (Hi) /T (Dan) /NM (a3)";

} // namespace

TEST_CASE("pdf: probe, load, and dump") {
    Fx fx;
    const auto f = make_pdf({});
    CHECK(fx.pdf.probe(as_bytes(f), "x.pdf").confidence == 95);
    CHECK(fx.pdf.probe(as_bytes("junk\n" + f), "x").confidence == 70);
    CHECK(fx.pdf.probe(as_bytes("%PDF-3.0\n"), "x").confidence == 0);
    CHECK(&fx.reg.detect(as_bytes(f), "a.bin") == &fx.pdf);
    CHECK_THROWS_AS(fx.load("not a pdf"), Error);

    const auto d = fx.dump(f);
    CHECK(d.starts_with("pdf 1.7 pages=2 revisions=1 signed=0 encrypted=no\n"));
    CHECK(d.find("info Author=\"Alice\" Title=\"Doc\"\n") != std::string::npos); // /Producer is noise
    CHECK(d.find("== page 1 612x792 rotate=0\nHello World\n== page 1 annots\nText @ 72,650,20,20 by \"Bob\": "
                 "\"Typo?\"\n") != std::string::npos);
    CHECK(d.find("== page 2 612x792 rotate=0\nSecond page\nmore text\n") != std::string::npos);
    CHECK(d.find("== fields\nname (Tx) = \"Alice\"\n") != std::string::npos);
    CHECK(fx.dump(f) == d); // deterministic

    auto doc = fx.load(f);
    auto* map = fx.pdf.envelope(*doc);
    REQUIRE(map);
    map->validate(f.size());
    CHECK(map->ranges.front().cls == ByteClass::framing); // the header
    auto j = fx.pdf.inspect(*doc);
    CHECK(j["pages"] == 2);
    CHECK(j["xref"] == "table");
}

TEST_CASE("pdf: text extraction through a ToUnicode CMap and TJ spacing") {
    Fx fx;
    const std::string cmap = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
                             "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                             "2 beginbfchar <0001> <0048> <0002> <00E9> endbfchar\n"
                             "1 beginbfrange <0010> <0012> <0061> endbfrange\n"
                             "endcmap CMapName currentdict /CMap defineresource pop end end";
    std::vector<std::string> o{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << /Font << /F1 5 0 R /F2 7 0 R >> "
        ">> "
        "/Contents 4 0 R >>",
        stream("BT /F1 10 Tf 10 100 Td <000100020010> Tj [<0011> -500 <0012>] TJ ET "
               "BT /F2 10 Tf 10 80 Td (caf\xE9 \x93q\x94) Tj ET"),
        "<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H /ToUnicode 6 0 R >>",
        stream(cmap),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"};
    const auto d = fx.dump(pdf_file(o, "/Root 1 0 R"));
    CHECK(d.find("== page 1 200x200 rotate=0\nH\xC3\xA9"
                 "a"
                 "b c\ncaf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D\n") != std::string::npos);
}

TEST_CASE("pdf: diff layers") {
    Fx fx;
    const auto base = make_pdf({});
    Fix f;
    f.title = "Doc 2";
    f.pages[1] = "(Second page) Tj 0 -14 Td (much more text) Tj";
    f.annots[0].push_back(kNote2);
    f.field = "(Bob)";
    auto cs = fx.diff(base, make_pdf(f));
    REQUIRE(at(cs, "/info/Title"));
    CHECK(at(cs, "/info/Title")->after.text == "\"Doc 2\"");
    auto* t = at(cs, "/pages/p:2/text");
    REQUIRE(t);
    CHECK(t->metrics["words_inserted"] == 1);
    CHECK(t->after.text == "much");
    REQUIRE(at(cs, "/pages/p:1/annots/a2"));
    CHECK(at(cs, "/pages/p:1/annots/a2")->op == Op::insert);
    REQUIRE(at(cs, "/fields/name"));
    CHECK(at(cs, "/fields/name")->after.text == "Tx = \"Bob\"");
    CHECK(!at(cs, "/pages/p:1/text"));
    CHECK(fx.diff(base, base).empty());

    SECTION("noise is hidden unless asked") {
        auto noisy = base;
        noisy.replace(noisy.find("(fixture)"), 9, "(fixturX)");
        CHECK(fx.diff(base, noisy).empty());
        fx.opts["pdf-show-noise"] = "";
        CHECK(at(fx.diff(base, noisy), "/info/Producer"));
    }
    SECTION("pages inserted, deleted, and moved") {
        Fix three;
        three.pages = {"(Hello World) Tj", "(Inserted) Tj", "(Second page) Tj 0 -14 Td (more text) Tj"};
        three.annots.resize(3);
        auto c3 = fx.diff(base, make_pdf(three));
        REQUIRE(at(c3, "/pages/p:new:theirs:2"));
        CHECK(at(c3, "/pages/p:new:theirs:2")->op == Op::insert);
        auto back = fx.diff(make_pdf(three), base);
        REQUIRE(at(back, "/pages/p:2"));
        CHECK(at(back, "/pages/p:2")->op == Op::erase);

        Fix swapped;
        swapped.pages = {"(Second page) Tj 0 -14 Td (more text) Tj", "(Hello World) Tj"};
        swapped.annots = {{}, {}};
        Fix plain;
        plain.annots = {{}, {}};
        auto mv = fx.diff(make_pdf(plain), make_pdf(swapped));
        CHECK(std::any_of(mv.begin(), mv.end(), [](const Change& c) { return c.op == Op::move; }));
    }
    SECTION("layer selection") {
        fx.opts["pdf-layers"] = "forms";
        auto only = fx.diff(base, make_pdf(f));
        REQUIRE(only.size() == 1);
        CHECK(only[0].at.path == "/fields/name");
        fx.opts["pdf-layers"] = "visual";
        CHECK_THROWS_AS(fx.diff(base, base), Error);
    }
}

TEST_CASE("pdf: a merge that changes nothing is byte-identical, with no appended section") {
    Fx fx;
    const auto base = make_pdf({});
    Fix f;
    f.title = "Ours";
    const auto ours = make_pdf(f);
    auto r = fx.merge(base, ours, base);
    CHECK(r.conflicts.empty());
    CHECK(fx.out(r) == ours);
    auto same = fx.merge(base, base, base);
    CHECK(fx.out(same) == base);
    auto both = fx.merge(base, ours, ours);
    CHECK(fx.out(both) == ours);
}

TEST_CASE("pdf: annotations from both sides merge into an incremental update") {
    Fx fx;
    const auto base = make_pdf({});
    Fix fo, ft;
    fo.annots[0].push_back(kNote2);
    ft.annots[0].push_back(kNote3);
    ft.author = "Zoe";
    const auto ours = make_pdf(fo), theirs = make_pdf(ft);
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    const auto out = fx.out(r);
    REQUIRE(out.size() > ours.size());
    CHECK(out.compare(0, ours.size(), ours) == 0); // every carrier byte is a prefix (pdf.md §11)
    CHECK(count(out, "%%EOF") == 2);
    const auto d = fx.dump(out);
    CHECK(d.find("Text @ 72,650,20,20 by \"Bob\"") != std::string::npos);
    CHECK(d.find("Text @ 100,500,20,20 by \"Carol\"") != std::string::npos);
    CHECK(d.find("Highlight @ 72,690,88,22 by \"Dan\"") != std::string::npos);
    CHECK(d.find("Author=\"Zoe\"") != std::string::npos);
    CHECK(d.find("revisions=2") != std::string::npos);
    CHECK(fx.diff(out, theirs).size() == 1); // only ours' note differs from theirs
    CHECK(!r.auto_merged.empty());

    SECTION("deterministic") {
        CHECK(fx.out(fx.merge(base, ours, theirs)) == out);
    }
    SECTION("a deletion by the non-carrier side applies") {
        Fix del;
        del.annots[0].clear();
        auto rd = fx.merge(base, ours, make_pdf(del));
        const auto dd = fx.dump(fx.out(rd));
        CHECK(dd.find("\"Bob\"") == std::string::npos);
        CHECK(dd.find("\"Carol\"") != std::string::npos);
    }
}

TEST_CASE("pdf: replies follow their parent and popups are copied") {
    Fx fx;
    const auto base = make_pdf({});
    Fix ft;
    ft.annots[0].push_back(
        "/Subtype /Text /Rect [72 650 92 670] /Contents (Agreed) /T (Eve) /NM (r1) /IRT 9 0 R "
        "/Popup 11 0 R");
    ft.annots[0].push_back("/Subtype /Popup /Rect [100 600 200 650] /Parent 10 0 R");
    const auto theirs = make_pdf(ft);
    // Object 9 is a1, the first annotation of page 1, in every fixture without XMP or signature.
    REQUIRE(theirs.find("9 0 obj\n<< /Type /Annot /Subtype /Text /Rect [72 650 92 670] /Contents (Typo?)") !=
            std::string::npos);
    Fix fo;
    fo.title = "Ours";
    const auto ours = make_pdf(fo);
    auto r = fx.merge(base, ours, theirs);
    CHECK(r.conflicts.empty());
    const auto out = fx.out(r);
    CHECK(out.find("/IRT 9 0 R", ours.size()) != std::string::npos); // the copy replies to the carrier's a1
    CHECK(out.find("/Subtype /Popup", ours.size()) != std::string::npos);
    CHECK(fx.diff(out, theirs).size() == 1); // only the title differs
}

TEST_CASE("pdf: annotation conflicts and the carrier") {
    Fx fx;
    const auto base = make_pdf({});
    SECTION("both modify one annotation") {
        Fix fo, ft;
        fo.annots[0][0] = "/Subtype /Text /Rect [72 650 92 670] /Contents (Ours) /T (Bob) /NM (a1)";
        ft.annots[0][0] = "/Subtype /Text /Rect [72 650 92 670] /Contents (Theirs) /T (Bob) /NM (a1)";
        auto r = fx.merge(base, make_pdf(fo), make_pdf(ft));
        REQUIRE(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].at.path == "/pages/p:1/annots/a1");
        CHECK(r.conflicts[0].reason == "both-modified");
        CHECK(fx.dump(fx.out(r)).find("\"Ours\"") != std::string::npos);
        ConflictOptions c;
        c.policy = Policy::theirs;
        auto rt = fx.merge(base, make_pdf(fo), make_pdf(ft), c);
        CHECK(fx.dump(fx.out(rt)).find("\"Theirs\"") != std::string::npos);
        CHECK(warned(rt, "derived-stale") == false); // no /AP in the fixture
    }
    SECTION("theirs changes page content: theirs is the carrier, ours' notes transfer to unchanged pages") {
        Fix fo, ft;
        fo.annots[0].push_back(kNote2);
        fo.annots[1].push_back("/Subtype /Text /Rect [1 1 5 5] /Contents (On p2) /NM (p2note)");
        ft.pages[1] = "(Second page, rewritten) Tj";
        const auto theirs = make_pdf(ft);
        auto r = fx.merge(base, make_pdf(fo), theirs);
        CHECK(warned(r, "carrier"));
        REQUIRE(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].at.path == "/pages/p:2/annots/p2note");
        CHECK(r.conflicts[0].reason == "dependency");
        const auto out = fx.out(r);
        CHECK(out.compare(0, theirs.size(), theirs) == 0);
        const auto d = fx.dump(out);
        CHECK(d.find("Second page, rewritten") != std::string::npos);
        CHECK(d.find("\"Carol\"") != std::string::npos);
        CHECK(d.find("On p2") == std::string::npos);
    }
    SECTION("both change page content: conflict at /pages, policy picks the carrier") {
        Fix fo, ft;
        fo.pages[0] = "(Ours) Tj";
        ft.pages[1] = "(Theirs) Tj";
        auto r = fx.merge(base, make_pdf(fo), make_pdf(ft));
        REQUIRE(!r.conflicts.empty());
        CHECK(r.conflicts[0].at.path == "/pages");
        CHECK(warned(r, "content-not-merged"));
        CHECK(fx.out(r) == make_pdf(fo));
        ConflictOptions c;
        c.resolutions[r.conflicts[0].id] = "theirs";
        auto rt = fx.merge(base, make_pdf(fo), make_pdf(ft), c);
        CHECK(fx.out(rt) == make_pdf(ft));
        CHECK(rt.conflicts[0].resolved);
        CHECK(!warned(rt, "resolve-unmatched"));
    }
}

TEST_CASE("pdf: form field values merge onto the carrier") {
    Fx fx;
    const auto base = make_pdf({});
    Fix fo, ft;
    fo.title = "Ours";
    ft.field = "(Bob)";
    auto r = fx.merge(base, make_pdf(fo), make_pdf(ft));
    CHECK(r.conflicts.empty());
    const auto out = fx.out(r);
    CHECK(fx.dump(out).find("name (Tx) = \"Bob\"") != std::string::npos);
    CHECK(out.find("/NeedAppearances true") != std::string::npos);
    CHECK(warned(r, "need-appearances"));
    CHECK(out.find("/AP") != std::string::npos); // regenerated by qpdf

    SECTION("both set different values") {
        Fix fo2;
        fo2.field = "(Carol)";
        auto rc = fx.merge(base, make_pdf(fo2), make_pdf(ft));
        REQUIRE(rc.conflicts.size() == 1);
        CHECK(rc.conflicts[0].at.path == "/fields/name");
    }
    SECTION("PDF 2.0 gets appearances without /NeedAppearances") {
        Fix b2, t2;
        b2.version = t2.version = "2.0";
        t2.field = "(Bob)";
        auto r2 = fx.merge(make_pdf(b2), make_pdf(b2), make_pdf(t2));
        const auto o2 = fx.out(r2);
        CHECK(o2.find("/NeedAppearances") == std::string::npos);
        CHECK(fx.dump(o2).find("name (Tx) = \"Bob\"") != std::string::npos);
    }
}

TEST_CASE("pdf: XMP merges through the xml module") {
    Fx fx;
    auto xmp = [](const std::string& title, const std::string& creator, const std::string& date = "2026") {
        return "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
               "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
               "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
               "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
               "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n"
               "<dc:title>" +
               title + "</dc:title>\n<dc:creator>" + creator + "</dc:creator>\n<xmp:ModifyDate>" + date +
               "</xmp:ModifyDate>\n</rdf:Description>\n</rdf:RDF></"
               "x:xmpmeta>\n"
               "<?xpacket end=\"w\"?>";
    };
    Fix b, o, t;
    b.xmp = xmp("T", "C");
    o.xmp = xmp("T2", "C", "2027-ours");
    t.xmp = xmp("T", "C2", "2027-theirs");
    auto r = fx.merge(make_pdf(b), make_pdf(o), make_pdf(t));
    CHECK(r.conflicts.empty()); // both saved: the ModifyDate clash is noise
    const auto out = fx.out(r);
    CHECK(out.find("<dc:title>T2</dc:title>\n<dc:creator>C2</dc:creator>") != std::string::npos);
    auto d = fx.dump(out);
    CHECK(d.find("xmp ") != std::string::npos);
    CHECK(d.find("ModifyDate") == std::string::npos); // noise
    auto cs = fx.diff(make_pdf(b), make_pdf(o));
    REQUIRE(cs.size() == 1);
    CHECK(cs[0].at.path.starts_with("/xmp/"));

    Fix saved = b; // theirs only re-saved: a noise-only XMP change adds nothing to ours
    saved.xmp = xmp("T", "C", "2028");
    CHECK(fx.out(fx.merge(make_pdf(b), make_pdf(o), make_pdf(saved))) == make_pdf(o));
}

TEST_CASE("pdf: a carrier whose xref qpdf repairs is rewritten, not appended to") {
    Fx fx;
    auto shifted = [](std::string f) {
        return f.insert(f.find("1 0 obj"), "% padding that breaks every offset\n");
    };
    const auto base = shifted(make_pdf({}));
    Fix fo, ft;
    fo.title = "Ours";
    ft.author = "Zoe";
    auto doc = fx.load(base);
    CHECK(fx.pdf.inspect(*doc)["warnings"].size() > 0);
    auto r = fx.merge(base, shifted(make_pdf(fo)), shifted(make_pdf(ft)));
    CHECK(warned(r, "layout-rewritten"));
    const auto d = fx.dump(fx.out(r));
    CHECK(d.find("Author=\"Zoe\" Title=\"Ours\"") != std::string::npos);
    CHECK(fx.pdf.inspect(*fx.load(fx.out(r)))["warnings"].empty());
}

TEST_CASE("pdf: xref-stream carriers get an xref-stream update") {
    Fx fx;
    const auto plain = make_pdf({});
    const auto base = text(pdf::rewrite(as_bytes(plain), true));
    REQUIRE(base.find("/Type /XRef") != std::string::npos);
    // theirs: a new title, appended by hand to the same bytes.
    const auto rp = base.find("/Root ");
    const auto root = base.substr(rp, base.find('R', rp + 5) + 1 - rp); // qpdf renumbered the objects
    const auto theirs =
        append_update(base, {{40, "<< /Title (New) /Author (Alice) >>"}}, 41, root + " /Info 40 0 R");
    auto doc = fx.load(base);
    REQUIRE(fx.pdf.inspect(*doc)["xref"] == "stream");
    Fix fo;
    fo.annots[0].push_back(kNote2);
    CHECK(fx.out(fx.merge(base, base, theirs)) == theirs);
    const auto oursx = text(pdf::rewrite(as_bytes(make_pdf(fo)), true)); // ours: another object layout
    auto r = fx.merge(base, oursx, theirs);
    CHECK(r.conflicts.empty());
    const auto out = fx.out(r);
    CHECK(out.compare(0, oursx.size(), oursx) == 0);
    CHECK(out.find("/Type /XRef", oursx.size()) != std::string::npos);
    CHECK(fx.dump(out).find("Title=\"New\"") != std::string::npos);
    CHECK(fx.dump(out).find("\"Carol\"") != std::string::npos);
    CHECK(fx.pdf.inspect(*fx.load(out))["revisions"] == 2);
}

TEST_CASE("pdf: signatures without the crypto feature") {
    Fx fx;
    Fix b;
    b.sig = true;
    const auto base = make_pdf(b);
    Fix t = b;
    t.title = "Changed";
    const auto theirs = make_pdf(t);
    auto doc = fx.load(base);
    auto j = fx.pdf.inspect(*doc);
    REQUIRE(j["signatures"].size() == 1);
    CHECK(j["signatures"][0]["status"] == "no-crypto");
    CHECK(fx.dump(base).find("signed=1") != std::string::npos);

    Fix o = b;
    o.author = "Ours";
    const auto ours = make_pdf(o);
    SECTION("auto keeps earlier signatures through an incremental update") {
        auto r = fx.merge(base, ours, theirs);
        CHECK(warned(r, "signature-not-covering-changes"));
        const auto out = fx.out(r);
        CHECK(out.compare(0, ours.size(), ours) == 0);
        CHECK(fx.dump(out).find("signed=1") != std::string::npos);
    }
    SECTION("strip removes signature values") {
        fx.opts["signatures"] = "strip";
        auto r = fx.merge(base, ours, theirs);
        CHECK(warned(r, "signature-stripped"));
        CHECK(fx.dump(fx.out(r)).find("signed=0") != std::string::npos);
    }
    SECTION("compact layout rewrites and strips") {
        fx.opts["layout"] = "compact";
        auto r = fx.merge(base, ours, theirs);
        CHECK(warned(r, "signature-stripped"));
        const auto out = fx.out(r);
        CHECK(out.compare(0, 200, ours, 0, 200) != 0);
        CHECK(fx.dump(out).find("signed=0") != std::string::npos);
        CHECK(fx.dump(out).find("Title=\"Changed\"") != std::string::npos);
        fx.opts["signatures"] = "keep";
        auto rk = fx.merge(base, ours, theirs);
        CHECK(warned(rk, "signature-invalidated"));
        CHECK(fx.dump(fx.out(rk)).find("signed=1") != std::string::npos);
    }
    SECTION("resign is not available") {
        fx.opts["signatures"] = "resign";
        CHECK_THROWS_AS(fx.merge(base, ours, theirs), Error);
    }
    SECTION("DocMDP P=1 reports certification-violated") {
        Fix cb = b;
        cb.extra_root = " /Perms << /DocMDP 8 0 R >>"; // object 8: the signature value
        cb.sig_extra = " /Reference [<< /TransformMethod /DocMDP /TransformParams << /P 1 >> >>]";
        Fix ct = cb, co = cb;
        ct.title = "Changed";
        co.author = "Ours";
        const auto cbase = make_pdf(cb);
        REQUIRE(fx.pdf.inspect(*fx.load(cbase))["certification"] == 1);
        auto r = fx.merge(cbase, make_pdf(co), make_pdf(ct));
        CHECK(warned(r, "certification-violated"));
    }
}

TEST_CASE("pdf: encrypted inputs") {
    Fx fx;
    const auto plain = make_pdf({});
    Fix t;
    t.title = "Changed";
    SECTION("permission-only encryption is looked through; merges are full rewrites") {
        const auto base = text(pdf::rewrite(as_bytes(plain), false, ""));
        const auto theirs = text(pdf::rewrite(as_bytes(make_pdf(t)), false, ""));
        CHECK(fx.dump(base).find("encrypted=yes") != std::string::npos);
        CHECK(fx.dump(base).find("Hello World") != std::string::npos);
        auto cs = fx.diff(base, theirs);
        REQUIRE(cs.size() == 1);
        CHECK(cs[0].at.path == "/info/Title");
        Fix o;
        o.author = "Ours";
        const auto ours = text(pdf::rewrite(as_bytes(make_pdf(o)), false, ""));
        auto r = fx.merge(base, ours, theirs);
        CHECK(warned(r, "layout-rewritten"));
        const auto d = fx.dump(fx.out(r));
        CHECK(d.find("encrypted=yes") != std::string::npos);
        CHECK(d.find("Title=\"Changed\"") != std::string::npos);
        CHECK(d.find("Author=\"Ours\"") != std::string::npos);
    }
    SECTION("signed and encrypted: no silent unsigning") {
        Fix b;
        b.sig = true;
        Fix ts = b, os = b;
        ts.title = "Changed";
        os.author = "Ours";
        const auto base = text(pdf::rewrite(as_bytes(make_pdf(b)), false, ""));
        const auto ours = text(pdf::rewrite(as_bytes(make_pdf(os)), false, ""));
        const auto theirs = text(pdf::rewrite(as_bytes(make_pdf(ts)), false, ""));
        auto r = fx.merge(base, ours, theirs);
        REQUIRE(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].reason == "signature");
        CHECK(fx.out(r) == ours);
        fx.opts["signatures"] = "keep";
        auto rk = fx.merge(base, ours, theirs);
        CHECK(rk.conflicts.empty());
        CHECK(warned(rk, "signature-invalidated"));
    }
    SECTION("a user password makes the file opaque") {
        const auto base = text(pdf::rewrite(as_bytes(plain), false, "pw"));
        const auto theirs = text(pdf::rewrite(as_bytes(make_pdf(t)), false, "pw"));
        CHECK(fx.dump(base).find("locked bytes=") != std::string::npos);
        auto cs = fx.diff(base, theirs);
        REQUIRE(cs.size() == 1);
        CHECK(cs[0].at.path == "/encrypted");
        CHECK(fx.out(fx.merge(base, base, theirs)) == theirs);
        auto r = fx.merge(base, plain, theirs);
        REQUIRE(r.conflicts.size() == 1);
        CHECK(fx.out(r) == plain);
    }
}

TEST_CASE("pdf: churn of an incremental merge is the appended section") {
    Fx fx;
    const auto base = make_pdf({});
    Fix ft, fo;
    ft.annots[0].push_back(kNote3);
    fo.title = "Ours";
    const auto ours = make_pdf(fo);
    auto r = fx.merge(base, ours, make_pdf(ft));
    const auto out = fx.out(r);
    auto a = fx.load(ours), b = fx.load(out);
    auto churn = attribute_churn(as_bytes(ours), *fx.pdf.envelope(*a), as_bytes(out), *fx.pdf.envelope(*b));
    CHECK(churn.total() == out.size() - ours.size());
    CHECK(churn.by_class[static_cast<std::size_t>(ByteClass::unexplained)] == 0);
}

TEST_CASE("pdf: cli diff, dump, and merge with --layout") {
    namespace fs = std::filesystem;
    const auto dir = scratch_path("test-pdf");
    fs::create_directories(dir);
    auto file = [&](const std::string& name, const std::string& content) {
        std::ofstream(dir / name, std::ios::binary)
            .write(content.data(), static_cast<std::streamsize>(content.size()));
        return splinter::cli::from_path(dir / name);
    };
    auto read = [&](const std::string& name) {
        std::ifstream f(dir / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    Fix fo, ft;
    fo.title = "Ours";
    ft.field = "(Bob)";
    const auto base = file("base.pdf", make_pdf({})), ours = make_pdf(fo);
    const auto theirs = file("theirs.pdf", make_pdf(ft));
    CaptureConsole d;
    CHECK(run_cli(d, {base, theirs}) == 1);
    CHECK(d.out_text.starts_with("~ /fields/name Tx = \"Alice\" -> Tx = \"Bob\"\n"));
    CaptureConsole dump;
    CHECK(run_cli(dump, {"dump", base}) == 0);
    CHECK(dump.out_text.starts_with("pdf 1.7 pages=2"));

    auto mine = file("mine.pdf", ours);
    CaptureConsole m;
    CHECK(run_cli(m, {"merge", mine, base, theirs}) == 0);
    CHECK(read("mine.pdf").compare(0, ours.size(), ours) == 0); // incremental by default
    mine = file("mine.pdf", ours);
    CaptureConsole c;
    CHECK(run_cli(c, {"merge", "--layout=compact", mine, base, theirs}) == 0);
    CHECK(read("mine.pdf").compare(0, ours.size(), ours) != 0); // rewritten by qpdf
    CaptureConsole bad;
    CHECK(run_cli(bad, {"merge", "--layout=tight", mine, base, theirs}) == 2);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
