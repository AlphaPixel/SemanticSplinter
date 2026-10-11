# SVG — Implementation Document

**Tier:** T1 · **Module:** `semantic-splinter-svg` · **vcpkg feature:** `svg` (optional `svg-render` for the rasterizer) · **Depends on modules:** `xml`, `png` (visual output), registry recursion for `data:` URI images
**Extensions / magic:** `.svg`, `.svgz` (gzip). Probe: XML whose root element is `svg` in namespace `http://www.w3.org/2000/svg` (also accept a missing namespace with a lower confidence). `.svgz` is gzip magic `1F 8B` plus `.svgz` logical name.
**Status (v1, #9):** MVP plus subpath merge, the entity exception, and SVGZ, built as an `xml::Dialect` (hooks `accept_entity`, `attr_kind`, `text_mode`, `normalize`, `merge_attr`, `is_derived`, `rewrite_ids`, `renumber`, and `mark` with its site). Not yet: the lunasvg visual (§5.2), `<style>` CSS rule merge, `data:` image recursion, segment-level path merge, id-rename detection with reference rewriting (§3.2), gradient/`use` cycle checks, patch payloads, and XMLDSig `strip` (a signed SVG only warns `signature-invalidated`, as generic XML). Deviations: addresses are the xml module's (`/svg/g[@id=layer1]/rect[@id=r7]`, `@style` as a whole, no `id:`/`n:` steps); elements without ids pair by the xml module's similarity, not the geometry score or Hungarian assignment of §3.1; add-add ids follow SPEC §2.3: a counter-allocated id (`rect1234`) continues after the highest number in use with its prefix, any other id becomes `<id>-t<n>`, noted `auto: renumbered` (with `renumbered_from` in the change metrics); the dump is the xml module's canonical dump with comparison forms (sorted styles and classes, shortest numbers, absolute path data) and editor state left out; a merged `d` keeps each subpath's source text, only a relative leading `m` becomes absolute, without the `path-reserialized` warning; both sides' additions at one place are both kept, ours first, except under `text`, `tspan`, `textPath`, and `switch`; `--conflict=mark` falls back to the comment mark where a `g` cannot go (no lifting to an ancestor); colors compare `#rgb`, `rgb()` and the 16 HTML names plus `orange` (no full CSS color table); a dependency conflict is placed on the deleted element, so `--conflict=ours` can still leave the other side's new reference dangling (warning `dangling-ref`); `.svgz` deflate level comes from XFL alone (no trial compression).

## 1. Why it matters

SVG is the vector format for icons, UI art, logos, diagrams, and game UI. It is text, so Git already
line-merges it, and that is the problem: Inkscape and Illustrator rewrite whole attributes on one line,
reorder `<defs>`, regenerate IDs, and shift viewport state on every save. Line merges either conflict on
noise or "succeed" with broken references (`url(#linearGradient1234)` pointing to a gradient that the
other side renamed).

A user should be able to see which shapes, groups, styles, gradients, and paths changed, merge two
designers' edits to different elements or different attributes, and get a valid SVG with all references
resolved. Conflicts are named by element (`/svg/id:logo/@fill`) and can be marked visibly in the file.

## 2. Format anatomy

- SVG 1.1 Second Edition and SVG 2 (W3C). Real files are mostly SVG 1.1 plus editor namespaces.
- Tree of elements: structural (`svg`, `g`, `defs`, `symbol`, `use`, `switch`, `a`), shapes (`path`,
  `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`), `text`/`tspan`/`textPath`, `image`, paint
  servers (`linearGradient`, `radialGradient`, `pattern`), `clipPath`, `mask`, `filter` + `fe*`, `marker`,
  `style`, `script`, `metadata`, `title`, `desc`, animation (`animate*`, `set`).
- Presentation attributes (`fill="red"`) and the `style` attribute (`style="fill:red;stroke-width:2"`)
  express the same properties with different CSS precedence. `<style>` elements carry CSS rules.
- References: `url(#id)` in paint, `clip-path`, `mask`, `filter`, `marker-*` (as attributes or style
  properties); `href` / `xlink:href` on `use`, `image`, `textPath`, gradients (template inheritance),
  `pattern`, `feImage`, `a`, `mpath`; CSS selectors `#id` in `<style>`; SMIL timing `begin="other.end"`;
  ARIA `aria-labelledby`/`aria-describedby`; Inkscape `inkscape:path-effect="#path-effect12"`,
  `inkscape:original-d`, `sodipodi:namedview` `inkscape:current-layer`.
- Editor namespaces: `inkscape:` (`http://www.inkscape.org/namespaces/inkscape`), `sodipodi:`,
  Illustrator `i:`, `x:` (`adobe:ns:meta/`), `graph:`, Illustrator private data `i:pgf` (a large base64
  blob of the native AI file), Sketch/Figma/`data-name` attributes, RDF `metadata`.
- Illustrator's legacy exports declare namespaces through internal DTD entities (`<!ENTITY ns_svg "…">`
  then `xmlns="&ns_svg;"`). This matters because the core XML limits disable DTD entities (see 10).

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Element | `id:<id>` if it has `id` (or `xml:id`); else `n:<hash8>` from the matcher | `/svg/id:layer1/n:3f9a1c2b` | KeyedMerge of attributes + SequenceMerge of children |
| Child list (z-order) | element identity | `/svg/id:layer1` (children) | SequenceMerge with moves |
| Attribute | qualified name `{ns}local` rendered as `@prefix:local` | `/svg/id:logo/@fill` | KeyedMerge (scalar) |
| `style` property | CSS property name | `/svg/id:logo/@style/fill` | KeyedMerge |
| `class` | token | `/svg/id:logo/@class` | set merge (KeyedMerge over tokens, value = presence) |
| `transform` | singleton | `/svg/id:logo/@transform` | scalar with matrix-equality normalization |
| Path data `d` | subpath index (v1); segment sequence (v2) | `/svg/id:p1/@d#range=start,len` (segments) | SequenceMerge over subpaths, segments opt-in |
| `points` (polyline/polygon) | point index | `/svg/id:p1/@points#range=…` | SequenceMerge |
| Text content | text node | `/svg/id:t1/text()` | delegated to text module word diff |
| `<style>` CSS | rule selector text (+ ordinal if duplicate) | `/svg/n:…/css/#logo` | KeyedMerge of rules, KeyedMerge of declarations |
| Embedded raster `data:` URI | singleton | `/svg/id:img1/@href/data` | recurse into registry (PNG, JPEG, …) |
| Comments, PIs, DOCTYPE | position | in child sequence | SequenceMerge as leaves |

