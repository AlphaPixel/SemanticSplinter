# Format Survey and Priorities

Which formats benefit from Git-integrated semantic diff and merge, and in what order to build them.
Each format has its own implementation document, listed below. Every document follows [TEMPLATE.md](TEMPLATE.md)
and uses the vocabulary in [../SPEC.md](../SPEC.md) (Address, Change, KeyedMerge, SequenceMerge, GridMerge, OpaqueLeaf).

## How formats were scored

| Criterion | Question |
|---|---|
| **Pain** | How often does this file live in Git repos and collide in merges today? |
| **Diff value** | How much better is a semantic diff than "Binary files differ" or a textconv dump? |
| **Merge feasibility** | Are there stable units, identities, and a lossless write path? |
| **Effort** | Parser/writer complexity plus dependency weight. |
| **Leverage** | Does it exercise or reuse a primitive or container that other formats need? |

## Tiers

- **T0, Foundation**: needed by the CLI or by other formats. Build first.
- **T1, Flagship**: high pain, feasible merge, and it proves the architecture.
- **T2, Strong**: clear value, built after T1.
- **T3, Diff-first**: valuable diff and report. Merge is limited, risky, or impossible.
- **T4, Opportunistic**: textconv/inspect only, or niche.

## Index (v1)

| Doc | Formats | Tier | Diff | Merge | Primary primitive | Notes |
|---|---|---|---|---|---|---|
| [text.md](text.md) | plain text, any line-oriented file | T0 | ✔ | ✔ | Sequence | GNU diff/diff3/patch compatibility baseline, reference plugin |
| [binary.md](binary.md) | any unrecognized file | T0 | ✔ | opt-in | Sequence (bytes) | Fallback. Byte-range report. Merge only with `--binary-merge` |
| [containers.md](containers.md) | ZIP, TAR(.gz/.zst/.xz/.bz2), JAR/APK/NUPKG | T0 | ✔ | ✔ | Keyed | Recurses into members |
| [xml.md](xml.md) | generic XML | T0 | ✔ | ✔ | Keyed + Sequence | Basis for SVG and other XML dialects |
| [json.md](json.md) | JSON, JSON5/JSONC (read) | T0 | ✔ | ✔ | Keyed + Sequence | Order- and format-preserving |
| [png.md](png.md) | PNG (APNG: default image only) | T1 | ✔ | ✔ | Grid + Keyed | First proof-of-concept |
| [svg.md](svg.md) | SVG, SVGZ | T1 | ✔ | ✔ | Keyed + Sequence | Entity identity and references |
| [csv.md](csv.md) | CSV, TSV | T1 | ✔ | ✔ | Keyed rows / Grid | Data tables, localization sheets |
| [jpeg.md](jpeg.md) | JPEG | T2 | ✔ | ✔* | Grid (MCUs) | *Lossless merge in the DCT domain |
| [raster-misc.md](raster-misc.md) | TIFF, BMP, TGA, GIF (incl. animated), WebP, QOI | T2 | ✔ | per-format | Grid | Shared raster core |
| [sqlite.md](sqlite.md) | SQLite databases | T2 | ✔ | ✔ | Keyed (rows by PK) | Writes through SQLite itself |
| [pdf.md](pdf.md) | PDF | T3 | ✔ | partial | Keyed + Sequence | Structure, text, annotations, forms; no rendering in v1 |
| [mesh-simple.md](mesh-simple.md) | OBJ/MTL, PLY, STL | T3 | ✔ | partial | Sequence/Grid | Geometry metrics |

## Recommended order

T0 (text, binary, containers, xml, json), then PNG, Git integration, SVG, CSV, JPEG, raster-misc, SQLite,
PDF, and simple meshes. See [../PLAN.md](../PLAN.md).

## Cross-cutting observations

- **Containers compose.** Implementing ZIP, XML, and PNG well makes archives of images and data diffable
  member by member, and APK/JAR inspection comes almost for free.
- **Grid is the universal raster primitive.** Pixels, JPEG MCUs, and CSV cell grids all go through `GridMerge`.
  Invest in it early, with region grouping, guard bands, and tolerance.
- **Envelopes are everywhere.** Chunk lengths, absolute offsets, CRCs, digests, signatures, and compressed
  streams make byte diffs useless and naive writes invalid. Every format inventories them (section 7.1, and
  [../ENVELOPES.md](../ENVELOPES.md)) and writes through the shared fix-up Emitter (SPEC §12).
- **Derived data is everywhere.** Thumbnails, PDF appearance streams, and reduced-resolution images. The rule is
  never merge, then regenerate or mark stale (SPEC §5.3).
- **Lossy formats merge in their coded domain or not at all.** JPEG merges whole MCUs without re-encoding.
  Anything finer requires re-encoding, which is generational loss and therefore opt-in.
