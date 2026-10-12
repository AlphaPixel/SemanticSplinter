# Semantic Splinter — Implementation Plan

**Status (2026-10-12):** Phases 0–7 are done: every v1 format is implemented. Phase 8 (v1 hardening and
release) is in progress. Written as Draft v0.1 (2026-10-07). Effort estimates assume one experienced C++
engineer: S ≤ 1 week, M ≤ 3 weeks, L ≤ 6 weeks. Phases are sequential where noted. Format work after Phase 6
parallelizes well.

Guiding rules:

- Every phase ends with something runnable, plus the smallest tests that would fail if it broke.
- No speculative infrastructure. The plugin ABI, GUI, threading, and C API wait until a concrete consumer needs them.
- Third-party code comes through vcpkg ports only. Pin `builtin-baseline`. Each new dependency gets a line in
  `docs/DEPENDENCIES.md` (created in Phase 0) with its license and the reason it was chosen.

---

## Phase 0: Skeleton (S) — done

**Deliverables**

- `CMakeLists.txt` (C++20, warnings as in SPEC §2.4, `SEMSPLINTER_FORMAT_*` options, mapping to `VCPKG_MANIFEST_FEATURES`).
- `CMakePresets.json`: `windows-msvc` (VS 2022 Community, x64, static triplet for release), `linux-gcc`,
  `linux-clang`, `macos`. Each has debug and release variants and a `ctest` preset.
- `vcpkg.json` (manifest, features per format), `vcpkg-configuration.json` (baseline).
- Empty core library, `splinter` executable printing `--version`, and Catch2 test target wired into CTest.
- CI: GitHub Actions matrix (windows-latest MSVC, ubuntu GCC + Clang, macos AppleClang) with the vcpkg binary cache.
- `.clang-format`, `docs/DEPENDENCIES.md`, `README.md` (build instructions).

**Exit:** `cmake --preset windows-msvc && cmake --build --preset windows-msvc && ctest --preset windows-msvc`
passes on all three operating systems. The Visual Studio "Open Folder" workflow works with the presets.

---

## Phase 1: Core model and merge primitives (M) — done

**Deliverables**

- `Address`, `Region`, `Change`, `Conflict`, `ChangeSet`, `MergeResult`, `Error` categories, `Limits`.
- Hashing: xxHash XXH3-128 (vcpkg `xxhash`). Deterministic ID generation.
- JSON report writer (`nlohmann-json`, `ordered_json`), schema `splinter-report/1`, and a JSON Schema file in `docs/schema/`.
- I/O: bounded `ByteSource` (file and memory), `atomic_write` (temp file, flush, rename), temp-file helper.
- Merge primitives (SPEC §5.1): `KeyedMerge`, `SequenceMerge` (histogram diff, Myers fallback,
  `--minimal`, diff3 chunking, ID-matched moves), `GridMerge` (2-D and 1-D, per-cell three-way,
  8-connected regions via union-find, `--gap`, `--guard`, tolerance hook), and `OpaqueLeaf`.
- Dependency checking pass (SPEC §5.3) and the conflict policy application (SPEC §5.4) including `--resolve` files.
- Envelope basics (SPEC §12): byte-class enum, `EnvelopeMap`, churn attribution from two maps, and the **fix-up
  Emitter** core (bytes, raw, group, align, fixed-width fields, size_of/offset_of, fixed-point layout). The
  CRC-32 and Adler-32 checksums go in core. Further expressions are added by the formats that need them.

**Tests:** property tests over random inputs for each primitive:
`merge(b,b,b)=b`, `merge(b,x,b)=x`, `merge(b,b,x)=x`, `merge(b,x,x)=x`, ours/theirs symmetry modulo the
policy, and determinism (run twice, compare bytes). Table tests for every KeyedMerge row.

**Exit:** primitives are usable from a format module without any format code living in the core.

---

## Phase 2: Text and binary formats plus the diff CLI (M) — done

**Deliverables**

- `text` module: encoding/EOL detection, line model, all GNU whitespace and case options, normal,
  unified, context, and side-by-side output, diff3 merge with conflict markers (`merge`, `diff3`, `zdiff3` styles),
  `merge-file` (Git-compatible exit counts).
- `binary` module: anchor-based byte diff, `Binary files X and Y differ`, hexdump-style `dump`, opt-in `--binary-merge`.
- CLI: GNU-compatible option parser (CLI.md §1.2), personality selection (argv[0], subcommand, mode
  flag), diff, diff3, and merge personalities, `-r` directory diffs, `--json`, exit codes, `formats`, `dump`.
- Install rules that create the `splinter-diff`, `splinter-diff3`, `splinter-merge`, and `splinter-patch` aliases.