### 3.1 Matching elements without IDs

Many generators (Figma, hand-written, optimizer output) emit no IDs. Within each parent, after matching
ID-bearing children:

1. Exact match by subtree hash (canonical form, section 4) via histogram diff of the child list.
2. Remaining children: pair by tag, then a similarity score = 0.5·attribute-set Jaccard (names and values) +
   0.3·geometry similarity (bbox IoU computed from shape attributes / path bounds, no rendering) +
   0.2·descendant hash Dice. Hungarian assignment, accept ≥ 0.6 (`--svg-match-threshold`). Pairs record
   `confidence`. Auto-merge of a paired element requires ≥ 0.8; below that, edits on both sides become a
   `low-confidence-match` conflict.
3. The identity key `n:<hash8>` is derived from the base element's canonical hash (or from ours' for
   elements absent in base), so addresses are stable within one comparison. Deterministic tie-break by
   document order.

`inkscape:label` (layer and object names) is used as an extra strong signal (adds 0.3, capped at 1.0)
because Inkscape users rename objects but Inkscape regenerates IDs only on duplicate/paste.

### 3.2 Dependencies

Every reference is resolved at load into an edge `from element/attribute → target id`. A change's
`depends_on` lists target addresses. Rules after merge (SPEC 5.3):

- Side X deletes `#g1`; side Y adds or keeps a reference to `#g1` in an element it changed: `dependency` conflict.
- Side X renames an ID (detected as: an element matched structurally whose `id` changed, plus all its
  references rewritten on that side): recorded as an `update` of the `id` attribute with
  `metrics.rename = true`. Theirs' new references to the old ID are rewritten to the new ID in the merged
  output (`auto: reference-rewritten`). If both sides rename the same ID differently: `both-modified`.
