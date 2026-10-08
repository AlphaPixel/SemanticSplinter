# Simple meshes (OBJ/MTL, PLY, STL) — Implementation Document

**Tier:** T3 · **Module:** `semantic-splinter-mesh` · **vcpkg feature:** `mesh` · **Depends on modules:** `text`, `binary`
**Extensions / magic:** `.obj` (text; lines starting `v `, `f `, `o `, `g `, `mtllib`), `.mtl` (`newmtl`), `.ply`
(first line `ply`, second `format ascii|binary_little_endian|binary_big_endian 1.0`), `.stl` (binary: 80-byte header +
uint32 triangle count, file size must equal `84 + 50·n`; ASCII: `solid` … `endsolid`).

This document also specifies **`geomcore`** (`src/formats/geomcore/`), a helper static library with no format registration, built with `mesh` and kept free of mesh-format types so other geometry formats can reuse it.

## 1. Why it matters

OBJ, PLY, and STL are everywhere: 3-D printing repos, scanned assets, research datasets, CAD exports, game
placeholder art. OBJ is text, but a line diff of 200k `v` lines is useless: a single re-export renumbers or
re-precisions everything, and a small sculpt edit shows as thousands of changed lines with no sense of "how much did
the shape change". STL and binary PLY just say "Binary files differ". Users want a **quantitative geometry diff**
(vertex/face counts, bounding box, Hausdorff distance, which region moved) and, for multi-object OBJ files, a merge
when different people changed different objects.

## 2. Format anatomy

- **OBJ:** `v x y z [w] [r g b]`, `vt`, `vn`, `vp`; faces `f v/vt/vn …` with **1-based global indices** into the
  pools, or **negative relative indices** (`-1` = last defined); `l`, `p`; grouping `o name`, `g name…`, `s` smoothing,
  `usemtl`, `mtllib`. Pools are global across the file: a face in object B may reference vertices defined before
  object A. Comments `#`, line continuation `\`.
- **MTL:** `newmtl name` blocks with `Ka/Kd/Ks/Ns/d/Tr/illum/map_*` keyed lines.
- **PLY:** header declaring `element <name> <count>` with `property <type> <name>` and `property list <ctype> <itype>
  <name>`, then body in ASCII or binary (LE/BE). Common elements: `vertex` (x,y,z,nx,ny,nz,red,green,blue,…),
  `face` (vertex_indices list). `comment` and `obj_info` header lines.
- **STL:** one unnamed (binary) or named (ASCII `solid name`) triangle soup; per triangle a normal and three vertices;
  binary has a 16-bit attribute word (abused for color by some tools). Pitfall: binary files whose header starts with
  `solid`; detect by the size equation, not the first word.

## 3. Semantic units and identity

| Format | Unit | Identity key | Address | Primitive |
|---|---|---|---|---|
| OBJ | object/group | `o`/`g` name (+ occurrence `~k`) | `/objects/name:Chair` | KeyedMerge |
| OBJ | object geometry | object | `/objects/name:Chair/geometry` | OpaqueLeaf (MVP), GridMerge on vertices (v2) |
| OBJ | material binding | object + face-run | `/objects/name:Chair/usemtl` | KeyedMerge |
| MTL | material | `newmtl` name | `/materials/name:Wood/Kd` | KeyedMerge over statements |
| PLY | element | element name | `/elements/vertex` | KeyedMerge |
| PLY | element rows | row index | `/elements/vertex/rows#range=s,n` | GridMerge `[rows][props]` when counts equal and topology unchanged |
| PLY | header comments/obj_info | content | `/header/comments` | SequenceMerge |
| STL | solid | single | `/solid` | OpaqueLeaf (diff only metrics) |

**The vertex renumbering problem (OBJ).** Faces reference global pools, so "object granularity" is only real if
the file's layout is **object-local**: each object's `v/vt/vn` lines precede its faces and its faces reference only
that block. Most exporters write this layout. The loader classifies the file:

- **Object-local:** every object owns a contiguous pool slice. Objects become independent units. On merge, an object
  is re-emitted with its own pool lines, and **face indices of every later object are shifted** by the change in
  preceding pool sizes. Positive indices are rewritten; negative (relative) indices need no rewrite, so they are
  preserved as written. Unchanged objects keep their text except for shifted face indices.
- **Shared pool:** faces reference vertices across objects. Objects are not independent; the whole geometry is one
  OpaqueLeaf for merge (only one side may change it). Diff still reports per-object metrics.

**Dependencies:** `usemtl` names depend on MTL materials (cross-file, reported only); `mtllib` file references.
**Invariants:** indices in range; PLY counts match body; STL count matches size. **Derived data:** STL facet normals (recomputable; differences only in normals are `kind: derived`),
PLY/STL counts.

## 4. Normalization

