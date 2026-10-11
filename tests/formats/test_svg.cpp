#include "../cli/console.hpp"
#include "../scratch.hpp"

#include <semsplinter/envelope.hpp>
#include <semsplinter/format.hpp>
#include <semsplinter/formats/xml.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace semsplinter;

namespace {

std::vector<std::byte> vec(std::string_view s) {
    auto b = as_bytes(s);
    return {b.begin(), b.end()};
}

std::string str(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

struct Fx {
    Registry reg = Registry::with_builtin_formats();
    Limits limits;
    OptionValues opts;
    Context ctx{reg, limits, 0, opts};
    const Format* fmt = reg.find("svg");

    std::unique_ptr<Document> load(std::string_view s) {
        return fmt->load(ByteSource::from_memory(vec(s), "x.svg"), {ctx});
    }
    ChangeSet diff(std::string_view a, std::string_view b) { return fmt->diff(*load(a), *load(b), {ctx}); }
    MergeResult merge(std::string_view b, std::string_view o, std::string_view t, ConflictOptions c = {}) {
        auto db = load(b), dob = load(o), dt = load(t);
        return fmt->merge(*db, *dob, *dt, MergeOptions{ctx, std::move(c), {}});
    }
    std::string out(const MergeResult& r) {
        VectorSink s;
        fmt->save(*r.document, s, {ctx});
        return str(s.data);
    }
    std::string merged(std::string_view b, std::string_view o, std::string_view t) {
        auto r = merge(b, o, t);
        CHECK(r.conflicts.empty());
        return out(r);
    }
    std::string dump(std::string_view s) {
        std::ostringstream os;
        fmt->dump(*load(s), os, {ctx});
        return os.str();
    }
};

std::string replace(std::string s, std::string_view from, std::string_view to) {
    auto at = s.find(from);
    REQUIRE(at != std::string::npos);
    return s.replace(at, from.size(), to);
}

bool has(const std::string& s, std::string_view part) {
    return s.find(part) != std::string::npos;
}

bool warned(const MergeResult& r, std::string_view code) {
    return std::any_of(r.warnings.begin(), r.warnings.end(), [&](auto& w) { return w.code == code; });
}

// As Inkscape 1.x writes it: editor namespaces, a namedview, defs with a gradient chain, a layer.
const std::string kInk = R"~(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!-- Created with Inkscape (http://www.inkscape.org/) -->

<svg
   width="210mm"
   height="297mm"
   viewBox="0 0 210 297"
   version="1.1"
   id="svg5"
   inkscape:version="1.2.2 (b0a8486541, 2022-12-01)"
   sodipodi:docname="drawing.svg"
   xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
   xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
   xmlns:xlink="http://www.w3.org/1999/xlink"
   xmlns="http://www.w3.org/2000/svg"
   xmlns:svg="http://www.w3.org/2000/svg">
  <sodipodi:namedview
     id="namedview7"
     pagecolor="#ffffff"
     bordercolor="#000000"
     inkscape:zoom="0.7"
     inkscape:cx="396.4"
     inkscape:cy="561.4"
     inkscape:window-width="1920"
     inkscape:current-layer="layer1" />
  <defs
     id="defs2">
    <linearGradient
       inkscape:collect="always"
       id="linearGradient1">
      <stop
         style="stop-color:#ff0000;stop-opacity:1;"
         offset="0"
         id="stop1" />
      <stop
         style="stop-color:#0000ff;stop-opacity:1;"
         offset="1"
         id="stop2" />
    </linearGradient>
    <linearGradient
       xlink:href="#linearGradient1"
       id="linearGradient3"
       x1="10"
       y1="10"
       x2="100"
       y2="10"
       gradientUnits="userSpaceOnUse" />
  </defs>
  <g
     inkscape:label="Layer 1"
     inkscape:groupmode="layer"
     id="layer1">
    <rect
       style="fill:url(#linearGradient3);stroke:#000000;stroke-width:0.5"
       id="rect10"
       width="80"
       height="40"
       x="20"
       y="30"
       class="shape primary" />
    <path
       style="fill:#00ff00;stroke:none"
       d="m 20,100 h 40 v 20 h -40 z m 60,0 h 40 v 20 h -40 z m 60,0 h 40 v 20 h -40 z"
       id="path12"
       transform="translate(5,0)" />
    <text
       xml:space="preserve"
       x="20"
       y="200"
       id="text14"><tspan
         id="tspan16"
         x="20"
         y="200">Hello big world</tspan></text>
  </g>
</svg>
)~";

// Illustrator's legacy export: namespaces through internal DTD entities, i:pgf private data.
const std::string kAi = R"~(<?xml version="1.0" encoding="utf-8"?>
<!-- Generator: Adobe Illustrator 16.0.0, SVG Export Plug-In . SVG Version: 6.00 Build 0)  -->
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd" [
	<!ENTITY ns_extend "http://ns.adobe.com/Extensibility/1.0/">
	<!ENTITY ns_ai "http://ns.adobe.com/AdobeIllustrator/10.0/">
	<!ENTITY ns_graphs "http://ns.adobe.com/Graphs/1.0/">
	<!ENTITY ns_svg "http://www.w3.org/2000/svg">
	<!ENTITY ns_xlink "http://www.w3.org/1999/xlink">
]>
<svg version="1.1" id="Layer_1" xmlns:x="&ns_extend;" xmlns:i="&ns_ai;" xmlns:graph="&ns_graphs;"
	 xmlns="&ns_svg;" xmlns:xlink="&ns_xlink;" x="0px" y="0px" width="100px" height="100px"
	 viewBox="0 0 100 100" enable-background="new 0 0 100 100" xml:space="preserve">
