# PNG — Implementation Document

**Tier:** T1 · **Module:** `semantic-splinter-png` · **vcpkg feature:** `png` · **Depends on modules:** none (core only). Introduces the shared helper module `semantic-splinter-raster` (see 3.4), with no external dependencies.
**Extensions / magic:** `.png`, `.apng`. Magic `89 50 4E 47 0D 0A 1A 0A`, first chunk must be `IHDR`. APNG is detected by an `acTL` chunk before the first `IDAT` (extension alone is not trusted). Probe confidence: 100 on signature + well-formed 13-byte `IHDR`, 0 otherwise. An `IHDR` CRC mismatch does not fail the probe; it is reported as `integrity-mismatch-input` (SPEC §12.3).

## 1. Why it matters

PNG is the default lossless raster in game repos (UI, sprites, icons, atlases), documentation, web assets,
and is embedded inside ZIP and TAR archives and SVG data URIs, so the module is reused by
recursion (SPEC §4). Today Git reports "Binary files differ", and any concurrent edit to the same PNG is
a manual redo. Two artists touching different corners of one sprite sheet, or one person changing
metadata while another paints, is the common case and should merge cleanly.

A user should be able to:

- see *where* an image changed (bounding boxes of connected regions), *how much* (pixel count, max and
  mean per-channel delta, ΔE00), and whether only metadata or encoding changed;
- get a heatmap/overlay PNG of the change;
- merge base/ours/theirs where edits are spatially separated, with conflicts reported as rectangles;
- carry metadata (text, color profile, physical size) through merges without loss;
- produce and apply a `splinter-patch` containing only the touched regions.

This is the first proof-of-concept format, so it also defines the raster conventions that JPEG and
raster-misc reuse.

## 2. Format anatomy

PNG (ISO/IEC 15948, W3C PNG Third Edition 2025, which also standardizes APNG, `cICP`, `mDCV`, `cLLI`, `eXIf`).

- File = signature + chunks. Chunk = `length:u32be` (≤ 2^31-1), `type:4cc`, `data`, `crc32` over type+data.
- Chunk type bits: bit 5 of byte 0 lowercase = ancillary, byte 1 = private, byte 2 = reserved (must be
  uppercase), byte 3 lowercase = **safe-to-copy**. The safe-to-copy bit drives preservation (§7).
- Critical chunks: `IHDR` (width, height, bit depth, color type, compression=0, filter=0, interlace 0/1),
  `PLTE`, `IDAT` (one zlib stream split over consecutive chunks), `IEND`.
- Color types and legal bit depths: 0 gray (1,2,4,8,16), 2 RGB (8,16), 3 indexed (1,2,4,8), 4 gray+alpha (8,16), 6 RGBA (8,16).
- Alpha is always **straight** (unassociated). `tRNS` gives per-index alpha for type 3, or a single
  transparent key color for types 0 and 2.
- Raster encoding: per-scanline filter byte (None, Sub, Up, Average, Paeth), Adam7 interlace (7 passes),
  zlib/deflate. All of these are encoding choices with no semantic weight.
- Color interpretation chunks: `cICP` > `iCCP` > `sRGB` > `gAMA`+`cHRM` (precedence per PNG 3rd ed.).
  `sBIT` (significant bits), `mDCV`/`cLLI` (HDR mastering metadata).
- Other standard ancillaries: `tEXt`, `zTXt`, `iTXt` (keyword, optional language/translated keyword), `bKGD`, `hIST`,
  `pHYs`, `sPLT` (named suggested palettes), `tIME`, `eXIf`, plus registered extensions (`oFFs`, `pCAL`, `sCAL`, `sTER`, `gIFg`, `gIFx`).
- Ordering rules: `cICP`, `iCCP`/`sRGB`, `gAMA`, `cHRM`, `sBIT`, `mDCV`, `cLLI` before `PLTE`; `tRNS`, `bKGD`, `hIST` after `PLTE` and before `IDAT`;
  `pHYs`, `sPLT`, `eXIf`, `acTL` before `IDAT`; text and `tIME` anywhere.
