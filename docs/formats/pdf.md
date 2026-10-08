# PDF — Implementation Document

**Tier:** T3 · **Module:** `semantic-splinter-pdf` · **vcpkg feature:** `pdf` (structure, text, metadata, annotations, forms) and `pdf-render` (PDFium rendering and high-quality text extraction) · **Depends on modules:** `xml` (XMP), `png` (visual output and region grouping); registry recursion for attachments
**Extensions / magic:** `.pdf`, `.ai` (when PDF-compatible). Magic: `%PDF-` within the first 1024 bytes (the spec
allows leading garbage), version `1.0`–`1.7` or `2.0`. Confidence 95 at offset 0, 70 when found later.

## 1. Why it matters

PDFs in repos are usually build outputs (generated docs, datasheets, papers), vendor documents, signed contracts, and
forms. Most of the time the user wants to know *what changed between two versions*: which pages, which text, which
images, which metadata, which form values, which annotations. Merging page content is rarely meaningful: the PDF is a
rendering of some other source. Merging is useful only for the collaborative layers PDF really has: review
annotations, form field values, and metadata.

## 2. Format anatomy

ISO 32000-1 (PDF 1.7) and ISO 32000-2 (PDF 2.0).

- **File structure:** header, body of indirect objects (`n g obj … endobj`), cross-reference (classic `xref` tables or
  xref streams, PDF 1.5+), trailer (`/Root`, `/Info`, `/ID`, `/Encrypt`, `/Prev`), `%%EOF`. Object streams (`/Type
  /ObjStm`) hold compressed non-stream objects.
- **Incremental updates:** each save may append new/changed objects plus a new xref section whose trailer `/Prev`
  points to the previous one. The file is a stack of revisions. Signed documents rely on this: each signature covers
  a `/ByteRange` of the file as it was at signing time.
- **Linearization:** a hint-stream layout for fast web view; irrelevant to semantics, lost on rewrite.
- **Document catalog:** `/Pages` tree (inheritable `/Resources`, `/MediaBox`, `/CropBox`, `/Rotate`), `/Outlines`,
  `/Names` (`/Dests`, `/EmbeddedFiles`, `/JavaScript`), `/AcroForm` (`/Fields`, `/NeedAppearances`, `/DR`),
  `/Metadata` (XMP stream), `/StructTreeRoot` (tagged PDF), `/OCProperties` (layers), `/PageLabels`.
- **Page:** content stream(s) of operators (`BT/ET`, `Tf`, `Tj`/`TJ`, `cm`, `Do`, paths), resources (fonts with
  encodings and `/ToUnicode` CMaps, XObjects: images and forms), `/Annots` array.
- **Annotations:** dictionaries with `/Subtype` (Text, Highlight, Underline, StrikeOut, FreeText, Ink, Link, Widget,
  Stamp, …), `/Rect`, `/Contents`, `/NM` (unique name, optional), `/M`, `/T` (author), `/IRT` (reply-to), `/AP`
  appearance streams (derived from the other entries for most types).
- **Forms:** AcroForm field tree; fully qualified name = dotted `/T` chain; `/FT` (Tx, Btn, Ch, Sig), `/V` value, `/DV`,
  `/Ff` flags; widgets are annotations with `/AP` appearances (derived). XFA (`/XFA` in AcroForm) is a separate,
  deprecated XML forms architecture.

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Document info | `/Info` key | `/info/Title` | KeyedMerge |
| XMP metadata | XMP property path via xml module | `/xmp/dc:title` | KeyedMerge (xml) |
| Page | aligned page identity (§3.1) | `/pages/p:<base index or new:side:n>` | SequenceMerge (diff only) |
| Page text | word sequence in reading order | `/pages/p:3/text#rect=x,y,w,h` (PDF user space, points) | SequenceMerge (diff only) |
| Page visual | rendered raster | `/pages/p:3/render#rect=…` (pixels at `--dpi`) | GridMerge (diff only) |
| Page resources | font/image XObject by content hash | `/pages/p:3/images/<hash6>` | OpaqueLeaf (images recurse for metrics) |
| Annotation | `/NM` if present, else (`/Subtype`, `/T`, `/Rect` rounded to 1 pt, `/Contents` hash) | `/pages/p:3/annots/<key>` | KeyedMerge |
| Form field | fully qualified field name | `/fields/address.street` | KeyedMerge (value `/V`, flags, options) |
| Outline item | title path | `/outline/Chapter 1/Section 2` | SequenceMerge per level (diff only) |
| Named destination | name | `/dests/<name>` | KeyedMerge (diff only) |
| Embedded file | file spec name | `/attachments/<name>` | registry recursion |
| Revision (incremental update) | index from first | `/revisions/2` | diff only |
| Object graph | traversal path from `/Root` | `/objects/Root/Pages/Kids/0/Resources/Font/F1` | Merkle hash diff (diff only) |

