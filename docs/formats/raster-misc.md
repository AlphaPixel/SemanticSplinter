# Miscellaneous Raster Formats — Implementation Document

**Tier:** T2 (TIFF, GIF, WebP, BMP, TGA, QOI) · **Modules:** `semantic-splinter-tiff`, `-bmp`, `-tga`, `-gif`, `-webp`, `-qoi` · **vcpkg features:** same names · **Depends on modules:** `semantic-splinter-raster` (helper, png.md §3.4); `xml` for XMP (v2) · **Status:** §12.1
**Extensions / magic:**

| Format | Extensions | Magic |
|---|---|---|
| TIFF / BigTIFF | `.tif`, `.tiff` (also DNG read-only, see §10) | `II*\0`, `MM\0*`, `II+\0`, `MM\0+` |
| BMP | `.bmp`, `.dib` | `BM` + plausible header size (12, 40, 52, 56, 64, 108, 124) |
| TGA | `.tga`, `.icb`, `.vda`, `.vst` | no magic; header plausibility, TGA 2.0 footer `TRUEVISION-XFILE.\0`. Probe ≤ 40 without footer |
| GIF | `.gif` | `GIF87a`, `GIF89a` |
| WebP | `.webp` | `RIFF....WEBP` |
| QOI | `.qoi` | `qoif` |

## 1. Why it matters

These formats show up in the same repos as PNG: TIFF in print, scientific, GIS and scanning workflows;
BMP and TGA in game pipelines and legacy tools; GIF for UI animations and docs; WebP for web
assets; QOI in small engines. Each gets the PNG experience (region diff, heatmap,
three-way merge of disjoint edits) **where it can be done losslessly**, and an honest diff-only mode where it cannot.

## 2. Format anatomy and the shared raster core

All formats load into the raster core's canonical structures:

```cpp
struct Raster {                       // one image plane set
    uint32_t width, height;
    PixelLayout layout;               // gray, gray+a, rgb, rgba, cmyk(+a), index8, n-channel
    SampleType type;                  // u8, u16, u32, s8/s16/s32, f16, f32, f64
    AlphaMode alpha;                  // none, straight, premultiplied (stored as-is, never converted)
    std::vector<std::byte> cells;     // row-major, tightly packed, native endian
};
```

Unlike PNG, the canonical form here keeps the **stored sample type** (TIFF float and 32-bit data must
not be squashed into RGBA16). Cell equality is bitwise on the stored samples; tolerance is integer delta or
ULPs for floats (ULP comparison lives in the raster core). Premultiplied data stays premultiplied. Palette images
use indexed mode under the same rule as PNG (all three indexed → index grid + KeyedMerge palette).

The core supplies classification, CCL, guard band, conflict groups, visual heatmap, tile-hash dump and
the `SPPX` region payload. Each format module supplies: segment parsing, decode to `Raster`, encode,
byte-exact passthrough, and its keyed metadata.

Animated and multi-image formats (GIF, animated WebP, multi-page TIFF) map to a common **frame sequence** model:
SequenceMerge of frames, identity = hash(frame control + frame raster), matched frames recurse into
control-field KeyedMerge and frame GridMerge, composited-canvas diff for reports only.

**Lossless merge granularity** is the central per-format question:

| Format | Pixel merge without generational loss? | Coded-domain splice | Re-encode cost |
|---|---|---|---|
| TIFF uncompressed / LZW / Deflate / PackBits / ZSTD / LZMA | yes | strip/tile byte copy | lossless |
| TIFF with JPEG / WebP-lossy / LERC-lossy tiles | no (pixel) | whole tile copy | lossy |
| BMP, TGA, QOI | yes | n/a (cheap re-encode) | lossless |
| GIF | yes (index domain, same palette) | whole frame copy | lossless LZW |
| WebP lossless (VP8L) | yes | whole frame copy | lossless |
| WebP lossy (VP8) | no | whole frame copy (animated only) | lossy |

## 3. Semantic units and identity