- **APNG** (`acTL`, `fcTL`, `fdAT`): detected, but animation is not modelled (D-48). The default image (`IDAT`)
  is handled as a still PNG; all `acTL`, `fcTL`, and `fdAT` chunks form one opaque unit.

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Image raster (still PNG, or APNG default image when not a frame) | singleton | `/pixels#rect=x,y,w,h` | GridMerge |
| Image shape | singleton | `/shape` (width, height) | OpaqueLeaf (guards the grid) |
| Encoding attributes | field name | `/encoding/color_type`, `/encoding/bit_depth`, `/encoding/interlace` | Keyed (non-conflicting, §6.4) |
| Palette (indexed mode only, §3.2) | entry index | `/palette/<i>` (RGBA incl. tRNS alpha) | KeyedMerge |
| Color interpretation | chunk type | `/color/cICP`, `/color/iCCP`, `/color/sRGB`, `/color/gAMA`, `/color/cHRM`, `/color/sBIT`, `/color/mDCV`, `/color/cLLI` | KeyedMerge, values OpaqueLeaf |
| Text | type + keyword (+ lang for iTXt) | `/chunks/tEXt:Author`, `/chunks/iTXt:Title@de` | KeyedMerge, value = text |
| Suggested palette | name | `/chunks/sPLT:<name>` | KeyedMerge, OpaqueLeaf |
| Other singleton ancillaries | chunk type | `/chunks/pHYs`, `/chunks/bKGD`, `/chunks/tIME`, `/chunks/eXIf` | KeyedMerge, OpaqueLeaf |
| Unknown / private chunks | type + occurrence index within type | `/chunks/prVt:0` | KeyedMerge, OpaqueLeaf |
| APNG animation chunks (`acTL`, `fcTL`, `fdAT`) | singleton | `/animation` | OpaqueLeaf (D-48) |

Keywords in Addresses are escaped per RFC 6901 (`~0`, `~1`) and Latin-1 keywords are converted to UTF-8.
Duplicate `tEXt` keywords (legal) are disambiguated by occurrence index: `/chunks/tEXt:Comment:1`.

`eXIf` is OpaqueLeaf in the MVP; v2 recurses through the shared TIFF/Exif IFD parser from raster-misc
(`/chunks/eXIf/ifd0/0x010F`).

### 3.1 Canonical pixel model

Every input is decoded to a canonical grid so that encoding choices (palette vs truecolor, 8 vs 16 bit,
interlace, filters) never show up as pixel changes:

- **Cell = RGBA, straight alpha, 16 bits per channel.** Sub-8-bit and 8-bit samples are scaled by
  **bit replication** (`v8 -> v8 * 257`, `v4 -> v4 * 0x1111`, `v2 -> v2 * 0x5555`, `v1 -> v1 * 0xFFFF`). Replication is exact
  and invertible, so the reverse mapping in §7 is lossless and representability is a simple test.
- Gray expands to R=G=B. Missing alpha is 0xFFFF. `tRNS` key color: a pixel whose *stored* sample equals the
  key gets alpha 0, color kept. Indexed pixels expand through `PLTE` + `tRNS`.
- **Storage width is adaptive**: if all three inputs have bit depth ≤ 8 the grid is held as RGBA8 in memory
  (the 16-bit view is implicit); otherwise RGBA16. This halves memory for the common case.
- **No gamma, ICC, or cICP transform is applied** to stored values. Merge compares stored values. Color
  interpretation is merged as metadata (`/color/*`) and pixels declare `depends_on: ["/color"]`.
  lcms2 is used only for ΔE metrics (§5), never for the merge path.
- Color under zero alpha is significant by default (it is stored data and matters for premultiplying
  pipelines and bleeding). `--alpha-zero-equal` normalizes all alpha-0 cells to (0,0,0,0) for comparison
  only, reported in `options`.

### 3.2 Indexed mode

When base, ours and theirs are **all** color type 3, the grid holds **palette indices** (u8 cells) and the
palette becomes a KeyedMerge over entry index. This matches how indexed art is edited: recoloring palette
entry 12 is one keyed change, not 40 000 pixel changes, and painting with an existing index merges cleanly
against a palette tweak on the other side. Equality of two index cells is index equality. If any input is
not indexed, all three use RGBA mode. `--png-mode=rgba|index|auto` overrides (default `auto`).

### 3.3 Dependencies, invariants, derived data

- Pixels depend on `/color` (interpretation) and, in indexed mode, on `/palette/<i>` for every index used.
- `hIST` is **derived** from palette usage: never merged, recomputed from the merged raster when it was
  present in the chosen output, warning `derived-regenerated`.
- `bKGD` in indexed mode is a palette index: remapped if the palette is remapped (§6.3).
- `tIME` is "last modified" metadata. Splinter never sets it (determinism); it merges as an ordinary key.
- APNG animation chunks are copied verbatim from one side, so their sequence numbers stay valid. When the default
  image is also frame 0 (an `fcTL` precedes `IDAT`), that frame shows the merged default image.

### 3.4 Shared raster core

Everything that is not PNG-specific (canonical grid container, three-way cell classification, region
labeling, guard band, tolerance, heatmap renderer, tile-hash dump, region patch payload) lives in
`src/formats/raster/` as the static helper module `semantic-splinter-raster`. It is written as part of the
PNG work but kept free of PNG types so that JPEG and raster-misc reuse it. It has no
external dependencies.

## 4. Normalization

Insignificant (never reported as pixel changes):

