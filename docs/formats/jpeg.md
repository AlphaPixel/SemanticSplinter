# JPEG — Implementation Document

**Tier:** T2 · **Module:** `semantic-splinter-jpeg` · **vcpkg feature:** `jpeg` · **Depends on modules:** `semantic-splinter-raster` (helper, see png.md §3.4), `xml` (XMP, v2), shared TIFF/Exif IFD parser from `raster-misc` (v2)
**Extensions / magic:** `.jpg`, `.jpeg`, `.jpe`, `.jfif`. Magic `FF D8 FF`. Probe confidence 100 when the first segment after SOI is a valid marker and a SOFn is reached within the probe window, 60 on `FF D8 FF` alone.

## 1. Why it matters

JPEGs live in repos as photos in docs and websites, marketing assets, texture sources, and
inside ZIP archives and PDFs. Edits are usually small (a retouched area, a watermark, a metadata fix) but every
decode-edit-encode cycle loses quality, so the naive pixel merge (decode three, merge, re-encode) is
**generational loss** and is not acceptable as a default.

The key idea: when base, ours and theirs share quantization tables, sampling factors and dimensions,
the image can be merged **losslessly in the DCT-coefficient domain** at MCU granularity, the same
technique `jpegtran` uses for lossless crop and rotate. Untouched MCUs keep their exact coefficients, and
edited MCUs come verbatim from the side that edited them. No pixel is ever re-quantized.

A user should be able to see which MCU regions changed and by how much in pixel terms, merge disjoint
retouches without quality loss, and merge Exif/XMP/ICC edits as metadata.

## 2. Format anatomy

ITU-T T.81 / ISO/IEC 10918-1, JFIF (ISO/IEC 10918-5), Exif 3.0 (CIPA DC-008), Adobe APP14, ICC in APP2, MPF (CIPA DC-007), ISO 21496-1 / Ultra HDR gain maps.

- Segments: `SOI`, then markers `FF xx` with a 16-bit big-endian length. APPn (`E0`–`EF`), `COM`, `DQT`,
  `DHT`, `DAC`, `DRI`, `SOFn`, `SOS` followed by entropy-coded data with `FF 00` stuffing and `RSTn`, `EOI`.
- SOF variants: baseline (SOF0), extended (SOF1, 8 or 12-bit), progressive (SOF2), lossless (SOF3),
  arithmetic-coded (SOF9/10/11). Hierarchical (SOF5–7, 13–15) is rejected as unsupported.
- Components (1 gray, 3 YCbCr or RGB, 4 CMYK/YCCK) each with `h_samp`, `v_samp` (1..4) and a quant table
  selector. MCU size = (8·Hmax) × (8·Vmax) pixels, e.g. 16×16 for 4:2:0, 16×8 for 4:2:2, 8×8 for 4:4:4.
- Coefficients: per component, a grid of 8×8 blocks of 64 quantized `int16` (JCOEF). Blocks beyond the image
  edge (padding to whole MCUs) exist in the coded data but are invisible.
- Color transform is implied by JFIF (YCbCr), or by Adobe APP14 `transform` (0 none/RGB or CMYK, 1 YCbCr, 2 YCCK).
- Metadata segments: APP0 `JFIF` (density, optional thumbnail), APP0 `JFXX`, APP1 `Exif\0\0` (TIFF
  structure: IFD0, ExifIFD, GPS IFD, Interop IFD, IFD1 with a JPEG thumbnail), APP1 XMP
  (`http://ns.adobe.com/xap/1.0/\0`) and extended XMP (`http://ns.adobe.com/xmp/extension/\0`, GUID +
  offset, spans many segments), APP2 `ICC_PROFILE\0` (seq/count chunks), APP2 `MPF\0` (multi-picture
  index), APP13 Photoshop IRB (IPTC), APP14 Adobe, COM.
