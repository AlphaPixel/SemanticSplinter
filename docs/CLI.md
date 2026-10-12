# `splinter` Command-Line Specification

**Status (2026-10-12):** implemented for v1 (written as Draft v0.1). Options that are accepted but not built yet
fail as `unsupported` (exit 2, or 3 with `--exit-codes`), as noted where they are described and in the patch
personality's **Not yet** list; post-v1 options such as `--sign-key` fail the same way with
`unsupported (built without crypto)`. One executable, with five personalities that copy their
option syntax from the tools users already know:

| Personality | Emulates | Default output |
|---|---|---|
| **diff** | GNU `diff` 3.x | change report to stdout |
| **diff3** | GNU `diff3` | three-way report, or merged file with `-m` |
| **merge** | RCS `merge` / `git merge-file` | merged file written **in place** into the first file |
| **patch** | GNU `patch` | apply a splinter patch (or unified diff) |
| **tool** | — (splinter-only) | `dump`, `inspect`, `formats`, `git-diff`, `git-merge`, `git-setup` |

Every GNU option the personality defines is **accepted**. Options that have a semantic meaning are
**honoured**. Options that make no sense for a binary format (`-e` ed scripts for a PNG) produce
`splinter: option '-e' not supported for format 'png'` and exit 2, rather than being silently ignored.

---

## 1. Selecting the personality

The rules are evaluated in this order. The first match wins.

1. **argv[0] basename**. Strip the directory and any `.exe`, then compare case-insensitively (Windows):

   | basename contains | personality |
   |---|---|
   | `diff3` | diff3 |
   | `merge` | merge |
   | `patch` | patch |
   | `diff` | diff |
   | anything else (`splinter`) | continue with rule 2 |

   Installation creates `splinter-diff`, `splinter-diff3`, `splinter-merge`, and `splinter-patch` (symlinks
   on POSIX, hardlinks or copies on Windows). A user who links `splinter` as `diff` gets a drop-in `diff`.

2. **Subcommand word as the first argument**: `diff`, `diff3`, `merge`, `merge-file`, `patch`, `dump`, `inspect`,
   `formats`, `git-diff`, `git-merge`, `git-setup`, `help`, `version`. `merge-file` emulates `git merge-file`
   (text.md §6): its exit status is the conflict count capped at 127, as in Git, while RCS `merge` keeps 0/1/2. To use a *file* named like a
   subcommand, write `./diff` or put it after `--`.

3. **Mode flag as the first argument**:

   | Flag | Personality | Why this flag |
   |---|---|---|
   | `-m`, `--merge` | diff3 with `-m` (merged output) | `diff3 -m` already means exactly this. GNU `diff` does not use `-m`. |
   | `-3`, `--diff3` | diff3 (report) | Mnemonic. `-3` is `--easy-only` inside diff3, which is harmless here because it is consumed as the selector. |
   | `--patch`, `-P` | patch | **[D-12]** `-p` cannot select patch mode (see 1.1). GNU `diff` and `patch` do not use `-P`. |
   | `--diff` | diff | Explicit form for scripts. There is no short flag: diff is the default, and `-d`/`-D` already mean `--minimal`/`--ifdef` in diff. |

   Note: when `-3` is consumed as the selector, diff3's own `-3` (`--easy-only`) has to be spelled
   `--easy-only` or used after `splinter diff3`.

4. **Otherwise: diff.** `splinter a.png b.png` is a diff.

### 1.1 Why not `splinter -p` for patch and `splinter -d` for diff

- `-p` in diff means `--show-c-function`. In patch it is `-pNUM` (strip). Writing `splinter -p1 x.patch`
  would be ambiguous between "diff, show function, files `1`…" and "patch with strip 1". An unambiguous
  short selector is worth more than the mnemonic, so patch uses `-P` / `--patch` / `splinter patch` / `splinter-patch`.
- `-d` in diff means `--minimal`. Overloading it would break muscle memory (`diff -d`), and diff is the default anyway.

Once the personality is chosen, the remaining arguments are parsed **only** against that personality's table.

### 1.2 Parser behaviour (all personalities)