**Tests:** golden comparisons against GNU diffutils outputs, generated once on Linux and checked in,
for a corpus of text pairs × option sets. Parser tests for clustering, abbreviation, and permutation,
plus the GNU-prefix non-collision check.

**Exit:** `splinter -u a.txt b.txt` is byte-identical to GNU `diff -u` (labels and timestamps
controlled) on the corpus. `splinter -m` matches `diff3 -m` on the corpus.

---

## Phase 3: PNG proof of concept (M) — done

See [formats/png.md](formats/png.md).

- Decode via libpng to the canonical grid (straight alpha; RGBA8 when every input is 8-bit or less, else RGBA16, Q-PNG-1). Track the color type, bit depth, palette, and
  ancillary chunks.
- Diff: pixel regions, metrics, ancillary chunk changes, `--visual` heatmap and overlay.
- Merge: GridMerge on pixels, KeyedMerge on ancillary chunks. Re-encode preserving the original color type
  and bit depth where representable, otherwise widen and warn.
- `dump` for textconv: header, chunk list, per-tile hashes (a stable, line-diffable layout).
- Envelope: chunk CRC and length verification, the EnvelopeMap, raw-copy of unchanged IDAT, a re-encode that reuses
  the inferred zlib parameters and per-row filters, the churn summary line, and `inspect --layout`. The proof case is a
  fixture where one pixel changes and >90% of the bytes change, while the report attributes ≥99% of the churn to `encoding`.

**Exit:** the PNG MVP list of png.md is complete. A JSON report and heatmap are produced. A disjoint edit
merge round-trips. Conflicting pixel edits are reported with regions.

---

## Phase 4: Git integration (S) — done

- `git-diff` (7/9-argument handling, `/dev/null`, exit 0), `git-merge` (`%O %A %B %L %P`, sidecar conflicts,
  atomic write into `%A`), `git-setup --print|--local|--global`.
- End-to-end tests: CTest scripts create temp repos, set attributes, and perform clean merges, conflicting merges,
  `git merge --abort`, `git log -p` with textconv, and rename diffs. These run on all CI operating systems.
- Investigate the Git LFS merge-driver wrapper and document the configuration **[D-17]**.
- Keep the adapter thin: the Git-specific code only maps arguments and exit codes onto the public library API, and a
  test builds it against the installed headers alone **[D-53]**.

**Exit:** a PNG conflict in a real `git merge` leaves a valid PNG, a sidecar, and an unmerged index entry. A
clean merge commits without intervention.

---

## Phase 5: Patch (S–M) — done

- Splinter patch envelope writer (`--binary`) and reader. Payload codec: zstd+base85 (or zlib) **[D-7]**.
- `patch` personality: identity-based application through `merge()`, fuzz mapping, `-R`, `-N`,
  rejects as an envelope, backups, `--dry-run`, and `-p` strip for multi-file patches.
- Unified-diff sections are applied by the text module, GNU-compatibly.

**Exit:** `splinter --binary a.png b.png | splinter -P a.png` produces b.png pixel-exact. The patch also
applies to a third file that differs elsewhere.

---

## Phase 6: Containers, XML, JSON (M–L) — done

- `zip` (own reader and writer over zlib and zstd **[D-27]**, after the minizip-ng byte-exact spike failed: raw copy of untouched entries, deterministic writer, bomb limits) and `tar`. The OPC layer is post-v1 (D-44).
- Shared helpers as they become needed: `src/core/text`, `src/formats/common/splice` (SPEC §2.3).
- `xml` (expat tokenizer plus an own span tree **[D-26]**, identity profiles, attribute KeyedMerge, children SequenceMerge, splice writer).
- `json` (order- and format-preserving, identity key rules, JSON Pointer addresses).
- Recursion: members are dispatched through the registry by logical name, via the `Context` of SPEC §4 (registry, shared `Limits` budget, depth) **[D-47]**.

**Exit:** a ZIP containing a modified PNG and a modified JSON merges with both sub-merges reported under
`/entries/<path>/…` addresses.

---

## Phase 7+: Formats — done