- Trailing data after `EOI`: MPF secondary images (gain map, depth, stereo), or garbage. Must be preserved.

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Coefficient image (DCT mode) | singleton; cell = one MCU (all component blocks of it) | `/pixels#rect=x,y,w,h` (pixel coords, MCU-aligned) | GridMerge |
| Pixel image (pixel mode, §6.3, and lossless SOF3) | singleton; cell = pixel | `/pixels#rect=x,y,w,h` | GridMerge |
| Coding parameters | field | `/coding/precision`, `/coding/components`, `/coding/sampling`, `/coding/quant/<n>`, `/coding/colorspace` | OpaqueLeaf group (gates GridMerge) |
| Encoding choices | field | `/encoding/process` (baseline/progressive/arith), `/encoding/huffman`, `/encoding/restart` | Keyed, non-conflicting |
| JFIF header | field | `/jfif/density`, `/jfif/thumbnail` | KeyedMerge |
| Exif (MVP) | segment | `/exif` | OpaqueLeaf |
| Exif (v2) | IFD + tag ID | `/exif/ifd0/0x0112`, `/exif/exif/0x9003`, `/exif/gps/0x0002` | KeyedMerge |
| XMP | packet (recursed by xml module in v2) | `/xmp`, `/xmp/<xml address>` | OpaqueLeaf → Keyed (xml) |
| ICC profile | segment group | `/icc` | OpaqueLeaf |
| Photoshop IRB | resource ID | `/irb/0x0404` (IPTC), `/irb/0x040C` (thumbnail) | KeyedMerge, OpaqueLeaf values |
| Other APPn | marker + signature + occurrence | `/app/APP11:JP:0` | KeyedMerge, OpaqueLeaf |
| Comments | occurrence | `/com/<i>` | SequenceMerge |
| Trailer / MPF images | MPF entry index or `trailer` | `/mpf/<i>`, `/trailer` | KeyedMerge, values recurse as JPEG (v2) or OpaqueLeaf |

Region addresses are in **pixel coordinates aligned to MCU boundaries** (so `rect=32,16,48,16` in a 4:2:0
image means MCU columns 2..4, row 1), clipped to the image bounds for display. `metrics.mcu_rect` gives the
same region in MCU units, which is what the payload uses.

Dependencies and derived data:

- The coefficient grid depends on `/coding/*`: GridMerge in DCT mode is only legal when `/coding/*` is identical in all three inputs.
- Pixels depend on `/icc` and on Adobe `transform`: like PNG, a one-sided profile change against pixel
  edits on the other side is **conflict `dependency`**.
- **Derived data**: the Exif IFD1 thumbnail, JFIF/JFXX thumbnails, Photoshop IRB 0x0409/0x040C thumbnails,
  and MPF/gain-map images that were computed from the primary. Never merged. After a pixel merge they are
  marked stale (`derived-stale`); regeneration is v2 (Q-JPEG-5).
- Exif orientation (0x0112) is display-only and is **not** a pixel dependency: edits are made in stored coordinates.

## 4. Normalization

Insignificant in DCT mode: Huffman tables (optimized or standard), baseline vs progressive vs arithmetic
coding, scan script, restart interval and RST markers, segment ordering among APPn that have no ordering
semantics, `FF` fill bytes, JFIF density units when density values are equal in meaning (dpi vs dpcm is
not normalized, only reported).

Significant (never normalized): quantization table values, sampling factors, component count and IDs
order, precision, color transform, coefficient values including padding blocks, any metadata bytes.

In **pixel mode**, decoded samples are the cells (RGB or gray or CMYK as stored after color conversion;
IDCT `JDCT_ISLOW` and fancy upsampling fixed so decoding is deterministic across platforms, SIMD paths of
libjpeg-turbo produce identical output for ISLOW, assumed per libjpeg-turbo docs, verified by CI).

Options: `--jpeg-mode=auto|dct|pixels`, `--tolerance=N` (pixel mode, 8-bit units; in DCT mode it is a
coefficient tolerance, §6.2), `--ignore-markers=COM,APP12`.

## 5. Diff