GNU `getopt_long`-compatible parser **[D-13]**: vendored musl `getopt_long` (MIT) if it covers the behaviour below, otherwise
hand-written. The vcpkg `getopt` port is not used (on Windows it pulls `getopt-win32`, LGPL-3.0-only). Format modules declare
their own long options, merged into each personality's table at registration **[D-51]**:

- Short option clustering (`-uwB`), attached or detached values (`-U5`, `-U 5`, `-p1`, `-p 1`).
- Long options with `=` or a separate value, and optional arguments only with `=` (`--color`, `--color=always`).
- Unique-prefix abbreviation of long options (`--ign` is ambiguous, `--ignore-case` is not), as GNU does.
  Splinter-only long options are chosen so that **no GNU abbreviation of four or more characters after `--`
  changes meaning or becomes ambiguous** in any personality. Shorter prefixes (`--f`, `--in`) may become
  ambiguous, because a strict zero-collision rule would make nearly every new name impossible. CI has a test
  that checks every GNU prefix of length ≥ 4 against each personality's full table. An earlier draft used
  `--report`, which captured GNU's unique abbreviation of `--report-identical-files`, so it was renamed to `--json`.
- `--` ends options. Options and operands may interleave (GNU permutation), unless `POSIXLY_CORRECT` is set.
- Environment: `DIFF_OPTIONS` is **not** honoured (GNU removed it). `SPLINTER_OPTIONS` is prepended to argv, if set.

---

## 2. diff personality

```text
splinter [OPTION]... FILES
FILES: FILE1 FILE2 | DIR1 DIR2 | DIR FILE | FILE DIR | --from-file=F FILE... | --to-file=F FILE...
```

### 2.1 GNU diff options and their meaning in splinter

| Option | GNU meaning | Splinter |
|---|---|---|
| `--normal` | normal format | default. Format-specific "normal" report |
| `-q`, `--brief` | report only whether files differ | `Files A and B differ`, as GNU does for all files under `-q`. Without `-q`, the binary plugin prints GNU's `Binary files A and B differ` (to be confirmed against the GNU golden corpus) |
| `-s`, `--report-identical-files` | | ✔ |
| `-c`, `-C NUM`, `--context[=NUM]` | context format | context-style over dump lines and units |
| `-u`, `-U NUM`, `--unified[=NUM]` | unified format | unified-style over dump lines. `@@` headers carry addresses |
| `-e`, `--ed` / `-n`, `--rcs` / `-D NAME`, `--ifdef` | ed / RCS / ifdef output | text format only. Otherwise unsupported (exit 2) |
| `-y`, `--side-by-side`, `-W NUM`, `--width`, `--left-column`, `--suppress-common-lines` | side by side | ✔ over dump lines |
| `-p`, `--show-c-function`, `-F RE`, `--show-function-line` | function context | text: ✔. Structured formats always show the enclosing unit's address, so these are accepted no-ops |
| `--label LABEL` (×2) | header labels | ✔. Also used in JSON report `path` |
| `-t`, `-T`, `--tabsize`, `--expand-tabs`, `--initial-tab`, `--suppress-blank-empty` | layout | ✔ |
| `-l`, `--paginate` | pipe through `pr` | unsupported (exit 2) **[D-14]** |
| `-i`, `--ignore-case` | | text and text-bearing units (SVG text, CSV cells, XML text nodes, …) |
| `-E -Z -b -w -B` (whitespace) | | same, applied to text-bearing units |
| `-I RE`, `--ignore-matching-lines` | | text: lines. Structured formats: dump lines (documented as approximate) |
| `--strip-trailing-cr` | | ✔ |
| `-a`, `--text` | treat all files as text | forces the **text** plugin (no semantic parsing) |
| `--binary` | binary I/O | **re-purposed**: emit an applicable splinter patch with payloads (as `git diff --binary`) **[D-6]**. Text files get a plain unified diff (`-u` unless another style is given); formats without patch support (binary) are an error |
| `-r`, `--recursive`, `-N`, `--new-file`, `--unidirectional-new-file`, `-x PAT`, `--exclude`, `-X FILE`, `--exclude-from`, `-S FILE`, `--starting-file`, `--no-dereference`, `--ignore-file-name-case`, `--from-file`, `--to-file` | directory handling | ✔. Each file pair is dispatched by format |
| `-d`, `--minimal` | minimal diff | exhaustive matching in Sequence primitives (slower) |
| `--speed-large-files`, `--horizon-lines=NUM` | heuristics | accepted. `--speed-large-files` lets formats subsample metrics |
| `--color[=WHEN]`, `--palette=PALETTE` | colour | ✔ |
| `--GTYPE-group-format`, `--LTYPE-line-format`, `--line-format` | ifdef formats | text format only |
| `-v`, `--version`, `--help` | | ✔ |