- Filter bytes, zlib stream layout and compression level, `IDAT` chunk splitting, Adam7 vs non-interlaced.
- Color type and bit depth, *as long as the canonical values are identical* (e.g. RGB8 that only uses 200
  colors vs the same image as indexed). The encoding attributes are still reported as `kind: "encoding"`
  changes at `/encoding/*` so the user sees "re-saved as indexed".
- Palette order in RGBA mode (indices are not semantic there).
- `zTXt` vs `tEXt` vs `iTXt` for the same keyword and value: reported as `kind: "encoding"`, not a text change.
- Chunk CRC is verified, not compared.

Never normalized: stored sample values (no gamma or profile math), colors under alpha 0 (unless
`--alpha-zero-equal`), the presence or value of any ancillary chunk, palette order in indexed mode,
the bytes of APNG animation chunks.

Options: `--png-mode`, `--alpha-zero-equal`, `--tolerance=N` (per channel, in 8-bit units, internally
`N*257` on the 16-bit scale; not available in indexed mode), `--ignore-chunks=tIME,tEXt:Software` (diff
only; merge still carries them).

## 5. Diff

Detected change kinds: `pixels`, `shape`, `encoding`, `palette`, `color`, `text`, `metadata` (other
chunks), `animation` (APNG animation chunks changed).

**Pixel diff algorithm** (two-way, also the per-side half of a merge):

1. Decode both to the canonical grid (same storage width).
2. Row-by-row compare (memcmp fast path on whole rows, then per cell), emitting changed cells as horizontal
   **runs** `(y, x0, x1)`. Memory is O(runs), not O(pixels).
3. Connected-component labeling on runs with 8-connectivity: a run joins any run in the previous row
   whose `[x0-1, x1+1]` interval overlaps; union-find with path compression and union by rank. Labels are
   canonicalized afterward by (min y, min x) so output is deterministic.
4. Components closer than `--gap=G` (Chebyshev distance, default 0) are merged: bucket components by
   bounding box into a coarse spatial grid (cell size max(64, G)), test bbox distance first, then run-to-run distance.
5. One `Change{op: update, at: /pixels#rect=bbox, kind: "pixels"}` per region, with `metrics`:
   `pixels` (changed count), `bbox`, `max_delta` per channel, `mean_delta`, `alpha_changed` count,
   `delta_e00_max`/`mean` (computed in sRGB, or via lcms2 through the embedded ICC/cICP when present and
   the `png-icc` feature is on), and `before`/`after` region hashes.

Shape change (width or height differs) emits one `update /shape` change and no per-pixel diff, unless
Pure crop or canvas extension (v1, Q-PNG-3) is detected: the change is reported as `update /shape` with the offset, plus
the pixel regions of the overlap. `--detect-offset` (v2) extends this to general integer translation in diff.

Metadata: KeyedMerge-style key diff over `/color/*`, `/chunks/*`, `/palette/*`, with text values shown
inline (truncated at 200 chars) and binary values by hash and size (`iCCP` also shows the profile
description tag, read without lcms2 from the `desc`/`mluc` tag).

APNG: the animation unit is compared by hash and reported as one `animation` change.

**dump() (textconv)**, stable and line-oriented:

```text
png width=640 height=480 encoding=rgba8 interlace=none
color sRGB intent=perceptual
color gAMA 45455
chunk pHYs 2835x2835 per-metre
chunk tEXt:Author "Alice"
chunk iCCP name="Display P3" bytes=536 xxh3=5c0e…
pixels xxh3=91aa… alpha=varies colors=1873
tile 0,0,64,64 xxh3=17f2…
tile 64,0,64,64 xxh3=08bd…
```

Tile size starts at 64 and doubles until the image has ≤ 4096 tiles, so a 16k×16k image produces ≤ 4096
lines. Tile hashes are over canonical cells, so a textconv diff localizes edits without depending on the
encoder. APNG adds one `animation frames=N xxh3=…` line.

**Visual (`--visual`)**, rendered by the raster core and written as an RGBA8 PNG:

- Diff mode: base converted to luminance at 35% opacity over a checkerboard (for alpha), changed
  cells colored by a perceptually ordered ramp of max channel delta (log scale), region bboxes outlined in
  1-pixel strokes. `--visual=side-by-side` emits [a | b | heatmap] in one image.
- Merge mode: ours-only cells blue, theirs-only cells orange, identical edits green, conflict cells magenta,
  guard-band conflict groups outlined in yellow. Colors are fixed constants (deterministic) and chosen to
  remain distinguishable for common color-vision deficiencies.

**Cost**: decode dominates. Two passes over rows; memory = canonical grids (≤ 4 GiB enforced by
`--max-memory`; 2^30 pixels × 8 bytes would exceed it, so 16-bit images near the pixel limit are
rejected with exit code `limit` in the MVP; streaming in v2, §12). A 4096×4096 RGBA8 image diff should
complete in well under a second.

## 6. Merge

### 6.1 Pixel grid (GridMerge)