1. Parse segments ourselves (bounded, no libjpeg) to build the metadata units and find `SOFn`/`DQT`.
2. If `/coding/*` matches: `jpeg_read_coefficients()` on both, compare MCU by MCU (memcmp over each
   component's blocks inside the MCU). Changed MCUs feed the raster core's run builder in MCU units, then
   CCL (8-connectivity), `--gap`, regions.
3. Per region metrics: `mcus`, `blocks_changed` per component, `coef_l1` (sum |Δcoef|), plus pixel-domain
   metrics computed by decoding only the region's MCUs (with a one-MCU halo for fancy upsampling):
   `max_delta`, `mean_delta`, `psnr`, `delta_e00_max`.
4. If `/coding/*` differs: report `update /coding/quant/<n>` etc. and fall back to a **pixel-mode diff**
   (decode both, compare with optional tolerance). A full requantization shows as one region covering the
   image, plus `metrics.requantized: true`.

**Re-encode noise detection.** An editor that decodes, changes a corner and re-saves with the same
tables changes almost every MCU slightly (IDCT/color rounding is not idempotent). The diff detects this
(changed MCUs > 50% of the image and pixel-domain max delta ≤ 4 outside the largest-delta regions) and
reports `metrics.reencoded: true` with a hint: "ours appears fully re-encoded; use `--tolerance=3` to
isolate real edits".

**dump()**:

```text
jpeg 4032x3024 sof=progressive precision=8 colorspace=ycbcr sampling=2x2,1x1,1x1
quant 0 xxh3=3c1f… q~92
quant 1 xxh3=8a02… q~92
app APP0:JFIF density=72x72 dpi
app APP1:Exif bytes=12044 xxh3=…
exif ifd0 0x010F Make "Canon"
exif exif 0x9003 DateTimeOriginal "2026:05:01 10:00:00"
app APP2:ICC_PROFILE name="sRGB IEC61966-2.1" bytes=3144
mcutile 0,0,128,128 xxh3=…
```

`q~92` is an estimated libjpeg quality (scaling of the standard tables), informational. Tile hashes are
over coefficients in DCT-compatible files and over decoded pixels otherwise; tile size adapts so the image
has ≤ 4096 tiles, always a multiple of the MCU size. Exif tag lines appear in v2.

**Visual**: decoded base at 35% luminance with changed MCUs colored by pixel max-delta, MCU grid drawn
at the region borders. Merge visual uses the PNG color scheme (ours blue, theirs orange, conflict magenta).

**Cost**: coefficient read is entropy decode only (no IDCT), roughly 3–5× faster than full decode; a
24 MP 4:2:0 image holds 36M coefficients, about 72 MB per input as `int16`. The exact figure is computed from
the SOF header and checked against `--max-memory` before `jpeg_read_coefficients`.

## 6. Merge

### 6.1 DCT mode (default when `/coding/*` matches in all three)

- GridMerge over MCUs with the shared raster core: classify `U/O/T/I/X` per MCU by exact equality of all
  its component blocks; CCL; guard band in MCU units (default `--guard=0`, overridable, with `adjacent-edits` warnings, Q-JPEG-3); conflict
  groups resolved atomically.
- A merged MCU is copied **block-for-block** from the chosen side into a coefficient array cloned from
  ours (`jpeg_copy_critical_parameters` from ours, then `jpeg_write_coefficients`). No IDCT, no quantization.
- Why MCU and not block: in subsampled images a chroma block covers several luma blocks; mixing luma from
  one side with chroma from the other would invent colors. MCU is the smallest unit where all components align.
- DC coefficients are coded differentially in the entropy stream, but they are stored absolutely in the
  coefficient arrays, so splicing is safe; the encoder recomputes predictions. Restart intervals are re-emitted from ours' `DRI`.
- Edge MCUs: padding blocks are compared too. A change confined to invisible padding is reported with
  `metrics.invisible: true` and merged normally.

### 6.2 Tolerance in DCT mode

Opt-in `--tolerance=N`: an MCU counts as unchanged on a side if the region decoded to pixels differs from
base by ≤ N in every sample. Then the cell takes the other side's coefficients (or ours if both are within
tolerance). This rescues "re-encoded whole image plus one real edit" merges. Recorded in the report as
`tolerance_suppressed` MCU count. Default 0, per SPEC.

### 6.3 Pixel mode (coding parameters differ)

- Default: **no lossy merge**. If only one side changed `/coding/*` and the other side has no pixel
  changes, that side wins wholesale (byte passthrough). Otherwise: **conflict `encoding-mismatch`** at
  `/pixels` covering the image, with choices `ours`, `theirs`, `base`.
- Opt-in `--jpeg-reencode`: decode all three, GridMerge in pixel space (guard band per `--guard`, default 0), re-encode
  with ours' quant tables, sampling and process. Warning `lossy-reencode` with the PSNR of unchanged
  regions versus ours, so users see the generational loss.
- v2 opt-in `--jpeg-requantize`: requantize the minority side's coefficients to ours' tables in the DCT
  domain (`round(c · q_theirs / q_ours)`), then merge in DCT mode. One rounding step instead of a full
  decode/encode cycle. Reported as lossy.

### 6.4 Lossless JPEG (SOF3) and 12/16-bit