- Number spelling and precision in OBJ/ASCII PLY/ASCII STL: compared as floats with `--tolerance` (default 0,
  recommended 1e-6). Exporters vary precision; `--tolerance` makes re-exports diff as "no change".
- Whitespace, comments (OBJ/MTL/PLY comments are reported separately as `kind: comment`), line endings.
- STL: ASCII vs binary of the same triangles diff as `kind: storage`. Facet normal recomputation noise ignored by
  default (normals are derived).
- Triangle order and starting vertex of a triangle (rotation `abc` = `bca`): insignificant for STL and for the metric
  diff, significant for merge identity (we do not reorder output).
- Never normalized: vertex order in OBJ/PLY (it is the identity for GridMerge), object names.

## 5. Diff

**Metrics** (`geomcore`, per object/solid/element): vertex, face, edge counts; bounding box and its delta; centroid
shift; surface area; signed volume (closed meshes); boundary edge and non-manifold edge counts (watertightness
change matters for 3-D printing); symmetric **Hausdorff distance** and RMS surface distance; when vertex counts match
and topology is equal, per-vertex displacement max/mean and the count above tolerance, grouped into **regions**
(connected components over mesh adjacency of moved vertices) reported with bbox and max displacement.

`geomcore` algorithms: uniform grid (cell size from bbox and triangle count) over triangles for nearest-point
queries; one-sided distance computed for every vertex plus area-weighted samples (default 16k, deterministic
low-discrepancy sequence seeded by content hash so reports are reproducible); exact mode for small meshes.
Topology equality by hash of the face index arrays.

**`dump()`:** summary header per object (counts, bbox, area, volume, hash of positions and of topology), then
`--dump-data` lists vertices and faces in canonical form (fixed `%.6g`), so a textconv diff is either a short
summary diff or a full data diff on request.

**Visual (`--visual`):** SVG with three orthographic projections, base wireframe grey, changed regions red;
optional PLY output with per-vertex displacement as a color heatmap (viewable in any mesh viewer).
**Cost:** O(n log n); Hausdorff is skipped above `--max-geom-tris` (5M) with a note.

## 6. Merge

- **OBJ (object-local):** KeyedMerge of objects by name. Object geometry changed on both sides: MVP conflict
  `both-modified`; v2 GridMerge on vertex positions when both sides kept topology (same faces), with coupled
  `v`/`vn` and tolerance. Object order: SequenceMerge with ids. After merge, pools are re-laid out and positive face
  indices shifted.
- **OBJ (shared pool):** whole-geometry OpaqueLeaf; group/material statement changes still merge if geometry changed
  on one side only.
- **MTL:** KeyedMerge per material and per statement key; `map_*` options are values.
- **PLY:** vertex element GridMerge if counts and face lists equal on all sides; else one side wins wholesale, both
  sides changed counts: `shape-mismatch`. Header comments: SequenceMerge.
- **STL:** one side changed: take it. Both: `both-modified` (no meaningful sub-unit in a triangle soup).
- **Conflict reasons:** `both-modified`, `modify-delete`, `add-add`, `shape-mismatch`, `dependency` (face to deleted material).
- **`--conflict=mark`:** OBJ: theirs' conflicting object is added as `o <name>.splinter_theirs` (with a comment line
  naming the conflict id) next to ours, so both are visible in any viewer. PLY/STL: falls back to ours.
- **Patch payload:** OBJ object blocks as text; vertex regions and binary formats as `payload zstd+base85`.

## 7. Serialization and preservation

- OBJ/MTL: line-span preserving; untouched lines byte-exact, only shifted face lines rewritten (preserving the
  `v/vt/vn` slash pattern of each corner).
- PLY: header preserved byte-exact except counts; binary body rewritten with the same layout and endianness; ASCII
  rows untouched where unchanged.
- STL: whole file is one unit, so output is one input byte-exact.

### 7.1 Envelope fields