### 3.1 Page alignment

Pages are matched with SequenceMerge's matcher over a page fingerprint: (normalized text hash, content stream hash after
resource-name normalization, media box). Unmatched pages in gaps are paired by text similarity (token LCS ratio ≥ 0.5),
then by perceptual hash of the rendered page when `pdf-render` is on, giving `confidence`. Result: inserted, deleted,
moved, and modified pages instead of "every page after page 3 changed".

### 3.2 Dependencies, invariants, derived data

- Widget annotations depend on their field; `/IRT` replies depend on their parent annotation; link annotations and
  outline items depend on destination pages.
- Invariants for the write path: xref offsets correct; every reference resolves; `/Annots` entries are indirect
  dictionaries whose `/P` (if present) is the page; field tree parent/kids consistent; `/NM` unique per page.
- **Derived:** appearance streams (`/AP`) of widgets and markup annotations, page thumbnails (`/Thumb`), linearization
  hints. Xref and object numbering are framing; `/ID[1]` (changes on every save), `/Info /ModDate`, and `/Producer` are
  volatile (§7.1).

## 4. Normalization

- Object numbers, generation numbers, object-stream packing, xref form, linearization, and stream compression
  (`/Filter`) are representation only: object graph comparison works on decoded streams and on traversal paths, never
  on object numbers.
- Resource names (`/F1`, `/Im3`) are renamed by producers; content streams are compared after mapping resource names
  to the hash of the resource they name.
- Text: extracted Unicode, NFC, whitespace collapsed; hyphenation at line ends is kept (no dehyphenation by default,
  `--pdf-dehyphenate` opts in).
- Noise: `/ID`, `/Info` `ModDate`/`Producer`/`CreationDate` when everything else is equal, XMP `xmp:MetadataDate`,
  `xmpMM:InstanceID`, `xmpMM:History`. Hidden unless `--pdf-show-noise`.
- Never normalized: text content, page geometry, annotation contents, field values.

## 5. Diff

Layers, each optional (`--pdf-layers=meta,pages,text,visual,annots,forms,outline,attachments,objects,revisions`;
default all except `objects` and `visual`):