SOF3 has no DCT. It is decoded via libjpeg-turbo's lossless support (`jpeg16_read_scanlines` for > 12 bit),
merged as an exact pixel grid, and re-encoded losslessly with the same predictor and point transform. This
is exact. 12-bit DCT JPEGs use the same coefficient API (JCOEF is 16-bit for both precisions; assumed from
the API, verify in Phase 1).

### 6.5 Metadata

- JFIF fields, Photoshop IRB resources, other APPn: KeyedMerge (SPEC §5.1).
- Exif: MVP OpaqueLeaf per segment (both changed differently: `both-modified`). v2: KeyedMerge per
  IFD/tag through the shared IFD parser, with MakerNote treated as OpaqueLeaf that must not move (§7.2).
- XMP: MVP OpaqueLeaf; v2 recursion into the xml module (RDF descriptions keyed by property QName).
  Extended XMP is reassembled before merging and re-split with a new GUID = MD5 of the merged extended packet, as the XMP spec requires.
- ICC: OpaqueLeaf over the reassembled profile.
- Comments: SequenceMerge.

### 6.6 `--conflict=mark` and patches

`mark` falls back to `ours` plus the sidecar visual, warning `mark-unsupported` (same reasoning as PNG).

Patch payload for DCT-mode hunks: `'SPJC'`, version, MCU rect, per-component block dimensions, then for each
changed MCU in raster order the before and after coefficient blocks (`int16` little-endian, zig-zag order,
which compresses well under zstd). Application requires `/coding/*` equality with the target, else the
hunk is rejected with reason `encoding-mismatch`. Pixel-mode hunks use the raster core's `SPPX` payload and
are only applicable with `--jpeg-reencode`.

## 7. Serialization and preservation

1. **Passthrough**: if the merged document equals one input, emit that input's bytes (ours, theirs, base order).
2. **Metadata-only merge** (coefficients equal to ours): splice segments. Keep ours' SOI..SOS entropy data
   bytes verbatim and rewrite only the metadata segments. No libjpeg involvement, so the scan data is byte-exact.
3. **Coefficient merge**: write with libjpeg-turbo:
   - `jpeg_copy_critical_parameters(ours)`, then reproduce ours' entropy-coding parameters (§7.1): ours' scan
     script parsed from its SOS headers and passed as `scan_info` (`jpeg_simple_progression` only if that
     script is rejected), arithmetic coding if ours used it, ours' restart interval, and ours' Huffman tables
     when they can code every merged symbol.
   - Suppress libjpeg's own `write_JFIF_header` and `write_Adobe_marker`, and emit **all** metadata
     segments ourselves via `jpeg_write_marker` in ours' order, so APP0/APP14 bytes are byte-exact.
   - If ours' Huffman tables cannot code a merged symbol, optimized tables are built for the image and
     `encoding-reencoded` is reported; that is an encoding change only.
4. **Trailer and MPF**: bytes after `EOI` are appended unchanged. If an MPF APP2 segment is present, its
   MP Entry offsets (relative to the MPF header) are recomputed for the new primary length. Without this,
   gain-map (Ultra HDR) and depth images become unreachable.

Losses and warnings: `derived-stale` for thumbnails and gain maps after pixel changes; `lossy-reencode` in
pixel mode; `scan-script-changed` only when ours' scan script had to be replaced.

### 7.1 Envelope fields