<switch>
	<foreignObject requiredExtensions="&ns_ai;" x="0" y="0" width="1" height="1">
		<i:pgf  id="adobe_illustrator_pgf">
		<![CDATA[
			eJzLSM3JyVcozy/KSQEAGgQEXQ==
		]]>
	</i:pgf>
	</foreignObject>
	<g i:extraneous="self">
		<rect id="r1" x="10" y="10" fill="#FF0000" width="30" height="30"/>
		<circle id="c1" fill="#0000FF" cx="70" cy="70" r="20"/>
	</g>
</switch>
<i:pgfRef  xlink:href="#adobe_illustrator_pgf">
</i:pgfRef>
</svg>
)~";

// A gzip member with stored deflate blocks (enough to build .svgz fixtures without zlib).
std::string gzip(std::string_view plain, unsigned char xfl = 0) {
    auto le = [](std::uint64_t v, int n) {
        std::string s;
        for (int i = 0; i < n; ++i)
            s += static_cast<char>((v >> (8 * i)) & 0xFF);
        return s;
    };
    std::string g = std::string("\x1F\x8B\x08\x08", 4) + le(0x5F5E1000, 4) + static_cast<char>(xfl) + "\x03" +
                    std::string("drawing.svg\0", 12);
    for (std::size_t at = 0;;) {
        const std::size_t n = std::min<std::size_t>(plain.size() - at, 65535);
        const bool last = at + n == plain.size();
        g += static_cast<char>(last ? 1 : 0) + le(n, 2) + le(~n & 0xFFFF, 2) +
             std::string(plain.substr(at, n));
        at += n;
        if (last)
            break;
    }
    return g + le(crc32(as_bytes(plain)), 4) + le(plain.size(), 4);
}

} // namespace

TEST_CASE("svg: detection, round trip, and one-sided merges are byte-exact") {
    Fx fx;
    REQUIRE(fx.fmt);
    auto detect = [&](std::string_view s, std::string_view name) {
        return std::string(fx.reg.detect(as_bytes(s), name).info().name);
    };
    CHECK(detect(kInk, "drawing.svg") == "svg");
    CHECK(detect(kInk, "noext") == "svg");
    CHECK(detect(kAi, "logo.svg") == "svg");
    CHECK(detect("<svg><rect/></svg>", "x") == "svg"); // no namespace: lower confidence, still above xml
    CHECK(detect("<Project><a/></Project>", "x") == "xml");
    CHECK(detect(gzip(kInk), "drawing.svgz") == "svg");

    CHECK(fx.merged(kInk, kInk, kInk) == kInk);
    const std::string o = replace(kInk, "x=\"20\"\n       y=\"30\"", "x=\"25\"\n       y=\"30\"");
    CHECK(fx.merged(kInk, o, kInk) == o);
    CHECK(fx.merged(kInk, kInk, o) == o);
    CHECK(fx.merged(kAi, kAi, kAi) == kAi);
}