### 2.2 Splinter-only diff options

| Option | Meaning |
|---|---|
| `--format=NAME` | force the format plugin (`splinter formats` lists names) |
| `--json[=FILE]` | write the JSON report (stdout if no FILE, which replaces the text output). With `--binary`, the patch is written as JSON instead (SPEC §6) |
| `--md[=FILE]` | write the report as Markdown (stdout if no FILE, which replaces the text output) **[D-22]**: a summary table (inputs, format, changed units, conflicts, format statistics, churn), then one section per conflict and changed unit with its address, values, and metrics. Combines with `--json`; when both go to stdout the JSON comes first |
| `--visual=FILE` | write the format's visual diff (heatmap PNG, waveform PNG, contact sheet, …) |
| `--dump` | print the canonical dump of a single file (textconv) |
| `--semantic` / `--raw` | `--raw` diffs the format's raw structure (chunks, parts, XML) instead of the semantic model |
| `--tolerance=SPEC` | numeric tolerance for grid units (`2`, `0.5%`, `ulp:4`, `de:1.0`), recorded in the report. Meshes: a distance in model units for positions and other numbers (default 0) |
| `--gap=N` | region-grouping gap for grids |
| `--annotate` | add per-hunk metric comments (`# …`) to text renderings of structured formats (Q-TEXT-9) |
| `--ignore=CLASS[,…]` | ignore change classes: `metadata`, `timestamps`, `encoding`, `derived`, … (per format) |
| `--max-memory=SIZE`, `--max-pixels=N`, `--max-input=SIZE`, `--max-depth=N`, `--max-zip-entries=N`, `--max-zip-ratio=R`, `--max-zip-size=SIZE` | limits (SPEC §9) |
| `--exit-codes=posix\|extended` | SPEC §8 |
| `--diff-algorithm=myers\|minimal\|patience\|histogram` | text: sequence algorithm (default myers, as GNU) |
| `--png-mode=auto\|rgba`, `--alpha-zero-equal`, `--png-ignore-chunks=LIST`, `--png-color-dependency=conflict\|warn` (merge) | PNG options (png.md §4, §6.2), declared by the module (D-51) |
| `--tiff-ignore-tags=LIST` (diff), `--reencode` (merge) | raster-misc options (raster-misc.md §4, §6): hide TIFF tag keys such as `0x0131,0x0132` from the diff; merge lossy WebP frames in pixel space and re-encode them (warning `lossy-reencode`). `--alpha-zero-equal`, `--tolerance`, `--gap` and `--guard` apply to BMP, TGA, QOI, GIF, TIFF and WebP as to PNG |
| `--zip-lenient`, `--zip-times=ignore\|strict` | ZIP/TAR options (containers.md §2.1, §4): trust the central directory over a disagreeing local header (warning); hide member time changes, or count them as differences (by default they are listed but do not count) |
| `--xml-ws=formatting\|significant`, `--xml-ignore-comments`, `--xml-ignore-pis`, `--xml-strict-prefixes`, `--xml-strict-cdata`, `--dump-attr-lines` | XML options (xml.md §4, §5), declared by the module (D-51) |
| `--csv-key=COL[,COL]`, `--csv-header=auto\|yes\|no` | CSV options (csv.md §3, §2): key columns by name or 1-based index; header detection override |
| `--svg-show-editor-state`, `--svg-id-collision=rename\|conflict` (merge), `--svg-path-merge=subpaths\|none` (merge) | SVG options (svg.md §3.3, §4, §6), declared by the module (D-51) |
| `--pdf-layers=meta,pages,text,annots,forms`, `--pdf-show-noise` | PDF options (pdf.md §5, §4): diff layers (default all five); report `/ModDate`, `/Producer`, `/CreationDate`, and XMP dates and instance ids |
| `--verbose`, `--quiet` | diagnostics on stderr |

