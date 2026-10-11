# Dependencies

Third-party code comes only through vcpkg ports, at the versions fixed by `builtin-baseline` in `vcpkg.json`
(D-52). Bump the baseline deliberately and update this table in the same commit. Default dependencies must be
permissive (MIT, BSD, Apache-2.0, Zlib, public domain); copyleft libraries are opt-in features (D-19).

**Baseline:** vcpkg `b08975f4fdf01ad9a5c08db8f1953a57d11e8b26` (2026-09-25).

| Port | Version | License | vcpkg feature | Used for | Why this one |
|---|---|---|---|---|---|
| bzip2 | 1.0.8#6 (`default-features: false`) | bzip2-1.0.6 (BSD-style) | `tar` | `.tar.bz2` decode and re-encode | The reference implementation; tiny, no dependencies |
| catch2 | 3.16.0 | BSL-1.0 | `tests` | Unit and property tests | D-9: CTest discovery, readable property-style tests |
| expat | 2.8.5 | MIT AND CC0-1.0 | `xml`, `svg` | XML tokenizer: byte index and count per event, UTF-8/UTF-16/ISO-8859-1 | D-26: exact spans for the splice writer; fuzzed; billion-laughs protection |
| liblzma | 5.8.4 | 0BSD (library; the LGPL tools are not built) | `tar` | `.tar.xz` decode and re-encode | containers.md §9: the reference xz implementation |
| libjpeg-turbo | 3.2.0#1 (`default-features: false`) | BSD-3-Clause AND IJG | `jpeg` | JPEG coefficient read/write (`jpeg_read_coefficients`, `jpeg_write_coefficients`), pixel decode for metrics and visuals; pulls libspng and zlib as port dependencies | jpeg.md §9: the only permissive library with coefficient access; no recompression |
| libpng | 1.6.58 | libpng-2.0 | `png` | PNG pixel decode (unfilter, Adam7, bit unpacking) | png.md §9: hardened and fuzzed; own chunk layer around it |
| nlohmann-json | 3.12.0#2 | MIT | core | JSON reports (`ordered_json`), `--resolve` files | Plan Phase 1: ordered output for deterministic reports |
| qpdf | 12.4.0 (`default-features: false`) | Apache-2.0 AND MIT | `pdf` | PDF object model, xref repair, content-stream parser, permission-only decryption, form appearance generation, `--layout=compact` rewrite (QPDFWriter); pulls libjpeg-turbo and zlib, native crypto (no OpenSSL) | pdf.md §9: the permissive choice (MuPDF is AGPL, Poppler GPL); the incremental writer is our own. Apache-2.0: ship its NOTICE |
| sqlite3 | 3.53.4#1 (`default-features: false`; features `fts5`, `json1`, `rtree`, `session`) | blessing (public domain) | `sqlite` | All database reads and writes (`sqlite3_deserialize` copies, schema and row queries, the merge writer) | sqlite.md §9: the reference implementation; the features let databases with FTS5, R*Tree, and JSON open; `session` is for Q-SQLITE-1 |
| xxhash | 0.8.4 | BSD-2-Clause | core | XXH3-128 content hashes and change/conflict ids | D-8: speed; no adversarial collision requirement |
| zlib | 1.3.2#2 | Zlib | core | IDAT, zTXt, iCCP, iTXt inflate and deflate; parameter inference; ZIP deflate members; gzip | png.md §7.3: byte identity is pinned to this port (Q-PNG-4) |
| zstd | 1.5.7 | BSD-3-Clause OR GPL-2.0-only (BSD chosen) | core | patch payloads (D-7), ZIP method 93, `.tar.zst` | containers.md §9; the zstd module of D-7 |
| giflib | 6.1.3 | MIT | `gif` | GIF LZW decode (`DGifSlurp`, de-interlaced) | raster-misc.md §9, Q-RAST-4: decoder only; our own block parser and LZW writer keep output byte-stable |
| tiff (libtiff) | 4.7.2 (`default-features: false`, feature `zip`) | libtiff (BSD-like) | `tiff` | TIFF strip and tile decode (LZW, Deflate, PackBits, predictors) | raster-misc.md §9: hardened decoder; JPEG, LZMA, ZSTD, WebP and LERC codecs left out to stay light, so such pages are opaque units. Strips are encoded by our own LZW/PackBits and zlib, byte identity pinned to the `zlib` port |
| libwebp | 1.6.0#3 (`default-features: false`) | BSD-3-Clause | `webp` | VP8/VP8L decode, lossless re-encode (`method=6, quality=100, exact=1`, Q-RAST-5) | raster-misc.md §9; `libwebpmux` and SIMD are not built (our own RIFF writer). Lossless re-encode bytes are pinned to this port and feature set; on macOS x64 the port forces `simd`, which may change them |

**Vendored (not a port):** `src/formats/sqlite/cksumvfs.c` from SQLite 3.53.4 `ext/misc/cksumvfs.c`, public domain
(blessing), unmodified. Used for its page checksum function (Q-SQLITE-7). Update it together with the `sqlite3` port.
