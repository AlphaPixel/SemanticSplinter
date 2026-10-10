# Semantic Splinter — Core Specification

**Status:** Draft v0.1 (2026-10-07). Nothing here is frozen. Items marked **[D-n]** are recorded in
[DECISIONS.md](DECISIONS.md) as open decisions with a provisional default.

Companion documents:

- [CLI.md](CLI.md): the `splinter` command-line contract (diff / patch / diff3 / merge modes).
- [PLAN.md](PLAN.md): phased implementation plan.
- [DECISIONS.md](DECISIONS.md): open questions, provisional answers, rationale.
- [ENVELOPES.md](ENVELOPES.md): per-format catalog of framing, integrity, authenticity, and encoding fields (§12).
- [formats/README.md](formats/README.md): format survey, priority tiers, one document per format.

---

## 1. Goals and non-goals

### Goals

1. **Diff** non-text files by their logical content and report *what* changed, *where*, and *by how much*.
2. **Three-way merge** (base / ours / theirs) that auto-combines independent edits and emits explicit,
   addressable conflicts for incompatible ones.
3. **Patch**: serialize a change set so it can be reviewed and re-applied elsewhere, with fuzz.
4. One binary, `splinter`, whose command line feels like GNU `diff`, `diff3`, `patch`, and RCS `merge`.
5. A reusable C++ library, **semantic-splinter**, that does all the work. The CLI is a thin shell over it.
6. Git integration: `textconv`, external diff driver, and merge driver.
7. Modular formats: each format is an independently buildable module with its own optional dependencies.
8. Portable modern C++ (C++20) that builds with MSVC, GCC, Clang, and AppleClang through CMake + vcpkg.

### Non-goals (for now)

- A GUI conflict resolver. The CLI emits machine-readable conflicts and a resolution file format, so a GUI can come later.
- Runtime-loaded plugins with a stable binary ABI in v1 **[D-4]**. Formats are compiled in. A C-ABI plugin interface
  follows after v1.
- Heuristic or perceptual *merges* in the default path. Any heuristic is opt-in and is reported as heuristic.
- Replacing Git LFS or binary delta storage (xdelta, bsdiff).

### Invariants (non-negotiable)

- **Inputs are never modified** except when the user explicitly asks for in-place output (RCS `merge`
  semantics, the Git merge driver writing `%A`). Even then, the result goes to a temp file, is
  validated, and is atomically renamed over the target.
- **No silent best-effort.** Every merged entity is either provably independent, identical on both sides, or reported.
- **Determinism.** The same inputs and options produce byte-identical outputs and reports, including
  change and conflict IDs. No wall-clock timestamps, no hash-map iteration order, no thread-dependent ordering.
- **Envelopes are regenerated, never merged.** Sizes, offsets, checksums, digests, and compressed encodings
  are recomputed from the merged content, and signatures follow an explicit policy (§12).
- **Untouched content is preserved.** Unknown chunks, parts, and blocks are carried through byte-exact
  whenever the format allows it. Any loss of fidelity is reported as a validation warning.
- **Resource limits are enforced** before allocation (decompressed size, pixel count, ZIP entry count
  and expansion ratio, recursion depth).

---

## 2. Architecture

```text
              splinter (CLI)                 other hosts (GUI, bindings, Git adapters)
                   |                                        |
                   +------------------+---------------------+
                                      |
                         semantic-splinter (library)
   +-----------------------------------------------------------------------------+
   | api:      diff() / merge() / apply_patch() / dump() / inspect()             |
   | detect:   registry, probe by content -> extension -> --format override      |
   | model:    Change, Conflict, Address, ChangeSet, MergeResult, Report         |
   | merge:    primitives: KeyedMerge, SequenceMerge, GridMerge, OpaqueLeaf      |
   | io:       bounded reads, atomic write, temp files, limits                   |
   | report:   JSON report, text renderers (normal/unified/context/brief/side)   |
   | patch:    splinter patch file format (text envelope + encoded payload)      |
   +-----------------------------------------------------------------------------+
         |         |          |         |         |          |
      formats/  formats/   formats/  formats/  formats/   formats/ ...
       text      binary      png       zip       xml        svg   (each a static lib)
```

### 2.1 Library naming

| Thing | Name |
|---|---|
| Project / repo | Semantic Splinter |
| CMake package | `SemanticSplinter` (`find_package(SemanticSplinter CONFIG)`) |
| Core library target | `semantic-splinter`, alias `SemanticSplinter::core` |
| Format module targets | `semantic-splinter-<fmt>`, alias `SemanticSplinter::<fmt>` |
| Library file | `semantic-splinter.lib` / `libsemantic-splinter.a` |
| C++ namespace | `semsplinter` **[D-1]** |
| Public include root | `#include <semsplinter/...>` |
| Executable | `splinter` (plus mode aliases; see CLI.md) |

The name `splinter` alone is avoided for the library, namespace, and package so that it cannot collide
with the existing SPLINTER spline library (`libsplinter`, namespace `SPLINTER`).

### 2.2 Source tree

```text
CMakeLists.txt  CMakePresets.json  vcpkg.json  vcpkg-configuration.json
include/semsplinter/        public headers
src/core/                   format-neutral library code
src/formats/<fmt>/          one directory per format module (CMakeLists.txt + sources)
src/cli/                    splinter executable (option parser, modes, renderers glue)
tests/core/ tests/cli/ tests/formats/<fmt>/
tests/fixtures/<fmt>/       small checked-in fixtures (prefer generated-at-test-time)
tests/git/                  end-to-end Git driver tests (scripted temp repos)
docs/
```

### 2.3 Module system

- Every format lives in `src/formats/<fmt>/` and is a **static library**.
- CMake option `SEMSPLINTER_FORMAT_<FMT>` (ON/OFF) enables it. Each option maps to a **vcpkg manifest
  feature** of the same name, so heavy dependencies (qpdf, libtiff, SQLite) are only fetched when
  the module is on. The mapping is appended to `VCPKG_MANIFEST_FEATURES` before `project()`.
- The modules `text` and `binary` are mandatory, because they are the fallback for everything else.
- Registration is **explicit, not static-initializer magic**, because static libraries drop unreferenced
  self-registering objects. CMake generates `formats_registry.cpp` with one
  `register_<fmt>(Registry&)` call per enabled module.