Common units for all modules:

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Image raster | page/frame/item + singleton | `/pixels#rect=`, `/pages/<i>/pixels#rect=`, `/frames/<i>/pixels#rect=` | GridMerge |
| Palette (indexed mode) | entry index | `/palette/<i>`, `/frames/<i>/palette/<j>` (GIF local tables) | KeyedMerge |
| Frame / page | position, matched by content hash | `/frames/<i>`, `/pages/<i>` | SequenceMerge |
| Metadata | tag/chunk key | per format below | KeyedMerge, OpaqueLeaf values |
| ICC profile | singleton | `/icc` | OpaqueLeaf; pixels depend on it (conflict `dependency`, as PNG Q-PNG-5) |
| XMP | singleton | `/xmp` | OpaqueLeaf → xml module (v2) |
| Exif | IFD + tag | `/exif/<ifd>/<tag>` | OpaqueLeaf (MVP) → KeyedMerge via shared IFD code (v2) |

### 3.1 TIFF

- File = header + chain of IFDs (pages). Each IFD: tags (ID, type, count, value/offset), SubIFDs (tag 330),
  strips or tiles with offset/bytecount arrays. BigTIFF uses 64-bit offsets.
- Pages: `/pages/<i>`, identity by content hash; `PageNumber` (297) is used as an identity hint when present.
  `NewSubfileType` bit 0 (reduced-resolution) marks **derived** pages and SubIFD pyramids (previews, COG
  overviews): never merged; regenerated (v2) or stale.
- Tags: KeyedMerge by numeric tag ID `/pages/0/tags/0x0131` (Software), with **structural tags excluded**
  from user-level merge (ImageWidth, BitsPerSample, Compression, Photometric, StripOffsets,
  TileOffsets, ByteCounts, RowsPerStrip, TileWidth/Length, PlanarConfiguration, Predictor, SampleFormat,
  ExtraSamples, JPEGTables): they are encoding or shape and are regenerated on write.
- Private IFDs: Exif (34665), GPS (34853), Interop: keyed as `/pages/0/exif/<tag>`. GeoTIFF keys (34735
  directory + 34736/34737) are parsed into `/pages/0/geo/<KeyID>` so CRS edits merge as keys, not blobs.
- Pixel units: GridMerge over the page raster. **Storage cell** = strip or tile, used for passthrough.
- **Danger tags**: Photoshop `ImageSourceData` (37724) and `Photoshop` IRB (34377) hold layer data that
  the flat raster is derived from (or vice versa). If pixels change while 37724 is kept, Photoshop shows the
  old layers. Rule: after a pixel merge, 37724 is dropped with warning `layers-dropped`, unless both sides
  left pixels unchanged. This is loud on purpose (Q-RAST-3).

### 3.2 BMP

Header variants (CORE, INFO, V2/V3 bitfields, V4, V5 with ICC embedded or linked). Bottom-up vs
top-down (negative height) is encoding. 1/4/8 bpp palette, 16 bpp 555/565 bitfields, 24, 32 bpp with or
without alpha mask, RLE4/RLE8 (encoding). Metadata units: `/bmp/ppm` (resolution), `/icc`. The V3/32 bpp
"alpha or padding?" ambiguity: treat the 4th byte as alpha only if the header declares an alpha mask or any
byte differs from 0 in at least one input, decided once for all three inputs so the canonical form is stable.

### 3.3 TGA

18-byte header, image ID field (`/tga/id`), color map, origin bits (bottom/top, left/right: encoding),
RLE (encoding), 15/16/24/32 bpp, TGA 2.0 extension area (author, comments, timestamp, job, software,
`attribute type` 3 = straight alpha, 4 = **premultiplied**) as KeyedMerge `/tga/ext/<field>`. The extension's
postage-stamp thumbnail and color-correction table: thumbnail is **derived** (stale after pixel changes),
color-correction table is OpaqueLeaf. Developer area entries keyed by tag.

### 3.4 GIF

Logical screen (size, background index, aspect), global color table, then blocks: Graphic Control
Extension (disposal, user input, transparent index, delay), Image Descriptor (rect, local table,
interlace), LZW data, Comment, Plain Text, Application extensions (`NETSCAPE2.0` loop count, `XMP DataXMP`,
`ICCRGBG1012`). Units: `/screen/*`, `/palette/<i>` (global), `/animation/loop`, `/comments/<i>`,
`/app/<id+auth>`, frames as the common frame sequence. Each frame's raster is an **index grid** over its
effective table (local or global). Two frames with identical RGBA but different tables are equal only in RGBA
mode; indexed mode is used when every compared frame resolves to identical color tables.