Names were checked against GNU diff's long options using the four-character rule in §1.2.
Abbreviations of the splinter-only options are accepted, like everything else.

#### Envelope options (SPEC §12)

| Option | Meaning |
|---|---|
| `--show=CLASS[,…]` | list envelope-level changes too: `framing`, `integrity`, `authenticity`, `encoding`, `derived`, `volatile`, `all` |
| `--integrity=recompute\|repair\|strict` | integrity policy (SPEC §12.7). In diff mode only `strict` has an effect (refuse inputs with mismatches) |
| `--lenient` | accept framing inconsistencies that can be recovered from, with warnings |
| `--verify-key=FILE` | public key or certificate used to verify signatures when the file does not embed one |
| `--scan-streams` | binary fallback: find and look through embedded zlib/gzip/zstd/LZ4 streams |
| `--binary --exact` | patch output that also carries the encoded bytes of changed streams (byte-exact application) |

### 2.3 Examples

```bash
splinter old.png new.png                          # normal report
splinter -u old.svg new.svg                       # unified, over canonical dump lines
splinter -q a.tif b.tif                           # "Files a.tif and b.tif differ"
splinter --json=r.json --visual=d.png a.png b.png
splinter -r -N --exclude='*.tmp' assets_v1/ assets_v2/
splinter --binary a.png b.png > logo.splinterpatch
splinter --dump data.sqlite                       # textconv
```

---

## 3. diff3 personality

```text
splinter -3 [OPTION]... MYFILE OLDFILE YOURFILE        # report
splinter -m [OPTION]... MYFILE OLDFILE YOURFILE        # merged output to stdout (or -o)
```

The operand order is **mine, old, yours**, the same as GNU `diff3`, RCS `merge`, and `git merge-file`
(current, base, other). All three tools agree, so splinter does too.

| Option | GNU meaning | Splinter |
|---|---|---|
| `-m`, `--merge` | output merged file | ✔ |
| `-A`, `--show-all` | all changes, bracket conflicts | text: ✔. Structured formats: same as `--conflict=mark` |
| `-e`, `--ed`, `-E`, `--show-overlap`, `-x`, `--overlap-only`, `-X`, `-3`, `--easy-only`, `-i` | ed-script variants | text only. `-3`/`-x` are honoured as filters on the *report* for all formats |
| `-L LABEL`, `--label` (up to 3) | labels | ✔ (labels for conflict markers and the report) |
| `-T`, `--initial-tab`, `-a`, `--text`, `--strip-trailing-cr` | | ✔ |
| `--diff-program=PROGRAM` | use another diff | text only |
| `-v`, `--version`, `--help` | | ✔ |
| **splinter-only** | | |
| `-o FILE`, `--output=FILE` | write merged result to FILE instead of stdout (atomic) | |
| `--conflict=ours\|theirs\|base\|mark` | what conflicted units contain in the output (SPEC §5.4) | |
| `--ours`, `--theirs`, `--union` | resolve all conflicts (exit 0), as in `git merge-file` | |
| `--resolve=FILE` | per-conflict resolutions by conflict ID | |
| `--conflicts=FILE` | write conflict report JSON | |
| `--integrity=…`, `--show=…`, `--lenient`, `--verify-key=FILE` | as in diff (SPEC §12) | |
| `--layout=stable\|compact` | keep payload offsets and bytes stable (default) or rewrite canonically (SPEC §12.4). PDF (pdf.md §7): `stable` appends an incremental update to the carrier, `compact` rewrites through qpdf and strips signatures under `auto` | |
| `--signatures=auto\|keep\|strip\|resign` | signature policy for the output (SPEC §12.5). ZIP (containers.md §6.3): `auto` and `strip` remove JAR/APK v1 signature files, the APK Signing Block, and `.signature.p7s` when the content changes; `resign` fails as unsupported (exit 2, or 3 with `--exit-codes`) until `--sign-command` lands. PDF (pdf.md §7.1): `auto` keeps earlier signatures through the incremental update (`signature-not-covering-changes`), `strip` removes signature values, `keep` keeps them through a `compact` rewrite (`signature-invalidated`) | |
| `--sign-key=FILE`, `--sign-cert=FILE`, `--sign-pass-file=FILE` | signing material for `resign`. A private key is required. Post-v1 (D-36): v1 exits 2 | |
| `--sign-command=CMD` | external signer run on the validated output before the final rename. `%f` is the file | |
| `--mac-key=FILE` | key for formats protected by a keyed MAC. Never bundled, never guessed | |
| `--guard=N`, `--tolerance=…`, `--gap=N`, `--format=…`, `--json[=FILE]`, `--md[=FILE]`, `--visual=FILE`, limits, `--exit-codes` | as in diff | |
| `--conflict-style=merge\|diff3\|zdiff3`, `--marker-size=N`, `--eol=auto\|keep` | text: marker layout, marker length, line-ending handling (text.md §4, §6.2) | |
| `--binary-merge[=force]` | byte-level merge of unrecognized files (binary.md §6) | |