- A format module may depend on other format modules (`svg` uses `xml`; `jpeg` uses the
  raster helper). Dependencies are declared in the module's CMake and checked at configure time.
- A module may expose **sub-features** for optional heavy or differently licensed dependencies, for
  example `png-icc` and `pdf-render`. Sub-features are named
  `<module>-<what>` and map to vcpkg features in the same way.
- **Internal helper modules** register no format. They hold code that several formats share:

  | Helper | Contents | Used by |
  |---|---|---|
  | `src/core/diff` | Myers, histogram, patience, diff3 | text, every SequenceMerge |
  | `src/core/align` | content-defined chunking, rolling hash, alignment refinement | binary, SQLite |
  | `src/core/text` | encoding detection, line splitting with byte offsets | text, csv, xml, json |
  | `src/formats/common/splice` | span-splice writer (re-emit untouched source spans verbatim) | json, xml, csv |
  | `semantic-splinter-raster` | canonical `Raster`, region grouping, guard band, tolerance, heatmap, tile-hash dump, region payload | png, jpeg, raster-misc |
  | `src/formats/geomcore` | bounding boxes, area and volume, Hausdorff distance, displacement regions | mesh-simple |

- **Add-add of counter-allocated IDs** (Inkscape SVG ids, SQLite integer row ids, PNG
  text-chunk sequence numbers) is resolved by one shared rule: keep ours' IDs, renumber theirs' additions deterministically
  (`max(all sides) + 1…` in theirs' order), and rewrite theirs' references. This is reported as
  `auto: renumbered`, not as a conflict. The general pattern for IDs that are only **local aliases**
  (array indices, vertex indices in OBJ faces) is to lift them to
  identities, merge, then lower them again with fresh values. The core provides this as a helper.

### 2.4 Build baseline **[D-2] [D-3]**

- C++20. No compiler extensions. Warnings: `/W4 /permissive- /utf-8` (MSVC), `-Wall -Wextra -Wpedantic` (GCC/Clang).
- CMake ≥ 3.25, `CMakePresets.json` with presets `windows-msvc`, `linux-gcc`, `linux-clang`, `macos`.
- vcpkg manifest mode with a pinned `builtin-baseline`. On Windows, the release distribution uses the
  `x64-windows-static` triplet so `splinter.exe` is a single file.
- Tests: Catch2 v3 via CTest **[D-9]**.
- Windows: the executable embeds a manifest with `activeCodePage=UTF-8`. All paths are
  `std::filesystem::path`, and arguments are converted to UTF-8 at the edge (`wmain`).

---

## 3. Core data model

### 3.1 Address

An **Address** names a semantic unit independently of byte layout.

```text
Address := path [ "#" region ]
path    := RFC 6901 JSON Pointer over *identity keys*, not array positions where identity exists
region  := "rect=" x "," y "," w "," h          (2-D grid, e.g. pixels, tiles)
         | "range=" start "," length            (1-D grid / sequence, e.g. samples, lines, bytes)
         | "box=" x "," y "," z "," w "," h "," d (3-D grid)
```

Examples:

- `/pixels#rect=10,20,32,8`: PNG pixel region.
- `/chunks/tEXt:Author`: a PNG text chunk keyed by keyword.
- `/entries/assets~1logo.png/content/pixels#rect=0,0,64,64`: pixels of a PNG inside a ZIP.
- `/tables/items/rows/pk:17`: a SQLite row identified by its primary key.
- `/lines#range=120,4`: text lines.

Shared path conventions:

- Container members: `/entries/<member-path>/content/…` and `/entries/<member-path>/meta/<field>`.
- XML steps use XPath-like predicates when identity comes from an attribute: `/svg/g[@id=layer1]/rect[@id=r7]`.

Identity keys (`p:3f9a1c`, `id:17`) are produced by each format's matcher. They are stable within a
single comparison and, where the format offers real IDs (SVG `id`, SQLite primary keys, JSON identity
keys), are derived from them.

### 3.2 Change

```cpp
namespace semsplinter {

enum class Op : uint8_t { insert, erase, update, move };
enum class Side : uint8_t { base, ours, theirs };

struct Region { /* variant of rect / range / box / time, see 3.1 */ };

struct Address {
    std::string path;               // JSON-Pointer-like
    std::optional<Region> region;
};

struct Value {                      // small values inline, large ones by reference
    std::optional<std::string> text;    // human-readable rendering (truncated for reports)
    std::optional<Hash128> hash;        // content hash of the exact bytes
    std::optional<BlobRef> blob;        // payload for patches; resolved by the producing format
};

struct Change {
    std::string id;                 // deterministic: "c-" + hex(hash(side, op, address)) [first 12]
    Op op;
    Address at;
    std::optional<Address> from;    // for move
    Value before, after;
    Side side;                      // which side produced it (ours/theirs for merges; theirs for 2-way diff)
    std::vector<std::string> depends_on;  // addresses of entities this change requires/affects
    std::string kind;               // format-specific subtype, e.g. "pixels", "text", "style", "metadata"
    std::optional<double> confidence;     // present only when heuristic matching produced the pairing
    nlohmann::ordered_json metrics; // format-specific quantities (pixel count, delta-E, sample RMS, ...)
};

struct Conflict {
    std::string id;                 // "x-" + hex(hash(address, sorted change ids))
    Address at;
    std::vector<std::string> changes;   // competing change ids (ours..., theirs...)
    std::string reason;             // machine token: "both-modified", "modify-delete", "add-add",
                                    // "dependency", "move-move", "shape-mismatch", "guard-band", ...
    std::vector<std::string> choices;   // supported resolutions: "ours", "theirs", "base", "both-ours-first", ...
    std::string resolved_as;        // what the written output contains ("ours" by default; see 5.4)
};
}
```

**Reason and warning tokens are an open vocabulary.** The core defines the generic tokens, and formats
may add their own. Every token in use must be listed in `docs/schema/tokens.md` (generated from the source
in CI) so that GUIs can localize and explain them. Tokens used in the format documents so far:

- Conflicts: `both-modified`, `modify-delete`, `add-add`, `move-move`, `dependency`, `shape-mismatch`,
  `guard-band`, `overlap` (time ranges), `encoding-mismatch`, `codec-config-mismatch`, `derived-collision`.
- Auto-merge notes: `independent`, `identical`, `both-inserted`, `dependency-touched`, `renumbered`, `editor-state`.
- Warnings: `derived-stale`, `derived-regenerated`, `mark-unsupported`, `lossy-reencode`, `encoding-promoted`,
  `unsafe-chunk-dropped`, `layers-dropped`, `signature-invalidated`, `base-mismatch`, `match-capped`,
  `pvrtc-seam`, `av-sync-offset`, `composite-changed`.

A **ChangeSet** is an ordered vector of `Change`. The order is canonical (by address, then op) so that
reports are deterministic. Formats must separate **identity from position**: a moved paragraph is one
`move` (plus an optional `update`), not an `erase` plus an `insert`.

### 3.3 Hashing **[D-8]**

- `Hash128` = XXH3-128 (xxHash). It is used for content identity, change/conflict IDs, and patch base
  verification. It is fast and has good distribution, but it is **not** a security guarantee, and nothing
  in Splinter relies on collision resistance against an adversary.
- Report input fingerprints use the same hash. SHA-256 can be added later if signed patches are ever wanted.

---

## 4. Format module contract

```cpp
namespace semsplinter {

struct Document { virtual ~Document() = default; };   // format-private model

struct Probe { int confidence; /* 0..100 */ };

struct FormatInfo {
    std::string_view name;               // "png"
    std::string_view description;
    std::span<const std::string_view> extensions;    // ".png"
    Capabilities caps;                   // diff, merge, patch, dump, visual, validate
    std::span<const OptionSpec> options; // format-specific long options (`--png-*`), merged into the
                                         // CLI tables at registration; D-42 collision test runs on the union [D-51]
};

// Shared by every call of one invocation, including recursion into container members [D-47].
struct Context {
    const Registry& registry;            // recurse: registry.load(member bytes, logical name, ctx.child())
    Limits& budget;                      // one decompression/memory budget across all recursion levels
    int depth;                           // checked against --max-depth
    const OptionValues& format_options;  // values of the declared format options, by name
};
// LoadOptions, DiffOptions, MergeOptions, and SaveOptions each carry a `Context&`.

class Format {
public:
    virtual ~Format() = default;
    virtual const FormatInfo& info() const = 0;

    // Detection: magic-number sniff over the first <= 64 KiB plus the logical name.
    virtual Probe probe(std::span<const std::byte> head, std::string_view logical_name) const = 0;

    // Load. Must enforce Limits; throws semsplinter::Error{invalid_input} on bad data.
    virtual std::unique_ptr<Document> load(ByteSource src, const LoadOptions&) const = 0;

    // Two-way structural diff.
    virtual ChangeSet diff(const Document& a, const Document& b, const DiffOptions&) const = 0;

    // Three-way merge. The output document is complete; conflicted entities hold the
    // policy-selected side (5.4). Must not invent content that neither side contains.
    virtual MergeResult merge(const Document& base, const Document& ours, const Document& theirs,
                              const MergeOptions&) const = 0;

    // Serialize. Must re-emit untouched units byte-exact when the format permits.
    // Implemented on top of the fix-up Emitter (§12.4): the format describes content and
    // envelope fields; the Emitter computes sizes, offsets, checksums, and digests.
    virtual void save(const Document&, ByteSink, const SaveOptions&) const = 0;

    // Envelope map of a loaded document (§12.3): byte ranges classified as content, framing,
    // integrity, authenticity, encoding, derived, or volatile, with the unit address each belongs
    // to, plus the integrity and signature verification results. Used for churn attribution
    // and `inspect --layout`. A format without one gets a single "content" range.
    virtual const EnvelopeMap* envelope(const Document&) const { return nullptr; }

    // Structural/semantic validation of serialized bytes (re-load + invariants).
    virtual Diagnostics validate(std::span<const std::byte>) const;          // default: reload

    // Canonical human-readable text, used for textconv and `--dump`. Stable, line-oriented,
    // diff-friendly: one unit per line where possible, no volatile data.
    virtual void dump(const Document&, std::ostream&, const DumpOptions&) const = 0;

    // Optional visual output (heatmap PNG, side-by-side, ...).
    virtual bool render_visual(const Document& a, const Document& b, const ChangeSet&,
                               ByteSink, const VisualOptions&) const { return false; }
};
}
```

**Patch application is not a separate entry point.** A patch carries, for every touched unit, its
*before* and *after* values. Applying a patch to a target is
`merge(base = patch.before-view, ours = target, theirs = patch.after-view)`, restricted to the touched
units. "Fuzz" becomes the matcher's tolerance when locating units in the target, and a reject is a conflict.
Every format with merge support therefore gets patch support for free, except for the payload
encoding, which is per-format (`encode_payload` / `decode_payload`, defaulting to the opaque-blob encoding).

Container formats (ZIP, TAR) and SQLite blobs **recurse**: a member is loaded through the
registry by its logical name and content, so a PNG inside a ZIP is diffed and merged as a PNG.

---

## 5. Merge engine

### 5.1 Primitives

Formats compose these primitives instead of writing ad-hoc three-way logic. Each primitive is
generic over the unit type and takes callbacks for identity, equality, and recursive merge.

| Primitive | Shape | Identity | Used by (examples) |
|---|---|---|---|
| **KeyedMerge** | unordered or loosely ordered map | explicit key | PNG ancillary chunks, ZIP members, JSON objects, XML attributes, SQLite rows by PK |
| **SequenceMerge** (diff3) | ordered list | content hash and/or explicit ID | text lines, CSV rows, JSON arrays, XML child lists, SVG elements, PDF pages |
| **GridMerge** | dense N-D array of equal shape | coordinates | PNG/JPEG/TIFF pixels, CSV cell grids, unknown-binary byte ranges (1-D) |
| **OpaqueLeaf** | indivisible value | hash | anything without exploitable structure: ICC profiles, compressed blobs, unknown members |

#### KeyedMerge rules (per key, after recursion where values are composite)

| base | ours | theirs | result |
|---|---|---|---|
| A | A | A | A |
| A | B | A | B |
| A | A | C | C |
| A | B | B | B (identical edit) |
| A | B | C | recurse if composite; else **conflict `both-modified`** |
| A | — | A | deleted |
| A | — | C | **conflict `modify-delete`** |
| — | B | — | B (added) |
| — | B | B | B |
| — | B | C | recurse if composite and format allows; else **conflict `add-add`** |

Output key order: ours' order, with theirs-only keys inserted after their nearest preceding key that
survives (ties are broken by theirs' order). This is deterministic and minimizes churn for ordered serializations.
A format may override placement when position is itself a reference, for example by appending theirs'
additions to the end of an index-referenced array so that every one of ours' indices stays valid.

#### SequenceMerge

- Matching: elements with explicit IDs are matched by ID first. The rest are matched by content hash
  using histogram diff (Git's default family), falling back to Myers for small inputs. `-d/--minimal`
  forces a minimal Myers diff.
- Then classic diff3 chunking against base: stable, ours-only, theirs-only, both-identical, both-different (conflict).
- **Moves**: an ID-matched element whose position changed on one side becomes a `move`. Move plus
  edit on the other side merges (the element moves *and* gets the edit). Move on both sides to different
  places is a conflict `move-move`.
- Insertions by both sides at the same anchor: conflict `add-add` unless identical, or unless the format
  declares insertion order insignificant there (then both are kept, ours first, and reported as `auto: both-inserted`).
- Matched elements that differ on both sides recurse through the element's own merge
  (a paragraph's runs, a layer's pixels). This is what keeps conflicts small.

#### GridMerge

- Precondition: base, ours, and theirs have the same shape *and* the same cell encoding (after
  normalization, 5.2). If one side changes the shape and the other side left the grid untouched, the
  reshaping side wins. If the other side *also* edited cells, or both sides changed the shape, the result is a
  **conflict `shape-mismatch`**. Taking the reshaped grid wholesale would silently drop the other side's
  edits. **Exception: pure crop and canvas extension merge (Q-PNG-3).** When the reshaping side's grid equals
  base at one integer offset over their overlap, the other side's regions are translated by that offset and merge
  normally. Only a region that falls partly or wholly outside the new extent conflicts (`shape-mismatch`,
  reason `cropped-away`). Scaling, rotation, and resampling remain `shape-mismatch`.
- In **lossy coded formats** the cell is the coded unit, copied verbatim: the JPEG MCU, the lossy
  TIFF tile, the lossy WebP image. Merging below that granularity means
  re-encoding, which is always opt-in.
- Per cell: classic three-way (unchanged, ours, theirs, identical, conflict). Tolerance is opt-in:
  `--tolerance` treats |Δ| ≤ ε as unchanged and is recorded in the report.
- Changed cells are grouped into **regions**: connected components with 8-connectivity (2-D),
  or runs (1-D), merged when closer than `--gap`. Reports describe regions, not cells.
- **Guard band** (coupled edits such as anti-aliasing, shadows, or crossfades): if an ours-region and a
  theirs-region come within `--guard=N` cells of each other, both regions conflict as one
  (`reason: guard-band`), even if no cell overlaps. **Default N = 0 for every grid [D-10]**: regions merge unless a
  cell was changed by both sides. Regions from the two sides within 1 cell of each other are still reported, as warning
  `adjacent-edits` (anti-aliasing seams), without blocking the merge.
- **No averaging, ever**, in the default path. A conflicted cell takes the policy-selected side (5.4).

### 5.2 Normalization

Before diffing, formats normalize away representation differences that carry no meaning, such as PNG
filter and compression choices, XML attribute order,
JSON whitespace, and ZIP entry timestamps when requested. Each format document lists exactly what it
normalizes. Normalization never feeds back into serialized output: untouched units are re-emitted from
their original bytes.

### 5.3 Dependencies

A change's `depends_on` lists the addresses it relies on (an SVG `<use>` referencing a gradient, a
PDF annotation anchored to a page, a SQLite row referencing a foreign key). After primitive merging, the
engine checks each auto-merged change against the other side's changes:

- If side X **deleted** an entity that side Y's surviving change depends on: **conflict `dependency`**.
- If side X **modified** an entity that Y's change depends on: allowed by default and reported as
  `auto: dependency-touched`. Formats can upgrade specific pairs to conflicts. For example, in SQLite a
  row and the row its foreign key references changed on different sides can be a conflict.
- **Envelope fields** (sizes, offsets, counts, checksums, digests, signatures, compressed encodings) are
  never merged as values. They are regenerated from the merged content according to §12.
- **Derived data** (PDF appearance streams, embedded thumbnails, TIFF reduced-resolution
  subfiles) is never merged. It is either regenerated by the format or explicitly marked stale, and a
  validation warning is emitted (`derived-stale`).

### 5.4 Conflict policy and output

Git requires *some* file in `%A`. Binary formats cannot hold inline conflict markers. Therefore:

| Option | Meaning |
|---|---|
| `--conflict=ours` (default) | Conflicted units take ours. Exit status still reports conflict. In `git-merge` mode the default follows Git instead: `mark` for formats whose marking is classic `<<<<<<<` markers (see below), `ours` for the rest **[D-21]**. |
| `--conflict=theirs` / `--conflict=base` | Same, other side. |
| `--conflict=mark` | Format-specific visible marking where meaningful, e.g. SVG: both elements kept in a `<g id="splinter-conflict-…">`; text: classic `<<<<<<<` markers. Formats without a meaningful marking fall back to `ours` with a warning. |
| `--ours` / `--theirs` / `--union` | Git `merge-file` compatible. These *resolve* conflicts (exit 0) instead of only choosing what to write. `--union` applies only where the format declares a union semantics (sequences); otherwise it is an error. |
| `--resolve=FILE` | JSON `{ "x-1a2b…": "theirs", … }` that resolves individual conflicts by ID. Because IDs are deterministic, a user (or GUI) can inspect conflicts, write decisions, and re-run the merge. |

`--conflict=mark` per format, as drafted in the format documents:

| Format(s) | Marking |
|---|---|
| text | classic `<<<<<<<` markers around the smallest conflicting region. The file will not load until it is resolved, which is intended |
| SVG | theirs in a hidden Inkscape-layer group `splinter-conflict-…` |
| XML (generic) | comment holding theirs' version |
| OBJ | sibling or object holding theirs' version |
| PNG, JPEG, other rasters | not meaningful. Ours plus a sidecar overlay PNG, warning `mark-unsupported` |

A conflict sidecar (`--conflicts=FILE`, default `<output>.splinter-conflicts.json` in Git-merge mode)
holds the full report (§7).

---

## 6. Patch file format **[D-6]**

A splinter patch is a **text envelope**, which keeps it mailable and reviewable and lets it coexist
with unified diffs in one file. The payload is binary-safe encoded:

```text
splinter-patch 1
--- art/logo.png	xxh3:7c1e…
+++ art/logo.png	xxh3:90ab…
format png
@@ update /pixels#rect=10,20,32,8 c-4f1a99e0c2b7 @@
payload before zstd+base85 1840
<base85 lines…>
payload after zstd+base85 1912
<base85 lines…>
@@ update /chunks/tEXt:Author c-0be2… @@
- "Alice"
+ "Bob"
payload before zstd+base85 21
<base85 lines…>
payload after zstd+base85 19
<base85 lines…>
```

- Every envelope section starts with its own `splinter-patch 1` line, so envelope sections and unified diffs
  can follow each other in any order. Text outside a section (mail headers, `diff` lines) is skipped, as GNU
  patch does.
- The `---`/`+++` names are the operands as given (like `diff -u`), each followed by a tab and the hash of that
  file. The `format` line names the format module.
- A hunk header is `@@ OP ADDRESS CHANGE-ID @@` (the address may contain spaces; the operation and the ID
  cannot). `- ` and `+ ` lines show the before and after values for review and are ignored when applying;
  `#` lines are comments. `payload before|after CODEC N` is followed by the base85 lines of N encoded bytes
  (64 bytes, 80 characters per line, Git's base85 alphabet). A side without a payload is absent (insert,
  erase).
- What a payload holds is defined by the format's `payload()` (SPEC §4); png.md §6.7 has PNG's.

- `diff` mode writes a human report only, unless `--binary` is given. With `--binary` it writes the
  envelope with payloads, mirroring `git diff --binary`. For text files the output is a plain unified
  diff that GNU `patch` can also apply.
- Hunks carry *before* and *after* for each touched unit, so `patch -R` (reverse) is symmetric and
  application can run as a three-way merge (§4).
- Multi-file patches (from `-r`) list multiple `---/+++` sections. `-pNUM` strips path components exactly as GNU patch does.
- Compression is zstd (`zstd+base85`, level 19). zstd and zlib are core dependencies, so every build writes
  `zstd+base85` and reads both `zstd+base85` and `zlib+base85` (Phase 5 patches) **[D-7]**.
- `--binary --json` writes the same patch as JSON: the §7.1 report with each change carrying its payload
  (`payload: {codec, encoding: "base64", bytes}`). `patch` mode accepts both serializations **[D-6]**.
- Payloads are expressed at the **content layer** (decoded pixels, samples, text), not as encoded bytes, so a
  patch survives different compression of the target. Applying such a patch re-encodes per §12.2, so the result
  equals the intended target *semantically*, and byte equality is reported, not promised (`reencode-nonidentical`).
  `--exact` additionally stores the target's encoded bytes for each changed stream. When the target's base hash
  matches, application then reproduces the target byte for byte. Envelope fields are never stored in a patch,
  because they are regenerated.

---

## 7. Reports

### 7.1 JSON report (schema `splinter-report/1`)

```json
{
  "schema": "splinter-report/1",
  "tool": { "name": "splinter", "version": "0.1.0" },
  "mode": "merge",
  "format": "png",
  "inputs": {
    "base":   { "path": "base.png",   "hash": "xxh3:…", "bytes": 10234 },
    "ours":   { "path": "ours.png",   "hash": "xxh3:…", "bytes": 10301 },
    "theirs": { "path": "theirs.png", "hash": "xxh3:…", "bytes": 10288 }
  },
  "options": { "guard": 0, "tolerance": 0, "conflict": "ours" },
  "changes_ours":   [ /* Change */ ],
  "changes_theirs": [ /* Change */ ],
  "auto_merged":    [ { "change": "c-…", "rule": "independent" } ],
  "conflicts":      [ /* Conflict */ ],
  "stats":  { "changed_units_ours": 3, "changed_units_theirs": 2, "conflicts": 1, "format": { } },
  "envelope": {                                   /* §12.6: why the bytes changed */
    "integrity": { "base": "ok", "ours": "ok", "theirs": { "status": "mismatch", "at": ["/chunks/7"] } },
    "signatures": [ { "at": "/entries/META-INF~1CERT.RSA", "base": "valid", "result": "stripped" } ],
    "churn": { "bytes_changed": 1288410, "content": 3120, "framing": 16, "integrity": 12,
               "encoding": 1285262, "authenticity": 0, "derived": 0, "volatile": 0, "unexplained": 0 }
  },
  "validation": { "ok": true, "warnings": [ { "code": "derived-stale", "at": "/composite" } ] },
  "result": { "path": "merged.png", "hash": "xxh3:…", "status": "conflicts" }
}
```

For a diff, `changes` replaces `changes_ours`/`changes_theirs`, and `auto_merged`/`conflicts` are absent.

### 7.2 Text renderers

`--normal` (default), `-u/--unified`, `-c/--context`, `-q/--brief`, `-y/--side-by-side`, `--dump`, and
`--md[=FILE]`, a Markdown rendering of the §7.1 report (summary table, then one section per changed unit) **[D-22]**.
For structured formats, "lines" are the format's `dump()` lines, so `-u` produces a familiar unified
diff of canonical dumps, annotated with `@@ address @@` hunk headers, and with per-hunk metrics comments under `--annotate`.
This also makes `textconv` and `splinter -u` consistent.

---

## 8. Exit status **[D-5]**

Default (`--exit-codes=posix`), matching the emulated tool:

| Mode | 0 | 1 | 2 |
|---|---|---|---|
| diff | identical | differences | trouble |
| diff3 / merge | clean merge | conflicts | trouble |
| patch | all hunks applied | some hunks rejected | trouble |
| git-diff | always 0 unless trouble (Git treats non-zero as a crash) | — | trouble |
| git-merge | clean | conflicts or trouble (Git only distinguishes zero and non-zero) | — |

`--exit-codes=extended` splits "trouble": `0` none/clean, `1` differences/conflicts, `2` invalid input,
`3` unsupported format or operation, `4` resource limit exceeded, `5` I/O error, `6` internal error.
Extended codes are never used in Git modes.

---

## 9. Safety and limits

| Limit | Default | Option |
|---|---|---|
| Max input file size | 2 GiB | `--max-input` |
| Max decoded memory per document | 4 GiB | `--max-memory` |
| Max pixels per image | 2^30 | `--max-pixels` |
| ZIP: max entries / max total uncompressed / max ratio | 65 536 / 4 GiB / 200:1 | `--max-zip-*` |
| Container recursion depth | 8 | `--max-depth` |
| XML: entity expansion | disabled (no DTD entities); external entities are never loaded | `--xml-internal-entities` allows *internal* general entities only, under expat's amplification limits. The SVG dialect has a default bounded exception for Illustrator exports (svg.md §10, Q-SVG-2) |

- Parsers are fuzzed (libFuzzer/AFL harness per format in CI; see PLAN.md).
- Output: write to a sibling temp file, validate, flush, then rename. On failure, the temp file is removed and the target is untouched.
- No network access and no external processes. The only exceptions are the opt-in `--diff-program` for text diffs (diff3 compatibility), the opt-in `--sign-command` external signer (§12.5), and `git config --get` in `git-diff` mode to read `trustExitCode` (D-16; skipped when `--trust-exit-code` is given or `git` is not on PATH).

---

## 10. Library API (high level)

```cpp
#include <semsplinter/semsplinter.hpp>

semsplinter::Registry reg = semsplinter::Registry::with_builtin_formats();

semsplinter::Limits limits;
auto input = [&](const char* p) { return semsplinter::Input{semsplinter::ByteSource::from_file(p, limits), p}; };

semsplinter::DiffRequest d{.a = input("old.png"), .b = input("new.png"), .report = true,
                           .render = semsplinter::RenderRequest{.style = semsplinter::OutputStyle::unified}};
semsplinter::DiffResult dr = semsplinter::diff(reg, d);        // differs, rendered text, Report, visual

semsplinter::MergeRequest m{.base = input("b.png"), .ours = input("o.png"), .theirs = input("t.png"),
                            .conflict = {semsplinter::Policy::ours}, .output = "m.png"};
semsplinter::MergeOutcome mo = semsplinter::merge(reg, m);     // writes atomically; Report, conflict count
```

`diff` and `merge` detect the format (inputs of different formats compare and merge as bytes), load through
the Format contract, and render through the format's `Comparison` (`Format::compare`): text renders GNU output,
binary the GNU "differ" line, and every other format its change list and churn line, or unified, context, and
side-by-side output over its dump lines. Options for text-bearing units (`TextLoadOptions`, `TextDiffOptions`,
`TextMergeOptions`) travel inside the Format option structs, so a text file merges the same way at top level and
inside a container. `dump` and `inspect` complete the API.

- Errors are exceptions deriving from `semsplinter::Error` with a category (`invalid_input`,
  `unsupported`, `limit`, `io`, `internal`) **[D-11]**. The CLI maps categories to exit codes.
  A future C API wraps calls and translates exceptions into status codes.
- Inputs can be paths or in-memory spans (`ByteSource`). The library itself does not touch argv, environment variables, or the terminal.
- Thread safety: `Format` objects are stateless and shareable. Documents are not shared across threads.
  The library starts no threads in v1.
- **Version-control adapters are thin layers over this API [D-53].** Everything `git-diff` and `git-merge` do
  beyond argument mapping (conflict policy, sidecar report, atomic output, exit status) lives in the library, as
  `merge(reg, MergeRequest)` plus the report renderers. The Git adapter uses only public headers, so adapters for
  other version-control systems can live in separate programs or plugins without changes to the core.

---

## 11. Detection order

1. `--format=NAME` (explicit).
2. Logical name extension. In Git modes this is `%P` or the 7-argument path, **not** the temp file
   name, because Git temp files have no useful extension.
3. Content probe by all registered formats. Highest confidence wins, and ties are broken by registry order.
4. `text` if the content looks like text (no NUL bytes in the first 8 KiB, valid UTF-8 or a declared encoding), else `binary`.

If base, ours, and theirs detect as different formats, the merge refuses with "trouble" unless `--format` is given.

---

## 12. The encoding envelope: framing, integrity, authenticity, and codec layers

Many formats wrap their content in bookkeeping that is computed *from* the content: chunk lengths,
absolute offsets, CRCs, digests, signatures, and compressed encodings. A one-pixel edit in a PNG
changes one IDAT deflate stream (often most of the file's bytes), its Adler-32, its chunk CRC, and possibly its
length. Renaming one member of a ZIP archive shifts every later local header, so every later offset in the
central directory changes. A naive byte diff reports all of that. Splinter must instead
**understand the envelope, look through it, and regenerate it**. The per-format catalog is in
[ENVELOPES.md](ENVELOPES.md) **[D-34]**.

### 12.1 Byte classes

Every byte range of a loaded file belongs to exactly one class:

| Class | What it is | Examples | Diffed as content? | Merged? | On write |
|---|---|---|---|---|---|
| **content** | the semantic payload, after decoding | pixels, text, samples, cell values, chunk *values* | yes | yes | from the model |
| **framing** | structure that locates content: lengths, sizes, counts, offsets (relative or absolute), alignment and padding, indexes and tables of contents | PNG chunk length, ZIP local/central offsets and sizes, TAR header sizes, TIFF IFD offsets, PDF xref and stream `/Length`, SQLite page pointers | no | no | recomputed (§12.4) |
| **integrity** | unkeyed checks over content: CRCs, checksums, unkeyed digests, parity, and FEC | PNG chunk CRC-32, zlib Adler-32, gzip CRC-32 + ISIZE, ZIP member CRC-32, TAR header checksum, JAR `MANIFEST.MF` SHA-256 digests, XMP extended GUID (MD5), SQLite checksum VFS | no (status is reported) | no | recomputed |
| **authenticity** | keyed values: digital signatures and MACs | JAR/APK signatures, PDF `/ByteRange` + CMS `/Contents`, XMLDSig in generic XML, WinZip AES HMAC | no (validity is reported) | no | policy §12.5 |
| **encoding** | a lossless transform between stored bytes and content: compression, filters, predictors, entropy-coding framing, re-pagination, text transport encodings | deflate/zlib/gzip, zstd, LZW, PackBits/RLE, LZF, brotli, PNG scanline filters, TIFF predictors, base64/ASCII85/hex, UTF-16 | no (looked through) | no | raw copy if the decoded content is unchanged, else re-encode (§12.2) |
| **derived** | content-like data computed from other content | PDF appearance streams, embedded thumbnails, TIFF reduced-resolution subfiles, OBJ/PLY bounding data written by exporters | no (reported as derived) | no | regenerate or mark stale (§5.3) |
| **volatile** | bookkeeping that legitimately changes on every save | gzip header mtime, ZIP DOS times, PDF `/ID[1]`, generator strings, export timestamps | no (with `--ignore=volatile`, the default in Git modes) | KeyedMerge, newest wins | preserved from ours for untouched units, set deterministically for new ones |

The class table is a property of the format, not of a single file. Each format documents its own
fields in section 7.1 of its implementation document.

### 12.2 Codec layers

A format declares its **layer stack** from bytes to content, for example
PNG `file → chunks(length, type, CRC) → IDAT concatenation → zlib(Adler-32) → deflate → scanline filters → pixels`.

- **Diff and merge happen at the innermost semantic layer.** Outer layers are transport. Two inputs whose
  decoded content is equal are *equal*, however different their bytes are (the difference is reported as
  `encoding` churn).
- **Untouched streams are copied raw.** If a stream's decoded content is identical to the side the output
  is built from, its encoded bytes (and their framing and integrity fields) are copied verbatim.
- **Touched streams are re-encoded** with parameters inferred from the original. The parameters include the
  zlib header's FLEVEL and window size, the deflate strategy guessed by trial compression of a sample,
  per-row PNG filter types for rows that did not change, the TIFF predictor, the zstd level (frames do not
  record it, so it is guessed from the window size plus a trial compression). The warning `encoding-reencoded` names the stream. Lossless
  re-encoding is semantically exact. Lossy codecs follow the coded-unit rule (§5.1 GridMerge) and are never
  re-encoded without opt-in.
- **Bit-exact reconstruction** of a re-compressed stream, so that patch application reproduces the
  target's exact bytes, is not needed for correctness. It is an optional later optimization using
  deflate reconstruction data (preflate/preflate-rs or Chromium's Puffin; licenses to verify) **[D-40]**.
  Until then, `--exact` patches carry the target's encoded bytes for every changed stream (§6).
- **Encryption** is a codec layer that needs a secret. In v1, encrypted members are detected, reported
  as `encrypted`, and treated as OpaqueLeaf. The exception is "permission-only" PDF encryption, where the user password is
  empty, so the file key can be derived and the layer can be looked through and re-applied with the same keys.
  Password-based decryption is deferred **[D-41]**.
- The **binary fallback** may scan unknown files for embedded zlib, gzip, zstd, and LZ4 streams (trial
  decoding at signature hits, as precomp does) with `--scan-streams`. It diffs their decoded content
  but never merges them, because their framing and integrity fields are unknown.

### 12.3 Reading: verify and map

On load, every format:

1. Checks **framing consistency**: lengths inside bounds, offsets pointing at what they claim,
   counts matching. Inconsistency means invalid input (trouble), unless `--lenient` is set, which keeps
   the content that can be recovered and warns `framing-inconsistent`. A format may declare specific fields
   **advisory** when writers commonly get them wrong and readers ignore them (BMP `bfSize`, the WebP RIFF pad).
   An inconsistency in an advisory field always only warns, and the field is rewritten correctly on output.
2. Verifies **integrity fields** and records each one as `ok`, `mismatch`, `unverifiable`, or `not-checked`.
   Formats may check lazily, for example ZIP CRCs of members that are only raw-copied are not checked (Q-ZIP-11),
   and `--integrity=strict` forces every check.
   A mismatch is reported (`integrity-mismatch-input`), never silently "fixed" in a diff.
3. Verifies **authenticity fields** when the verification material is in the file or supplied
   (embedded certificates, `--verify-key`): `valid`, `invalid`, `unverifiable`, or `no-crypto` (built without
   the `crypto` feature). Splinter checks cryptographic validity only. Certificate chain trust is not evaluated
   and is reported as `chain-not-evaluated`.
4. Builds the **EnvelopeMap**: sorted, non-overlapping byte ranges, each with a class, a unit address,
   and (for encoding ranges) the layer they belong to.

### 12.4 Writing: the fix-up Emitter

Formats serialize through a shared core component instead of hand-patching sizes and checksums:

```cpp
namespace semsplinter::emit {
// A tree of segments. Fields are placeholders resolved after layout.
Seg bytes(std::span<const std::byte>);                  // literal bytes
Seg raw(const ByteSource&, uint64_t off, uint64_t len); // verbatim copy from an input (streamed)
Seg group(GroupId, std::vector<Seg>);                   // addressable region
Seg align(unsigned n, std::byte fill);                  // padding to a boundary
Seg encode(Codec, CodecParams, Seg child);              // compress/encode a subtree
Seg field(Width, Endian, Expr);                         // fixed-width numeric field
Seg varfield(VarEncoding, Expr);                        // variable width: varint, VLQ, decimal text
// Expr: size_of(g), offset_of(g [, relative_to g2]), count_of(g), constant, custom(fn)
//       crc32(g), adler32(g), crc8/crc16 variants, md5/sha1/sha256(g ...),
//       xxh64(g), checksum_adjust(file, field_zeroed)
}
```

Resolution:

- **Layout.** Compute all sizes and offsets. Variable-width fields (decimal `/Length`,
  pax record lengths that include their own digits, ZIP64 promotion) iterate to a fixed point.
  Widths only grow, so the iteration terminates, and it is capped at 16 iterations, after which the result is internal error.
- **Digests.** These are evaluated innermost first, in dependency order. Self-referential checks use the
  format's convention, for example the field is zeroed while summing (TAR header checksum), or a byte range
  is excluded (PDF `/ByteRange` around `/Contents`, with the placeholder width reserved in advance).
- **Streaming.** Raw segments larger than 1 MiB are streamed from the source rather than buffered, and digests over them are computed in the
  same pass. A second pass is needed only when a digest has to be *written before* the data it covers.
- **Writer delegation.** When an authoritative library writes the format, as SQLite does for databases, the format delegates to it and the Emitter frames only what is around it.

The Emitter starts minimal. Phase 3 (PNG) needs bytes, raw, group, size_of, crc32, adler32, and
encode(zlib). Every later format adds only the expressions it uses.

#### Layout stability **[D-38]**

`--layout=stable` (default) prefers writes that keep large payloads at the same offsets and byte-identical.
Examples:

- Absorb metadata growth into existing padding, such as TIFF slack or unused space before a ZIP central directory.
- Append PDF changes as an incremental update.

`--layout=compact` rewrites canonically. Stable layout reduces byte churn, keeps external offsets valid, and lets Git's
delta compression store merged results cheaply.

### 12.5 Authenticity policy **[D-35] [D-36] [D-37]**

A merged or patched file has new content, so any signature over that content is necessarily invalid.
Splinter never pretends otherwise.

| `--signatures=` | Effect |
|---|---|
| `auto` (default) | Per-format default from ENVELOPES.md. It chooses whichever leaves a *loadable* file: strip signatures whose invalidity makes consumers reject the file (signed JAR entries throw at load, while unsigned ones load), keep earlier PDF signatures by writing an incremental update (they remain valid for their revision), and otherwise **keep** the signature with `signature-invalidated`, because consumers reject unsigned and badly signed files alike, and the kept structure is what an external re-signer needs |
| `keep` | Keep the invalid signature bytes and warn `signature-invalidated` |
| `strip` | Remove signatures and the references to them, and warn `signature-stripped` |
| `resign` | Re-sign with a key the user supplies (below). Fails with trouble if no key was supplied |

**Re-signing needs the private key.** A public key or certificate can *verify* a signature but cannot
produce one. Splinter can re-sign only when the user explicitly supplies signing material for that
invocation or through config:

- `--sign-key=FILE` (PEM or PKCS#8, or PKCS#12 with `--sign-pass-file=FILE`) and `--sign-cert=FILE`, for the
  signature schemes splinter implements: JAR v1 and PDF CMS incremental signature. APK v2/v3 come later.
  **Post-v1** (needs the `crypto` feature, D-36): a v1 build exits 2 with `unsupported (built without crypto)`.
- `--sign-command=CMD`, which runs an external signer (`apksigner`, `signtool`, `jarsigner`, `gpg`) on the validated
  output before the final rename. This is the one opt-in exception to "no external processes" (§9) **[D-37]**.
- `--mac-key=FILE`: the key for keyed MACs (HMAC) in formats that use one, including formats whose key is
  publicly known. Splinter **ships no third-party keys**, and it never derives, guesses, or brute-forces keys. Keys
  are read from files or environment variables, are never written to reports or logs (only a SHA-256
  fingerprint is recorded), and are wiped from memory after use. Splinter does not remove or bypass DRM or
  access-control encryption.
- Git modes never sign unless `git config splinter.signKey` (or `splinter.signCommand`) is set. CI is the
  usual place to re-sign.

**Recomputable "signature-like" fields are integrity, not authenticity.** JAR `MANIFEST.MF` per-entry
digests and `.SF` digests of manifest sections are unkeyed, so splinter recomputes them. Only the PKCS#7 block over
the `.SF` file needs a key. Formats state precisely where the keyed boundary lies.

**External integrity references** are hashes of *other* files: lockfile `integrity`/`checksum` fields,
`go.sum`, `.sfv`, and package manifests. They cannot be recomputed by a merge driver that sees one file. They are
**coupled** to their subject: a version string and its hash always come from the same side. A merge that
would combine one side's version with the other side's hash is a `dependency` conflict.

### 12.6 Reporting churn

The report's `envelope` object (§7.1) explains byte-level change:

- `integrity` and `signatures`: per-input verification results and what happened to each signature.
- `churn`: changed bytes between the two files being compared (or between ours and the result), attributed
  by aligning the two EnvelopeMaps. Each byte is classed as `content`, `framing`, `integrity`, `encoding`, `authenticity`, `derived`,
  `volatile`, or `unexplained`. A high `unexplained` share means the format module is missing a field.
  It is a quality signal that CI tracks on the fixture corpus.

Text output adds one summary comment, for example:
`# bytes changed 1.2 MiB: 99.8% encoding (IDAT deflate re-encoded), <0.1% framing/integrity; content: 212 px in 1 region`.
`--show=CLASS[,...]` lists envelope-level changes individually (for example `--show=integrity,framing`) for people
debugging writers or tampering.

### 12.7 Integrity policy **[D-39]**

| `--integrity=` | Effect on output |
|---|---|
| `recompute` (default) | Regenerated units get fresh integrity fields. Raw-copied untouched units keep their bytes exactly, *including* an incorrect CRC from the input, with `integrity-mismatch-input` warned |
| `repair` | Recompute all integrity fields, even on untouched units. The changes are reported as `integrity` churn |
| `strict` | Refuse (trouble) if any input has an integrity mismatch |

FEC and parity (CD-ROM EDC/ECC, RAR recovery records, Reed-Solomon in custom formats) are recomputed when the
format module implements the encoder. Otherwise the module refuses to write changed content under them
(`unsupported`). Separate parity files such as PAR2 become stale, which splinter cannot fix: they are another file.

### 12.8 Crypto dependency **[D-36]**

Digests (MD5, SHA-1, SHA-2) and signature verification and creation (RSA, ECDSA, EdDSA, X.509, CMS/PKCS#7) come
from OpenSSL 3 libcrypto (vcpkg `openssl` 3.6.x, Apache-2.0), as the optional `crypto` feature. **Deferred: not built in v1**, because no v1 format relies on signatures. When added, it is on in release
builds. Without it, splinter still recomputes CRC and Adler checksums (implemented in core), but it reports
digest and signature fields as `no-crypto`, refuses merges that would require recomputing a digest (`unsupported`), and
cannot re-sign. Splinter never implements cryptographic primitives itself.