### 3.5 WebP

RIFF chunks: `VP8 ` (lossy), `VP8L` (lossless), `VP8X` (feature flags, canvas size), `ICCP`, `ANIM`
(background, loop), `ANMF` (frame rect, duration, blend, dispose, then `ALPH`+`VP8 ` or `VP8L`), `EXIF`,
`XMP `, unknown chunks. Units: `/canvas`, `/icc`, `/exif`, `/xmp`, `/animation/*`, frames, `/chunks/<fourcc>:<n>`.
Each frame records `codec: lossy|lossless` as an encoding attribute.

### 3.6 QOI

14-byte header (width, height, channels 3/4, colorspace 0 sRGB-with-linear-alpha / 1 all linear), op
stream, 8-byte end marker. Units: `/pixels`, `/qoi/colorspace` (OpaqueLeaf, pixels depend on it).


## 4. Normalization

Common: compression method, predictor, strip/tile layout, byte order, row order (bottom-up/top-down),
RLE on/off, interlace, palette order in RGBA mode, chunk order where the format assigns no meaning.
Never normalized: stored sample values and type, alpha mode, palettes in indexed mode, metadata values,
frame timing/disposal. TIFF `Photometric=MinIsWhite` is **not** inverted (stored values are compared);
visuals invert for display. TIFF `Software`/`DateTime` tags are volatile (SPEC §12.1): hidden by `--ignore=volatile` (the Git-mode default), merged by the volatile rule, still listed by `--ignore-tags=0x0131,0x0132` for explicit control.

## 5. Diff

Pixel diff, regions, metrics and visual as png.md §5 via the raster core, with float metrics (max abs, max
ULP, NaN/Inf introduced) for float TIFFs. dump() follows the PNG layout: one header line, one line per
metadata key, `pixels xxh3`, adaptive tile-hash lines; multi-page and animated files emit a block per
page/frame. For lossy codecs (JPEG-in-TIFF tiles, WebP lossy), tile/coded-unit hash lines are
also emitted (`coded tile 3,1 xxh3=…`) so users can see which coded units were rewritten.

## 6. Merge

Per format, after metadata KeyedMerge (SPEC §5.1):

- **TIFF (lossless compressions)**: GridMerge on the page raster. Then per storage cell (strip/tile): if
  every pixel in the cell comes from one side and that side has the same compression, predictor and tiling
  as the output, copy that side's **compressed bytes verbatim**; otherwise decode, merge, re-encode that
  cell with the output's compression. Pages: SequenceMerge; page insert/delete merge like frames.
- **TIFF (lossy tiles: JPEG compression 7, WebP, LERC with MaxZError > 0)**: merge at **tile granularity**
  only: cells are whole tiles (compared by decoded pixels, copied by compressed bytes). A tile edited on both
  sides is a conflict. `JPEGTables` must be identical across sides, else conflict `encoding-mismatch`.
  `--reencode` opt-in decodes and merges in pixel space with warning `lossy-reencode`.
- **BMP, TGA, QOI**: GridMerge, full re-encode with the output encoding (always lossless). Same
  representability and promotion rules as PNG §7.3 (bpp, palette size, alpha presence).
- **GIF**: indexed mode when tables match, else RGBA mode with re-palettization only if the merged frame
  has ≤ 256 colors (else conflict `encoding-mismatch`, never dither). Frames via the frame sequence.
  Transparent index is part of the palette entry identity. Unchanged frames keep their LZW bytes.
- **WebP lossless**: decode, GridMerge, re-encode lossless with `WebPConfig.lossless=1`, `exact=1` (keeps
  RGB under alpha 0), method and quality from Q-RAST-5. Lossless re-encoding is deterministic per libwebp version (assumed, verified by a CI determinism test).
- **WebP lossy, still**: **no merge**. One-sided change wins wholesale; both sides changed pixels: conflict
  `encoding-mismatch` (`--reencode` to override). VP8 intra prediction couples neighboring macroblocks, so
  macroblock splicing like JPEG MCUs is not possible without re-encoding.
- **WebP animated**: frames are independent bitstreams, so frame-level merge is lossless even for lossy
  frames: SequenceMerge of `ANMF`, frame raster changed on one side only takes that side's bytes; changed on both
  sides is a conflict for that frame (lossless frames recurse into GridMerge instead).