- Gradient template chains (`href` between gradients) and `use` chains are followed for cycle detection; a
  merge that creates a cycle is a `dependency` conflict.
- Dangling references that already exist in base are not reported as new problems (warning only).

### 3.3 ID collisions on add-add

Inkscape numbers IDs per document (`rect1234`, `linearGradient5678`), so two designers adding objects
from the same base often produce the same new ID for different things.

- Same new ID, canonical subtree equal: one element (identical insert).
- Same new ID, different content: default `--svg-id-collision=rename`. Theirs' element is renamed to
  `<id>-t<n>` with the smallest `n ≥ 1` that is unused in the merged document; every reference *originating
  in theirs-inserted or theirs-modified content* is rewritten. Reported as `auto: id-renamed` with old/new
  IDs. `--svg-id-collision=conflict` reports `add-add` instead.
- Theirs' references inside `<style>` (`#id` selectors) are rewritten the same way when the rule was added
  or changed by theirs.

### 3.4 Invariants after merge

IDs unique; every `url(#…)`/`href="#…"` added or changed by the merge resolves; one root `svg`;
namespaces declared for every prefix used; `defs` content referenced or explicitly kept (no garbage
collection, Splinter never deletes unreferenced defs).

## 4. Normalization

For comparison only; serialization re-emits untouched elements from original bytes (xml module span preservation).

- XML level (from `xml` module): attribute order, namespace prefix choice, quote style, insignificant
  whitespace between elements outside `text`, empty-element form, character references.
- Numbers in geometry attributes and path data: parsed to double and compared with `--tolerance`
  (default exact after parsing, so `1.0` == `1` == `1e0`, but `1.00001` != `1`).
- Colors: `#fff`, `#ffffff`, `white`, `rgb(255,255,255)` compare equal. `currentColor`, `none`, and
  paint-server URLs are distinct values.
- `style` parsed to a property map; whitespace and trailing semicolons ignored; property order ignored.
- `transform`: compare by resulting matrix within tolerance (`translate(10)` == `matrix(1,0,0,1,10,0)`),
  but report the textual form.
- Path data: parsed to absolute commands with implicit repeats expanded, for comparison only.
- **Editor state** (classed `editor-state`, hidden from diff by default, `--svg-show-editor-state` shows it):
  `sodipodi:namedview` attributes `inkscape:zoom`, `inkscape:cx`, `inkscape:cy`, `inkscape:window-*`,
  `inkscape:current-layer`, `inkscape:pageopacity`, `showgrid`; `sodipodi:docname`; `inkscape:version`;
  `inkscape:export-filename`/`export-xdpi`/`export-ydpi`; RDF `dc:date`; Illustrator `i:pgf` and
  `x:xmpmeta` metadata timestamps; `data-name` duplicates of `id`.
  In a merge these are KeyedMerged like anything else, but a both-modified conflict on editor state is
  resolved ours-wins and reported as `auto: editor-state` instead of a conflict.
- Never normalized: presentation attribute vs `style` property (different CSS precedence), element
  order (z-order), text content, `id` values, unknown-namespace attributes.

## 5. Diff

Change kinds: `element` (insert/erase/move), `attr`, `style`, `class`, `transform`, `path`, `text`,
`css`, `id-rename`, `reference`, `editor-state`, `embedded` (recursive raster diff).

Metrics: for geometry, bbox before/after and translation delta; for paths, subpaths/segments
inserted/removed/changed and max point displacement; for colors, ΔE00 between old and new fill/stroke;
for moves, old and new index and parent.

### 5.1 `dump()` layout

Canonical pretty-printed SVG-like outline, one element per line, attributes sorted, style properties
sorted, path data on continuation lines one subpath per line, numbers in shortest round-trip form:

```text
svg viewBox="0 0 512 512" width=512 height=512
  defs
    linearGradient id:gradSky x1=0 y1=0 x2=0 y2=1
      stop offset=0 style{stop-color:#88ccff}
  g id:layer1 inkscape:label="Background" inkscape:groupmode=layer
    rect id:rect12 x=0 y=0 width=512 height=512 fill=url(#gradSky)
    path id:hill style{fill:#3a7d2c;stroke:none}
      d: M0,400 C128,320 256,360 512,300 L512,512 L0,512 Z
```