Precondition: base, ours and theirs have equal shape. Otherwise:

- Only one side changed shape and the other side has **no** pixel changes: that side's raster wins wholesale.
- Only one side changed shape but the other side changed pixels: **conflict `shape-mismatch`**. SPEC §5.1 adopted
  this rule (DECISIONS D-32), because silently discarding the other side's
  paint would violate "no silent best-effort". **Exception (Q-PNG-3, decided):** a pure crop or canvas extension (the
  reshaped side equals base at one integer offset over their overlap) is detected in v1, and the other side's regions are
  translated and merged. Regions that fall outside the new extent conflict (`cropped-away`).
- Both changed shape: conflict `shape-mismatch`.

Per cell, classify against base: `U` unchanged, `O` ours-only, `T` theirs-only, `I` both identical,
`X` both different. With `--tolerance`, a side whose delta is within ε counts as unchanged for that cell,
and the cell takes the other side's value (or ours if both are within ε); suppressed cells are counted
in `stats.format.tolerance_suppressed`.

Region and conflict grouping:

1. Label components separately for the masks `O`, `T`, and `X` (8-connectivity, `--gap` applied).
   `I` cells are agreed and never seed conflicts.
2. Build a union-find over components. Union every `X` component with every `O` or `T` component it
   touches. Union every `O` component with every `T` component within Chebyshev distance ≤ `--guard`
   (default 0, D-10). Distance tests use bbox prefilter, then run-interval distance. `O`/`T` pairs within
   distance 1 that were not unioned emit warning `adjacent-edits` with both bboxes.
3. Each set containing an `X` component or both an `O` and a `T` component is a **conflict group**:
   `Conflict{at: /pixels#rect=<bbox of group>, reason: "both-modified" if it contains X else "guard-band"}`.
   `changes` lists the ours and theirs region change IDs involved. `choices`: `ours`, `theirs`, `base`.
4. All other components auto-merge (`rule: independent`). `I` regions are reported as `rule: identical`.

Resolution of a conflict group applies to every cell of the group atomically: `ours` writes ours' values
for all cells in the group, including cells that only theirs changed (which therefore revert to base).
This keeps coupled edits (anti-aliasing, shadows) coherent. Never average, never per-cell mix inside a group.

### 6.2 Metadata (KeyedMerge)

`/color/*`, `/chunks/*`, `/animation/*` follow SPEC §5.1 KeyedMerge rules. Text values are OpaqueLeaf
by default; `--png-text=lines` (v2) recurses into the text module's SequenceMerge for long multi-line
`iTXt` (e.g. embedded XMP or JSON is better merged by the xml/json module, which is what v2 does for
`iTXt:XML:com.adobe.xmp`).

**Color dependency upgrade:** if one side changed any `/color/*` key and the other side changed pixels,
the pixel changes get **conflict `dependency`** (not merely `dependency-touched`). Rationale: a profile
swap reinterprets every stored value, so pixels painted under the old interpretation would display wrong.
`--png-color-dependency=warn` downgrades it to the SPEC default.

### 6.3 Palette (indexed mode)

- Entries are KeyedMerge by index; value = RGBA (PLTE + tRNS alpha).
- **Append remap**: if both sides appended entries beyond base's palette length and the appended sets differ,
  theirs' appended entries are renumbered to follow ours' (and theirs' pixel indices remapped through the
  same table) when the result fits in 2^bit_depth entries, else 256 with bit depth promotion. Reported as
  `auto: palette-remap`. This is not a conflict because new indices have no prior meaning.
- Same index modified differently on both sides: conflict `both-modified` at `/palette/<i>`; pixels using
  index `i` that either side changed join that conflict via `depends_on`.
- Entry deleted (palette shortened) while the other side uses that index in a changed cell: conflict `dependency`.
- Unused trailing entries are preserved (some pipelines rely on fixed palette size).

### 6.4 Encoding attributes

`/encoding/*` never conflicts. Rule: if only one side changed an attribute, take it; if both changed
differently, take ours and report `auto: encoding-ours`. The chosen encoding is then checked for
representability (§7.3) and promoted if needed.

### 6.5 APNG animation (D-48)

- The default image merges as `/pixels` like any still PNG.
- `/animation` is an OpaqueLeaf: if one side changed it, that side's chunks are taken whole; if both sides changed
  it differently, conflict `unsupported` and the policy side's chunks are written.

### 6.6 `--conflict=mark`

There is no in-band conflict marker for raster data that keeps the file truthful, so `mark` falls back to
`ours` with warning `mark-unsupported`, **and** additionally writes the merge visual
`<output>.splinter-conflicts.png` next to the conflict sidecar. Drawing outlines into the output image
was rejected because it invents pixels neither side made.

### 6.7 Patch payloads

For each `/pixels#rect=` hunk: a binary record, then compressed by the envelope (zstd or zlib):

