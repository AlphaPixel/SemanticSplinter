#include <semsplinter/formats/png.hpp>
#include <semsplinter/formats/text.hpp>
#include <semsplinter/semsplinter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace semsplinter;
namespace fs = std::filesystem;

namespace {

Input in(std::string_view text, std::string path = "x.txt") {
    auto b = as_bytes(text);
    return {ByteSource::from_memory({b.begin(), b.end()}, path), std::move(path)};
}

std::string str(const std::vector<std::byte>& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

// #27: every route to a text merge honours the same options.
TEST_CASE("api: text options travel through the Format contract") {
    const auto reg = Registry::with_builtin_formats();
    const auto& fmt = *reg.find("text");
    Limits limits;
    OptionValues ov;
    Context ctx{reg, limits, 0, ov};
    auto load = [&](std::string_view s) { return fmt.load(in(s).source, {ctx}); };
    auto base = load("a\r\nb\r\nc\r\n"), ours = load("a\r\nB1\r\nc\r\n"), theirs = load("a\r\nB2\r\nc\r\n");

    TextMergeOptions git;
    git.engine = MergeEngine::git;
    git.style = MarkerStyle::diff3;
    git.marker_size = 3;
    MergeOptions mo{ctx, {Policy::mark}, {"O", "B", "T"}, MergeOptions::Visual::none, git};
    auto r = fmt.merge(*base, *ours, *theirs, mo);
    VectorSink sink;
    fmt.save(*r.document, sink, {ctx});
    CHECK(str(sink.data) == "a\r\n<<< O\r\nB1\r\n||| B\r\nb\r\n===\r\nB2\r\n>>> T\r\nc\r\n");
    REQUIRE(r.marked);
    CHECK(*r.marked == 1);

    // Same bytes as the module's own entry point.
    auto direct_opts = git;
    direct_opts.labels = {"O", "B", "T"};
    direct_opts.crlf_markers = true;
    auto direct =
        text::merge(dynamic_cast<const text::TextDoc&>(*base), dynamic_cast<const text::TextDoc&>(*ours),
                    dynamic_cast<const text::TextDoc&>(*theirs), direct_opts);
    CHECK(direct.bytes == sink.data);

    // Diff normalization: -w makes a whitespace-only change disappear.
    auto x = load("a b\n"), y = load("a  b\n");
    CHECK(fmt.compare(*x, *y, {ctx})->differs());
    TextDiffOptions w;
    w.normalize.ignore_all_space = true;
    CHECK_FALSE(fmt.compare(*x, *y, {ctx, w})->differs());
    CHECK(fmt.diff(*x, *y, {ctx, w}).empty());
}

// #26: the library does detection, conflict policy, atomic output, reports, and sidecars.
TEST_CASE("api: merge writes the output, the conflicts report, and counts conflicts") {
    const auto reg = Registry::with_builtin_formats();
    auto dir = fs::temp_directory_path() / "splinter-test-api";
    fs::remove_all(dir);
    fs::create_directories(dir);

    MergeRequest rq;
    rq.base = in("1\n2\n3\n", "base.txt");
    rq.ours = in("1\nO\n3\n", "ours.txt");
    rq.theirs = in("1\nT\n3\n", "theirs.txt");
    rq.labels = {"ours", "base", "theirs"};
    rq.conflict.policy = Policy::mark; // the CLI default; the library defaults to ours
    rq.output = dir / "out.txt";
    rq.output_name = "out.txt";
    rq.conflicts_file = dir / "conflicts.json";
    auto m = semsplinter::merge(reg, rq);
    CHECK(m.format == "text");
    CHECK(m.conflicts == 1);
    CHECK(slurp(dir / "out.txt") == "1\n<<<<<<< ours\nO\n=======\nT\n>>>>>>> theirs\n3\n");
    auto j = nlohmann::json::parse(slurp(dir / "conflicts.json"));
    CHECK(j["result"]["path"] == "out.txt");
    CHECK(j["conflicts"].size() == 1);

    rq.conflict.resolve_all = ResolveAll::theirs;
    CHECK(semsplinter::merge(reg, rq).conflicts == 0);
    CHECK(slurp(dir / "out.txt") == "1\nT\n3\n");

    // merge-file accepts text only; binary inputs merge opaquely elsewhere.
    MergeRequest bin;
    bin.base = in(std::string("\0\1", 2), "b.bin");
    bin.ours = in(std::string("\0\2", 2), "o.bin");
    bin.theirs = in(std::string("\0\1", 2), "t.bin");
    CHECK(str(semsplinter::merge(reg, bin).bytes) == std::string("\0\2", 2));
    bin.text_only = true;
    CHECK_THROWS_AS(semsplinter::merge(reg, bin), Error);

    // A PNG conflict under --conflict=mark leaves ours plus the visual sidecar.
    raster::Raster r(8, 8, false);
    auto png = [&](std::uint16_t v, const char* name) {
        auto img = r;
        img.set(3, {v, 0, 0, 0xFFFF});
        auto b = png::write(img);
        return Input{ByteSource::from_memory(b, name), name};
    };
    MergeRequest pr;
    pr.base = png(0, "b.png"), pr.ours = png(0xFFFF, "o.png"), pr.theirs = png(0x8080, "t.png");
    pr.conflict.policy = Policy::mark;
    pr.output = dir / "m.png";
    auto pm = semsplinter::merge(reg, pr);
    CHECK(pm.format == "png");
    CHECK(pm.conflicts == 1);
    CHECK(fs::exists(dir / "m.png.splinter-conflicts.png"));
    fs::remove_all(dir);
}

TEST_CASE("api: diff renders through the format's comparison") {
    const auto reg = Registry::with_builtin_formats();
    DiffRequest rq;
    rq.a = in("1\n2\n", "a");
    rq.b = in("1\n3\n", "b");
    rq.report = true;
    rq.render = RenderRequest{OutputStyle::unified, {}, {"a", "b"}, {"a", "b"}, {"A", "B"}};
    auto r = semsplinter::diff(reg, rq);
    CHECK(r.differs);
    CHECK(r.text == "--- A\n+++ B\n@@ -1,2 +1,2 @@\n 1\n-2\n+3\n");
    REQUIRE(r.report);
    CHECK(r.report->options["algorithm"] == "myers");

    rq.a = in(std::string("\0\1", 2), "x.bin");
    rq.render->style = OutputStyle::normal;
    r = semsplinter::diff(reg, rq);
    CHECK(r.format == "binary");
    CHECK(r.text == "Binary files a and b differ\n");

    CHECK(semsplinter::dump(reg, in("x\ny", "t.txt")) == "x\ny\n");
    auto j = semsplinter::inspect(reg, in("hello\n", "t.txt"), {}, true);
    CHECK(j["format"] == "text");
    CHECK(j["layout"].size() == 1);
    CHECK(render_inspect(j).starts_with("path t.txt\nformat text\nbytes 6\n"));
}