Anonymous elements show `n:<hash8>` instead of `id:`. Editor state is omitted unless requested.

### 5.2 Visual output

`--visual` rasterizes base and other side (and merged in merge mode) at the SVG's intrinsic size (clamped
to `--svg-render-max=2048` px on the long edge), then runs the PNG module's pixel diff to produce a
heatmap PNG. Changed elements' bounding boxes are outlined (computed by the rasterizer if it exposes
bounds, else from our geometry). Text rendering depends on installed fonts, so visual output is
explicitly non-deterministic across machines and is excluded from determinism guarantees and golden tests
(golden tests use font-free fixtures).

### 5.3 Cost

Tree diff is O(n log n) per child list with hashing; Hungarian matching runs only on unmatched leftovers
per parent and is capped at 200×200 per parent (beyond that: greedy best-first, reported in stats).
Files with 100k+ elements (maps, CAD exports) remain fine; path data diff is linear in segments.

## 6. Merge

1. Load all three, resolve references, match elements (3.1).
2. Recursive merge from the root: attributes by KeyedMerge, children by SequenceMerge with moves.
   A moved element (reparented or reordered) plus attribute edits on the other side merges.
3. `style`: KeyedMerge of properties. Output keeps ours' property order, theirs-only properties appended
   per SPEC ordering rule. If one side moved a property from `style` to a presentation attribute and the
   other changed its value: conflict `both-modified` (cannot tell which precedence wins).
4. `class`: token set merge (add/remove tokens independently).
5. `transform`: scalar. Both changed differently: `both-modified` (no matrix composition; that would
   invent a transform neither made).
6. Path `d`: if only one side changed, take it (byte-exact text from that side). If both changed:
   split into subpaths (at `M`/`m`, with relative `m` resolved to absolute for alignment) and SequenceMerge
   subpaths by normalized content; subpath-disjoint edits merge, and the output is re-serialized from
   the merged absolute command list in the number style of ours (precision = max decimals observed in
   ours). `--svg-path-merge=segments` goes further, aligning segments within a subpath (opt-in, because a
   moved node changes two adjacent segments and tangent continuity can break). `--svg-path-merge=none`
   treats `d` as a scalar.
7. Text content: text module word-level three-way merge on each text node; `tspan` structure via children merge.
8. `<style>` CSS: parse with a minimal CSS tokenizer (rules, at-rules kept opaque); KeyedMerge by selector
   text, then declarations. Unparseable CSS falls back to OpaqueLeaf for the whole `<style>` text.
9. Embedded `data:` images: decoded and merged by their format module; re-encoded as base64 with the
   original MIME type and line wrapping (unchanged payloads keep their original text, §7.1). External `href` files are not followed.
10. Dependency checks and ID collision handling (3.2, 3.3), then invariant check (3.4).

Conflict reasons: `both-modified`, `modify-delete`, `add-add`, `move-move`, `dependency`,
`low-confidence-match`, `id-collision` (only with `--svg-id-collision=conflict`).

### 6.1 `--conflict=mark`

For an element-level conflict (`both-modified` on the element or its subtree, `modify-delete`, `add-add`):

```xml
<g id="splinter-conflict-1a2b3c4d" data-splinter-conflict="x-1a2b3c4d…"
   inkscape:groupmode="layer" inkscape:label="CONFLICT x-1a2b3c4d">
  <g id="splinter-ours-1a2b3c4d" inkscape:label="ours" data-splinter-side="ours">…ours element…</g>
  <g id="splinter-theirs-1a2b3c4d" inkscape:label="theirs" data-splinter-side="theirs"
     display="none">…theirs element, IDs suffixed -splinter-theirs…</g>
</g>
```

- Theirs' copy has all IDs inside it suffixed `-splinter-theirs` and its internal references rewritten,
  so the document stays valid; external references keep pointing to ours.
- Theirs is hidden (`display="none"`) so the rendering equals the `ours` policy, and the designer toggles
  it in Inkscape's Layers panel or Illustrator's Layers list (the group is an Inkscape layer).