**Layer stack:** `file → SOI, marker segments (FF xx + u16 length) → per-scan entropy-coded segment (FF00 byte
stuffing, RSTn markers, 1-bit padding before markers) → Huffman or arithmetic decoding (DHT/DAC, SOS Ss/Se/Ah/Al)
→ quantized DCT coefficients per MCU (merge domain in DCT mode) → [IDCT, upsampling, color transform] → pixels`.
Metadata segments carry their own inner layers: APP1 Exif is a TIFF structure, APP2 ICC and APP1 extended XMP are
split across segments, APP11 JUMBF is split into packets, MPF images follow `EOI`.

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| Segment length (u16 BE, includes its own 2 bytes, excludes the marker; payload ≤ 65 533) | framing | inside the file, ≥ 2 | `field(u16, BE, size_of(payload) + 2)` |
| `FF` fill bytes before markers | encoding | any count | dropped in re-encoded output, copied in raw spans |
| `FF00` stuffing, final-byte 1-bit padding | encoding | decoder | produced by libjpeg-turbo |
| `RSTn` markers (n = interval index mod 8) and DRI `Ri` | framing (`RSTn`), encoding (`Ri`) | sequence n = 0..7 cyclic, count matches `Ri` | `Ri` copied from ours; markers emitted by libjpeg-turbo |
| DHT tables, DAC conditioning, SOS scan script | encoding | well-formed | ours' tables and scan script reused (§7 item 3) |
| Exif TIFF offsets (IFD0 → ExifIFD, GPS, Interop, IFD1; out-of-line values; IFD1 `JPEGInterchangeFormat`/`Length` 0x0201/0x0202), relative to the TIFF header after `Exif\0\0` | framing | in bounds of the APP1 payload | MVP: segment raw-copied (offsets stay valid because they are segment-relative). v2: `offset_of(g, relative_to tiff_header)` via the shared IFD writer (§7.2) |
| ICC chunk header `ICC_PROFILE\0` + seq (u8, 1-based) + count (u8), ≤ 65 519 profile bytes per segment | framing | all seq 1..count present once | re-split from the reassembled profile at ours' chunk size |
| Extended XMP header: GUID (32 hex chars), full length (u32 BE), offset (u32 BE), ≤ 65 400 bytes per portion | framing (length, offset), integrity (GUID) | portions tile the full length; GUID = MD5 of the reassembled extended packet; `xmpNote:HasExtendedXMP` in the main packet equals the GUID | `md5(extended)` written as hex into every portion header **and** into the main packet's `xmpNote:HasExtendedXMP` |
| Photoshop IRB in APP13 (`Photoshop 3.0\0`, `8BIM` resources with even padding) | framing | resource lengths in bounds | `field(u32, BE, size_of(res))`, `align(2, 0)` |
| IRB 0x0425 (1061) IPTC digest: MD5 of the IPTC-IIM data in 0x0404 | integrity | `ok`/`mismatch` (a mismatch is meaningful to Photoshop, see Q-JPEG-9) | `md5(irb_0404)` when it matched on input; else copied |
| APP11 JUMBF packets: `JP` CI, box instance `En` (u16), packet sequence `Z` (u32 BE), repeated LBox/TBox | framing | packets of one box contiguous in sequence | raw-copied (policy below) |
| MPF (APP2 `MPF\0`): MP Entry individual image size (u32) and data offset (u32, relative to the MP header's byte-order mark, 0 for the primary) | framing | each offset lands on `FF D8` | `size_of(image_i)`, `offset_of(image_i, relative_to mp_header)` after the primary is laid out |
| JFIF/JFXX thumbnails, Exif IFD1 thumbnail, IRB 0x0409/0x040C, MPF gain map and depth images | derived | none | kept and marked `derived-stale` after pixel changes (Q-JPEG-5) |
| Exif `DateTime` 0x0132, `Software` 0x0131, XMP `xmp:MetadataDate`/`ModifyDate` | volatile | none | preserved from ours (inside OpaqueLeaf segments in the MVP) |
| C2PA manifest store (APP11 JUMBF, label `c2pa`) | authenticity | COSE claim signature plus `c2pa.hash.data` over the file minus the APP11 exclusion range | policy below |

**Parameter inference** (SPEC §12.2) for a coefficient merge: ours' scan script from its SOS headers, arithmetic
vs Huffman, `Ri`, and ours' DHT tables. Ours' tables are reused only if every (run, size) symbol in the merged
coefficients has a code in them, which a symbol-statistics pass checks first; otherwise `optimize_coding` builds
new tables for the whole image and `encoding-reencoded` is reported. Lossless re-encoding of coefficients is exact;
nothing is re-quantized.

**Raw copy and layout stability.** Whole-input passthrough; metadata-only merges keep every byte from SOS through
`EOI` verbatim (§7 item 2); untouched APPn/COM segments are copied with their length; bytes after `EOI` are copied
and only MPF offsets are rewritten. Segment order is ours'.

**Signatures under `--signatures=auto`.** C2PA: **keep** the APP11 segments verbatim and warn
`signature-invalidated` (ENVELOPES.md). The keyed boundary is the COSE_Sign1 signature over the claim; the
`c2pa.hash.data` hard binding is an unkeyed SHA-256 but sits inside the signed claim, so it cannot be recomputed
without re-signing (`--sign-command` only). `strip` removes only JUMBF boxes labelled `c2pa`, not other APP11
users (JPEG XT, JPEG 360). No other JPEG field is keyed.

**Expected churn.** One retouched MCU block in a 3 MiB baseline JPEG without restart markers: `content` = one MCU;
the Huffman bit stream is identical up to that MCU and shifted at bit level afterwards, so nearly every later
scan byte is `encoding`. With `DRI` (common in camera files), only the restart intervals containing changed MCUs
differ and the rest are byte-identical after an offset shift. Progressive files change in every scan from the
edited MCU's position onward. `framing`: MPF offsets and sizes when a gain map follows. A one-tag Exif edit
(v2, value fits its slot): a few `content` bytes, no length change.

### 7.2 Exif re-serialization (v2)

Value edits that fit the original value slot are patched in place (offsets unchanged). Growing values are
appended at the end of the TIFF block with offsets updated. MakerNote blocks never move, because many
vendors use absolute offsets inside them. If a merge would require moving MakerNote, the Exif unit becomes
a `both-modified` conflict instead.

## 8. Validation

- Re-parse segments; reload coefficients and verify merged MCUs equal the in-memory result exactly.
- Verify unchanged MCUs equal ours' coefficients (proves no generational loss in DCT mode).
- Decode the full image once with libjpeg-turbo to catch entropy errors; `jpeg_std_error` warnings are errors in validation.
- MPF offsets point to `FF D8` markers.
- CI: `djpeg` and `jpegtran -copy all` round trips (tools from the same port, `tools` feature), exiftool (Perl, Artistic/GPL, external, CI only) to cross-check Exif tags.

## 9. Libraries

| Library | vcpkg port | Version seen | License | Notes |
|---|---|---|---|---|
| libjpeg-turbo | `libjpeg-turbo` | 3.2.0 | BSD-3-Clause AND IJG (and zlib for SIMD parts, upstream) | **Verified port and license.** `jpeg_read_coefficients`/`jpeg_write_coefficients`, `jpeg12_*`/`jpeg16_*` (12-bit and lossless) **verified in `jpeglib.h`**. Very active (pushed 2026-10-07). Recommended. `transupp.c` (jpegtran's helper) is not part of the installed library; we do not need it. |
| mozjpeg | not in vcpkg (verified) | n/a | BSD/IJG | No benefit; better encoder irrelevant to lossless splice. |
| exiv2 | `exiv2` | 0.28.9 | **GPL-2.0-or-later** (verified) | Rejected: would make a statically linked `splinter` GPL. |
| libexif | `libexif` | 0.6.26 | **LGPL-2.1-or-later** (verified) | Rejected: static linking requires relinkable object distribution. |
| TinyEXIF | `tinyexif` | 1.2.0 | MIT AND BSD-2-Clause (verified) | Read-only, no write. Possible test oracle only. |
| easyexif | `easyexif` | port dated 2022-10-07 | BSD-2 (upstream, assumed; manifest has no license) | Read-only, stale. |
| lcms2 | `lcms` | 2.19.1 | MIT; `fastfloat`/`threaded` features GPL-3.0 (verified) | ΔE via ICC only. Do not enable GPL plugins. |

**Recommendation:** libjpeg-turbo for coefficients and pixels; own segment parser and writer (about 500
lines); own Exif/TIFF IFD reader/writer shared with the TIFF module (about 800 lines, written once in raster-misc).

## 10. Hard cases and explicit non-goals

- **Whole-image re-encode by an editor** with identical tables: every MCU differs slightly. Without
  tolerance, any concurrent edit conflicts. Diff detects and explains it; `--tolerance` is the remedy.
- **Quality change** on one side: different DQT, pixel mode. Merge refuses by default.
- **Lossless rotate/flip via jpegtran** on one side: dimensions and block layout transform; shape
  mismatch. v2 could detect the eight orientations by coefficient mapping (transpose plus sign flips), but it is out of scope.
- **Crop to non-MCU boundary** (jpegtran `-crop` with `-trim` or partial MCU): shape mismatch.
- **CMYK/YCCK Adobe JPEGs** with inverted values: DCT mode is unaffected. Pixel mode visuals handle the Adobe inversion.
- **Gain-map JPEGs** (Ultra HDR): primary merged, gain map stale. Merging gain maps independently as their own JPEG is v2 and still needs a dependency check against the primary.
- Non-goals: hierarchical JPEG, JPEG 2000, JPEG XL (separate future doc; libjxl is in vcpkg, BSD-3, verified), JPEG XT, perceptual merges.

## 11. Test plan

- **Generated fixtures** via libjpeg-turbo from synthetic images: 4:4:4/4:2:2/4:2:0/4:4:0, gray, CMYK,
  12-bit, progressive, arithmetic, restart intervals, odd sizes (padding MCUs), SOF3 lossless.
- **Edit simulation** without generational loss: modify coefficients directly in selected MCUs (as an
  editor with lossless region support would), so tests have exact ground truth; plus a "realistic"
  set made by decode, local edit, re-encode to test noise detection and tolerance.
- **Golden reports**: disjoint MCU edits, guard-band adjacency, identical edits, Exif-only change on
  both sides, ICC swap vs pixel edit (dependency), quant change (encoding-mismatch), MPF offset rewrite.
- **Properties**: unchanged MCUs in output equal ours' coefficients; passthrough identities; determinism;
  diff-then-patch round trip in DCT mode is coefficient-exact.
- **Fuzz**: segment parser (`jpeg_segments`), coefficient load with limits, Exif IFD parser (cycles, offsets beyond bounds, huge counts).
- **Limits**: SOF dimensions checked against `--max-pixels` and coefficient memory against `--max-memory`
  before calling `jpeg_read_coefficients`; progressive images with thousands of scans capped
  (libjpeg-turbo `max_scans`-style limit via a progress monitor, as recommended upstream for untrusted input).

## 12. Phases

| Phase | Scope | Effort |
|---|---|---|
| MVP | Segment parser/writer, DCT-mode diff and merge, MCU regions, passthrough, metadata-only splice, OpaqueLeaf metadata, pixel-mode diff, dump, visual | M |
| v1.1 | Re-encode noise detection, DCT-mode tolerance, `--jpeg-reencode`, MPF offset rewrite, patches | S |
| v2 | Exif keyed merge via shared IFD module, XMP via xml module, extended XMP, `--jpeg-requantize`, thumbnail regeneration | M |
| Later | Lossless transform detection, gain-map aware merge | M |

## 13. Open questions

- **Q-JPEG-1**: Merge cell: 8×8 block or MCU? **Provisional:** MCU. **Why:** mixing luma and chroma blocks from different sides invents colors in subsampled images.
- **Q-JPEG-2**: Merge default when quant tables differ? **Provisional:** refuse with `encoding-mismatch`; lossy merge only with `--jpeg-reencode`. **Why:** silent generational loss violates the no-best-effort rule.
- **Q-JPEG-3**: Guard band unit for DCT mode? **Provisional:** 1 MCU. **Decided 2026-10-08:** 0 MCUs, with `--guard=N` for a larger band (D-10). **Why (original):** consistent with raster default; MCUs are coarse already but anti-aliased edits still couple across MCU borders.
- **Q-JPEG-4**: Should tolerance be on by default for JPEG given re-encode noise? **Provisional:** no, diff detects re-encoding and recommends `--tolerance`. **Why:** SPEC says tolerance is opt-in and recorded.
- **Q-JPEG-5**: Regenerate Exif/JFIF thumbnails after pixel merges? **Provisional:** MVP marks `derived-stale`; v2 regenerates with ours' thumbnail size and quality. **Why:** stale thumbnails are harmless for most viewers; regeneration needs pixel decode + encode.
- **Q-JPEG-6**: Write our own Exif writer or depend on exiv2? **Provisional:** own, shared with TIFF. **Why:** exiv2 is GPL-2.0-or-later and libexif is LGPL; both conflict with a static permissive binary.
- **Q-JPEG-7**: Preserve ours' custom progressive scan script? **Provisional:** yes, parse it from ours' SOS headers and pass it as `scan_info`; fall back to `jpeg_simple_progression` with `scan-script-changed` only if libjpeg-turbo rejects it. **Why:** SPEC §12.2 re-encodes with parameters inferred from the original, and reproducing the script and tables keeps unchanged scan bytes stable.
- **Q-JPEG-8**: Treat Exif orientation change as a pixel dependency? **Provisional:** no. **Why:** pixels are edited in stored coordinates; orientation only affects display.
- **Q-JPEG-9**: Recompute the Photoshop IPTC digest (IRB 0x0425) after merging IPTC (0x0404)? **Provisional:** recompute only when the digest matched on the input the IPTC came from and the merged IPTC and XMP come from the same side; otherwise copy the input's value. **Why:** Photoshop uses a digest mismatch to detect IPTC edited by other tools and reconcile it with XMP *(verify)*; a fresh digest over mixed-side metadata would hide that.