`--conflict=mark`: falls back to ours plus sidecar visual (PNG Q-PNG-7). Patch payloads: raster core
`SPPX` for pixel hunks; coded-unit hunks (TIFF lossy tiles, WebP frames) carry the compressed bytes
before/after as OpaqueLeaf payloads.

## 7. Serialization and preservation

- **Passthrough** first (output equals an input → its bytes).
- **TIFF**: own writer on the Emitter through the shared IFD layer (the phase rule: writers are built on the
  Emitter, so libtiff only decodes and validates). Byte-copied cells are raw segments; re-encoded strips come from
  our own LZW, PackBits and zlib encoders. Unknown tags are re-emitted with their original type and bytes. IFD
  order and page order follow the merge. Under `--layout=stable` (default) ours' file is kept whole: unchanged
  strips, tiles and opaque blobs keep their absolute offsets and changed IFDs and strips are appended (§7.1);
  `--layout=compact` relays out the whole file (later). Classic TIFF is promoted to BigTIFF when > 4 GiB, with
  warning (later).
- **GIF**: own writer (LZW encoder with deterministic code-size and clear-code policy, Q-RAST-4). Unchanged frames
  re-emit their original table and sub-blocks.
- **WebP**: own RIFF writer on the Emitter (libwebpmux is not built); unchanged chunks and frames re-emitted
  byte-exact, re-encoded frames from libwebp's encoder output.
- **BMP/TGA/QOI**: own writers (about 200 lines each), header version and options from ours.
- Warnings: `derived-stale` (TIFF reduced-res pages/SubIFDs, TGA postage stamp),
  `layers-dropped` (TIFF 37724), `encoding-promoted`, `lossy-reencode`.

### 7.1 Envelope fields