- `modify-delete`: the surviving version is wrapped, with an empty sibling group for the deleting side.
- Conflicts inside `<defs>`, on the root `svg`, inside `text` content, or on attributes of elements whose
  parent cannot contain `g` (e.g. `stop` inside a gradient, `fe*` inside `filter`): the conflict is lifted
  to the nearest ancestor that can be wrapped (a gradient is wrapped as a whole inside `defs`, which is
  legal), with the theirs copy renamed. If no such ancestor exists (root attributes), fall back to ours with a warning.

### 6.2 Patch payloads

Text payloads: XML fragments for element insert/update (canonical serialization of the subtree), attribute
values as plain text, path changes as subpath lists. No binary payload except embedded images (delegated).
Fuzz = matcher threshold when locating the target element.

## 7. Serialization and preservation

- The `xml` module preserves original byte spans per node. Untouched elements, comments, whitespace, the
  XML declaration, DOCTYPE, and CDATA are re-emitted byte-exact.
- Changed attributes are rewritten in place keeping the original attribute order; new attributes append
  at the end; quoting follows ours' style. New elements are serialized with the indentation of the
  preceding sibling.
- `style` attribute changes are re-serialized as `prop:value;prop:value` in ours' order (Inkscape style).
- Namespaces used by theirs-only content are declared on the root if missing (same prefix if free, else `ns1…`).
- `.svgz`: decompressed through zlib with the decompressed-size limit. If the merged XML bytes equal ours',
  ours' `.svgz` bytes are copied verbatim; otherwise the XML is recompressed with ours' gzip header fields
  (mtime and name preserved from ours) and a deflate level inferred from ours' stream (§7.1). Byte-exact
  reproduction of a changed stream is not attempted.
- Lost: nothing for untouched content. Merged path data loses ours' original command style when
  re-serialized (warning `path-reserialized`).

### 7.1 Envelope fields