| Layer | Output |
|---|---|
| `meta` | Info + XMP key changes |
| `pages` | page insert/delete/move/modify, geometry (`MediaBox`, `Rotate`) |
| `text` | per aligned page, word-level text changes with bounding boxes (`#rect=` in points), `metrics: {words_inserted, words_deleted}` |
| `visual` | per aligned page pair, pixel regions (via the png module's grid grouping), `metrics: {changed_pixels, changed_fraction, max_delta}`; `--visual DIR` writes heatmap PNGs per changed page |
| `annots` | annotation add/remove/modify (contents, author, rect, color, reply threading) |
| `forms` | field value and flag changes, fields added/removed |
| `outline`, `attachments` | bookmarks tree and embedded file changes (attachments diffed by their own format) |
| `objects` | qpdf-style object graph diff: Merkle hashes over traversal paths, reporting the smallest changed subtrees |
| `revisions` | incremental update stack: count, objects touched per revision, which revisions are signed; `splinter diff --pdf-revision A..B file.pdf` diffs two revisions of one file |

**Text extraction.** With `pdf-render`, PDFium's text API (`FPDFText_*`) supplies characters with boxes; words are
built from characters and ordered by PDFium's reading order. Without it, a qpdf-based extractor walks content streams
(`QPDFPageObjectHelper` content parsing), tracks the text matrix, decodes strings through `/ToUnicode` or the standard
encodings, and emits words with approximate boxes. The fallback is good for diffing text but has no layout analysis
(multi-column order may be off; reported as `info: text-extraction-basic`).

**`dump()`** (textconv):

```text
pdf 1.7 pages=12 revisions=2 signed=1 encrypted=no
info Title="Datasheet" Author="ACME"
xmp dc:title="Datasheet"
== page 1 612x792 rotate=0
Product Datasheet
Rev. C, October 2026
[image 1200x800 xxh3:7c1e…]
== page 1 annots
Text @ 72,700 by "Bob": "Typo in heading?"
== fields
customer.name (Tx) = "Alice"
== outline
1 Overview -> p1
1.1 Features -> p2
```

One extracted line per output line, page by page. Images are printed as dimensions plus hash. No object numbers.

**Cost.** Parsing is fast (qpdf). Text extraction is linear in content size. Rendering dominates: about 50–200 ms per
page at 96 dpi with PDFium; `visual` runs only on pages whose content fingerprint differs, `--max-render-pages`
(default 200) bounds it.

## 6. Merge

Merge covers the collaborative layers and, since Q-PDF-4 was decided, page content without layout checks:

1. **Choose a carrier.** If page content (aligned page fingerprints, resources) is unchanged on one side, the other side
   is the carrier (its pages, fonts, structure are used as is). If both sides changed page content, the side with more
   changed pages is the carrier and page content merges (step 1a).

1a. **Page content (Q-PDF-4, decided 2026-10-08).** The page list merges with SequenceMerge over the aligned pages, so
   pages added, deleted, or moved on either side combine. Within a page changed on both sides, each content stream is
   split into graphics objects (a `BT…ET` text object, a path construction plus its paint operator, an XObject `Do`,
   an inline image), each carrying the graphics-state operators that precede it. These objects merge with SequenceMerge
   after resource names are normalized. Resources used by the non-carrier side are copied in, renamed on collision.
   Overlapping edits to the same object conflict `both-modified` at `/pages/N/content#range=…`. Splinter does not
   reflow or check the layout: overlapping or misplaced text in the result is accepted and reported only as
   `info: content-merged`.
2. **Metadata:** KeyedMerge of Info keys and XMP properties (via the xml module). Noise keys take the carrier's.
3. **Annotations:** KeyedMerge per page, applied onto the carrier. An annotation from the non-carrier side is
   transferred only if its page is aligned with confidence 1 (identical page fingerprint) or, with
   `--pdf-transfer-annots=similar`, if the page text around the annotation's rect is unchanged; otherwise
   `conflict dependency` ("annotation anchor changed"). Replies follow parents (`/IRT`). Appearance streams of transferred
   annotations are copied unchanged when the annotation is unchanged; for modified markup annotations `/AP` is removed so
   viewers regenerate it (`derived-stale`).
4. **Form field values:** KeyedMerge by fully qualified name on `/V` (and `/AS` for checkboxes/radios). Appearances are
   derived: regenerated with qpdf's `QPDFFormFieldObjectHelper::generateAppearance` for text and choice fields; for
   PDF 1.x documents `/NeedAppearances true` is also set; buttons use their existing `/AP` states. Field structure changes
   (fields added/removed, types changed) follow the carrier; structure changes on the non-carrier side conflict.
5. **Outline, attachments, page labels, named destinations:** carrier wins; non-carrier changes are `conflict` unless
   identical. (Attachment content changed on the non-carrier side only: KeyedMerge by name, recursing into the attachment's
   format.)
6. **Signatures.** If an input revision is signed and the merge changes anything, the result must not silently
   invalidate it: the default writes an **incremental update** on top of the carrier's bytes (§7), which keeps existing
   signatures valid as far as byte ranges are concerned (whether the signer's permissions, `/DocMDP`, allow the change is
   reported as `certification-violated`, not enforced). With incremental writing unavailable for a case, the merge
   refuses with conflict `signature` unless `--signatures=strip` or `--signatures=keep` is given explicitly (§7.1).

`--conflict=mark`: conflicts with an anchor on a page add a Text annotation by author `splinter` at the anchor with
the conflict id and both values; `/pages`-level conflicts add one note on page 1. Otherwise policy side.

**Patch payloads:** `pdf-annot/1` (annotation dictionary serialized as PDF object syntax with indirect references
replaced by symbolic paths; appearance streams omitted), `pdf-field/1` (FQN + value), `pdf-meta/1` (key/value).
Page content patches use `pdf-content/1` (the graphics objects of a page as content-stream operator text, with symbolic resource names).

## 7. Serialization and preservation

- **Incremental update by default** for merge output: the carrier file's bytes are copied verbatim, then new and
  changed objects, a new xref section (classic table if the carrier uses tables, xref stream otherwise), and a trailer
  with `/Prev`. This is byte-exact for everything untouched and signature-preserving. Implementation: object
  serialization via qpdf (`QPDFObjectHandle::unparse` / stream data), xref/trailer writing in our code (~600 LOC).
  qpdf's `QPDFWriter` performs full rewrites and is used only with `--layout=compact` (formerly `--pdf-rewrite`; smaller
  file, renumbered objects, drops old revisions, strips signatures per §7.1; *qpdf incremental write not available, assumed from documentation, not verified*).