**Layer stacks:**
- TIFF: `file → header (byte order, 42 or 43, IFD0 offset) → IFD chain → strips/tiles via offset and byte-count arrays → compression (none, LZW, Deflate, PackBits, ZSTD, LZMA, JPEG, WebP, LERC) → predictor (2 horizontal, 3 floating point) → samples`.
- GIF: `file → header, logical screen, color tables → blocks → sub-blocks (≤ 255 bytes + 0 terminator) → LZW (min code size, variable code width, clear/EOI codes) → [interlace row order] → indices`.
- WebP: `RIFF → chunks (fourcc, u32 LE size, pad to even) → [ANMF → nested chunks] → VP8 / VP8L bitstream, ALPH (raw or VP8L, optional filter) → pixels`.
- BMP: `file header → info header (+ masks, + V5 profile) → [RLE4/RLE8] → rows padded to 4 bytes, bottom-up or top-down → pixels`.
- TGA: `header → ID → color map → [RLE packets] → origin-ordered rows → pixels`, plus the TGA 2.0 extension area, developer area and footer.
- QOI: `header → op stream (INDEX, DIFF, LUMA, RUN, RGB, RGBA) → end marker`.

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| TIFF IFD offsets (header, `NextIFD`, SubIFDs 330, Exif 34665, GPS 34853, Interop 40965): u32, or u64 in BigTIFF, absolute from file start, even (word-aligned) | framing | in bounds, no cycles, no overlap | `field(u32\|u64, order, offset_of(ifd))`, `align(2, 0)` |
| TIFF out-of-line tag values (> 4 bytes, > 8 in BigTIFF) | framing | in bounds | `offset_of(value)`; in-line when they fit |
| `StripOffsets`/`TileOffsets` and `StripByteCounts`/`TileByteCounts` (SHORT, LONG or LONG8) | framing | each strip inside the file; count = strips per plane × planes | `offset_of(cell_i)`, `size_of(cell_i)`; type widened SHORT → LONG when a value exceeds 65 535 (fix-point); classic → BigTIFF at 4 GiB (warned) |
| `Compression`, `Predictor`, `JPEGTables`, `FillOrder`, `RowsPerStrip`/tile size | encoding | supported by libtiff | ours' values; Deflate level from the first strip's zlib FLEVEL (0→1, 1→5, 2→6, 3→9; libtiff `ZIPQuality` is not stored); predictors only with LZW and Deflate. A tiled or planar page that must be re-encoded is written as contiguous strips (`encoding-promoted`) |
| GeoTIFF `GeoKeyDirectoryTag` (34735) with `GeoDoubleParams` (34736) and `GeoAsciiParams` (34737) | content (keys `/pages/<i>/geo/<KeyID>`); the directory layout is framing | key locations inside their tags | ours' three tags when the keys are ours', else regenerated in key order |
| DNG `NewRawImageDigest` (0xC7A7) / `RawImageDigest` (0xC71C), MD5 | integrity | MD5 over the raw image data *(verify exact scope)* | DNG is diff-only (§10): reported, never rewritten |
| Exif `MakerNote`, Photoshop 37724 and other opaque blobs with internal absolute offsets | framing (unknown) | none | kept at the same absolute offset under `--layout=stable`; if they must move, `opaque-offsets-moved` |
| TIFF `DateTime` 306, `Software` 305 | volatile | none | preserved from ours |
| Reduced-resolution IFDs, SubIFD pyramids, COG overviews | derived | none | kept, `derived-stale` |
| GIF logical screen `GCT size` field (N → 3·2^(N+1) bytes), local table size, LZW minimum code size, sub-block lengths and terminator | framing | table sizes in bounds; sub-block chain terminated | `field(u8, size_of(sub_block))` per ≤ 255-byte block, `bytes(0)` terminator; table size = smallest power of 2 ≥ entries |
| GIF LZW code stream (clear-code and code-width policy) | encoding | bounded decode | `raw` for unchanged frames, else our LZW writer (Q-RAST-4): clear code first, width grows when the next code reaches 2^width, clear when the table is full, clear + end code at the end (the gif.h policy) |
| GIF Graphic Control Extension (disposal, user input, delay, transparent index) | content (frame keys); transparent index is encoding | block size 4 | keys `/frames/<i>/delay`, `/disposal`, `/user-input`; written when a key or a transparent entry needs it. Transparent pixels are canonical (0,0,0,0): GIF stores no color there |
| GIF image descriptor left/top, local-table flag and size, interlace flag | content (left/top: key `/rect`), encoding (table, interlace) | rect inside 16 bits | from the source frame; a re-encoded frame uses the global table when its palette fits, else a local one |
| RIFF size (u32 LE = file size − 8), chunk sizes (u32 LE, exclude header and pad), pad byte | framing | sizes in bounds; RIFF size may be off by the trailing pad in old writers *(verify)* | `field(u32, LE, size_of(riff_body))`, `field(u32, LE, size_of(chunk))`, `align(2, 0)` |
| WebP `VP8X` flags (ICC, alpha, EXIF, XMP, animation) and canvas width−1/height−1 (u24 LE) | derived | flags match chunks present | from the merged chunk set and canvas. The canvas is the key `/canvas` only in animations; a still image's canvas is its frame size. `VP8X` is written when a metadata or unknown chunk, an animation, or lossy alpha needs it |
| WebP `ANIM` background color and loop count | content | size ≥ 6 | keys `/animation/background`, `/animation/loop` |
| WebP `ANMF` X/2, Y/2, width−1, height−1, duration (u24 LE) | framing (rect is content) | frame inside canvas | from the merged frame model |
| VP8 frame tag (first-partition size), VP8L header (14-bit width−1/height−1), ALPH header byte | framing / encoding | consistent with frame size | written by libwebp |
| BMP `bfSize`, `bfOffBits`, `biSizeImage` (may be 0 for BI_RGB), `biClrUsed`, V5 `bV5ProfileData` (offset from the info header) and `bV5ProfileSize`, 4-byte row padding | framing | `bfSize` and `biSizeImage` are advisory and often wrong in the wild: a mismatch is warned `framing-inconsistent` without needing `--lenient` (Q-RAST-9); `bfOffBits` must be in bounds | `size_of(file)`, `offset_of(pixels)`, `size_of(pixels)`, `align(4, 0)` per row |
| BMP color table (1-8 bpp; order is not semantic in RGBA mode), bitfield masks, `biClrImportant` | encoding | indices inside the table | ours' table order with new colors appended (png.md §7.3); exact indices kept when every contributing side has the same table |
| BMP `bfReserved1/2`, V5 reserved | content (opaque) | none | from the encoding side |
| TGA ID length (u8), color-map spec, footer extension and developer-area offsets (u32 LE), extension-area size (495), scan-line table offsets, postage-stamp and color-correction offsets | framing | in bounds | `offset_of(...)`; the scan-line table is not written (offset 0: it is optional and only speeds up random access) |
| TGA x/y origin (screen position), attribute type | content | none | keys `/tga/origin`, `/tga/ext/attribute`; attribute type is rewritten to 3 (warned) when the merged pixels need alpha the written type would hide |
| TGA extension date/time stamp, software ID and version | volatile | none | preserved from ours |
| TGA RLE packets; BMP RLE4/RLE8; QOI op choice | encoding | bounded decode | re-encoded by our writers (packets never cross rows for TGA 2.0) |
| QOI header and 8-byte end marker `00×7 01` | framing | magic, end marker present | `bytes` |