```text
u32 magic 'SPPX', u8 version=1, u8 mode (0 rgba16, 1 rgba8, 2 index8)
u32 x, y, w, h
mask: w*h bits, row-major, LSB-first, rows padded to bytes
before: samples for masked cells only, row-major
after:  samples for masked cells only, row-major
```

Applying: `merge(base = before-view, ours = target, theirs = after-view)` restricted to the rect. With
fuzz F > 0, if the before-cells do not match at (x,y), the applier searches offsets within ±F pixels
(raster order, first exact match wins, deterministic) and reports the offset. A mismatch is a reject
(= conflict). Metadata hunks use the generic text/opaque payloads.

## 7. Serialization and preservation

### 7.1 Envelope fields

**Layer stack:** `file → signature → chunks (length, type, data, CRC-32) → IDAT concatenation →
zlib (CMF/FLG, Adler-32) → deflate → [Adam7 passes] → per-row filter byte + filtered row → samples → canonical grid`.
`zTXt`, `iCCP` and compressed `iTXt` carry their own `zlib → deflate` layer under the chunk. `eXIf` holds a TIFF
structure with internal offsets (OpaqueLeaf in the MVP, so those offsets are never rewritten).

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| Signature `89 50 4E 47 0D 0A 1A 0A` | framing (constant) | exact | `bytes` |
| Chunk `length` (u32 BE, counts data only, ≤ 2³¹−1) | framing | inside the file, ≤ 2³¹−1, `IHDR` = 13 | `field(u32, BE, size_of(data))` |
| Chunk CRC (u32 BE, CRC-32 ISO 3309, reflected poly `0xEDB88320`, init and xorout `0xFFFFFFFF`, over **type + data**, not length) | integrity | every chunk; `ok`/`mismatch` per chunk | `field(u32, BE, crc32(group(type, data)))`. Raw-copied chunks keep their stored CRC (wrong ones too, warned) unless `--integrity=repair` |
| IDAT chunk split | framing | IDATs consecutive; concatenation is one zlib stream | re-chunked at the source's dominant IDAT data size (libpng writes 8192 by default; fallback 64 KiB) |
| zlib header CMF/FLG (CM = 8, CINFO = log2(window) − 8, FLEVEL 0..3, FDICT = 0, FCHECK) | encoding | `(CMF·256 + FLG) mod 31 = 0`, CM = 8, FDICT = 0 | written by the encoder; CINFO and FLEVEL feed parameter inference |
| Adler-32 (u32 BE, over the **filtered** bytes including each row's filter-type byte) | integrity | after inflate | `adler32` inside `encode(zlib, …)` |
| Deflate data | encoding | bounded inflate (§11 limits) | `raw` if the canonical raster and encoding attributes are unchanged, else `encode(zlib, params)` |
| Row filter-type byte (0..4) and filtered bytes | encoding | filter ≤ 4 | reused per row when the row and the row above are unchanged, else adaptive |
| `zTXt`/`iCCP` compression method byte (0) and zlib stream; `iTXt` compression flag and method | encoding | method = 0, inflate ≤ 64 MiB | `raw` when the value is unchanged; else `encode(zlib)` with parameters inferred from that chunk's own stream |
| APNG `acTL`, `fcTL`, `fdAT` (sequence numbers, frame count) | framing | sequence numbers strictly +1, `acTL` count matches | copied verbatim with their CRCs as one unit (D-48); never renumbered in v1 |
| `hIST` | derived | entry count = `PLTE` count | recomputed from the merged raster (§3.3) |
| `tIME` | volatile | date fields in range | preserved from ours; never set by splinter (Q-PNG-11) |
| `caBX` (C2PA manifest store, JUMBF) | authenticity | COSE signature over the claim, plus the claim's `c2pa.hash.data` hard binding (SHA-256 over the file minus the exclusion range around `caBX`) | `--signatures` policy, below |
| `dSIG` (PNG extension, rarely seen) | authenticity | `unverifiable` in v1 | `--signatures` policy, below |

**Parameter inference** for a touched IDAT stream (SPEC §12.2): read CINFO (window) and FLEVEL from ours' zlib
header, then trial-compress the first 64 KiB of ours' original filtered data with candidate (level, memLevel,
strategy) settings inside that FLEVEL class (FLEVEL 0 → level 0..1, 1 → 2..5, 2 → 6, 3 → 7..9) and keep the first
candidate whose output reproduces ours' compressed prefix. If none matches (other deflate implementations such as
zopfli, libdeflate or Photoshop's encoder), use the fallback in §7.3 and report `encoding-reencoded`.

**Raw copy and layout stability.** Passthrough of a whole input (§7.2). Ancillary chunks whose value is
unchanged are copied with their length, type, data and CRC. The IDAT payload is copied whole when the raster is
unchanged even if ancillary chunks changed. APNG animation chunks are copied verbatim from the chosen side. Chunk order follows ours, and the IDAT chunk size is kept, so
only the IDAT chunks from the first changed one onward differ.