- Deterministic: `/ID[1]` derived from the XXH3 of the output content instead of time/random; no `/ModDate` update
  unless `--pdf-touch-moddate`.
- Encrypted inputs (D-41): permission-only encryption (empty user password) is looked through, and merges write the
  incremental update with the carrier's encryption and keys (qpdf supports writing encrypted objects). Files that need a
  password are `encrypted` OpaqueLeaf in v1; `--password-file` support comes later. Unsupported security handlers
  (certificate/public-key) are an explicit error.
- Lost or stale (warnings): appearance streams regenerated or removed, linearization (incremental update makes the file
  non-linearized), `/NeedAppearances` set.

### 7.1 Envelope fields

**Layer stack:** `file → revisions (incremental update stack, /Prev chain) → xref (table or stream) → indirect objects and object streams → [security handler: RC4/AES per object] → stream /Filter chain (Flate, LZW, ASCIIHex, ASCII85, RunLength, Crypt) → /DecodeParms predictor (PNG 10-15, TIFF 2) → decoded data (content operators, fonts, ICC, XMP, embedded files)`. Image codecs (DCT, JPX, JBIG2, CCITTFax) are the innermost layer of image XObjects and are never re-encoded; such streams are compared by decoded parameters plus encoded bytes and copied raw.

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| Classic xref entry `oooooooooo ggggg n\r\n` (20 bytes, 10-digit offset) | framing (absolute) | offset points at `N G obj`; qpdf repairs and splinter warns `framing-inconsistent` | `offset_of(obj_g)` as fixed-width decimal, in the appended section only |
| xref stream `/W`, `/Index`, `/Size`, binary rows | framing | same as above | `/W` widths chosen to fit the largest offset; a fix-point when the xref stream's own offset grows a width (SPEC §12.4) |
| `startxref` value, trailer `/Prev`, `/Size`, `/XRefStm` (hybrid files) | framing (absolute) | each `/Prev` lands on an xref | `offset_of(xref_g)`; `/Prev` = carrier's last `startxref`; `/Size` = max object number + 1 |
| Stream `/Length` (direct or indirect) | framing | matches the bytes up to `endstream`; mismatch repaired by qpdf with a warning | direct decimal `/Length` is variable width (fix-point); writer prefers a direct value |
| Object stream `/N`, `/First`, header pairs `objnum offset` | framing (relative to `/First`) | consistent | new objects are written as plain indirect objects in the appended section with a new xref stream; a new object stream only above 100 small objects (Q-PDF-8) |
| Linearization dict `/L`, `/H`, `/O`, `/E`, `/N`, `/T`, hint stream | derived (index) | checked only for the report | not updated; an incremental update makes the file non-linearized in effect (§7) |
| Flate/LZW/RunLength/ASCII filters, predictors | encoding | decoded within limits | untouched streams raw; changed streams `encode(flate)` with the original `/DecodeParms` and a zlib level inferred from the zlib header FLEVEL plus trial; LZW and ASCII85 kept only if the source used them on that stream |
| `/ID [permanent changing]` | identity (`/ID[0]`) and volatile (`/ID[1]`) | none | `/ID[0]` kept from the carrier (required: the encryption key derivation uses it), `/ID[1]` = deterministic hash (§7) |
| `/Info /ModDate`, `/Producer`, XMP `xmp:ModifyDate`, `xmp:MetadataDate`, `xmpMM:InstanceID` | volatile | none | ours unless `--pdf-touch-moddate` |
| Appearance streams `/AP`, page `/Thumb`, `/NeedAppearances` | derived | none | Q-PDF-7 |
| Encryption dictionary `/Encrypt` (`/O`, `/U`, `/OE`, `/UE`, `/Perms`), per-object keys from object number and generation | encryption (keyed by password) | permission-only (empty user password): file key derived and looked through; otherwise `encrypted` OpaqueLeaf (D-41) | re-encrypted with the carrier's keys, which requires object numbers to stay stable: one more reason for incremental updates |
| Signature field `/V` → sig dict: `/ByteRange [0 a b c]`, `/Contents <hex CMS>`, `/SubFilter`, `/M`, `/Reference` (DocMDP/FieldMDP) | authenticity | verified (below) | policy (below) |
| `/Perms /DocMDP` (certification), `/Perms /UR3` (usage rights) | authenticity (permissions) | evaluated against the changes in later revisions | reported, see below |
| DSS (`/DSS` with `/Certs`, `/OCSPs`, `/CRLs`, `/VRI`), document timestamps (`/Type /DocTimeStamp`, `ETSI.RFC3161`) | authenticity (validation material) | carried | kept; never rebuilt |