Writing binary merged output (a NUL byte in its first 8 KiB, the GNU rule) to a terminal is refused (exit 2) unless
`-o` is given or stdout is redirected. Text-like results (text, XML, JSON, CSV, SVG, OBJ) print as text.

When the three inputs detect as different formats (a `.json` side holding conflict markers is text), they merge
as text if every input is text underneath, with `warning: inputs detect as different formats (json, text, json);
comparing as text` on stderr; otherwise they merge as binary (SPEC §11). Diff and the Git drivers do the same.

---

## 4. merge personality (RCS `merge` / `git merge-file`)

```text
splinter merge [OPTION]... FILE1 FILE2 FILE3      # = mine base theirs; result written into FILE1
splinter-merge [OPTION]... FILE1 FILE2 FILE3
```

| Option | Source | Splinter |
|---|---|---|
| `-p` | RCS merge, git merge-file: write to stdout instead of FILE1 | ✔ |
| `-q`, `--quiet` | do not warn about conflicts | ✔ |
| `-L LABEL` (up to 3) | labels | ✔ |
| `-A`, `-E`, `-e` | RCS output styles | as in diff3 |
| `--ours`, `--theirs`, `--union` | git merge-file | ✔ |
| `--diff3`, `--zdiff3` | git merge-file conflict style | text only |
| `--marker-size=N` | git merge-file | text only |
| `--object-id` | git merge-file (operands are blob IDs) | **[D-15]** later. Requires running `git cat-file` |
| `-o FILE` and all diff3 splinter-only options | | ✔ |

Exit status: 0 for clean, 1 for conflicts, 2 for trouble. As in `git merge-file`, `--exit-codes=extended`
can report the conflict *count* capped at 127 **[D-5]**.

---

## 5. patch personality

```text
splinter -P [OPTION]... [ORIGFILE [PATCHFILE]]
splinter patch [OPTION]... [ORIGFILE [PATCHFILE]]
splinter-patch [OPTION]... [ORIGFILE [PATCHFILE]]       # patch read from stdin if absent, like GNU patch
```

Accepted inputs: splinter patch envelopes (SPEC §6) and plain unified/context diffs (handled by the text
plugin, GNU-compatible). Mixed files, where some sections are splinter and some are unified, are allowed.