**Signatures under `--signatures=auto`.** `caBX`: **keep** the manifest bytes verbatim and warn
`signature-invalidated` (same default as JPEG C2PA in ENVELOPES.md; validators will show the hard binding as
broken, which is the truthful state). The keyed boundary is the COSE_Sign1 claim signature inside the JUMBF; the
hard-binding hash is an unkeyed digest but lives *inside* the signed claim, so splinter cannot update it without
re-signing. Re-signing is possible only with `--sign-command` (for example a C2PA tool); splinter itself does not
implement C2PA signing. `caBX` has an uppercase fourth letter (not safe-to-copy), but as a known chunk it is
governed by this policy, not by the `unsafe-chunk-dropped` rule of §7.4 (Q-PNG-12). `dSIG`: **strip** with
`signature-stripped` (no PNG consumer rejects an unsigned file). The exact C2PA exclusion range for PNG (whether
the chunk's length, type and CRC are inside it) is *(verify)* against the C2PA specification.

**Expected churn.** One pixel painted in a 512×512 RGBA8 PNG of about 300 KiB: `content` = 1 pixel. If inference
reproduced ours' encoder, the deflate output is identical up to the start of the deflate block containing the
edited row and differs from there to the end of the stream, typically tens to hundreds of KiB of `encoding`; if
inference failed, the whole stream is `encoding`. `integrity`: the Adler-32 plus 4 bytes per IDAT chunk from the
first changed one. `framing`: the length of the last IDAT chunk when the compressed size changed. A `tEXt` edit
on one side only: that chunk's bytes (`content`), its length (`framing`) and CRC (`integrity`); everything else is
byte-identical.

### 7.2 Passthrough first

1. If the merged document (pixels, palette, all keys, animation) is equal to one input's document **and**
   the chosen encoding equals that input's encoding, emit that input's bytes verbatim. Order of
   preference: ours, theirs, base. This covers "only theirs changed" and "metadata-only on one side when
   the other side is untouched" without re-encoding.
2. Otherwise, assemble a new file from ours' chunk layout:
   - Unchanged ancillary chunks (same value as in ours) are re-emitted from ours' **original bytes**,
     including the original zlib stream inside `zTXt`/`iCCP`/`iTXt`.
   - Chunks taken from theirs are emitted from theirs' original bytes.
   - Theirs-only chunks are placed per SPEC KeyedMerge order rule, constrained to the legal **position class**
     (pre-PLTE, PLTE..IDAT, pre-IDAT, post-IDAT); if the nearest anchor is in a different class, the chunk goes
     to the start of its class.
   - `IHDR`, `PLTE`, `tRNS`, `IDAT`, `hIST` are regenerated. APNG animation chunks are copied verbatim.
   - If the merged raster equals ours' raster exactly and the encoding is unchanged, ours' `IDAT`
     payload bytes are reused (only chunk boundaries may change).

### 7.3 Re-encode choices

Target encoding = merged `/encoding/*` (normally ours'). Before writing, test **representability** of the
merged canonical grid and promote minimally if needed, warning `encoding-promoted`:

| Target | Requirement on merged cells | Promotion if violated |
|---|---|---|
| bit depth d < 16 | every sample is a d-bit replication pattern | next legal depth |
| gray (0/4) | R = G = B | RGB/RGBA same depth |
| no alpha (0/2) | alpha = max, or a single key color for tRNS, never used opaque | add alpha (4/6) or emit tRNS |
| indexed (3), RGBA mode | ≤ 2^d distinct RGBA values | grow d up to 8; > 256 colors → RGB(A)8 |

Palette construction in RGBA mode when the target is indexed: start from ours' palette in ours' order,
drop nothing, append new colors in order of first occurrence in raster order (ours' pixels first, then
the merged result), and keep `tRNS` minimal (trailing opaque entries omitted). In indexed mode the merged palette is used as-is.

Encoder settings are inferred from the side the output raster is built from (normally ours; SPEC §12.2,
details in §7.1): interlace as target; per-row filter types reused for rows whose filter input is unchanged,
other rows chosen by the minimum sum of absolute values heuristic (libpng's adaptive filter), forced `None`
for indexed and < 8-bit; zlib FLEVEL, windowBits and memLevel/strategy inferred by trial compression; IDAT
chunk size copied from the source. New images with no source stream use the fallback: zlib level 9,
windowBits 15, memLevel 9, default strategy, one `IDAT` per 64 KiB of compressed data. Determinism across
platforms relies on the pinned vcpkg `zlib` port (zlib-ng or a different zlib version can produce different
but equally valid streams, see Q-PNG-4).

### 7.4 Loss and warnings

- Unknown chunks with safe-to-copy = 0 (uppercase byte 3): PNG rules say an editor that changed critical
  data must drop them. If the merged raster differs from the input the chunk came from, the chunk is dropped
  with warning `unsafe-chunk-dropped`. Otherwise it is kept.
- `hIST` regenerated (`derived-regenerated`). `sBIT` kept as-is but warned (`sbit-mismatch`) if the
  merged data uses more significant bits than declared.
- `eXIf` thumbnails or orientation are not touched (MVP treats `eXIf` as opaque).
- Encoding promotion (`encoding-promoted`).

## 8. Validation

- Re-load the written bytes through the module (CRCs, chunk ordering rules, IHDR legality) and compare the decoded canonical
  document with the merged in-memory document: **must be identical**, else internal error and the target is untouched.
- In debug and CI builds, also decode via libpng (independent decoder) and compare canonical pixels.
- Optional CI validator: `pngcheck -v` (GPL-2.0, external tool only, never linked).

## 9. Libraries

| Library | vcpkg port | Version seen | License | Notes |
|---|---|---|---|---|
| zlib | `zlib` | 1.3.2 | Zlib | **Verified.** Inflate/deflate for IDAT, zTXt, iCCP, iTXt. Already needed by core patches (SPEC §6). |
| libpng | `libpng` | 1.6.59 | libpng-2.0 | **Verified.** Mainline 1.6.45+ supports `cICP`/`cLLI`/`mDCV` and exposes `acTL` etc. only as unknown chunks (verified in `png.h`). OSS-Fuzz covered. |
| libspng | `libspng` | 0.7.4 | BSD-2-Clause AND libpng-2.0 | **Verified.** Simpler API. |
| lodepng | `lodepng` | port dated 2021-12-04 | Zlib (assumed, upstream) | **Verified port exists but is stale.** Not recommended. |
| stb_image | `stb` | 2026-08-02 | MIT / public domain (upstream) | Loses chunks, not hardened for untrusted input. Tests only. |
| Wuffs | not in vcpkg (verified) | n/a | Apache-2.0 (assumed) | Memory-safe decoder; vendoring possible later. |
| lcms2 | `lcms` | 2.19.1 | MIT | **Verified.** Features `fastfloat` and `threaded` are **GPL-3.0-or-later** plugins: never enable them. Only for ΔE via ICC (`png-icc` sub-feature). |

**Recommendation:** own chunk layer (parse, CRC, position classes, byte-exact re-emit), about 600 lines, plus
**libpng for pixel decode/encode**, so Adam7, bit unpacking and filter handling stay in a fuzzed library. Encoding uses our own scanline filter step (five filters, about 80 lines) plus zlib
`deflate` directly, because per-row filter reuse and inferred zlib parameters (§7.1) need control that
libpng's `png_set_filter`/`png_set_compression_*` do not give; libpng decodes the result in validation.
Rejected: writing our own unfilter/Adam7 (small, but libpng is already hardened), libspng (no gain over
libpng, extra dependency).

## 10. Hard cases and explicit non-goals

- **Re-quantization**: one side saved 16-bit as 8-bit. Every cell changes by a rounding delta. Reported as
  one region covering the image with `metrics.requantized: true` (detected when every changed cell equals
  the base value rounded to the new depth). Merging it with any pixel edit on the other side conflicts.
- **Re-save with a different color profile but converted pixels**: all cells change plus `/color/*`.
  Conflicts with any pixel edit on the other side; this is correct.
- **Logical coupling beyond the guard band** (drop shadows 8 px away, global color grading on one side,
  content-aware fill): not detectable. Users raise `--guard`. Global adjustments are one huge region and conflict with everything, which is correct.
- **Premultiplied data stored in PNG by broken exporters**: treated as stored values; no detection.
- **Translations, rotations, scaling**: non-goal for merge. Pure crop/extension merges in v1 (Q-PNG-3); `--detect-offset` (v2) adds general integer translation in diff.
- **Huge images** beyond `--max-memory`: rejected in MVP (exit `limit`), streaming in v2.
- Non-goals: perceptual or object-aware merges, averaging, CgBI (Apple-optimized PNG with premultiplied BGRA
  and raw deflate) beyond rejecting it with a clear error, MNG/JNG.

## 11. Test plan

- **Generated fixtures** (C++ test helper writing PNGs through the module's own encoder plus libpng):
  every color type × bit depth × interlace × tRNS variant, checked for canonical equivalence (same image
  in 12 encodings must diff as zero pixel changes, only `encoding` changes).
- **Golden reports**: disjoint edits, adjacent edits (default: merge plus `adjacent-edits` warning; with `--guard=1`, conflict at distance 1, not at 2),
  identical edits, X conflict, palette recolor + paint, palette append on both sides, metadata-only,
  `iCCP` swap + paint (dependency conflict), shape change on one side, APNG animation changed on one side and on both sides.
  Goldens are JSON reports plus output canonical hashes, never output bytes alone.
- **Property tests** (Catch2 generators): merge(b, b, x) == x and merge(b, x, b) == x byte-identically via passthrough;
  merge(b, x, x) == x; merge symmetry up to policy (swapping ours/theirs swaps conflict sides and keeps
  conflict rects); diff(a, b) applied as patch to a reproduces b canonically; determinism (run twice, compare bytes).
- **Round trip**: load + save of unchanged documents is byte-identical (passthrough) and, with
  `--force-reencode`, canonically identical.
- **Corpus**: PngSuite (Willem van Schaik, freely redistributable) for decoder coverage.
- **Fuzz**: libFuzzer targets `png_load`, `png_diff_pair` (two inputs split from one buffer), and
  `png_merge3`, with IHDR size limits lowered to keep runs fast; seeds from PngSuite.
- **Limits**: decompression bomb (tiny IDAT inflating to 2^31 bytes) must fail before allocation;
  `zTXt`/`iCCP` inflation capped at 64 MiB per chunk; IHDR 2^31-1 dims rejected by `--max-pixels`.

## 12. Phases

| Phase | Scope | Effort |
|---|---|---|
| MVP | Chunk layer, libpng decode/encode, canonical RGBA grid, raster core (classification, runs, CCL, guard band, tolerance), still-PNG diff/merge, APNG default image with opaque animation (D-48), pure crop/extension detection (Q-PNG-3), KeyedMerge metadata, passthrough, representability + promotion, dump, heatmap visual, JSON report, Git driver end-to-end | M (3 wk) |
| v1.1 | Indexed mode + palette merge/remap, patch payloads + fuzzed apply, color-dependency upgrade, ΔE via lcms2 | S |
| v2 | Streaming two-pass merge for huge non-interlaced images, `--detect-offset` (general translation), eXIf via shared IFD parser, XMP in iTXt via xml module | M |
| Later | Wuffs as alternate decoder, object-aware heuristic merge (opt-in) | L |

## 13. Open questions

- **Q-PNG-1**: Should the canonical model be RGBA16 always, or adapt storage width? **Provisional:** adapt (RGBA8 when all inputs ≤ 8-bit), semantics are always 16-bit. **Why:** halves memory on the common case at no fidelity cost.
- **Q-PNG-2**: Default merge domain for all-indexed inputs: palette index or RGBA? **Provisional:** index (`--png-mode=auto`). **Why:** palette recolors become one keyed change and merge cleanly with paint.
- **Q-PNG-3**: One-sided shape change while the other side painted: wholesale win or conflict? **Provisional:** conflict `shape-mismatch`. **Why:** the SPEC default would silently drop the other side's pixels. **Decided 2026-10-08:** conflict, except that content edits against a pure crop or uncrop merge (§6.1).
- **Q-PNG-4**: Should byte-identical output be guaranteed across platforms and builds, given zlib implementation differences? **Provisional:** guaranteed per pinned vcpkg baseline with the `zlib` port only; document zlib-ng overlay as breaking cross-build byte identity. **Why:** canonical identity is guaranteed regardless; byte identity needs a fixed deflate.
- **Q-PNG-5**: Do color-interpretation changes against pixel changes conflict? **Provisional:** yes, `dependency`, with `--png-color-dependency=warn` to downgrade. **Why:** a profile swap reinterprets every stored value.
- **Q-PNG-7**: What does `--conflict=mark` produce? **Provisional:** ours plus a sidecar overlay PNG, warning `mark-unsupported`. **Why:** any in-image marking invents pixels.
- **Q-PNG-8**: Default guard band for PNG? **Provisional:** 1 (SPEC D-10). **Decided 2026-10-08:** 0, overridable with `--guard=N`; touching edits warn `adjacent-edits` (D-10). **Why:** catches anti-aliased edges touching; larger values cause too many false conflicts on sprite sheets.
- **Q-PNG-9**: Should colors under alpha 0 be compared? **Provisional:** yes by default, `--alpha-zero-equal` to ignore. **Why:** they are stored data used by texture bleeding and premultiplication pipelines.
- **Q-PNG-10**: Own decoder or libpng? **Provisional:** libpng for pixels, own chunk layer. **Why:** byte-exact chunk control without re-implementing a hardened inflate/unfilter path.
- **Q-PNG-11**: Should `tIME` be updated on merge? **Provisional:** no, merged as an ordinary key. **Why:** determinism invariant forbids wall-clock data.
- **Q-PNG-12**: Under `--signatures=auto`, keep or strip a C2PA `caBX` manifest after a content change? **Provisional:** keep it verbatim and warn `signature-invalidated`, overriding the not-safe-to-copy drop rule; `strip` removes it. **Why:** matches the JPEG C2PA default, keeps provenance history visible, and validators then report the edit truthfully instead of the file silently losing its credentials.