**Raw copy and layout stability.** The default `--layout=stable` writes an incremental update (§7): every byte of the carrier, including all earlier revisions, signatures, and linearization, is copied verbatim, and the appended section holds only changed and new objects, a new xref, and a trailer. Untouched streams inside changed objects are never re-encoded, because a changed dictionary is written with its original stream bytes when the decoded data is unchanged (`raw`, with `/Length` copied). `--layout=compact` (formerly `--pdf-rewrite`) rewrites through `QPDFWriter`, renumbers objects, and recompresses streams.

**Signatures and the keyed boundary.** `/ByteRange` covers the whole file as it was at signing time except the `/Contents` hex string (including its angle brackets). The digest over those ranges is unkeyed, but it is embedded as the `messageDigest` signed attribute inside the CMS `SignedData` in `/Contents` (`adbe.pkcs7.detached`, `ETSI.CAdES.detached`), so the keyed boundary is the CMS signature value. On load, with the `crypto` feature, each signature is reported `valid`, `invalid`, or `unverifiable`, plus `chain-not-evaluated`, and whether it covers the whole file or only an earlier revision (`signature-not-covering-changes`).

- `--signatures=auto` with `--layout=stable`: earlier signatures are **kept and stay cryptographically valid**, because their byte ranges are untouched; the new revision is unsigned and every signature reports `signature-not-covering-changes`.
- `--layout=compact`, or any case where an incremental update is impossible: `auto` **strips** signature values (the field stays, `/V` removed, `/Perms` cleared) with `signature-stripped`. When the user did not ask for compact layout, the merge instead refuses with conflict `signature` unless `--signatures=strip` or `keep` is given explicitly (§6), so a signed document is never silently unsigned.
- **DocMDP** (certification signature, `/TransformMethod /DocMDP`, `/P`): P=1 allows no change; P=2 allows form filling, page template instantiation, and signing; P=3 additionally allows annotation creation, deletion, and modification. A merge whose appended changes exceed `P` is still written (the signature bytes stay valid) but reports `certification-violated`, which Acrobat shows as an invalid certification. **FieldMDP** locks named fields: a merged value for a locked field is `certification-violated` too.
- **UR3** usage-rights signatures (Reader extensions) are invalidated by changes outside the permitted kinds; Reader then disables the extended features. Reported as `signature-invalidated` on `/UR3`, kept.
- `resign` (`--sign-key`/`--sign-cert`, post-v1, D-36): appends a second incremental update with a new signature field, `/ByteRange` written as fixed-width decimal placeholders, and `/Contents` reserved at a fixed hex width (default 16 KiB of CMS, larger with a timestamp token), then digests the two ranges, creates the CMS, and writes it zero-padded into the reservation (SPEC §12.4 excluded range).

**Expected churn.** Merging one added annotation into a signed 5 MB PDF: 0 changed bytes in the carrier, plus an appended 1-4 KB section holding the page dictionary with its new `/Annots` (content), the annotation and its appearance stream (content, derived), a new xref and trailer (framing), and `/ID[1]` (volatile). Attribution is close to 100% explained. A diff of two full saves by different producers instead shows nearly every byte moved: object renumbering and offsets (`framing`), restreamed content (`encoding`), new `/ID` and dates (`volatile`), with the semantic change isolated as `content`.