TEST_CASE("svg: normalization and editor state") {
    Fx fx;
    // Numbers, colors, transforms, and style order compare by value (svg.md §4).
    CHECK(
        fx.diff(
              R"~(<svg xmlns="http://www.w3.org/2000/svg"><rect x="1.0" fill="#fff" transform="translate(10)" style="fill:#FFF;stroke:none;"/></svg>)~",
              R"~(<svg xmlns="http://www.w3.org/2000/svg"><rect x="1" fill="white" transform="matrix(1,0,0,1,10,0)" style="stroke:none; fill:rgb(255,255,255)"/></svg>)~")
            .empty());
    CHECK(fx.diff(R"~(<svg xmlns="http://www.w3.org/2000/svg"><path d="M 10,10 L 20,20 30,30"/></svg>)~",
                  R"~(<svg xmlns="http://www.w3.org/2000/svg"><path d="m10 10 l10 10 10 10"/></svg>)~")
              .empty());
    CHECK(fx.diff(R"~(<svg xmlns="http://www.w3.org/2000/svg"><rect class="a b"/></svg>)~",
                  R"~(<svg xmlns="http://www.w3.org/2000/svg"><rect class="b  a"/></svg>)~")
              .empty());

    // Editor state: no diff, not dumped, shown on request.
    const std::string zoomed = replace(replace(kInk, "inkscape:zoom=\"0.7\"", "inkscape:zoom=\"1.4\""),
                                       "inkscape:cx=\"396.4\"", "inkscape:cx=\"100\"");
    CHECK(fx.diff(kInk, zoomed).empty());
    CHECK(!has(fx.dump(kInk), "inkscape:zoom"));
    fx.opts["svg-show-editor-state"] = "";
    CHECK(fx.diff(kInk, zoomed).size() == 2);
    CHECK(has(fx.dump(kInk), "inkscape:zoom"));
    fx.opts.clear();

    // Both sides moved the view and edited elsewhere (theirs also the namedview's page color): ours' view
    // wins, noted, no conflict.
    const std::string o = replace(zoomed, "y=\"30\"", "y=\"35\"");
    const std::string t = replace(replace(replace(kInk, "inkscape:zoom=\"0.7\"", "inkscape:zoom=\"3\""),
                                          "stroke:none", "stroke:#ff0000"),
                                  "pagecolor=\"#ffffff\"", "pagecolor=\"#eeeeee\"");
    auto r = fx.merge(kInk, o, t);
    CHECK(r.conflicts.empty());
    const auto out = fx.out(r);
    CHECK(has(out, "inkscape:zoom=\"1.4\""));
    CHECK(has(out, "y=\"35\""));
    CHECK(has(out, "stroke:#ff0000"));
    CHECK(has(out, "pagecolor=\"#eeeeee\""));
    CHECK(std::any_of(r.auto_merged.begin(), r.auto_merged.end(),
                      [](auto& a) { return a.rule == "editor-state"; }));
}

TEST_CASE("svg: style properties, classes, and text merge by part") {
    Fx fx;
    const std::string o = replace(kInk, "stroke:#000000;stroke-width:0.5", "stroke:#000000;stroke-width:2");
    const std::string t =
        replace(kInk, "fill:url(#linearGradient3);stroke:#000000", "fill:#123456;stroke:#000000");
    CHECK(has(fx.merged(kInk, o, t), "style=\"fill:#123456;stroke:#000000;stroke-width:2\""));
    auto r = fx.merge(kInk, o, replace(kInk, "stroke-width:0.5", "stroke-width:3"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].at.path == "/svg[@id=svg5]/g[@id=layer1]/rect[@id=rect10]/@style");

    CHECK(has(fx.merged(kInk, replace(kInk, "class=\"shape primary\"", "class=\"shape primary selected\""),
                        replace(kInk, "class=\"shape primary\"", "class=\"shape\"")),
              "class=\"shape selected\""));

    CHECK(has(fx.merged(kInk, replace(kInk, "Hello big world", "Hello small world"),
                        replace(kInk, "Hello big world", "Hello big world!")),
              ">Hello small world!</tspan>"));
    CHECK(fx.merge(kInk, replace(kInk, "big", "small"), replace(kInk, "big", "huge")).conflicts.size() == 1);
}

TEST_CASE("svg: path data merges by subpath") {
    Fx fx;
    // ours widens the first square, theirs lowers the third
    const std::string o = replace(kInk, "m 20,100 h 40", "m 20,100 h 45");
    const std::string t = replace(kInk, "m 60,0 h 40 v 20 h -40 z\"", "m 60,0 h 40 v 30 h -40 z\"");
    auto r = fx.merge(kInk, o, t);
    CHECK(r.conflicts.empty());
    CHECK(has(fx.out(r),
              "d=\"M20,100 h 45 v 20 h -40 z M80,100 h 40 v 20 h -40 z M140,100 h 40 v 30 h -40 z\""));
    // The same subpath on both sides conflicts; so does any two-sided edit under --svg-path-merge=none.
    CHECK(fx.merge(kInk, o, replace(kInk, "m 20,100 h 40", "m 20,100 h 50")).conflicts.size() == 1);
    fx.opts["svg-path-merge"] = "none";
    CHECK(fx.merge(kInk, o, t).conflicts.size() == 1);
    fx.opts["svg-path-merge"] = "segments";
    CHECK_THROWS_AS(fx.merge(kInk, o, t), Error);
}

TEST_CASE("svg: ids both sides added are renumbered with their references") {
    Fx fx;
    const std::string grad_o = "\n    <linearGradient id=\"linearGradient4\"><stop offset=\"0\" "
                               "style=\"stop-color:#00ff00\" /></linearGradient>";
    const std::string grad_t = "\n    <linearGradient id=\"linearGradient4\"><stop offset=\"0\" "
                               "style=\"stop-color:#ffff00\" /></linearGradient>";
    const std::string o =
        replace(replace(kInk, "\n  </defs>", grad_o + "\n  </defs>"), "\n  </g>",
                "\n    <rect id=\"rect11\" x=\"1\" fill=\"url(#linearGradient4)\" />\n  </g>");
    const std::string t =
        replace(replace(kInk, "\n  </defs>", grad_t + "\n  </defs>"), "\n  </g>",
                "\n    <rect id=\"rect11\" x=\"2\" style=\"fill:url(#linearGradient4)\" />\n  </g>");
    auto r = fx.merge(kInk, o, t);
    CHECK(r.conflicts.empty());
    const auto out = fx.out(r);
    // The highest number in use with the prefix is 10 for rect, 4 for linearGradient (SPEC §2.3).
    CHECK(has(out, "<rect id=\"rect11\" x=\"1\" fill=\"url(#linearGradient4)\" />"));
    CHECK(has(out, "<rect id=\"rect12\" x=\"2\" style=\"fill:url(#linearGradient5)\" />"));
    CHECK(has(out,
              "<linearGradient id=\"linearGradient5\"><stop offset=\"0\" style=\"stop-color:#ffff00\" />"));
    CHECK(std::count_if(r.auto_merged.begin(), r.auto_merged.end(),
                        [](auto& a) { return a.rule == "renumbered"; }) == 2);
    CHECK(!warned(r, "id-duplicate"));
    CHECK(!warned(r, "dangling-ref"));

    fx.opts["svg-id-collision"] = "conflict";
    CHECK(!fx.merge(kInk, o, t).conflicts.empty());
}

TEST_CASE("svg: deleting what the other side newly references is a dependency conflict") {
    Fx fx;
    const std::string o = replace(kInk, "\n  </g>", "\n    <use xlink:href=\"#linearGradient1\" />\n  </g>");
    const std::string grad3 =
        kInk.substr(kInk.find("\n    <linearGradient\n       xlink:href"),
                    kInk.find("</defs>") - 3 - kInk.find("\n    <linearGradient\n       xlink:href"));
    const std::string gone = replace(replace(kInk, "fill:url(#linearGradient3);", ""), grad3, "");
    // theirs deleted gradient 3 (and its use); ours newly references gradient 1, which survives: fine.
    CHECK(fx.merge(kInk, o, gone).conflicts.empty());
    // ours newly references gradient 3, which theirs deleted.
    const std::string o3 =
        replace(kInk, "\n  </g>", "\n    <circle r=\"5\" fill=\"url(#linearGradient3)\" />\n  </g>");
    auto r = fx.merge(kInk, o3, gone);
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "dependency");
    CHECK(has(fx.out(r), "id=\"linearGradient3\"")); // ours keeps the gradient
}

TEST_CASE("svg: derived data: path effects and Illustrator private data") {
    Fx fx;
    const std::string b =
        R"~(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"><path id="p" inkscape:path-effect="#pe1" inkscape:original-d="M 0,0 L 10,0" d="M 0,0 C 3,1 7,1 10,0" fill="red"/></svg>)~";
    auto r = fx.merge(b, replace(b, "L 10,0", "L 20,0"), replace(b, "C 3,1", "C 4,1"));
    REQUIRE(r.conflicts.size() == 1);
    CHECK(r.conflicts[0].reason == "dependency");

    // Ours and theirs edit different shapes; Illustrator would reopen from the stale i:pgf.
    auto ai = fx.merge(kAi, replace(kAi, "#FF0000", "#00FF00"), replace(kAi, "#0000FF", "#000000"));
    CHECK(ai.conflicts.empty());
    CHECK(warned(ai, "derived-stale"));
    const auto out = fx.out(ai);
    CHECK(has(out, "fill=\"#00FF00\""));
    CHECK(has(out, "fill=\"#000000\""));
    CHECK(has(out, "<!ENTITY ns_svg \"http://www.w3.org/2000/svg\">")); // the DOCTYPE stays verbatim
    auto pgf = fx.merge(kAi, replace(kAi, "eJzLSM3", "AAAAM3"), replace(kAi, "eJzLSM3", "BBBBM3"));
    CHECK(pgf.conflicts.size() == 1);
}

TEST_CASE("svg: the entity exception is narrow") {
    Fx fx;
    auto ok = [&](const std::string& s) {
        try {
            fx.load(s);
            return true;
        } catch (const Error&) {
            return false;
        }
    };
    const std::string text =
        "<!DOCTYPE svg [<!ENTITY who \"World\">]><svg xmlns=\"http://www.w3.org/2000/svg\"><text "
        "id=\"t\">Hello &who;</text><rect id=\"r\" x=\"1\"/></svg>";
    CHECK(ok(text));
    CHECK(fx.merged(text, replace(text, "<text ", "<text y=\"2\" "), text) ==
          replace(text, "<text ", "<text y=\"2\" "));
    CHECK(!ok("<!DOCTYPE svg [<!ENTITY a \"x\"><!ENTITY b \"&a;&a;\">]><svg/>"));
    CHECK(!ok("<!DOCTYPE svg [<!ENTITY a \"<g/>\">]><svg/>"));
    CHECK(!ok("<!DOCTYPE svg [<!ENTITY x SYSTEM \"file:///etc/passwd\">]><svg>&x;</svg>"));
    CHECK(!ok("<!DOCTYPE svg [<!ENTITY % p \"x\">]><svg/>"));
    std::string many = "<!DOCTYPE svg [";
    for (int i = 0; i < 65; ++i)
        many += "<!ENTITY e" + std::to_string(i) + " \"x\">";
    CHECK(!ok(many + "]><svg/>"));
    CHECK(!ok("<!DOCTYPE svg [<!ENTITY big \"" + std::string(5000, 'x') + "\">]><svg/>"));
    // Generic XML keeps refusing every entity.
    CHECK_THROWS_AS(fx.reg.find("xml")->load(ByteSource::from_memory(vec(text)), {fx.ctx}), Error);
}

TEST_CASE("svg: --conflict=mark wraps both versions in layers") {
    Fx fx;
    const std::string o = replace(kInk, "stroke-width:0.5", "stroke-width:2");
    const std::string t = replace(kInk, "stroke-width:0.5", "stroke-width:3");
    auto r = fx.merge(kInk, o, t, {Policy::mark});
    REQUIRE(r.conflicts.size() == 1);
    const auto out = fx.out(r); // the writer re-parses its output
    const std::string h = r.conflicts[0].id.substr(2, 8);
    CHECK(has(out,
              "<g id=\"splinter-conflict-" + h + "\" data-splinter-conflict=\"" + r.conflicts[0].id + "\""));
    CHECK(has(out, "inkscape:groupmode=\"layer\""));
    CHECK(has(out, "display=\"none\""));
    CHECK(has(out, "id=\"rect10-splinter-theirs\""));
    CHECK(has(out, "stroke-width:3"));
    CHECK(has(out, "stroke-width:2"));
    // Element conflict (modify-delete) inside the layer; a conflict on a `stop` falls back to a comment.
    auto e = fx.merge(
        kInk, replace(kInk, ">Hello big world<", ">Bye<"),
        replace(kInk,
                kInk.substr(kInk.find("\n    <text"), kInk.find("</text>") + 7 - kInk.find("\n    <text")),
                ""),
        {Policy::mark});
    REQUIRE(e.conflicts.size() == 1);
    CHECK(has(fx.out(e), "splinter-conflict-"));
    auto s = fx.merge(kInk, replace(kInk, "offset=\"1\"", "offset=\"0.9\""),
                      replace(kInk, "offset=\"1\"", "offset=\"0.8\""), {Policy::mark});
    REQUIRE(s.conflicts.size() == 1);
    CHECK(has(fx.out(s), "<!-- splinter-conflict"));
}

TEST_CASE("svg: svgz is looked through and recompressed only when changed") {
    Fx fx;
    auto load = [&](const std::string& s) {
        return fx.fmt->load(ByteSource::from_memory(vec(s), "d.svgz"), {fx.ctx});
    };
    auto save = [&](const Document& d) {
        VectorSink s;
        fx.fmt->save(d, s, {fx.ctx});
        return str(s.data);
    };
    auto mergez = [&](const std::string& b, const std::string& o, const std::string& t) {
        auto db = load(b), dob = load(o), dt = load(t);
        return fx.fmt->merge(*db, *dob, *dt, MergeOptions{fx.ctx, {}, {}});
    };
    const std::string o = replace(kInk, "y=\"30\"", "y=\"35\""),
                      t = replace(kInk, "stroke:none", "stroke:#ff0000");
    const std::string gb = gzip(kInk), go = gzip(o, 2), gt = gzip(t);
    CHECK(fx.fmt->diff(*load(gb), *load(gzip(kInk, 4)), {fx.ctx}).empty()); // encoding only
    std::ostringstream d1;
    fx.fmt->dump(*load(gb), d1, {fx.ctx});
    CHECK(d1.str() == fx.dump(kInk));

    auto same = mergez(gb, go, gb);
    CHECK(save(*same.document) == go); // ours verbatim
    CHECK(!warned(same, "encoding-reencoded"));

    auto r = mergez(gb, go, gt);
    CHECK(r.conflicts.empty());
    CHECK(warned(r, "encoding-reencoded"));
    const auto out = save(*r.document);
    CHECK(out.substr(0, 22) == go.substr(0, 22)); // ours' header: flags, mtime, XFL, OS, name
    std::ostringstream d2;
    fx.fmt->dump(*load(out), d2, {fx.ctx});
    CHECK(d2.str() == fx.dump(fx.merged(kInk, o, t)));
    auto* map = fx.fmt->envelope(*r.document);
    REQUIRE(map);
    CHECK_NOTHROW(map->validate(out.size()));
    CHECK(map->ranges.back().cls == ByteClass::integrity);

    // A wrong CRC-32 loads with integrity "mismatch".
    std::string bad = gb;
    bad[bad.size() - 8] ^= 0x55;
    auto j = fx.fmt->inspect(*load(bad));
    CHECK(j["integrity"]["status"] == "mismatch");
    CHECK(fx.fmt->inspect(*load(gb))["integrity"] == "ok");
    CHECK_THROWS_AS(load(gb.substr(0, gb.size() / 2)), Error);
}

TEST_CASE("cli: svg merge and report") {
    const auto dir = scratch_path("test-svg");
    std::filesystem::create_directories(dir);
    auto file = [&](const std::string& name, std::string_view content) {
        std::ofstream(dir / name, std::ios::binary)
            .write(content.data(), static_cast<std::streamsize>(content.size()));
        return splinter::cli::from_path(dir / name);
    };
    const std::string o = replace(kInk, "y=\"30\"", "y=\"35\""),
                      t = replace(kInk, "stroke:none", "stroke:#ff0000");
    auto b = file("base.svg", kInk), fo = file("ours.svg", o), ft = file("theirs.svg", t);
    CaptureConsole m;
    CHECK(run_cli(m, {"-m", "--json=-", fo, b, ft}) == 0);
    auto at = m.out_text.find("\n{");
    REQUIRE(at != std::string::npos);
    CHECK(nlohmann::json::parse(m.out_text.substr(at + 1))["format"] == "svg");
    CHECK(has(m.out_text, "stroke:#ff0000"));
    CaptureConsole d;
    CHECK(run_cli(d, {b, fo}) == 1);
    CHECK(has(d.out_text, "/svg[@id=svg5]/g[@id=layer1]/rect[@id=rect10]/@y"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