**Layer stack:** `[.svgz: gzip member (header, deflate, CRC-32 + ISIZE)] → character encoding (UTF-8, or UTF-16
with BOM, or a declared legacy encoding) → XML (entities, character references, CDATA) → SVG tree → attribute
micro-syntaxes (path data, transforms, colors, CSS)`. Embedded payloads add their own layers: `data:` URIs
(`;base64` or percent-encoded) wrap nested files that recurse into their format module, and Illustrator `i:pgf`
is base64 over Illustrator's private compressed data (OpaqueLeaf, never decoded).

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| gzip header: ID, CM = 8, FLG, MTIME, XFL, OS, optional FNAME/FCOMMENT/FEXTRA/FHCRC | encoding (MTIME, FNAME: volatile) | magic and CM; FHCRC (CRC-16 of the header) when present | copied from ours' header (MTIME and name preserved from ours, not zeroed); MTIME 0 and no name only for a new file |
| gzip deflate data | encoding | inflate within `--max-memory` (the decompressed-size limit) | `raw` when the decoded XML bytes equal ours'; else re-deflated (warning `encoding-reencoded`) with the level XFL names (2 = 9, 4 = 1, else zlib's default); trial compression is later |
| gzip trailer CRC-32 (of the uncompressed data) and ISIZE (length mod 2³²) | integrity | checked, with FHCRC; a mismatch loads, sets `inspect` integrity to `mismatch`, and warns `integrity-mismatch-input` in merges | `crc32(xml)`, `size_of(xml)` |
| Multi-member gzip | encoding | concatenated members decoded as one stream | written as one member (warning `gzip-members-joined`) |
| BOM, XML declaration `encoding`, character references, entity use, CDATA vs escaped text | encoding | decodes cleanly | from ours' spans (xml module); new text in ours' encoding |
| `data:` URI base64 (line wrapping, padding) or percent-encoding | encoding | not decoded in v1 (the attribute is one opaque value) | v2: `raw` when the decoded payload is unchanged; else `encode(base64, …)` with ours' MIME type and wrapping width |
| `i:pgf` and other Illustrator private data | derived (Illustrator's native copy of the document) | none | never merged below the element (two-sided changes conflict `dependency`); warning `derived-stale` when both sides changed the document |
| `d` under an Inkscape path effect (`inkscape:original-d` + `inkscape:path-effect`) | derived | none | the element is never merged attribute by attribute: two-sided changes conflict `dependency` (§10) |
| Editor state (§4: `sodipodi:namedview` view attributes, `inkscape:version`, `sodipodi:docname`, export hints) | volatile | none | left out of hashes, so an element whose only change is editor state keeps ours'; inside a merged element KeyedMerged, a two-sided change keeps ours, noted `editor-state`. RDF `dc:date` and Illustrator metadata timestamps are element text and still compare as content in v1 |

Plain SVG has no framing (no lengths, counts or offsets) and no integrity fields. The only integrity fields are
the gzip CRC-32 and ISIZE of `.svgz`.

**Raw copy and layout stability.** Untouched elements are re-emitted from their original byte spans (§7), so a
change touches only the bytes of the changed nodes. For `.svgz`, an unchanged document is copied byte for byte;
a changed one is fully recompressed (deflate gives no partial reuse), which is reported as `encoding` churn.

**Signatures under `--signatures=auto`.** SVG has no signature mechanism of its own. A W3C XML Signature
(`ds:Signature`) inside an SVG is handled like generic XML (ENVELOPES.md): verified when possible (C14N via the xml
module), and **stripped** with `signature-stripped`, because no SVG consumer rejects an unsigned file. (v1: the xml
module has no `strip` yet, so the signature is kept with `signature-invalidated`.) The keyed
boundary is the `ds:SignatureValue` over the canonicalized `ds:SignedInfo`; the `ds:Reference` digests inside
`SignedInfo` are unkeyed but covered by the signature, so they are not recomputed alone. `--signatures=keep`
retains it with `signature-invalidated`. Embedded `data:` payloads apply their own format's policy.

**Expected churn.** A fill color edit in a 40 KiB Inkscape SVG: the attribute value bytes (`content`) and nothing
else; an Inkscape save on one side typically adds `volatile` churn in `sodipodi:namedview`. The same edit in a
`.svgz`: the whole compressed body after the first changed byte is `encoding` (in practice nearly the whole file),
plus 8 bytes of CRC-32 and ISIZE (`integrity`). Re-encoding a `data:` PNG after a merged pixel edit changes the
base64 text from the first changed decoded byte onward (`encoding`), with the pixel change itself as `content`.

## 8. Validation

Re-parse; check invariants 3.4 (unique IDs, resolvable references, declared namespaces, no reference
cycles); check that `svg-render` (if enabled) renders without error. CI-only external validators: render
with resvg CLI and Inkscape `--export-type=png` on fixture outputs and compare with our rasterizer
within tolerance; xmllint well-formedness.

## 9. Libraries

XML parsing and span-preserving serialization come from the `xml` module (see xml.md). Only the rasterizer
is SVG-specific.

| Candidate | Lang | License | vcpkg | Status (checked 2026-10-07) | Notes |
|---|---|---|---|---|---|
| lunasvg | C++ | MIT | `lunasvg` (3.5.0) | active, last push 2026-09, ~1.2k stars | Static SVG subset incl. CSS `<style>` basics and text (3.x). No filters, no animation. Depends on plutovg (MIT, in vcpkg). **Recommended default** |
| plutosvg | C | MIT | `plutosvg` (0.0.8) | active, last push 2026-05 | Aimed at OpenType-SVG glyphs (emoji). Too narrow for documents |
| resvg | Rust (C API) | Apache-2.0 (GitHub SPDX) | no port | very active (push 2026-10-07), best static-SVG conformance incl. filters and text | Requires a Rust toolchain in the build, breaks the pure CMake+vcpkg story. Optional `svg-render-resvg` later, or CI oracle via its CLI |
| nanosvg | C | Zlib | `nanosvg` | low activity | No `<style>` CSS, no text, limited feature set; fallback only |
| librsvg | Rust/C | LGPL-2.1+ | (GTK stack) | active | LGPL plus heavy GLib/Cairo dependencies; static linking obligations. Reject |

Recommendation: own SVG semantics on top of the `xml` module (path/transform/color/CSS-lite parsers are
small, S each); `lunasvg` behind `svg-render` for visual diff only (merge never depends on rendering).
Estimate: M for MVP merge, S for visual.

## 10. Hard cases and explicit non-goals

- DTD internal entities (Illustrator exports): the core forbids DTD entities. The SVG module allows a
  narrow exception: internal general entities whose replacement text contains no `&`, `<`, or `%`, at most
  64 entities and 4 KiB total, substituted at load; the DOCTYPE is preserved verbatim on write. Anything else
  is rejected as `invalid_input`. (Q-SVG-2)
- Inkscape path effects: `d` is derived from `inkscape:original-d` plus the effect; edits to
  `original-d` and `d` on different sides conflict (`dependency`), and `d` is never merged independently
  when `inkscape:path-effect` is present.
- `<switch>` with `systemLanguage` and Illustrator's `foreignObject` + `i:pgf`: `i:pgf` is OpaqueLeaf;
  if it changed on both sides it conflicts, because Illustrator reopens from it and ignores SVG edits.
  Warning in the report: "Illustrator private data present; Illustrator may ignore merged SVG content".
- Font-dependent text layout and `textLength`: no geometry reasoning for text.
- Animations (SMIL) and scripts: merged structurally, never executed.
- Non-goals: geometric merging (boolean ops, node-level tangent repair), CSS cascade evaluation, minification.

## 11. Test plan

- Fixtures: Inkscape 1.x documents (layers, path effects, clones), Illustrator exports (with and without
  `i:pgf`, entity namespaces), Figma/Sketch exports (no IDs, `data-name`), svgo-optimized output, SVGZ.
- Generated: random SVG trees with IDs and references for property tests.
- Golden: dumps and JSON reports for scenario pairs; `--conflict=mark` outputs (text, deterministic).
- Scenarios: both add `rect1234` with different content (rename and reference rewrite); gradient deleted
  vs gradient referenced; reorder vs attribute edit; subpath-disjoint `d` edits; style property merge;
  editor-state-only change produces empty default diff.
- Properties: round-trip byte-exact without changes; merge(base, x, base) == x; output always re-parses and
  resolves references.
- Fuzz: load over arbitrary bytes; path-data parser, transform parser, CSS tokenizer as separate targets.

## 12. Phases

| Phase | Scope | Effort |
|---|---|---|
| MVP | ID-based identity, structural matching, attribute/style/class merge, z-order SequenceMerge, references and dependency checks, ID rename on collision, editor-state normalization, dump, `--conflict=mark` | M |
| v2 | Subpath merge, `<style>` CSS rule merge, lunasvg visual diff, `data:` image recursion, entity exception, SVGZ | M |
| Later | Segment-level path merge, resvg backend, Inkscape path-effect awareness beyond conflict | S-M |

## 13. Open questions

- **Q-SVG-1**: In `--conflict=mark`, should theirs be hidden or visible? **Provisional:** hidden (`display="none"`). **Why:** rendering then equals the default `ours` policy, and editors expose the toggle.
- **Q-SVG-2**: Allow limited DTD internal entities for Illustrator files despite the global no-DTD rule? **Provisional:** yes, narrow bounded exception in the SVG module only. **Why:** many legacy Illustrator SVGs are otherwise unloadable, and the bounds remove expansion attacks.
- **Q-SVG-3**: Rename suffix for colliding IDs? **Provisional:** `<id>-t<n>`. **Why:** short, deterministic, still a valid XML name and CSS identifier.
- **Q-SVG-4**: Default path merge granularity? **Provisional:** subpaths. **Why:** independent shapes inside one path are common; segment merges risk broken tangents.
- **Q-SVG-5**: Treat presentation attribute and same-name style property as one key? **Provisional:** no. **Why:** different cascade precedence; merging them would change rendering.
- **Q-SVG-6**: Default rasterizer? **Provisional:** lunasvg via vcpkg. **Why:** MIT, maintained, pure C++; resvg's quality does not justify a Rust toolchain dependency for a visual-only feature.
- **Q-SVG-7**: Should editor-state both-modified be silent? **Provisional:** auto-resolve ours-wins but report as `auto: editor-state`. **Why:** satisfies "no silent best-effort" without blocking merges on zoom levels.