## 8. Validation

Reload with qpdf (`QPDF::processMemoryFile`), `qpdf --check` equivalent (`QPDF::checkLinearization` not needed; xref and
object stream check via full traversal); invariants of §3.2; for signed inputs, verify every original `/ByteRange` still
hashes to the same bytes (structural check, not cryptographic validation). CI: `qpdf --check` CLI, veraPDF for PDF/A
fixtures (Java, GPL-3/MPL-2 dual, external process only), and PDFium render of every output page without errors.

## 9. Libraries

Licensing is the deciding factor: the release `splinter` binary is statically linked (SPEC §2.4), so any GPL/AGPL
library would make the whole binary GPL/AGPL. Verified 2026-10-07 against the vcpkg ports tree and GitHub API unless
marked *assumed*.

| Library | vcpkg port | License | Maintenance | Verdict |
|---|---|---|---|---|
| **qpdf** | `qpdf` 12.4.2 | Apache-2.0 AND MIT | active (push 2026-09) | **Use** (core of `pdf`): parsing, xref repair, object model, decrypt/encrypt, content stream tokenizer, form appearance generation. Pulls `libjpeg-turbo` and `zlib`; crypto: native provider (verified: the vcpkg portfile sets `REQUIRE_CRYPTO_NATIVE=ON`), so permission-only decryption (D-41) needs no OpenSSL; never enable the `openssl`/`gnutls` features. Apache-2.0 requires shipping NOTICE text. |
| **PDFium** | **none** (verified: no port in vcpkg master) | BSD-3-Clause, with third-party parts under FreeType (FTL), libjpeg-turbo, OpenJPEG (BSD-2), lcms2 (MIT), zlib, Abseil (Apache-2.0) (*assumed from Chromium licensing*) | very active (Chromium) | **Use, optional** (`pdf-render`): rendering and text with layout. Build via a vcpkg **overlay port**. `bblanchon/pdfium-binaries` (latest release `chromium/8086`, 2026-10-05, verified) ships **shared libraries only** and states it will not add static builds; its build script supports static builds with `-s`. Plan: overlay port that builds static PDFium with that script in CI and caches it as a binary artifact; on Windows the single-file release either includes static PDFium or ships without `pdf-render`. |
| MuPDF | `libmupdf` 1.26.10 | AGPL-3.0-only (or commercial from Artifex) | active | **Reject.** AGPL would apply to the whole statically linked binary. |
| Poppler | `poppler` 26.4.0 | GPL-2.0-or-later | active | **Reject.** GPL for the whole binary. |
| PoDoFo | `podofo` 1.1.1 | LGPL-2.0-or-later OR MPL-2.0 | active (push 2026-10) | Not needed (qpdf covers it). If ever used, take the MPL-2.0 option: file-level copyleft, compatible with static linking if modified PoDoFo files are published. |
| vanillapdf | `vanillapdf` 2.3.0 | Apache-2.0 | small project (*maintenance not checked*) | Not needed. |
| libharu | `libharu` 2.4.6 | Zlib | active | Writer only; not needed. |

Recommended permissive stack: **qpdf + optional PDFium**, with the png module for heatmaps. Own code: page alignment,
basic text extractor over qpdf (~1.5 kLOC), annotation/form/metadata merge (~2 kLOC), incremental writer (~0.6 kLOC),
dump and reports.

## 10. Hard cases and explicit non-goals

- Text extraction quality: Type 3 fonts, fonts without `/ToUnicode` and with custom encodings (text becomes garbage;
  reported `info: text-unmappable`), ligatures, right-to-left scripts, text drawn as paths (outlined text has no text).
- Scanned PDFs: no text layer; only `visual` diff is meaningful. OCR is a non-goal.
- Re-generated PDFs with identical rendering but completely different structure (fonts subset differently, new
  object numbering): `objects` layer is noisy, `text`/`visual` layers are the useful ones, which is why `objects` is off by default.
- Page alignment when a document is re-flowed (one inserted paragraph shifts all following page breaks): every page
  differs; text diff falls back to a whole-document word sequence with page boundaries as soft markers
  (`--pdf-text-flow`), reported per page range.