**Done 2026-10-11:** steps 7–13 (#9–#15), each with the v1 scope below. Later rows of each format document are
tracked as issues (#71–#86).

v1 steps follow D-44. Each step implements the format's **v1 scope** from the table below, not only the
document's MVP row: v1 includes every later row that a decided item depends on **[D-45]**.

| Step | Format | Builds on | v1 scope (format document phase rows) | Effort |
|---|---|---|---|---|
| 7 | SVG | xml | MVP + v2 (subpath merge, entity exception, SVGZ), without the lunasvg visual | M |
| 8 | CSV/TSV | text | MVP | S |
| 9 | JPEG (DCT domain) | raster core | MVP | M |
| 10 | raster-misc | raster core | MVP + v1.1 (WebP) | M |
| 11 | SQLite | json, text | MVP (vendor `cksumvfs.c`) | S–M |
| 12 | PDF | xml, qpdf | MVP + incremental writer + annotation/form merge with carrier logic **[D-46]**; no rendering; page-content merge post-v1 | L |
| 13 | Simple meshes | `geomcore` | MVP | M |

Phases 2–6 take the same rule: text MVP + v2 (zealous refinement, `merge-file`, `--eol`, UTF-16, zdiff3);
binary MVP + v2 (`--binary-merge`), without `--scan-streams` **[D-49]**; containers MVP + v2 (TAR), without
similarity renames; png MVP + v1.1 (indexed mode, patch payloads, color dependency), plus
pure crop/extension detection (Q-PNG-3); xml MVP + v2 (comment mark, dialect hooks); json MVP.

**Post-v1:** PDF rendering and page-content merge, the later rows of each format document, and **authenticity**:
the `crypto` feature (OpenSSL 3), signature verification, and `resign` for JAR v1 and PDF incremental CMS **[D-36]**.
Further formats are planned separately.

Without `crypto`, v1 reports signature and digest fields as `no-crypto`, applies the `strip`/`keep` policies
(they need no keys), and supports `resign` only through `--sign-command` **[D-37]**. `--sign-key` exits 2 with
`unsupported (built without crypto)`.

**Rule:** a format's section 7.1 envelope inventory (and its ENVELOPES.md row) is reviewed before its
writer is written. The writer is built on the Emitter from that inventory.

### Cross-cutting tracks (run alongside Phase 7+)

- **Fuzzing (S, then continuous):** a libFuzzer target per format `load()`, run nightly on Clang. Corpus from fixtures.
- **Fixture corpus:** generated fixtures in tests where possible (PNG, CSV, JSON, SQLite are easy to
  synthesize). Real-application files (exports from image editors, PDF producers, archivers) live in a separate LFS-backed corpus
  repo, referenced by CI, to keep this repository small **[D-18]**.
- **Performance budget:** a 4096×4096 RGBA16 three-way merge in under 2 s, and a 200-page PDF diff in
  under 3 s, on a mid-range desktop. Benchmarks run in CI but do not fail builds until the first baseline exists.
- **Packaging:** GitHub releases with static binaries (Windows x64/arm64, Linux x64 glibc and musl,
  macOS universal). Later: a vcpkg port for the library, plus winget and Homebrew.

### Deferred until a consumer asks

- C API and language bindings (Python first, if anyone asks).
- Runtime-loadable plugins with a C ABI **[D-4]**: planned after v1.
- Conflict-resolution GUI (it consumes `--conflicts` JSON and writes `--resolve` JSON, so the CLI contract already supports it).
- Heuristic/perceptual merge modes (object-aware raster merges, fuzzy geometry matching).
- Multithreading. Profile first. Grid primitives are the likely first candidate.

---

## Phase 8: v1 hardening and release (in progress)

Added 2026-10-11, after the formats were done. It turns the cross-cutting tracks above into release work, in
three waves.

| Wave | Work | Issues | State |
|---|---|---|---|
| 1. Correctness and safety | Fuzz target per format `load()`; charge the remaining large allocations to `--max-memory` | #16, #61 | open |
| 1. Correctness and safety | GCC, Clang, and AppleClang checks of the Phase 7 formats | #91 | done |
| 2. Documented CLI gaps | Text-like merge results to a terminal; `--md` report; `@@ ADDRESS` hunk headers and `--annotate` (PNG, JSON, CSV); JSON patches, patch-mode reports, `--exact`; patch payloads for ZIP/TAR, XML, JSON | #48, #32, #33, #35, #44 | done (Windows-verified) |
| 3. Release | Performance budget benchmarks; library install and export; packaging and GitHub releases (with the qpdf NOTICE); fixture corpus repository | #18, #31, #19, #17 | open |

Not in Phase 8: the later rows of the format documents (#39–#50, #64, #71–#86) and the post-v1 items (#20, #21,
#22, #29). CI (#96) runs Windows, GCC, and Clang on pull requests and macOS on `main` and manual runs.

---

## Milestones

| Milestone | Contents | Cumulative effort | State |
|---|---|---|---|
| **M1: Usable diff tool** | Phases 0–2 | ~2 months | done |
| **M2: PNG + Git** | Phases 3–5 | ~3.5 months | done |
| **M3: Structured core** | Phase 6 + SVG | ~5 months | done |
| **M4: v1** | Steps 8–13: CSV, JPEG, other rasters, SQLite, PDF, simple meshes **[D-44]** | ~9 months | done (formats); release is Phase 8 |
| **Post-v1** | PDF rendering, authenticity (`crypto`), runtime plugin interface **[D-4]** | — | planned |

These estimates are rough and should be re-planned after M2, using measured velocity.
