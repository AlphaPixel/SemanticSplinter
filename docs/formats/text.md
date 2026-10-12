# Text — Implementation Document

**Tier:** T0 · **Module:** `semantic-splinter-text` (plus the shared sequence-diff engine in `src/core/diff`) · **vcpkg feature:** none (mandatory, no dependencies) · **Depends on modules:** none
**Extensions / magic:** any. Selected by `--format=text`, by the fallback rule of SPEC §11 step 4, or by `-a/--text`. Probe confidence is low (10) so that every structured format wins over it.
**Status (v1 = MVP + v2, Phase 2):** done: GNU `diff` output styles, `diff3`, RCS `merge`, and `git merge-file`,
byte-identical to GNU diff 3.12, GNU diff3, and Git on the golden corpus (`tests/golden/`); Myers, histogram, and
patience; zealous refinement, zdiff3, `--eol`, UTF-16/32; unified patch apply with fuzz and rejects, and the
splinter patch envelope (text or JSON). Text patches are unified diffs, not payloads (`caps.patch` is false). Not
yet: context, normal, and ed patch input, `patch -D`, `patch --merge` on unified hunks, and `GIT binary patch` hunks
(#36); the indent heuristic (#133), `diff -D` and the group/line formats (exit 2, 3 with
`--exit-codes`; #134), `--unicode-case`, `--unicode-space`, East Asian Wide columns and `--posix` byte columns in side by
side (Q-TEXT-7, Q-TEXT-8; #135), Git extended patch headers (#36), and, untracked, intra-line word diff (§12).

## 1. Why it matters

`text` is the reference plugin and the compatibility baseline. It has three jobs:

1. **GNU emulation.** `splinter` invoked as `diff`, `diff3`, `patch`, or `merge` (RCS) on plain text must behave like the GNU/RCS tools closely enough that scripts, editors, and CI jobs that parse their output keep working. This is how users will first judge the tool.
2. **Shared engine.** The sequence-diff algorithms (Myers, histogram, patience) and the diff3 chunker live in `src/core/diff` and are reused by every `SequenceMerge` (JSON arrays, XML children, archive members, CSV rows). The text module is their thinnest client and therefore their test bed.
3. **Renderer for every format.** SPEC §7.2 says structured formats render `-u`, `-c`, `-y` and normal output by diffing their `dump()` lines. The text renderers produce that output, with hunk headers annotated by Address.

Users should be able to run `splinter -u a.txt b.txt | patch -p0` and `diff3 -m mine older yours` with byte-identical results to GNU tools on the compatibility corpus, and also use Git-style merge features (`zdiff3`, histogram) that GNU lacks.

## 2. Format anatomy

A text file here is: optional BOM, a sequence of lines in one encoding, each terminated by a line terminator, except possibly the last line ("no newline at end of file").

| Property | Values | Detection |
|---|---|---|
| Encoding | UTF-8 (default, also covers ASCII and is assumed for unknown 8-bit), UTF-8 with BOM `EF BB BF`, UTF-16LE/BE with BOM, UTF-32LE/BE with BOM, BOM-less UTF-16 (heuristic) | BOM first. Then BOM-less UTF-16 if at least 40% of the first 4 KiB of even (BE) or odd (LE) bytes are NUL and the other parity has almost none. Else bytes. |
| Line terminator | LF, CRLF, lone CR (classic Mac) | Counted per line. A file has a dominant convention plus per-line exceptions. |
| Final newline | present / absent | Last code unit. |
| Binary test | GNU rule: a NUL byte in the first buffer (we use 8 KiB) means binary, unless `-a`. UTF-16/32 with BOM is exempt (it is text in a known encoding). |

Line splitting follows GNU: **only LF terminates a line**. CR before LF is part of the line content unless `--strip-trailing-cr` is given. A lone-CR file is therefore one long line in GNU emulation. `--eol=auto` (Splinter extension, default only in merge modes, see §4) splits on CR, LF, and CRLF.

Encoding detection and the line splitter are exported as a shared helper (`src/core/text.cpp`, `semsplinter/text.hpp`) for CSV, JSON, and XML text, with a mapping from decoded positions back to original byte offsets so that span-splicing writers work on UTF-16 input too.

For UTF-16/32 the splitter works on code units (LF = `0A 00` or `00 0A`), so that line boundaries never fall inside a code unit. Lines are stored as **byte spans into the original buffer**, never decoded copies, so that output is byte-exact.

```cpp
struct Line { uint32_t off, len; uint8_t eol; /* none, lf, crlf, cr */ };
struct TextDoc : Document {
    std::vector<std::byte> bytes;   // owned input
    Encoding enc; bool bom;
    std::vector<Line> lines;
    EolStyle dominant_eol;
};
```

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Line | equivalence class of normalized content (see §4) | `/lines#range=START,LEN` (0-based line index into the side that the change refers to) | SequenceMerge |
| Encoding + BOM | singleton | `/encoding` | OpaqueLeaf (KeyedMerge of file properties) |
| Dominant line terminator | singleton | `/eol` | OpaqueLeaf |
| Final newline | singleton | `/final-newline` | OpaqueLeaf |

Human renderers print 1-based line numbers like GNU. Addresses are 0-based, like every other `range=` in SPEC §3.1.

There are no dependencies between lines. The only invariant after merge is that the output encoding is consistent: lines that came from a side with a different encoding are transcoded (see §6), never mixed.

**Equivalence classes.** Each line is hashed (XXH3-64) after normalization. Hash buckets map to class IDs, and a class is assigned only after a byte comparison of the normalized form, so hash collisions can never make two different lines equal. This is the same contract as GNU diff's equivalence classes and Git's `xdl_classify_record`. Class IDs are assigned in order of first appearance in A then B (then C for diff3), so they are deterministic. All algorithms run on `std::span<const uint32_t>` of class IDs.

## 4. Normalization

All options affect only **comparison**. Output always prints original bytes.

| Option | GNU semantics (we match) | Implementation |
|---|---|---|
| `-i`, `--ignore-case` | Case-insensitive comparison | ASCII fold in GNU. We fold ASCII by default and full Unicode simple case fold with `--unicode-case` (extension; not implemented yet). |
| `-E`, `--ignore-tab-expansion` | Tab vs equivalent spaces | Expand tabs to `--tabsize` (default 8) columns before hashing. |
| `-Z`, `--ignore-trailing-space` | Ignore white space at line end | Strip trailing `[ \t\v\f\r]`. |
| `-b`, `--ignore-space-change` | Runs of white space compare equal, trailing ignored | Collapse runs to one space, strip trailing. |
| `-w`, `--ignore-all-space` | Ignore all white space | Remove all white space. |
| `-B`, `--ignore-blank-lines` | Ignore changes whose lines are all blank | **Post-filter on hunks**, not a normalization: a change group whose deleted and inserted lines are all blank is dropped. Mixed groups are printed whole. |
| `-I RE`, `--ignore-matching-lines=RE` | Ignore changes whose lines all match RE | Post-filter as `-B`. RE is POSIX BRE in GNU (grep syntax). We use `std::regex` with `basic` grammar, which covers common patterns. Q-TEXT-6. |
| `--strip-trailing-cr` | Strip CR before LF on input | Applied at split time, so line content and output both lose the CR (GNU prints the stripped line). |
| `-a`, `--text` | Treat all files as text | Skips binary test. |

The "white space" set for `-b`/`-w` is GNU's: space, tab, VT, FF, CR. Not Unicode spaces (Q-TEXT-7).

Splinter extensions, merge modes only: `--eol=auto|keep` (accepted by `splinter merge` and `diff3`; default `auto` in `splinter merge`, `keep` in diff, `diff3`, `merge-file`, and the Git driver). With `auto`, lines are compared without their terminator, and an EOL convention change that covers at least 90% of one side's lines is lifted out as a single `/eol` change. This makes "Alice converted the file to CRLF, Bob edited line 40" merge cleanly. It is reported as `auto: eol-convert`, never silent.

Never normalized: Unicode normalization (NFC vs NFD), BOM presence (it is a separate `/encoding` unit), tab vs space for non-`-E` modes.

## 5. Diff

### 5.1 Engine (`src/core/diff`)

```cpp
namespace semsplinter::seqdiff {
enum class Algorithm { myers, minimal, patience, histogram };
struct Edit { uint32_t a0, a1, b0, b1; };    // half-open, a-range replaced by b-range
struct Options { Algorithm algo; bool indent_heuristic; uint64_t cost_limit; };
std::vector<Edit> diff(std::span<const uint32_t> a, std::span<const uint32_t> b, const Options&);
}
```

Common steps for every algorithm: trim common prefix and suffix; then run the algorithm on the middle; then post-process.

- **Myers** (default for GNU emulation). Linear-space O((N+M)·D) with the bidirectional middle-snake divide and conquer from Myers 1986. Two GNU-specific behaviours affect which of several equal-cost scripts is chosen and must be reproduced for golden compatibility:
  - *Discarding confusing lines*: lines that occur in only one file are removed before the search and re-inserted as changes; lines that occur very often in the other file are provisionally discarded when they sit in runs of discardable lines. This is the main speed trick and also changes hunk shapes.
  - *Too-expensive heuristic*: when the edit cost in a recursion exceeds a limit that grows with the square root of the input size (minimum 4096 in GNU), the search accepts the furthest-reaching diagonal instead of the optimal split. `-d/--minimal` disables both tricks. `--speed-large-files` (`-H`) lowers the limit.
  - This must be a clean-room implementation from the paper plus black-box testing against GNU outputs. **No GNU diff (GPLv3) code is read or copied.**
- **Minimal**: Myers without heuristics or discards.
- **Patience**: unique lines common to both regions form anchors; their longest increasing subsequence (patience sorting, O(n log n)) splits the problem; recurse; fall back to Myers in regions without unique common lines.
- **Histogram** (default for `SequenceMerge` per SPEC §5.1, and for `--diff-algorithm=histogram`): Git/JGit algorithm. For a region, build an occurrence histogram of A's elements, scan B for the element with the lowest occurrence count in A that also appears in B, extend it into the longest common run, split there, recurse. If every candidate occurs more than 64 times (`max_chain`), fall back to Myers for that region.

Post-processing:

- **Compaction (slider)**: a change group that can slide up or down because the lines at its edges are equal is moved to a canonical position: GNU's `shift_boundaries` behaviour for GNU emulation (merge with adjacent groups when possible, otherwise move down as far as possible).
- **Indent heuristic**: Git's default since 2.14. Score each slider position by blank lines and indentation around the boundaries and pick the best. On in Git-compatible modes, off in GNU modes. Not implemented yet: every mode uses the GNU compaction.

Cost control: `cost_limit` (default derived from input size as above) bounds time on pathological inputs; it is ignored when `algo == minimal`. Worst-case memory is O(N+M) for Myers, O(N) for the histogram table.

### 5.2 Output formats (GNU diff)

Implemented in `src/core/render.cpp` because structured formats use the same renderers on their dump lines.

| Format | Option | Priority | Notes |
|---|---|---|---|
| Normal | default | MVP | `NaM`, `NcM`, `NdM` commands, `<`/`>`/`---`. |
| Unified | `-u`, `-U NUM`, `--unified[=NUM]` | MVP | Header `--- LABEL\tTIMESTAMP`. Hunks join when the gap is at most 2×context. `@@ -l,s +l,s @@` with GNU's omitted `,1` rule and `,0` for empty ranges (line number is the line *before* the empty range). |
| Brief | `-q` | MVP | `Files A and B differ`, binary inputs included (GNU; see binary.md §5.3). |
| Context | `-c`, `-C NUM` | MVP | `***************`, `*** a,b ****`, `--- c,d ----`, `!`/`+`/`-` markers, omitted side when it has no changes. |
| `\ No newline at end of file` | all of the above | MVP | Exact GNU text. |
| Side by side | `-y`, `-W NUM`, `--left-column`, `--suppress-common-lines` | v2 | Gutter `|`, `<`, `>`, `(`, `)`. Column math follows GNU (half width minus gutter, tab expansion to tab stops). East-Asian-wide characters: GNU counts columns per byte in C locale; we count per code point by default (Q-TEXT-8). |
| ed script | `-e` | v2 | Commands emitted bottom-up; a line consisting of `.` is escaped with the `..`/`s/.//` trick as GNU does. |
| Forward ed | `-f` | v2 | Top-down, not runnable by ed. |
| RCS | `-n`, `--rcs` | v2 | `aN COUNT` / `dN COUNT`. Needed by RCS `merge` emulation only marginally. |
| ifdef | `-D NAME` | later | `#ifndef NAME` / `#else` / `#endif` merged output. Not implemented yet: exits as unsupported (2, or 3 with `--exit-codes`). |
| Group/line formats | `--old-group-format` etc. | later | printf-like specifiers `%<`, `%>`, `%=`, `%L`, `%l`, `%dN`. Implemented on top of ifdef machinery. Not implemented yet, as `-D`. |
| Function headers | `-p`, `-F RE` | v2 | Text after `@@` (unified) or `***************` (context): last preceding line matching RE (`-p` = `^[[:alpha:]$_]`). |
| Misc | `-t`, `-T`, `--tabsize`, `--color[=WHEN]`, `--label`, `--horizon-lines` | MVP (`--label`) / v2 | `--horizon-lines` is accepted and ignored (it only affects GNU's speed). |

Timestamps in headers: GNU prints the file mtime with nanoseconds and zone (`2026-10-07 15:17:03.123456789 +0200`). We do the same when inputs are real files, because tools parse it. `--label` overrides. In Git modes there is no timestamp (Git provides its own headers). File mtimes are input data, not wall-clock time, so this does not break the determinism invariant. Golden tests always pass `--label`.

### 5.3 Structured-format rendering

For a structured format, `dump()` produces lines plus a parallel vector `std::vector<AddressRef>` mapping each line to the Address of the unit it belongs to. The renderer diffs the dump lines with histogram, then prints hunk headers as `@@ -l,s +l,s @@ ADDRESS` (the GNU "function name" slot), where ADDRESS is the longest common Address prefix of the hunk's lines. Per-hunk metric comments (`# 212 pixels changed`) are emitted as lines starting with `#` *after* the header only when `--annotate` is on, because they would break `patch` on the output. Q-TEXT-9.

Consistency rule: `splinter -u x.png y.png` and `git diff` with `textconv = splinter --dump` must show the same lines; only headers differ.

### 5.4 Metrics

Per change: lines deleted, lines inserted. Totals: changed lines, hunks, and `similarity = 1 - changed/(len(a)+len(b))`. Reported in the JSON report's `metrics`.

### 5.5 Cost on large files

Memory is input bytes plus 16 bytes per line (span plus class ID). A 1 GiB log with 20 M lines needs about 1.3 GiB. Above `--max-memory` the load fails with a limit error. Myers with the cost limit stays roughly linear on realistic inputs; `-d` on two unrelated 1 M-line files is quadratic and is documented as such.

## 6. Merge

### 6.1 Algorithm (diff3)

1. Diff base→ours and base→theirs with the selected algorithm (`--diff-algorithm`, default Myers in GNU modes and in `git merge-file` mode to match those tools; histogram in `splinter merge` native mode).
2. Walk both edit scripts over base line ranges, forming chunks: *stable* (neither side changed), *ours-only*, *theirs-only*, *both-identical*, *both-different* (conflict). Two edits whose base ranges overlap or touch (adjacent, with zero stable lines between them) belong to one chunk. This adjacency rule is GNU diff3's and Git's, and it is deliberately conservative.
3. **Refinement** (Git and native modes, `XDL_MERGE_ZEALOUS` behaviour): inside a conflict chunk, diff ours against theirs; leading and trailing lines that are equal on both sides are moved out of the conflict, and the chunk can split into several smaller conflicts. Git's `zealous_alnum` further merges conflicts separated only by lines with no alphanumeric characters. GNU diff3 does not do this, so it is off in GNU modes.
4. Emit result and Conflicts (`reason: both-modified`, or `add-add` when the base range is empty).

### 6.2 Conflict output (`--conflict=mark`, the default for text)

Text is the one format where marking is natural, so `--conflict=mark` is the text default in every mode except when `--ours/--theirs/--union` resolve it.

| Style | Selected by | Layout |
|---|---|---|
| `merge` | `--conflict-style=merge`, RCS `-E`, Git default | `<<<<<<< L1` / ours / `=======` / theirs / `>>>>>>> L3` |
| `diff3` | `--conflict-style=diff3`, GNU `diff3 -m` (`-A` default), RCS `-A`, Git `--diff3` | adds `||||||| L2` / base before `=======` |
| `zdiff3` | `--conflict-style=zdiff3`, Git `--zdiff3` | as diff3, but lines common to ours and theirs at the start and end of the conflict are moved outside the markers (base section stays complete) |

- Marker length: `--marker-size=N` (default 7). In the Git merge driver it comes from `%L` (the `conflict-marker-size` attribute), and is passed positionally as in CLI.md §7.3.
- Labels: `-L` up to three times (diff3, RCS merge, git merge-file order: ours/mine, base/older, theirs/yours). Defaults are the file names. In Git driver mode, labels come from `%S`, `%X`, `%Y` where the Git version provides them (verified, CLI.md §7.3), else `ours`/`base`/`theirs`.
- Marker line endings: if the file's dominant EOL is CRLF, markers end with CRLF (Git does this; GNU always writes LF). Follow the active mode.
- A conflict at end of file where one side lacks the final newline: a newline is added before the closing marker so markers stay on their own line (Git behaviour).
- `--union`: ours lines then theirs lines, no markers (Git). Exit 0.
- `--ours` / `--theirs`: take that side for conflicted chunks only. Exit 0.

### 6.3 GNU diff3 non-merge outputs

| Option | Output |
|---|---|
| (none) | `====`, `====1/2/3` sections with `1:a,bc` / `2:...` / `3:...` and two-space indented lines |
| `-e` / `-E` / `-x` / `-X` / `-3` | ed scripts incorporating changes (all / with brackets / overlapping only / overlapping with brackets / non-overlapping only) |
| `-i` | append `w` and `q` to ed scripts |
| `-T` | tab-aligned lines |
| `-m` | merged file; with no `-e/-E/-x/-X/-3` this means `-A` (all changes, conflicts bracketed with base) |

ed script outputs are v2. `-m` with `-A` and `-E` is MVP.

### 6.4 RCS `merge` and `git merge-file` emulation

- RCS: `merge [-A|-E|-e] [-p] [-q] [-L l1 [-L l2 [-L l3]]] file1 file2 file3`. Default `-E`. Writes into file1 unless `-p`. Exit 0 clean, 1 conflicts, 2 trouble. RCS `merge` and native `splinter merge` are one mode (`run_merge`): histogram, zealous refinement, and `--eol=auto` apply to both (a deviation from Q-TEXT-2 to Q-TEXT-4).
- `git merge-file`: **yes, emulate it**, as the mode `splinter merge-file` (and the alias name `git-merge-file` is not installed, to avoid shadowing Git). Options `-p`, `-q`, `-L`×3, `--ours/--theirs/--union`, `--diff3`, `--zdiff3`, `--marker-size`, `--diff-algorithm`. Exit status is the **number of conflicts capped at 127**, negative-equivalent (255) on error. This costs little (the refinement step is needed anyway for the Git driver) and lets users swap `splinter` in for Git-generated golden tests. `--object-id` is not supported (no repository access; SPEC §9).

### 6.5 Encoding and EOL in merge

- All three sides decoded to the same encoding: if they differ, merging is done on UTF-8 transcodes and the output takes ours' encoding and BOM, with a warning `encoding-converted`. Lines that cannot be represented (UTF-8 to Latin-1 is never done because Latin-1 is not detected as distinct) do not arise.
- `/encoding`, `/eol`, `/final-newline` merge with KeyedMerge rules. Both sides changing the EOL style differently is a conflict on `/eol`, resolved to ours.

### 6.6 Patch payload

Text patches are plain unified diffs (SPEC §6), so GNU `patch` can apply them. The `splinter-patch` envelope is used only when a text file is part of a multi-format patch, and then the hunk body is the unified hunk verbatim.

## 7. Serialization and preservation

Output is the concatenation of original line spans chosen by the merge, plus markers. Byte-exact for every line that came from an input. Each line keeps the terminator it had in its source side, except when `--eol=auto` lifted an EOL conversion, in which case lines from the non-converting side are rewritten to the new terminator (reported). The final-newline property follows the merged `/final-newline`.

Nothing is lost. The only re-encoding case is the cross-encoding merge of §6.5, which is reported.

### 7.1 Envelope fields

**Layer stack:** `file → [BOM] → character encoding (UTF-8, UTF-16LE/BE, UTF-32LE/BE, or opaque 8-bit) → line terminators (LF, CRLF, CR) → lines → [final newline]`.

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| BOM (`EF BB BF`, `FF FE`, `FE FF`, UTF-32 forms) | encoding | detected (§2) | from the merged `/encoding` unit (ours on conflict) |
| Character encoding | encoding | UTF-8 well-formedness checked when the input was UTF-8; ill-formed input is kept as bytes | lines are byte spans and are copied raw; transcoded only in the cross-encoding merge (§6.5, `encoding-converted`) |
| Line terminator per line | encoding | counted (§2) | kept per line; rewritten only when `--eol=auto` lifted an EOL conversion (§7) |
| Final newline | content (`/final-newline`) | detected | from the merged unit |

There are no framing, integrity, authenticity, derived, or volatile fields. Embedded transport encodings (base64 data URIs, PEM blocks) are content here; only the structured modules (json, xml) look through them.

**Raw copy:** every line is a byte span of an input, so untouched lines are byte-identical by construction. **Layout stability** is not a concern (no offsets).

**GNU emulation:** BOM, encoding, and EOL bytes are plain content, so `diff`/`diff3` output matches GNU byte for byte. The encoding classes apply in native and Git modes, where an EOL-only or BOM-only change between two files is reported as `encoding` churn (`/eol`, `/encoding`) rather than as changed lines (§4). Git's `text`/`eol`/`working-tree-encoding` conversions happen before a driver sees the blob, so splinter sees repository-form bytes in Git modes (verify for merge drivers with `merge.renormalize`).

**Expected churn:** a one-line edit changes that line's bytes (`content`); a whole-file CRLF conversion is attributed 100% `encoding`.

## 8. Validation

- Re-split the output and check encoding validity (UTF-8 well-formed if the input was).
- In marker modes, check that marker counts are balanced and that the conflict count in the report equals the number of `<<<<<<<` blocks emitted.
- Patch mode: after applying, if the patch carries a `+++` hash (splinter envelope), compare.

## 9. Libraries

| Candidate | vcpkg | License (verified) | Status | Verdict |
|---|---|---|---|---|
| dtl (Diff Template Library) | `dtl` 1.21 (verified) | BSD 3-clause per the repo's COPYING. vcpkg metadata says `BSD-4-Clause`, which does not match the file (verified both). | Small, stable, little activity | Reject. Wu O(NP) only; no histogram, patience, GNU heuristics, diff3, or GNU renderers. Little would be reused. |
| Git's `xdiff` (fork of LibXDiff) | not packaged (vendoring needed) | Core files LGPL-2.1-or-later (verified header of `xdiff/xdiff.h`). `xhistogram.c` is Eclipse Distribution License 1.0, a BSD-3 style license (verified). | Actively maintained inside Git | Reject for linking. LGPL in a statically linked `splinter.exe` requires shipping relinkable objects. Clean-room rule (D-29): no xdiff source, including the EDL-licensed `xhistogram.c`, is read or adapted. A later optional feature may link libgit2/xdiff. |
| LibXDiff (Libenzi) | `libxdiff` 0.23 (verified, no license field) | LGPL-2.1+ (from upstream header, assumed for the port) | Unmaintained since about 2008 | Reject. Same LGPL issue, old, no histogram. |
| libgit2 (contains xdiff) | `libgit2` 1.9.7 (verified) | GPLv2 with linking exception (assumed from upstream; vcpkg metadata lacks it) | Active | Reject. Huge dependency for one function. |
| diff-match-patch (Google) | no port (verified) | Apache-2.0 (assumed) | Repository believed archived | Reject for lines. Its character-level semantic cleanup is an idea for a later intra-line word diff. |
| GNU diffutils | not a library | GPLv3 | Active | Never linked or copied. Used only as the black-box oracle in tests. |

**Recommendation: own implementation.** Estimates: Myers with GNU heuristics and middle snake 500 LOC, patience 200, histogram 250, slider compaction and indent heuristic 300, diff3 chunker and refinement 450, GNU renderers (normal, unified, context, brief, side-by-side, ed, rcs, ifdef) 1100, GNU-patch applier 700, encoding/EOL splitter 250. About 3.7k LOC in total, of which about 1.7k is the reusable core diff. No third-party dependency. Regex for `-I`/`-F` uses `std::regex` (Q-TEXT-6).

## 10. Hard cases and explicit non-goals

- **Exact GNU hunk shapes.** Equal-cost edit scripts can differ. The clean-room Myers aims for identical output on the corpus, but where GNU's internal tie-breaking cannot be reproduced without reading its code, we accept a different script of equal cost and document it (Q-TEXT-1).
- **Lone CR files** are a single line in GNU mode. That is faithful but useless; `--eol=auto` fixes it in merge modes.
- **BOM-less UTF-16 without NULs** (e.g. CJK-only text) is indistinguishable from binary noise. Use `--encoding=utf-16le` (not implemented yet).
- **Legacy 8-bit encodings** (Shift-JIS, Windows-1252) are treated as opaque bytes. That is correct for diff and merge because line splitting on LF is safe in all of them except Shift-JIS second bytes, which never equal 0x0A. Case folding for `-i` is ASCII only there.
- **Extremely long lines** (minified JS, 50 MB single line): diff is trivially one change; `--word-diff` style refinement is a later feature.
- **`patch` corner cases**: reversed-patch detection prompts in GNU; we never prompt (`-t`/`--batch` semantics always) and report "Reversed (or previously applied) patch detected" then skip, or apply with `-R`. `-N` and `--forward` supported.
- Non-goals: GNU `sdiff` interactive mode, `cmp`, locale-dependent messages (messages are English, like `LC_ALL=C`).

### 10.1 GNU patch emulation (summary; full contract in CLI.md)

Input formats: unified, context, normal, ed (via a built-in ed subset: `a`, `c`, `d`, `s/.//`, `.`). Hunk location: try the stated line plus the accumulated offset, then search outward alternately forward and backward. With fuzz F (`-F`, default 2), up to F leading and trailing context lines may be ignored, trying fuzz 0, then 1, up to F. Messages match GNU (`Hunk #2 succeeded at 47 (offset 3 lines).`, `Hunk #1 FAILED at 10.`, `with fuzz 1`). Rejects go to `FILE.rej` in the hunk's original format (unified when the input was unified). `--merge[=merge|diff3]` writes conflict markers instead of rejects. `-p`, `-R`, `-N`, `-o`, `-r`, `-b`/`-V`/`-z`, `--dry-run`, `-E`, `-s`, `--binary` supported. Git extended headers (`diff --git`, rename, mode, `GIT binary patch`) are parsed for renames and modes in v2.
Implemented today: unified input and splinter envelopes; context, normal, and ed input, `--merge` on unified hunks, and
`GIT binary patch` are #36; rename and mode headers are #36 too.

### 10.2 GNU compatibility findings (black-box, Phase 2)

These were found by comparing against GNU diffutils 3.12 and Git, not by reading their code (D-29). The golden
corpus in `tests/golden/` pins them.

- **Analysed region.** GNU sets aside the byte-identical head and tail of the two files and keeps only
  `max(--horizon-lines, output context)` lines of each next to the changes. Context is 0 for normal, ed, RCS,
  and side-by-side output. Discarding (lines with no match on the other side) and boundary compaction then
  look at that region only, which decides between equal-cost scripts. The identical ends are found on raw
  bytes even under `-i`/`-w`. GNU diff3 and `git merge-file` run their two-way diffs with a horizon of 0.
- **Myers split.** Recursion is on the midpoint of an optimal path; diagonals are scanned from high to low
  within the grid; matches inside the midpoint snake come from the prefix/suffix trimming of the halves.
- **Compaction.** A change group slides up to merge with earlier groups, then down as far as possible, and is
  moved back to the last position where it lines up with a change in the other file, if there is one.
- **Context length.** Several `-C`/`-U`/`-NUM` options give the largest value; `-c`/`-u` count as 3.
- **`-B`/`-I`.** An ignorable change never starts a hunk. It joins the current hunk when it is less than
  `context` lines from the previous change (a regular change joins within `2 × context`). A line is blank
  if it is empty after normalization.
- **Missing final newline.** `-Z`, `-b`, and `-w` treat it as trailing white space. Ed output compares as if
  the newline were there, prints its script, then fails (exit 2) naming each such file. Side by side marks
  `\` or `/` when only the left or right line is incomplete, and ends a row without a newline when every
  shown line is incomplete.
- **Side by side.** With tabs, the right column starts at `(W + t + 1) / (2t) × t` and each column is
  `min(start − 3, W − start)` wide; with `-t`, the right column starts at `W − (W − 3) / 2`. The gutter mark
  sits at `(width + start − 1) / 2`. A tab reaching the column edge is dropped (with `-t`, expanded only up
  to the edge). A CR returns to column 0, so GNU pads back to the column start; under `-t` the line prefix
  is repeated after a CR in the other formats. Identical files are still printed.
- **diff3.** Labels (`-L`) are accepted only with bracketing output (`-A`, `-E`, or `-m` alone), and `-m`
  rejects `-i`. `-X` prints like `-x`. The report mode always exits 0; ed scripts exit 1 only when they
  contain brackets. `-m` (that is, `-A`) brackets identical changes as `<<<<<<< older` / base /
  `=======` / mine and counts them as conflicts. Lines are written as they are, so a conflict after a line
  without a newline continues on that line (Git adds the newline instead).
- **Known differences** (Q-TEXT-1, listed in `tests/cli/test_golden.cpp`): side-by-side counts UTF-8 code
  points where GNU in the C locale counts bytes (Q-TEXT-8); and for a few inputs GNU diff3 3.12's `-A`/`-E`
  ed scripts and `-m` output contradict its own `====` report. Splinter follows the report.

## 11. Test plan

- **GNU oracle corpus.** A generator script (run on Linux CI with GNU diffutils and RCS installed) produces triples and pairs from: random edits on real-world files (source code, prose, CSV), adversarial cases (all lines equal, all different, repeated blocks, empty files, single line without newline, CRLF, lone CR, NUL-containing), and every option combination in a matrix. It stores GNU outputs as golden files. Splinter's tests compare byte-for-byte with the golden file. Categories where exact match is not required (Q-TEXT-1) instead assert: same number of changed lines, and `patch` round-trips.
- **Git oracle corpus** for `merge-file`, `--diff3`, `--zdiff3`, histogram, patience, indent heuristic, marker size, and exit codes (conflict count).
- **Property tests** (Catch2 generators): `apply(a, diff(a,b)) == b` for every algorithm and output format that `patch` understands; `merge(x, x, y) == y`; `merge(b, o, t)` symmetric up to marker order; minimal Myers cost is not larger than any other algorithm's cost; determinism across runs.
- **Fuzz**: `fuzz_text_diff` (two buffers split from one input), `fuzz_text_merge`, `fuzz_patch_parse` (malformed patches must never crash or loop).
- **Performance**: 1 M-line files with 1% random changes must diff in under 1 s release build; quadratic cases bounded by the cost limit.

## 12. Phases

| Phase | Content | Effort | State |
|---|---|---|---|
| MVP | Splitter (UTF-8, BOM, LF/CRLF), equivalence classes, Myers + minimal + histogram, normal/unified/context/brief renderers, `-i -E -Z -b -w -B -I --strip-trailing-cr -a`, diff3 merge with merge/diff3 styles, `-L`, `--marker-size`, GNU `diff3 -m`, RCS `merge`, unified patch apply with fuzz and rejects, dump-line rendering for other formats | L | done |
| v2 | Patience, indent heuristic, zealous refinement, zdiff3, `merge-file` mode, side-by-side, ed/forward-ed/rcs outputs, diff3 non-merge outputs, `-p`/`-F`, context/normal/ed patch input, UTF-16/32, `--eol=auto`, `--color` | M | partly: indent heuristic not started (#133); context/normal/ed patch input not started (#36) |
| later | ifdef and group/line formats, Git extended patch headers, intra-line word diff, `--from-file`/`--to-file` | M | partly: `--from-file`/`--to-file` done; the rest not started (`patch -D`: #36) |

## 13. Open questions

- **Q-TEXT-1**: Must Myers output be byte-identical to GNU diff on all inputs? **Decided:** Identical on the golden corpus is the goal; where tie-breaking differs, equal-cost scripts are accepted and listed as known differences. **Why:** exact equivalence would require studying GPL code, which a clean-room implementation must avoid.
- **Q-TEXT-2**: Default algorithm for `splinter merge` (native mode)? **Decided:** histogram, with Myers in `diff3`, RCS `merge`, and `merge-file` modes. The code deviates: RCS `merge` is the native mode and uses histogram (§6.4); the Git driver uses Myers. **Why:** histogram gives better merges; emulation modes must match their oracles.
- **Q-TEXT-3**: Enable Git-style zealous conflict refinement in native `splinter merge`? **Decided:** yes (native and Git driver), no in GNU/RCS modes. The code deviates: RCS `merge` refines too, as it is the native mode. **Why:** smaller conflicts are the project's goal, and GNU compatibility is only promised in emulation modes.
- **Q-TEXT-4**: Should `--eol=auto` be the default in merges? **Decided:** yes in native and Git merge modes, off in GNU emulation, always reported. The code deviates: on by default only in `splinter merge` (and so in RCS `merge`); the Git driver leaves it off and has no `--eol` option. **Why:** whole-file EOL conversion is a common Windows/Unix collision and is unambiguous when it covers at least 90% of lines.
- **Q-TEXT-5**: Emulate `git merge-file`? **Decided:** yes, as `splinter merge-file`, without `--object-id`. **Why:** small extra cost over the Git driver, and it gives a Git oracle for tests.
- **Q-TEXT-6**: Regex engine for `-I` and `-F`? **Decided:** `std::regex` basic grammar; switch to a small own POSIX BRE translator to ECMAScript if compatibility gaps show up. **Why:** no dependency, and patterns used in practice are simple; `std::regex` is slow but runs only on changed lines.
- **Q-TEXT-7**: Should `-b`/`-w` treat Unicode spaces (NBSP, U+3000) as white space? **Decided:** no by default, `--unicode-space` extension (not implemented yet). **Why:** GNU in C locale does not, and golden tests run in C locale.
- **Q-TEXT-8**: Side-by-side column width for multibyte characters? **Decided:** count code points, East Asian Wide as 2, when input is UTF-8; byte columns with `--posix`. The code deviates: it counts every code point as 1, and diff has no `--posix`. **Why:** byte counting breaks layout for non-ASCII text and nobody relies on that.
- **Q-TEXT-9**: How to annotate structured-format hunks without breaking `patch`? **Decided:** Address goes in the `@@ ... @@` trailing text (which patch ignores); metric comments only with `--annotate`. **Why:** keeps `-u` output appliable and consistent with textconv.
- **Q-TEXT-10**: Accept and diff BOM-less UTF-16 automatically? **Decided:** yes when the NUL-parity heuristic is strong (at least 40% parity NULs and under 2% on the other parity), else binary. **Why:** Windows tools (e.g. `.reg`, some `.rc` files) write UTF-16 and Git users want to see their diffs.
- **Q-TEXT-11**: Header timestamps in GNU modes? **Decided:** GNU format from file mtime when inputs are files, omitted for stdin and Git modes. **Why:** tools parse them, and mtimes are input data, not wall-clock time.
