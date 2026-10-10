# Dependencies

Third-party code comes only through vcpkg ports, at the versions fixed by `builtin-baseline` in `vcpkg.json`
(D-52). Bump the baseline deliberately and update this table in the same commit. Default dependencies must be
permissive (MIT, BSD, Apache-2.0, Zlib, public domain); copyleft libraries are opt-in features (D-19).

**Baseline:** vcpkg `b08975f4fdf01ad9a5c08db8f1953a57d11e8b26` (2026-09-25).

| Port | Version | License | vcpkg feature | Used for | Why this one |
|---|---|---|---|---|---|
| bzip2 | 1.0.8#6 (`default-features: false`) | bzip2-1.0.6 (BSD-style) | `tar` | `.tar.bz2` decode and re-encode | The reference implementation; tiny, no dependencies |
| catch2 | 3.16.0 | BSL-1.0 | `tests` | Unit and property tests | D-9: CTest discovery, readable property-style tests |
| expat | 2.8.5 | MIT AND CC0-1.0 | `xml` | XML tokenizer: byte index and count per event, UTF-8/UTF-16/ISO-8859-1 | D-26: exact spans for the splice writer; fuzzed; billion-laughs protection |
| liblzma | 5.8.4 | 0BSD (library; the LGPL tools are not built) | `tar` | `.tar.xz` decode and re-encode | containers.md §9: the reference xz implementation |
| libjpeg-turbo | 3.2.0#1 (`default-features: false`) | BSD-3-Clause AND IJG | `jpeg` | JPEG coefficient read/write (`jpeg_read_coefficients`, `jpeg_write_coefficients`), pixel decode for metrics and visuals; pulls libspng and zlib as port dependencies | jpeg.md §9: the only permissive library with coefficient access; no recompression |
| libpng | 1.6.58 | libpng-2.0 | `png` | PNG pixel decode (unfilter, Adam7, bit unpacking) | png.md §9: hardened and fuzzed; own chunk layer around it |
| nlohmann-json | 3.12.0#2 | MIT | core | JSON reports (`ordered_json`), `--resolve` files | Plan Phase 1: ordered output for deterministic reports |
| xxhash | 0.8.4 | BSD-2-Clause | core | XXH3-128 content hashes and change/conflict ids | D-8: speed; no adversarial collision requirement |
| zlib | 1.3.2#2 | Zlib | core | IDAT, zTXt, iCCP, iTXt inflate and deflate; parameter inference; ZIP deflate members; gzip | png.md §7.3: byte identity is pinned to this port (Q-PNG-4) |
| zstd | 1.5.7 | BSD-3-Clause OR GPL-2.0-only (BSD chosen) | core | patch payloads (D-7), ZIP method 93, `.tar.zst` | containers.md §9; the zstd module of D-7 |