- XFA forms: OpaqueLeaf (`/XFA` stream diff via the xml module only); dynamic XFA is unsupported for merge.
- JavaScript, 3D, multimedia annotations: diffed as objects, never executed.
- Non-goals: reflowing or re-laying out merged page content, certificate chain trust evaluation (signature validity is
  checked per SPEC §12.3), PDF/A conformance
  repair, OCR.

## 11. Test plan

- Generated fixtures: a test-only writer built on qpdf creates PDFs with known text, annotations, fields, metadata,
  incremental updates, and a dummy signature dictionary (fake `/Contents`, valid `/ByteRange`).
- Real files: LaTeX (pdfTeX/LuaTeX), Word/LibreOffice export, Chrome print, InDesign, scanned, PDF/A, encrypted
  (RC4/AES-128/AES-256), linearized, multiple incremental updates, signed.
- Golden reports and dumps; visual heatmaps compared by hash (PDFium version pinned in the overlay port).
- Properties: merge with only annotation edits on both sides equals carrier plus both annotation sets; incremental output
  keeps every original byte as a prefix; reload passes; determinism.
- Fuzz: qpdf has its own OSS-Fuzz coverage; ours: text extractor, annotation/form merge on mutated object graphs,
  incremental writer round trip.

## 12. Phases

| Phase | Scope | Effort |
|---|---|---|
| MVP | qpdf load, `meta`/`pages`/`text` (qpdf extractor)/`annots`/`forms` diff, page alignment, `dump()`, metadata merge | M |
| v2 | `pdf-render` (PDFium overlay port), `visual` layer and heatmaps, PDFium text; annotation and form merge with carrier logic; incremental writer; signatures check | L |
| v2 | page content merge (§6 step 1a) | L |
| Later | `objects` Merkle diff, `revisions` diff within one file, outline/attachment merge, `--pdf-text-flow` | M |

## 13. Open questions

- `Q-PDF-1`: Rendering/text library? **Decided 2026-10-08:** no rendering in v1; qpdf only (D-31). **Provisional (original):** PDFium as optional `pdf-render` feature via an overlay port building
  static libs; qpdf-only extractor otherwise. **Why:** only permissive renderer of production quality; no vcpkg port exists.
- `Q-PDF-2`: Accept MuPDF or Poppler as optional features for users who accept copyleft? **Provisional:** no. **Why:** a
  build flag that silently relicenses the binary is a trap; PDFium covers the need.
- `Q-PDF-3`: Default write mode for merges? **Provisional:** incremental update on the carrier. **Decided 2026-10-08:** as provisional; `--layout=compact` forces a full rewrite. **Why:** byte-exact
  preservation, keeps signatures' byte ranges intact.
- `Q-PDF-4`: What can merge? **Provisional:** metadata, annotations, form values, attachments; page content never. **Decided 2026-10-08:** page content merges too (additions, deletions, and edits from both sides, §6 step 1a); an ugly result is the user's call. **Why (original):**
  PDF is an output representation; content merge would invent layouts.
- `Q-PDF-5`: Transfer annotations onto a page whose content changed? **Provisional:** only for identical page fingerprints
  by default; `--pdf-transfer-annots=similar` relaxes. **Why:** an annotation's meaning is its position over specific content.
- `Q-PDF-6`: Default diff layers? **Provisional:** all except `objects` and `visual` (visual on when `pdf-render` is built and
  `--visual` given). **Why:** keeps default reports focused and fast.
- `Q-PDF-7`: Appearance streams for merged fields? **Provisional:** regenerate with qpdf for text/choice fields, set
  `/NeedAppearances` for PDF 1.x. **Why:** `/AP` is derived; PDF 2.0 deprecates `NeedAppearances`, so both are needed.
- `Q-PDF-8`: How are new objects stored in an incremental update of an xref-stream carrier? **Provisional:** as plain
  indirect objects, with a new xref stream; no new object stream unless more than 100 small objects are added. **Why:** plain
  objects keep the appended section readable and small, and every PDF 1.5+ reader accepts them next to an xref stream.
- `Q-PDF-9`: Merge result exceeds a DocMDP `/P` level: write it or refuse? **Provisional:** write it with
  `certification-violated`. **Why:** the file stays loadable and earlier revisions stay
  verifiable, which is the `auto` principle; refusing would block routine merges of certified forms.