There are no integrity fields in TIFF (except DNG digests), GIF, WebP, BMP, TGA or QOI.

**Raw copy and layout stability.** TIFF: under `--layout=stable` (default), unchanged strips and tiles keep their
absolute offsets and bytes. A metadata-only change rewrites the changed IFD at the end of the file and patches
the one pointer that referenced it (libtiff's `TIFFRewriteDirectory` does this), so strip data, MakerNotes and
37724 never move. Changed strips are appended (the old ones stay as dead bytes); rewriting them in place when they
fit is a later optimization. An IFD whose NextIFD must change is rewritten too. `--layout=compact` relays out the
file canonically (not implemented yet). GIF: unchanged frames re-emit their original LZW sub-blocks. WebP: unchanged chunks
and frames byte-exact.

**Signatures under `--signatures=auto`.** None in the core formats. C2PA defines embeddings for some of these
formats (a RIFF `C2PA` chunk in WebP, a TIFF tag) *(verify)*; when present they follow
the JPEG rule (keep, `signature-invalidated`, same keyed boundary as jpeg.md §7.1).

**Expected churn.** BMP/TGA/QOI pixel edit: only the edited pixel bytes for BMP and uncompressed TGA (`content`);
for RLE TGA and QOI the op stream from the edit to the end of the row (TGA) or of the file (QOI) is `encoding`,
plus `bfSize`-style framing. TIFF LZW strip edit under stable layout: that strip's bytes (`encoding`), its byte
count (`framing`), and, if it no longer fits, one offset entry (`framing`) plus the appended strip. GIF frame
edit: that frame's LZW sub-blocks (`encoding`) and sub-block lengths (`framing`). WebP lossless edit: the whole
VP8L stream of that frame (`encoding`) and the RIFF and chunk sizes (`framing`).

## 8. Validation

Reload and compare the canonical document with the merged model. TIFF: also `TIFFReadDirectory` on every
IFD with libtiff warnings treated as errors. WebP: `WebPDecode` of every frame. CI cross-checks: `tiffinfo`/`tiffdump`
(libtiff tools feature), `webpinfo`, ImageMagick `identify` (external, CI only).

## 9. Libraries

| Library | vcpkg port | Version seen | License | Notes |
|---|---|---|---|---|
| libtiff | `tiff` | 4.7.2 | libtiff (permissive, BSD-like) | **Verified.** Default features `jpeg`, `lzma`, `zip`; `zstd`, `webp`, `lerc`, `libdeflate` optional. Recommended. |
| giflib | `giflib` | 6.1.3 | MIT | **Verified.** History of CVEs; fuzz our usage. Decoder recommended, writer optional. |
| libwebp | `libwebp` | 1.6.0 | BSD-3-Clause | **Verified.** `libwebpmux` is a default feature (verified). Recommended. |
| qoi | `qoi` | port dated 2025-05-09 | MIT | **Verified.** Header-only reference codec. |
| stb_image / stb_image_write | `stb` | 2026-08-02 | MIT / public domain | Fallback for BMP/TGA tests; not used in production (not hardened, loses metadata). |

**Recommendation:** libtiff, giflib (decode), libwebp, and qoi are permissive and enabled by default in their features.
BMP, TGA, QOI writers and the GIF LZW writer are written in-house.

## 10. Hard cases and explicit non-goals

- Multi-page TIFF with page reordering and edits: SequenceMerge handles reorder as `move` if pages are hash-identical; reorder plus edit pairs positionally.
- Cloud-Optimized GeoTIFF: overview IFDs and ghost area layout must be regenerated to stay COG-valid; v2 marks `derived-stale` and loses COG layout (warning `cog-layout-lost`).
- Planar vs chunky, 1-bit fax (CCITT G3/G4 compressions): decode yes, re-encode with the same scheme via libtiff.
- DNG/camera raw (TIFF-based): diff tags only. Merging raw sensor data is a non-goal.
- GIF with > 256 colors after merge in RGBA mode: conflict, never dither.
- Animated GIF/WebP frame-rect changes interact with disposal: composited diff is report-only.
- Non-goals: JPEG 2000, JPEG XL (future doc), PCX, ICO/CUR (likely a small future addition reusing PNG/BMP).

## 11. Test plan

Generated fixtures per format via the module writers and the reference libraries (libtiff with every
compression and predictor, BigTIFF, tiled and stripped, float32, 16-bit, CMYK, premultiplied; GIF with
local tables and disposal modes; WebP lossless, lossy, animated mixed; QOI both colorspaces; BMP all
header versions and RLE; TGA all origins). Golden reports as png.md. Properties: passthrough identities,
determinism, compressed-cell byte copy verified (unchanged TIFF strips byte-identical to source). Fuzz targets
per parser: TIFF IFD walker (cycles, overlapping offsets, huge counts), GIF block parser and LZW decoder,
WebP RIFF parser, BMP/TGA header plausibility. Limits: TIFF strip byte
counts vs `--max-memory`, LZW expansion bound.

## 12. Phases

| Phase | Scope | Effort |
|---|---|---|
| MVP | BMP, TGA, QOI (diff + merge), GIF still and animated (index mode), TIFF single-page lossless | M |
| v1.1 | WebP all variants, TIFF multipage, GeoTIFF keys, TIFF lossy tile-granularity merge | M |
| v2 | Exif/XMP keyed merges, COG overviews regeneration | M |
| Later | Image sequences diff | L |

### 12.1 Implementation status (Phase 7, step 10)

v1 scope (D-45): MVP + v1.1. One engine (`src/formats/rastermisc/common.cpp`, internal library
`semantic-splinter-rastermisc`) does diff, merge, passthrough, reload validation, dump, inspect, and visuals for all
six modules; each module parses, encodes, and maps its envelope.

Done:

- **Shared:** canonical RGBA grid of the raster helper (8- or 16-bit storage); GridMerge per frame with guard band,
  `adjacent-edits`, tolerance and `--alpha-zero-equal`; frames paired by position when the counts agree, else
  SequenceMerge of whole frames by identity (frame keys + raster); frame control fields as keys; KeyedMerge of
  metadata with newest-wins for volatile timestamps; color-dependency conflict (`/icc`, `/bmp/colorspace`,
  `/qoi/colorspace`, TGA gamma and attribute type); exact palette indices when every contributing side has the same
  palette (indexed mode), else re-palettization in ours' table order; heatmap and merge visuals as PNG through a
  dependency-free writer in the raster helper (`raster_png.hpp`).
- **Shared TIFF/Exif IFD layer** (`include/semsplinter/formats/ifd.hpp`, in the raster helper, D-28): classic and
  BigTIFF, both byte orders, pointer-tag sub-IFDs, cycle and bounds checks; an Emitter IFD writer with absolute
  or header-relative offsets and `keep()` for values that stay in place. JPEG uses it for Exif in v2.
- **BMP:** every header version (core, info, v2, v3, OS/2 read as info, v4, v5 with embedded or linked profile),
  1-32 bpp, bitfields, alpha bitfields, RLE4/RLE8; keys `/bmp/ppm`, `/bmp/colorspace`, `/icc`; advisory
  `bfSize`/`biSizeImage` (`framing-inconsistent`); promotion in bpp, compression and header.
- **QOI:** reference op choice (byte-identical to qoi.h), 3/4 channels, `/qoi/colorspace` key.
- **TGA:** color-mapped, truecolor, gray (8, 15, 16, 24, 32 bpp), RLE (packets never cross rows on write), all
  origins; TGA 2.0 extension fields as keys, postage stamp (`derived-stale`), color-correction table and
  developer-area entries.
- **GIF:** own block parser, giflib decode, own LZW writer; still and animated; frame keys `/rect`, `/delay`,
  `/disposal`, `/user-input`; `/screen/*`, `/animation/loop`, `/comments/<i>`, `/app/<id>`; unchanged frames keep
  their table and LZW bytes.
- **TIFF:** single and multi-page (v1.1), strips and tiles, chunky and planar, 1-16-bit gray, RGB and palette with
  an alpha extra sample, LZW/Deflate/PackBits/none and predictor 2 (decode by libtiff, encode by our own codecs);
  tag keys `/pages/<i>/tags/0xNNNN` with structural tags excluded; Exif and GPS IFDs as opaque keys (deep-copied
  from another input, `opaque-offsets-moved` for a MakerNote); GeoTIFF keys (v1.1); stable-layout writer that
  appends rewritten IFDs and changed strips and keeps unchanged strips at their offsets; `layers-dropped` for 37724;
  undecodable pages (float, CMYK, YCbCr, JPEG/LZMA/ZSTD/WebP/LERC compression, extra samples) as opaque units
  merged whole; `--tiff-ignore-tags`.
- **WebP (v1.1):** own RIFF parser and Emitter writer, libwebp decode; lossless stills merge by pixels and re-encode
  with Q-RAST-5 settings; lossy stills merge only whole (`encoding-mismatch` when both changed, `--reencode` to
  merge in pixel space with `lossy-reencode`); animations merge frame by frame, unchanged frames keep their
  bitstreams; ICC, EXIF, XMP and unknown chunks as keys; advisory RIFF size.

Not yet (deferred, with follow-ups): TIFF lossy tile-granularity merge (v1.1 row: JPEG-compressed pages are opaque
units, merged whole, because the light libtiff build has no JPEG codec); float/CMYK TIFF in the canonical model
(the raster helper is RGBA only, Q-RAST-1); `--layout=compact`, in-place strip rewrite and BigTIFF promotion at
4 GiB; Exif and XMP keyed merges (v2); the GIF/WebP composited-canvas report; the cross-input Q-RAST-8 decision
(each BMP decides alpha on its own); a GIF merge past 256 colors is refused with an error, not reported as a
conflict; the TGA scan-line table; patch payloads (`SPPX`); fuzz targets and the CI cross-checks of §8.

## 13. Open questions

- **Q-RAST-1**: One shared canonical type or per-format canonical? **Provisional:** shared `Raster` with stored sample type (PNG keeps its RGBA16 view on top). **Why:** float/32-bit TIFF cannot be squeezed into RGBA16 without loss.
- **Q-RAST-3**: What to do with TIFF Photoshop layer data (tag 37724) after a pixel merge? **Provisional:** drop it with `layers-dropped`. **Why:** keeping it makes Photoshop silently show the pre-merge layers.
- **Q-RAST-4**: Own GIF LZW writer or giflib? **Provisional:** own writer, giflib for decode. **Why:** deterministic, byte-stable output independent of giflib version, about 250 lines.
- **Q-RAST-5**: WebP lossless re-encode settings? **Provisional:** `method=6, quality=100, exact=1`, fixed. **Why:** best compression and deterministic; speed is secondary for merges.
- **Q-RAST-6**: Lossy still WebP with both sides changed: conflict or re-encode? **Provisional:** conflict `encoding-mismatch`, `--reencode` to override. **Why:** VP8 has no lossless splice unit.
- **Q-RAST-7**: Allow TIFF re-layout (new offsets) when only metadata changed? **Provisional:** no under `--layout=stable` (default): append the rewritten IFD and keep strip data in place; full re-layout only with `--layout=compact`. **Why:** SPEC D-38, and private blobs with absolute offsets (MakerNote, 37724) break silently when data moves.
- **Q-RAST-8**: BMP 32 bpp V3 alpha ambiguity rule? **Provisional:** alpha if declared by mask or any nonzero 4th byte in any input. **Why:** one decision for all three inputs keeps the canonical form stable.
- **Q-RAST-9**: Treat wrong BMP `bfSize`/`biSizeImage` (and the RIFF size off-by-pad of old WebP writers) as invalid input per SPEC §12.3? **Provisional:** no, they are advisory: warn `framing-inconsistent` and continue without `--lenient`; the writer always emits correct values. **Why:** decoders ignore these fields and many real files get them wrong, so rejecting them would make common files unloadable.