| Option | GNU patch meaning | Splinter |
|---|---|---|
| `-p NUM`, `--strip=NUM` | strip path components | ✔ |
| `-i FILE`, `--input=FILE` | patch file | ✔ |
| `-o FILE`, `--output=FILE` | output file | ✔ (atomic) |
| `-d DIR`, `--directory=DIR` | chdir first | ✔ |
| `-R`, `--reverse` | reverse | ✔ (envelopes are symmetric) |
| `-N`, `--forward` | skip already-applied or reversed hunks | ✔ (target unit already equals *after* → skipped) |
| `-F NUM`, `--fuzz=NUM` | fuzz factor | maps to matcher tolerance: text lines as in GNU. Structured formats: `0` requires exact *before*, `≥1` allows locating units by identity even if the surroundings changed. Grid hunks use `--tolerance` |
| `-l`, `--ignore-whitespace` | | text-bearing units |
| `--merge[=merge\|diff3]` | write conflicts into the file | `--conflict=mark` semantics |
| `-r FILE`, `--reject-file=FILE` | rejects | rejected hunks are written as a splinter envelope (`FILE.rej` by default, or `-` to discard) |
| `--dry-run` | | ✔ |
| `-b`, `--backup`, `-B PFX`, `-Y PFX`, `-z SUF`, `--suffix`, `-V METHOD`, `--backup-if-mismatch`, `--no-backup-if-mismatch` | backups | ✔ |
| `-E`, `--remove-empty-files` | | ✔ (files that the patch deletes) |
| `-f`, `--force`, `-t`, `--batch` | no questions | splinter never prompts. Accepted |
| `-s`, `--silent`, `--quiet`, `--verbose` | | ✔ |
| `-u -c -n -e` | interpret as unified/context/normal/ed | text sections only |
| `-D NAME`, `--ifdef` | | text only |
| `-g NUM`, `--get`, `--read-only`, `--follow-symlinks`, `-T`, `-Z`, `--set-time`, `--set-utc`, `--posix`, `--quoting-style`, `--reject-format`, `-x NUM`, `--binary` | misc | accepted. `--binary` is a no-op. Time-setting options set mtime only |
| **splinter-only** | `--format`, `--conflicts=FILE`, `--json`, `--md`, `--tolerance`, `--guard`, limits, `--exit-codes`, and the envelope options of §3 (`--integrity`, `--layout`, `--signatures`, `--sign-*`, `--mac-key`, `--lenient`, `--show`) | |

Base verification: each envelope section stores the hash of the original file. If the target's hash
matches, the patch applies at fuzz 0 trivially. If not, it applies by identity, three-way, and the
report says `base-mismatch: applying by identity`.

Behaviour as implemented (Phase 5):

- **Target file:** ORIGFILE when given, else GNU's rule over the `---`/`+++` names after `-p` (unset: names are
  used whole): among the names that exist, fewest components, then shortest basename, then shortest; `/dev/null`
  on the old side creates the new file. Messages, reject files (`NAME.rej`, or `OUTPUT.rej` with `-o`), the
  default `--backup-if-mismatch` (off with `POSIXLY_CORRECT` or `--posix`), and exit status 0/1/2 follow GNU
  patch 2.7, checked against it in the tests.
- **Unified hunks** use GNU's `locate_hunk`: offsets searched outward from the expected line, then fuzz up to
  `-F` (default 2) context lines.
- **Envelope hunks:** an exact base applies every unit directly. Otherwise the target is merged with the patch
  (`merge(target with the before values, target, target with the after values)`), and a hunk that overlaps an
  unresolved conflict is rejected: the target keeps its value and the hunk goes to the reject file as an
  envelope section. `--merge` writes the conflict as `--conflict=mark` does instead (envelope sections only).
- **Already applied:** splinter never prompts. A unified patch whose first hunk only matches reversed, or an
  envelope whose units all hold their after values, is skipped with GNU's
  `Reversed (or previously applied) patch detected!  Skipping patch.` (the `-N` behaviour, also the default);
  `-t` applies it reversed, `-f` turns the check off.
- **JSON:** a JSON patch (`splinter --binary --json`) applies like its text form. `--json[=FILE]` writes the
  patch-mode report (SPEC §6): per section, what was applied, rejected, or merged by identity; `--md[=FILE]` writes
  the same as Markdown. With either report on standard output, GNU's messages go to standard error.
- **Not yet:** context, normal, and ed diffs (`-c`, `-n`, `-e`), `-D`, `--merge` on unified sections,
  `GIT binary patch` hunks (Q-BIN-6), and patches of binary files. `-T`, `-Z`, and the other misc options are
  accepted without effect.

---

## 6. Tool subcommands