**Layer stacks.**
- OBJ/MTL: `text (UTF-8 or legacy 8-bit) → lines (\ continuation) → statements → global v/vt/vn pools + faces`.
- PLY: `header (format line, element counts, property types) → body (ASCII rows | binary LE | binary BE) → element rows`.
- STL: binary `80-byte header → u32 triangle count → 50-byte facets`; ASCII `solid … facet … endsolid`.

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| OBJ positive face indices (`f 12/7/12`) | framing (absolute index into a global pool) | `1 ≤ i ≤ pool size at that line` | rewritten through the pool-offset table (§3); negative indices are relative and never change |
| PLY `element <name> <count>` | framing | body row count matches (ASCII lines or binary bytes consumed); declared size checked against file size before allocation | `varfield(decimal, count_of(element rows))` |
| PLY list length prefix (`property list uchar int`) | framing | prefix fits in its declared type; row stays in body | `field(ctype, endian, count_of(list))`; a count that overflows `uchar` is trouble, the header type is never widened silently |
| PLY `format` line | encoding | one of the three encodings, version `1.0` | kept from ours; rows are re-encoded in that encoding |
| PLY `comment` / `obj_info` | content | none | merged (§6) |
| binary STL u32 triangle count | framing | `84 + 50·n == file size` (this is also the binary/ASCII probe) | `field(u32, LE, count_of(facets))` |
| STL facet normal | derived | recomputed from the vertices (right-hand rule) and compared within tolerance; mismatch is `kind: derived`, never an error | ours' value for untouched facets; recomputed for changed ones |
| binary STL attribute byte count (u16) | framing, or content when the file uses it for color (VisCAM/SolidView 15-bit color, detected when any facet is non-zero) | none | copied per facet with the facet |
| binary STL 80-byte header | volatile (generator text), or content when it carries `COLOR=`/`MATERIAL=` (Magics) *(verify)* | does not start with `solid` (warn if it does, size equation decides) | ours' header |

**Raw copy and layout stability.** OBJ, ASCII PLY, and ASCII STL are span-preserving: untouched lines are byte-exact,
and the only envelope rewrite is the face-index shift. Binary PLY and binary STL with unchanged counts are rewritten
in place (same row offsets).

**Signature policy (`auto`):** OBJ, PLY, and STL have none.

**Expected churn.** Moving 50 vertices in a binary STL or PLY: those 12-byte positions (`content`) and the affected
facet normals (`derived`); nothing else moves. Adding one vertex to the first object of an object-local OBJ: one new
`v` line (`content`) and every later positive face index (`framing`), which `diff` hides and `--show=framing` lists.

## 8. Validation

Reload; index bounds; counts. Optional CI: `tinyobjloader` and `happly` read OBJ/PLY outputs as oracles.

## 9. Libraries

| Library | vcpkg | License | Note (verified 2026-10-07) |
|---|---|---|---|
| tinyobjloader | `tinyobjloader` 2.0.0rc13 | MIT | test oracle |
| happly | `happly` 1.1.0 | MIT (GitHub) | test oracle |
| tinyply / miniply | `tinyply` 3.0 (BSD-2) / `miniply` (MIT) | — | alternatives, not needed |
| meshoptimizer | `meshoptimizer` 1.3 | MIT | not needed here |
| Assimp | `assimp` 6.0.5 | BSD-3 and others | rejected: lossy model, heavy |

**Recommendation:** write the readers ourselves (OBJ ~400 lines, PLY ~500, STL ~150) because span preservation and
byte-exact rewriting are required and the formats are tiny. `geomcore` ~1k lines.

## 10. Hard cases and explicit non-goals

- Re-export with a different vertex order: GridMerge impossible; diff falls back to metrics (Hausdorff still exact).
- OBJ files with negative indices mixed with positive; `\` continuations; non-triangular faces (kept as-is).
- Huge scan data (100M points): metrics sampled, `--max-memory` enforced before allocation from header counts.
- Non-goals: remeshing-aware matching, CAD formats (STEP/IGES), point-cloud registration (alignment before diff is a
  possible opt-in heuristic later).

## 11. Test plan

Generated meshes (cube, sphere, grid) with controlled edits: vertex displacement regions, added objects, material
changes, format conversions (ASCII/binary STL and PLY equivalence), object-local vs shared-pool OBJ; golden metrics
with tolerances; property tests (Hausdorff symmetry and zero for identical meshes, translation by d gives Hausdorff
d, index shifting round-trip); fuzz OBJ/PLY/STL parsers (counts bounded by file size before allocation).

## 12. Phases

- **MVP (M):** parsers, `geomcore` metrics, dump, OBJ object-level merge (object-local), MTL merge, STL/PLY diff.
- **v2 (S–M):** vertex GridMerge for OBJ/PLY, SVG/PLY heatmap visuals.
- **Later:** optional rigid alignment before diff (`--align`, heuristic).

## 13. Open questions

- **Q-MESH-1:** Merge OBJ objects when the pool is shared? **Provisional:** no, whole-geometry OpaqueLeaf. **Why:** index
  coupling across objects makes independent edits unprovable.
- **Q-MESH-2:** Default sample count and exact threshold for Hausdorff? **Provisional:** exact ≤ 200k triangles, else
  16k deterministic samples plus all vertices. **Why:** bounded runtime with reproducible reports.
- **Q-MESH-3:** Where does `geomcore` live? **Provisional:** `src/formats/geomcore/`, a helper static library without a
  format registration, built with `mesh`. **Why:** geometry metrics stay separate from file parsing, so a future
  geometry format reuses them without depending on the mesh module.
- **Q-MESH-4:** Treat STL facet normals as derived? **Provisional:** yes. **Why:** most tools recompute them and
  exporters disagree on rounding.
