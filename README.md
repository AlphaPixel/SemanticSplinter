# Semantic Splinter

Format-aware diff, three-way merge, and patch for files that line-based tools handle badly: images,
archives, structured data, PDFs, databases, and 3-D meshes.
It works standalone and as a Git textconv, external diff driver, and merge driver.

- **Library:** `semantic-splinter`, C++20, built with CMake and vcpkg. Not related to the `libsplinter` library.
- **Tool:** `splinter`, one binary whose personalities copy the option syntax of GNU `diff`, `diff3`,
  `patch`, and RCS `merge` / `git merge-file`.
- **License:** MIT ([LICENSE](LICENSE)).

**Status (2026-10-12): pre-release, feature-complete for the v1 formats, in testing.** Every v1 format diffs
and merges (Phases 0–7 of [docs/PLAN.md](docs/PLAN.md)), with the Git drivers, patches, and JSON and Markdown
reports. Phase 8 (hardening and release) is in progress: fuzzing, memory limits, performance budgets, library
packaging, and binary releases are still open, so build from source for now. On text, `splinter` reproduces
GNU diff 3.12, GNU diff3, and `git merge-file` byte for byte on the golden corpus in `tests/golden/`.

## For testers

**Get a build.** There are no binary releases yet. Build from source as described in [Building](#building); the
tool lands in `build/<preset>/src/cli/Release/splinter` (`splinter.exe` on Windows). `splinter --version` prints
the version; include it in reports.

**Platforms checked.** Windows (MSVC 2022) for everything. Linux (GCC, Clang) and macOS (AppleClang) passed CI for
everything up to the Phase 7 formats; the newest features (patch payloads for archives, XML and JSON, JSON patches,
`--md`, `--annotate`) are so far checked on Windows only.

**What to try.** Diff (`splinter a b`, `-u`, `--json`, `--md`), three-way merge (`splinter -m mine base theirs`, or
through Git with `splinter git-setup`), and patches (`splinter --binary a b > p`, then `splinter -P < p`):

| Format | Diff | Merge | Patch | Main limits today | Doc |
|---|---|---|---|---|---|
| Text | ✔ | ✔ | ✔ | Patch mode reads unified diffs and splinter patches only (no context/ed input, #36) | [text](docs/formats/text.md) |
| Unknown binary | ✔ | opt-in (`--binary-merge`) | — | Byte ranges only | [binary](docs/formats/binary.md) |
| ZIP, TAR (gz/zst/xz/bz2) | ✔ | ✔ | ✔ | No similarity renames; ZIP prefix and APK Signing Block changes have no patch | [containers](docs/formats/containers.md) |
| XML | ✔ | ✔ | ✔ | No cross-parent moves; generic dialect only (#42, #43) | [xml](docs/formats/xml.md) |
| JSON, JSONC | ✔ | ✔ | ✔ | No JSON5 or JSON Lines (#40) | [json](docs/formats/json.md) |
| CSV, TSV | ✔ | ✔ | — | No column renames (#71) | [csv](docs/formats/csv.md) |
| SVG, SVGZ | ✔ | ✔ | — | `<style>` CSS rules do not merge separately (#77); no visual diff (#78) | [svg](docs/formats/svg.md) |
| PNG | ✔ | ✔ | ✔ | APNG merges as one unit; no indexed mode | [png](docs/formats/png.md) |
| JPEG | ✔ | ✔ (whole MCUs) | — | A re-encoded side wins whole or conflicts (#73) | [jpeg](docs/formats/jpeg.md) |
| TIFF, GIF, WebP, BMP, TGA, QOI | ✔ | ✔ | — | Lossy WebP frames and JPEG-compressed TIFF tiles merge only whole (#83) | [raster-misc](docs/formats/raster-misc.md) |
| SQLite | ✔ | ✔ | — | A schema change on one side plus row edits on the other conflicts (#79) | [sqlite](docs/formats/sqlite.md) |
| PDF | ✔ | metadata, annotations, forms | — | No rendering; page content does not merge (both sides changing pages conflicts) | [pdf](docs/formats/pdf.md) |
| OBJ/MTL, PLY, STL | ✔ | OBJ objects, MTL; PLY/STL whole | — | Large meshes diff slowly (#82) | [mesh](docs/formats/mesh-simple.md) |

**Reporting a problem.** Open a GitHub issue with the exact command line, the `splinter --version` output, the
report (`--json=report.json` or `--md=report.md`), the exit status, and, if you can share them, the input
files. For a Git merge, include the `<file>.splinter-conflicts.json` sidecar. Known gaps and planned work are
tracked as open issues and in each format document's status line.

## Why

A one-pixel edit to a PNG changes most of its compressed bytes. Git shows "Binary files differ" and
cannot merge two artists' edits to different parts of the same image. Splinter parses each format into
its semantic units (pixels, chunks, archive members, XML elements, JSON keys, table rows, PDF objects),
diffs and merges those, and writes a valid file back:

- **Untouched data is copied byte for byte.** Only changed units are re-encoded, so Git deltas stay small.
- **Format plumbing is handled for you.** Sizes, offsets, CRCs, and other checksums are recomputed. Compressed
  streams are compared by content, so recompression noise is never reported as a change (SPEC §12).
- **No silent best effort.** When two sides edit the same unit, splinter reports a conflict. It never averages
  or guesses. Every conflict has a deterministic ID that can be resolved individually (`--resolve`).
- **Familiar interface.** `splinter a.png b.png` behaves like `diff`, `splinter -m` like `diff3 -m`, and
  `splinter -P` like `patch`. Exit codes follow the tool being emulated.

## v1 scope

| Area | v1 support |
|---|---|
| Text | GNU diff output styles, diff3/zdiff3 merge, `merge-file`, EOL and UTF-16 handling, own clean-room diff algorithms (Myers, histogram, patience) |
| Unknown binaries | GNU-compatible "differ" line, byte-range reports, diff-friendly hex dumps, opt-in `--binary-merge` for fixed-layout files |
| Containers | ZIP family (JAR, APK, NUPKG, ...) and TAR (gzip/zstd/xz/bzip2), with recursion into members |
| Structured text | XML, JSON, CSV/TSV, SVG |
| Raster images | PNG (APNG files: default image only), JPEG (merged in the DCT domain, no recompression), TIFF, GIF, WebP, BMP, TGA, QOI |
| Documents and data | PDF (structure, text, metadata, annotations, forms; incremental writer; no rendering), SQLite |
| 3-D | Simple meshes (OBJ, STL, PLY) |

**After v1:** PDF rendering and page-content merge, and signature verification and re-signing (`crypto`
feature). [docs/formats/README.md](docs/formats/README.md) lists the v1 formats and their documents.

## Usage

All v1 formats work today (see [For testers](#for-testers) for per-format limits):

```bash
splinter old.png new.png                     # diff: changed regions, metrics, and metadata
splinter -u old.png new.png                  # unified diff over canonical dump lines
splinter --json=report.json a.png b.png      # machine-readable report
splinter --md a.png b.png                    # the same report as Markdown
splinter -u --annotate a.json b.json         # hunks named by address, with metric comments
splinter -m mine.png base.png theirs.png     # three-way merge, as diff3 -m
splinter -m mine.zip base.zip theirs.zip     # members merged by path, each by its own format
splinter -m mine.db base.db theirs.db        # SQLite: rows by primary key, cells merged
splinter -m a.csproj base.csproj b.csproj    # XML: elements, attributes, and text merge by identity
splinter -m a.svg base.svg b.svg             # SVG: styles, classes, subpaths; colliding new ids renumbered
splinter merge-file cur.txt base.txt other.txt   # as git merge-file
splinter dump photo.png                      # canonical text dump (Git textconv)
splinter inspect --layout photo.png          # structure, checksums, and byte layout
splinter --binary a.png b.png > logo.patch   # applicable semantic patch
splinter -P < logo.patch                     # apply it, as patch (also to an edited copy)
splinter --binary --json a.zip b.zip > p.json # the same as a JSON patch; -P reads both forms
splinter formats                             # list the built-in formats
```

Archive members are diffed and merged by their own format and reported under `/entries/<path>/content/…`.
XML merges elements by identity (id attributes, then content), attributes per key, and text per node. JSON and
JSONC are addressed by JSON Pointer (`/items/id:7/label`): objects merge per key and arrays per element (by an
`id`-like key when every element has one). XML and JSON write untouched bytes back verbatim, so formatting,
comments, and number spelling survive outside the changed values. CSV/TSV rows merge by a key column (`id`,
`--csv-key`) or by position, cell by cell, so a column added on one side merges with row edits on the other;
untouched records are copied verbatim and output keeps ours' dialect (delimiter, quoting, BOM, line ends).
SQLite databases diff and merge schema objects, rows (by primary key or rowid), and cells; the merge is applied to a
copy of ours through SQLite, so untouched pages keep their bytes, and rows both sides added under the same
SQLite-allocated id are renumbered instead of conflicting.
Meshes diff by geometry (counts, bounding box, area, volume, Hausdorff distance, displaced-vertex regions), with
numbers compared as floats (`--tolerance`) and ASCII/binary STL or PLY seen as the same mesh. OBJ objects merge by
name, with later face indices renumbered; MTL materials merge per statement; PLY and STL merge as one unit.
PDF diffs report metadata, aligned pages, page text, annotations, and form values; merges combine metadata,
annotations, and form values onto the side that changed page content, written as an incremental update that keeps
every byte (and earlier signatures) of that file.
SVG builds on XML: `style` properties, `class` tokens, and path subpaths merge separately, editor view state (zoom,
window) is ignored, deleting what the other side newly references conflicts, and `.svgz` is unpacked and repacked
only when the content changed. BMP, TGA, QOI, GIF, TIFF, and WebP merge pixels region by region and their metadata
per key; animated GIF/WebP and multi-page TIFF merge frame by frame, unchanged frames and TIFF strips keep their
bytes, and lossy WebP frames merge only whole. Patches (`--binary`, or JSON with `--binary --json`; `--exact`
for byte-identical results) cover text, PNG, JSON, XML, and ZIP/TAR archives (members, their metadata, and their
content through the member's own format); `splinter -P --json` reports what a patch applied.
Unified and context diffs of PNG, JSON, CSV, XML, and SVG name each hunk's unit
(`@@ -1,5 +1,5 @@ /pixels#rect=64,0,128,64`); `--annotate` adds per-hunk metric comments for PNG, JSON, and CSV.
`--md[=FILE]` renders any report (diff, merge, patch) as Markdown.

Personality selection follows the program name (`splinter-diff`, `splinter-merge`, ...), then a subcommand
(`splinter merge ...`), then a mode flag (`-m`, `-3`, `-P`). Plain `splinter` is `diff`.

### Git

```bash
splinter git-setup --global      # writes the drivers below; --print shows them without writing
```

```ini
[diff "splinter"]
    textconv = splinter dump
    cachetextconv = true
[merge "splinter"]
    name = Semantic Splinter three-way merge
    driver = splinter git-merge %O %A %B %L %P
    recursive = binary
```

`git-setup` also writes `merge.splinter-lfs`, the same driver wrapped in `git lfs merge-driver` (git-lfs 3.2.0+).

```gitattributes
*.png  diff=splinter merge=splinter
```

```gitignore
*.splinter-conflicts.*
```

On conflict the merge driver still writes a complete, loadable file, exits non-zero, and leaves a
`<file>.splinter-conflicts.json` sidecar. Text-like formats get classic conflict markers, as Git does;
binary formats keep "ours" in conflicted regions (D-21); `git config splinter.conflict theirs|base|mark`
changes that. For paths with `filter=lfs`, use `merge=splinter-lfs` (CLI.md §7.3). The optional external
diff driver (`diff.splinter.command = splinter git-diff`) gives richer output than textconv.

## Documentation

| Document | Contents |
|---|---|
| [docs/SPEC.md](docs/SPEC.md) | Architecture, data model, format module contract, merge engine, patch format, reports, exit codes, limits, and the envelope model (sizes, checksums, signatures, compression layers) |
| [docs/CLI.md](docs/CLI.md) | Command-line contract for every personality, and Git integration |
| [docs/PLAN.md](docs/PLAN.md) | Phased implementation plan, v1 scope per format, milestones |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Every design decision with its status: decided, deferred, or provisional (post-v1 formats only) |
| [docs/ENVELOPES.md](docs/ENVELOPES.md) | Per-format catalog of framing, integrity, signature, and encoding fields |
| [docs/formats/](docs/formats/README.md) | Format survey with priority tiers, and one implementation document per format |

## Building

Requirements: CMake 3.25+, a vcpkg checkout with `VCPKG_ROOT` set (manifest mode, pinned baseline, D-52), and
a C++20 compiler: MSVC 17.8+ (Visual Studio 2022), GCC 11+, Clang 14+, or AppleClang 15+. Linux and macOS
also need Ninja.

```bash
cmake --preset windows-msvc                   # or linux-gcc, linux-clang, macos
cmake --build --preset windows-msvc           # Debug; append -release for Release
ctest --preset windows-msvc                   # likewise windows-msvc-release
```

Visual Studio 2022 can open the folder directly and picks up the same presets. Builds go to
`build/<preset>/`. `-DSEMSPLINTER_BUILD_TESTS=OFF` skips the tests and their Catch2 dependency.
Third-party ports and their licenses are listed in [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).

Each format is a separate module (`semantic-splinter-<fmt>`) behind a vcpkg manifest feature, so builds
include only the formats they need. Default dependencies are permissive only (MIT, BSD, Apache-2.0, Zlib,
public domain); copyleft libraries are opt-in features that are off by default. The Windows release is a
single static `splinter.exe`.

## Roadmap

| Milestone | Contents | State |
|---|---|---|
| M1: usable diff tool | Skeleton, core model and merge primitives, text and binary, diff CLI | done |
| M2: PNG + Git | PNG proof of concept, Git drivers, patch personality | done |
| M3: structured core | ZIP/TAR, XML, JSON, SVG | done |
| M4: v1 formats | CSV, JPEG, other rasters, SQLite, PDF, simple meshes | done |
| v1 release | Fuzzing, limits, performance budgets, library install, release binaries (PLAN Phase 8) | in progress |
| Post-v1 | PDF rendering, authenticity (`crypto`), runtime plugins | planned |
