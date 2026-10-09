#include "args.hpp"
#include "tables.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace splinter::cli;

namespace {

const Opt kTable[] = {
    {"", 'u', Arg::none, 1},
    {"", 'U', Arg::required, 2},
    {"unified", 0, Arg::optional, 3},
    {"ignore-case", 'i', Arg::none, 4},
    {"ignore-all-space", 'w', Arg::none, 5},
    {"ignore-blank-lines", 'B', Arg::none, 6},
    {"color", 0, Arg::optional, 7},
    {"label", 'L', Arg::required, 8},
    {"quiet", 'q', Arg::none, 9},
    {"silent", 0, Arg::none, 9},
};

Parsed p(std::vector<std::string> a, bool posix = false) {
    return parse(a, kTable, posix);
}

} // namespace

TEST_CASE("parser: clusters and attached or detached values") {
    auto r = p({"-uwB", "-U5", "-U", "7", "-iLx", "a", "b"});
    REQUIRE(r.opts.size() == 7);
    CHECK(r.opts[0].id == 1);
    CHECK(r.opts[1].id == 5);
    CHECK(r.opts[2].id == 6);
    CHECK(*r.opts[3].value == "5");
    CHECK(*r.opts[4].value == "7");
    CHECK(r.opts[5].id == 4);
    CHECK(*r.opts[6].value == "x");
    CHECK(r.operands == std::vector<std::string>{"a", "b"});
}

TEST_CASE("parser: long options, '=', optional arguments only with '='") {
    auto r = p({"--label=X", "--label", "Y", "--color", "--color=always", "--unified", "5"});
    CHECK(*r.opts[0].value == "X");
    CHECK(*r.opts[1].value == "Y");
    CHECK(!r.opts[2].value);
    CHECK(*r.opts[3].value == "always");
    CHECK(!r.opts[4].value);
    CHECK(r.operands == std::vector<std::string>{"5"});
    REQUIRE_THROWS_AS(p({"--quiet=1"}), UsageError);
    REQUIRE_THROWS_AS(p({"--label"}), UsageError);
    REQUIRE_THROWS_AS(p({"-U"}), UsageError);
}

TEST_CASE("parser: unique-prefix abbreviation, exact match wins, aliases are not ambiguous") {
    CHECK(p({"--ignore-c"}).opts[0].id == 4);
    CHECK(p({"--uni"}).opts[0].id == 3);
    REQUIRE_THROWS_AS(p({"--ign"}), UsageError); // case / all-space / blank-lines
    try {
        p({"--ign"});
    } catch (const UsageError& e) {
        CHECK(std::string(e.what()) == "ambiguous option -- ign");
    }
    REQUIRE_THROWS_AS(p({"--bogus"}), UsageError);
    REQUIRE_THROWS_AS(p({"-k"}), UsageError);
}

TEST_CASE("parser: permutation, '--', '-', and POSIXLY_CORRECT") {
    auto r = p({"a", "-u", "b", "--", "-w", "-"});
    CHECK(r.opts.size() == 1);
    CHECK(r.operands == std::vector<std::string>{"a", "b", "-w", "-"});
    auto posix = p({"a", "-u", "b"}, true);
    CHECK(posix.opts.empty());
    CHECK(posix.operands == std::vector<std::string>{"a", "-u", "b"});
    CHECK(p({"-", "-u"}).operands == std::vector<std::string>{"-"});
}

// D-42: no GNU abbreviation of four or more characters after "--" may change meaning or become ambiguous.
TEST_CASE("D-42: GNU long-option prefixes keep their meaning in every personality") {
    const auto reg = semsplinter::Registry::with_builtin_formats();
    struct Ref {
        Personality p;
        std::vector<std::string_view> names;
    };
    const std::vector<Ref> refs = {
        {Personality::diff,
         {"normal",
          "brief",
          "report-identical-files",
          "context",
          "unified",
          "ed",
          "forward-ed",
          "rcs",
          "side-by-side",
          "width",
          "left-column",
          "suppress-common-lines",
          "show-c-function",
          "show-function-line",
          "label",
          "expand-tabs",
          "initial-tab",
          "tabsize",
          "suppress-blank-empty",
          "paginate",
          "recursive",
          "no-dereference",
          "new-file",
          "unidirectional-new-file",
          "ignore-file-name-case",
          "no-ignore-file-name-case",
          "exclude",
          "exclude-from",
          "starting-file",
          "from-file",
          "to-file",
          "ignore-case",
          "ignore-tab-expansion",
          "ignore-trailing-space",
          "ignore-space-change",
          "ignore-all-space",
          "ignore-blank-lines",
          "ignore-matching-lines",
          "text",
          "strip-trailing-cr",
          "binary",
          "ifdef",
          "old-group-format",
          "new-group-format",
          "unchanged-group-format",
          "changed-group-format",
          "line-format",
          "old-line-format",
          "new-line-format",
          "unchanged-line-format",
          "minimal",
          "horizon-lines",
          "speed-large-files",
          "inhibit-hunk-merge",
          "sdiff-merge-assist",
          "color",
          "palette",
          "help",
          "version"}},
        {Personality::diff3,
         {"show-all", "ed", "show-overlap", "easy-only", "overlap-only", "merge", "text", "strip-trailing-cr",
          "initial-tab", "diff-program", "label", "help", "version"}},
        {Personality::merge,
         {"quiet", "ours", "theirs", "union", "diff3", "zdiff3", "marker-size", "object-id",
          "diff-algorithm"}},
        {Personality::merge_file,
         {"stdout", "object-id", "diff3", "zdiff3", "ours", "theirs", "union", "diff-algorithm",
          "marker-size", "quiet"}},
        {Personality::patch,
         {"strip",
          "fuzz",
          "ignore-whitespace",
          "context",
          "ed",
          "normal",
          "unified",
          "forward",
          "reverse",
          "input",
          "output",
          "reject-file",
          "ifdef",
          "merge",
          "remove-empty-files",
          "set-utc",
          "set-time",
          "quoting-style",
          "backup",
          "backup-if-mismatch",
          "no-backup-if-mismatch",
          "version-control",
          "prefix",
          "basename-prefix",
          "suffix",
          "get",
          "batch",
          "force",
          "quiet",
          "silent",
          "verbose",
          "dry-run",
          "posix",
          "directory",
          "reject-format",
          "binary",
          "read-only",
          "follow-symlinks",
          "help",
          "version"}},
    };
    for (auto& ref : refs) {
        auto tbl = table(ref.p, reg);
        for (auto name : ref.names) {
            INFO("personality " << static_cast<int>(ref.p) << " option --" << name);
            const Opt* exact = find_long(name, tbl);
            REQUIRE(exact);
            REQUIRE(exact->long_name == name);
            for (std::size_t len = 4; len < name.size(); ++len) {
                auto prefix = name.substr(0, len);
                // What the prefix means to the GNU tool: an exact name, or the only name it starts.
                std::vector<std::string_view> hits;
                for (auto n : ref.names)
                    if (n.starts_with(prefix))
                        hits.push_back(n);
                bool gnu_exact = std::find(hits.begin(), hits.end(), prefix) != hits.end();
                if (gnu_exact || hits.size() != 1)
                    continue;
                INFO("prefix --" << prefix);
                const Opt* got = nullptr;
                REQUIRE_NOTHROW(got = find_long(prefix, tbl));
                REQUIRE(got);
                CHECK(got->id == exact->id);
            }
        }
    }
}