| Command | Purpose |
|---|---|
| `splinter dump FILE` (≡ `splinter --dump FILE`) | canonical text dump. Intended for `textconv` |
| `splinter inspect FILE [--json] [--layout]` | structure overview: detected format, units, statistics, warnings, and integrity and signature verification. `--layout` prints the EnvelopeMap (byte ranges with class and unit). Also takes `--json=FILE`, `--format`, the `--max-*` limits, `--exit-codes`, and format options |
| `splinter formats [--json]` | list compiled-in formats, extensions, and capabilities |
| `splinter git-diff …` | Git external diff driver (§7) |
| `splinter git-merge …` | Git merge driver (§7) |
| `splinter git-setup [--global\|--local\|--print] [--formats=LIST] [--textconv-only]` | print or apply `git config` entries and `.gitattributes` lines |

---

## 7. Git integration

### 7.1 textconv (recommended default, cheapest)

```ini
[diff "splinter"]
    textconv = splinter dump
    cachetextconv = true
```

```gitattributes
*.png  diff=splinter merge=splinter
*.zip  diff=splinter merge=splinter
```

Git then line-diffs canonical dumps. This works with `git log -p`, `--word-diff`, GUIs, and `git blame --textconv`.

### 7.2 External diff driver (richer output)

`[diff "splinter"] command = splinter git-diff` (or `GIT_EXTERNAL_DIFF=splinter git-diff`). Git passes
7 arguments, or 9 when a rename is detected:

```text
path old-file old-hex old-mode new-file new-hex new-mode [new-path rename-info]
```

- A missing side appears as `/dev/null`, or as `nul` from Git for Windows (observed with Git 2.54), and is
  treated as absent. An unmerged path is passed as the single argument `path`; splinter prints
  `* Unmerged path PATH` and exits 0.
- Output: `diff --git a/PATH b/NEW-PATH`, Git's rename information (the 9th argument) and mode lines, then a
  unified diff without timestamps for text, or the format's normal report and churn line for other formats.
- Format detection uses `path`, not the temp file names.
- **Exit 0 on differences**, because Git treats a non-zero exit from an external diff as a failure, unless trust is
  enabled **[D-16]**. Since Git 2.46, `diff.trustExitCode` (or `diff.<driver>.trustExitCode`) makes Git expect GNU
  codes. Splinter enables the same: `--trust-exit-code` in the driver command, the environment variable
  `GIT_EXTERNAL_DIFF_TRUST_EXIT_CODE` (which Git also documents), or automatically when `git config --get` reports
  either key as true (`--driver=NAME` names the driver; default `splinter`). Then it exits 0 for equal, 1 for
  different, and 2 for trouble.
- `GIT_DIFF_PATH_COUNTER` and `GIT_DIFF_PATH_TOTAL` are used only to separate output.

### 7.3 Merge driver

```ini
[merge "splinter"]
    name = Semantic Splinter three-way merge
    driver = splinter git-merge %O %A %B %L %P
    recursive = binary
```

- Operands: `%O` base, `%A` ours (**result is written here**), `%B` theirs, `%L` marker size, `%P` path.
  Current Git also documents `%S %X %Y` (conflict labels for ancestor, local, and other; verified against the
  gitattributes docs on 2026-10-07). They are accepted as optional trailing arguments, and `-L LABEL` (ours, base,
  theirs) works too. `git-setup` writes only `%O %A %B %L %P`, which every Git version substitutes.
- Text merges use the `git merge-file` engine (myers, zealous-alnum refinement) with markers of `%L` characters,
  labelled `ours`, `base`, and `theirs` unless labels are given.
- Exit 0 for clean, non-zero for conflicted or trouble. On conflict, `%A` holds the policy output
  (default per D-21: `mark` for formats marked with classic `<<<<<<<` markers, `ours` for the rest; override with
  `git config splinter.conflict ours|theirs|base|mark`, or `--conflict=` in the driver command) plus a sidecar
  `<%P>.splinter-conflicts.json` in the work tree, and a summary on stderr. With `mark`, a format without markers
  also writes `<%P>.splinter-conflicts.png`. A clean merge removes a stale JSON sidecar. Sidecars are untracked
  files: `git merge --abort` leaves them, so add `*.splinter-conflicts.*` to `.gitignore`.
  Exit status: 0 clean, 1 conflicts, 2 trouble (Git treats every non-zero status as a conflict).
- Temp files have no extensions. Detection uses `%P`, with content probing as a fallback.
- `recursive = binary` makes inner merges of a recursive merge take the simple route.
- Envelope behaviour in Git modes: `--ignore=volatile` is on, the signature policy is `auto`, and splinter **never re-signs**
  unless `git config splinter.signKey` or `splinter.signCommand` is set. Signing is normally a CI step after the merge.
- **Git LFS:** without help, a merge driver receives LFS *pointers*, not content. Git LFS ships
  `git lfs merge-driver`, which smudges the three versions, runs `--program` (placeholders `%O %A %B %D %L`,
  with `%%` doubled inside Git config), and cleans the result. It exists since **git-lfs 3.2.0** (May 2022, the
  minimum, D-17); the form below was checked against the git-lfs 3.8 man page and runs end to end in the
  Phase 4 tests with git-lfs 3.7.1:

  ```ini
  [merge "splinter-lfs"]
      driver = git lfs merge-driver --ancestor %O --current %A --other %B --marker-size %L --output %A --program 'splinter git-merge %%O %%A %%B %%L --output %%D'
  ```

  `git-setup` writes this driver as `merge.splinter-lfs`; use it for paths that also have `filter=lfs`. The `%P`
  path is not available through the wrapper, so detection falls back to content probing (`--format=NAME` in the
  program forces a format), and no sidecar is written.

### 7.3.1 `git-setup`

`splinter git-setup [--print|--local|--global] [--program=CMD]` writes `diff.splinter` (textconv, cached),
`merge.splinter`, and `merge.splinter-lfs` with `git config`; `--print` (the default) shows them as config-file
text with suggested `.gitattributes` and `.gitignore` lines. `--program` names the executable (default
`splinter`, found on `PATH`); backslashes become slashes and a path with spaces is quoted, because Git runs
drivers through `sh`. The external diff (`diff.splinter.command = splinter git-diff`) is optional and not
written. The adapter lives in `src/git/` and is compiled against the public headers only (D-53).

### 7.4 Named-flag form

`splinter git-merge --base %O --ours %A --theirs %B --output %A [--path %P]` is accepted as an
equivalent spelling.

---

## 8. Exit status

See SPEC §8. Summary: diff `0/1/2`, diff3/merge `0/1/2`, patch `0/1/2`, git-diff `0` unless trouble,
git-merge zero/non-zero. `--exit-codes=extended` gives the detailed codes `0`–`6` outside Git modes.

---

## 9. Output conventions

- Text reports go to stdout, and diagnostics go to stderr, prefixed `splinter:` as GNU tools do.
- Headers mimic GNU: `--- a/file<TAB>timestamp` for unified. Timestamps are omitted with `--label`. In
  Git modes there are no timestamps, for determinism.
- Hunk header for structured formats: `@@ -L,N +L,N @@ ADDRESS`. Line numbers refer to dump lines,
  and ADDRESS is the unit's address (SPEC §3.1): the common address of the lines the hunk changes, with
  their regions joined (`@@ -1,5 +1,5 @@ /pixels#rect=64,0,128,64`). Context-style hunks put it after
  `***************`. Implemented for PNG, JSON, CSV, XML, and SVG; other formats show plain GNU headers.
- With `--annotate`, per-hunk metrics follow as `# …` comment lines (e.g. `# 212 px changed, bbox 10,20 32x8, max ΔE00 14.2`), one per change whose address meets the hunk's lines; changes without pixel metrics show their normal-report line (`# ~ /version "0.1.0" -> "0.2.0"`). Off by default, so output stays GNU-shaped and `patch`-applicable (Q-TEXT-9). Annotations need line addresses, so they cover PNG, JSON, and CSV; XML and SVG don't have them yet.
- `--color` uses GNU diff's palette keys (`sl`, `cx`, `mt`, `ac`, `de`, `ln`) plus `cf` for conflicts.
