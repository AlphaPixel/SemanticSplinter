# Binary (fallback) — Implementation Document

**Tier:** T0 · **Module:** `semantic-splinter-binary` · **vcpkg feature:** none (mandatory, no dependencies beyond core xxHash) · **Depends on modules:** none (uses `src/core/diff` from text.md)
**Extensions / magic:** none. Chosen when nothing else probes and the text test of SPEC §11 step 4 fails, or with `--format=binary`.
**Status (v1 = MVP + `--binary-merge`, D-49, Phase 2):** done: GNU "differ" line and exit status, byte-range
report, chunked hex dump, OpaqueLeaf merge, and opt-in `--binary-merge` with guard band and size rule. Not yet:
binary patch payloads (`--binary` on unknown binaries exits as unsupported), `--scan-streams` (D-49), and the
`later` row of §12.

## 1. Why it matters

Every file Splinter does not understand still flows through it, as a Git driver or as `splinter diff`. Today such files show up as `Binary files a and b differ` and nothing else. The binary module should do strictly better without pretending to understand the content:

- Report **which byte ranges** changed, how many bytes were inserted, deleted, or replaced, and whether the change looks like an in-place patch (same size, few ranges) or a restructuring (shifted content).
- Produce a stable, diff-friendly hex `dump()` for textconv in which a 3-byte insertion changes a few lines, not every line after it.
- Keep exact GNU behaviour where scripts depend on it (`-q`, exit status, the message text).
- Offer a byte-level merge only behind `--binary-merge`, because for most binary formats a byte merge produces a corrupt file (internal offsets, checksums, compression).

## 2. Format anatomy

None. The input is an opaque byte string of length N (bounded by `--max-input`, default 2 GiB). The module is memory-mapped where the platform allows it (`ByteSource` mmap path), otherwise read into one buffer.

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Chunk (content-defined) | XXH3-64 of chunk bytes | internal only, never in reports | SequenceMerge (anchor level) |
| Byte range | offset in the side the change refers to | `/bytes#range=OFFSET,LENGTH` | SequenceMerge over bytes (refinement level) |
| Whole file | XXH3-128 | `/` | OpaqueLeaf (default merge behaviour) |

No dependencies, no invariants known. That lack of knowledge is exactly why merge is opt-in.

## 4. Normalization

None. Bytes are bytes. Options that make sense for text (`-i`, `-w`, ...) are rejected with a usage error in binary mode unless `-a` forces text treatment, in which case the text module handles the file.

## 5. Diff

### 5.1 Algorithm: anchors, then exact refinement

1. **Fast paths.** Equal size and equal XXH3-128: identical. One side empty: one insert or erase. Both under 64 KiB: skip to step 4 on the whole buffers.
2. **Content-defined chunking (CDC).** FastCDC style gear hash: `h = (h << 1) + GEAR[byte]` with a fixed 256-entry table of 64-bit constants (generated once from a fixed seed and checked in, so results are deterministic across platforms). A boundary is declared where `h & mask == 0`, with normalized chunking (a stricter mask before the average size, a looser one after). Parameters: min 2 KiB, average 8 KiB, max 64 KiB. These scale up (min 16 KiB, average 64 KiB, max 256 KiB) for inputs over 256 MiB to bound the chunk count. Because boundaries depend only on local content, an insertion re-synchronizes boundaries within about one chunk.
3. **Anchor diff.** Each chunk becomes its XXH3-64 hash. The two hash sequences are diffed with `seqdiff` histogram (the core engine). Equal chunks are anchors. Hash equality is confirmed by `memcmp`, so collisions cannot produce a false match.
4. **Exact refinement.** For each non-anchor region pair (a gap between anchors on both sides): trim the common byte prefix and suffix; if both remainders are at most 64 KiB, run `seqdiff` Myers on bytes (class IDs are the byte values, so no hashing) with the cost limit; otherwise report the remainder as one replace range. The refinement step is what turns "chunk 17 differs" into "4 bytes at 0x1A2F0 changed".
5. **Coalescing.** Ranges closer than `--gap` bytes (default 8) merge into one reported range, to avoid reporting every other byte of a changed integer table as separate changes.

Complexity is O(N) for chunking and hashing plus histogram over N/8 KiB elements, plus bounded refinement. A 2 GiB file pair with sparse edits diffs in a few seconds and needs memory for the two inputs (mapped) plus about 16 bytes per chunk.

### 5.2 Changes and metrics

Each change: `op` = insert, erase, or update (replace), `at` = `/bytes#range=` in the new side (for erase: in the old side, with `before` set), `before`/`after` values carrying hashes and, for ranges up to 64 bytes, the hex text. Report-level metrics: `bytes_inserted`, `bytes_deleted`, `bytes_replaced`, `ranges`, `size_a`, `size_b`, `anchor_coverage` (fraction of B covered by anchors; low coverage means "the file was recompressed or re-encrypted and nothing useful can be said"), and `in_place` (true when sizes are equal and only updates exist).

When `anchor_coverage < 0.1` the human report says so explicitly instead of listing hundreds of ranges: `Binary files a and b differ (no common content found; compressed or encrypted?)`. (The `--scan-streams` hint returns when that option ships, post-v1, D-49.) This is a typical outcome for ZIP-like formats that are not recognized, and the message tells the user to check whether a format module exists.

### 5.3 GNU compatibility

| Situation | GNU output (we match) | Exit |
|---|---|---|
| `diff a b`, either input binary, contents differ | `Binary files a and b differ` | 1 |
| `diff -q a b`, any files differ | `Files a and b differ` | 1 |
| identical | nothing | 0 |
| `diff3` with a binary input | GNU reports trouble; we print `diff3: FILE: binary file` style diagnostics to stderr unless `-a` (verify exact wording against GNU in the golden corpus) | 2 |
| Git diff driver | `Binary files a/x and b/x differ` is Git's own text and is not emitted by us; in git-diff mode we emit the range report | 0 |

An earlier draft used "Binary files differ" for `-q`. GNU actually prints `Files X and Y differ` under `-q` and uses the `Binary files X and Y differ` text only without `-q` (believed from GNU diffutils behaviour; confirmed by the golden corpus generator before release). Splinter's own richer report replaces the GNU line only when the user asks for it (`--json`, `--dump`, `-u` with `--binary-detail`), so plain `splinter diff a b` stays GNU-identical by default (Q-BIN-1).

### 5.4 `dump()` layout (textconv)

Diff-friendly hex: one header line per content-defined chunk, then hex rows prefixed with offsets **relative to the start of their chunk** (Q-BIN-5), so an insertion only changes the lines of the chunks it touches.

```text
# binary 1048576 bytes xxh3:7c1e...
## chunk 0 8231 bytes xxh3:a19f...
89504e470d0a1a0a0000000d49484452  |.PNG........IHDR|
00000400000003000806000000a1b7c3  |................|
...
## chunk 1 9012 bytes xxh3:03be...
```

Rows are 16 bytes, lowercase hex, then an ASCII column. `--dump-offsets` adds absolute offsets (like `xxd`) for humans who do not care about diff stability. Dumps over `--max-dump` (default 1 MiB of input) print only the chunk header lines, which still give a meaningful textconv diff ("chunks 40 to 43 changed").

### 5.5 Visual output

None in MVP. Later: a byte-map PNG (one pixel per N bytes, colored by change type), useful for large assets.

## 6. Merge

Default: **OpaqueLeaf**. If only one side changed the file, take it. If both changed it identically, take it. Otherwise conflict `both-modified` at `/`, output per `--conflict` policy (`ours` by default). `--conflict=mark` has no meaningful marking and falls back to `ours` with a warning (SPEC §5.4).

With `--binary-merge` (opt-in):

1. Compute base→ours and base→theirs range diffs as in §5.1.
2. Run diff3 chunking over base byte offsets (SequenceMerge with bytes as elements and ranges as edits). Non-overlapping, non-adjacent ranges merge. Adjacent ranges (touching with zero stable bytes between them) conflict, as in text.
3. A guard of `--guard=N` bytes (default 0, D-10) is applied: ours and theirs ranges closer than N bytes conflict as one (`reason: guard-band`); ranges within 16 bytes warn `adjacent-edits`. Nearby byte edits in binaries are very often one logical edit (a multi-byte integer, a length-prefixed string).
4. Every successful byte merge carries the validation warning `binary-merge-unverified` and the human output prints a prominent warning: `WARNING: byte-level merge of an unrecognized format. Internal offsets, checksums, and compression are not understood. Verify the result.`
5. If either side changed the file size and the other side has any edit after the first size-changing edit, the merge refuses with conflict `dependency` (reason: offsets after an insertion likely shifted). `--binary-merge=force` disables this check. This rule catches the most common corruption pattern (a table of absolute offsets after an inserted blob).

Patch payload (`--binary` in diff mode): per change, `base_offset`, `delete_length`, inserted bytes, plus `xxh3` of the deleted bytes for verification. Encoded with the default zstd+base85 envelope. Applying uses the same merge path: a hunk applies if its deleted bytes are found at `base_offset` plus the running offset, or, with fuzz, at the nearest position where the deleted bytes and up to 32 bytes of recorded leading context match exactly (scan ±64 KiB).

## 7. Serialization and preservation

Output is a byte concatenation of ranges from the inputs. Trivially byte-exact. Nothing is lost or re-encoded.

### 7.1 Envelope fields

**Layer stack:** `file → bytes`. The envelope is unknown, so every byte is `content` by default and nothing is regenerated on write. The one exception is **`--scan-streams`** (SPEC §12.2), which looks through embedded compressed streams in diff mode:

| Signature searched | Accepted when | Envelope classes recorded |
|---|---|---|
| zlib: `CMF` with CM = 8 and CINFO ≤ 7, and `(CMF·256 + FLG) mod 31 = 0`, FDICT clear | inflate ends cleanly and the Adler-32 trailer matches | 2-byte header `framing`, deflate data `encoding`, Adler-32 `integrity` |
| gzip: `1F 8B 08`, reserved flag bits clear | inflate ends cleanly and CRC-32 and ISIZE match | header `framing` (MTIME `volatile`), deflate data `encoding`, trailer `integrity` |
| zstd frame: `28 B5 2F FD` (skippable frames `5x 2A 4D 18` are skipped as `framing`) | frame decodes completely; XXH64 matches when present | header `framing`, blocks `encoding`, checksum `integrity` |
| LZ4 frame: `04 22 4D 18` | frame decodes; header checksum byte and optional content/block checksums match | descriptor `framing`, blocks `encoding`, checksums `integrity` |

Raw deflate and LZ4 block format have no signature and are not scanned. Trial decoding runs inside the shared `Limits` budget (`--max-zip-ratio`-style ratio cap and total output cap) and a cap of 4096 candidate offsets per file (`scan-streams-capped` warning when reached); a stream must decode to at least 64 bytes to count, which removes most false-positive zlib headers.

**Diff with `--scan-streams`.** Streams found in A and B are paired by order and by XXH3 of their decoded content. A pair with equal decoded content is reported once as `encoding` churn (`stream at 0x1F40: zlib, equal content, re-encoded`). A pair with different content is diffed on the decoded bytes with this module's own CDC algorithm, or through the registry when the decoded bytes probe as a known format (depth + 1), and reported at `/streams/<n>/…` with the stream's offsets in both files. Bytes outside streams are diffed as before. The churn summary attributes stream bytes to `encoding`/`framing`/`integrity` and everything else to `content`, which turns "no common content found" (§5) into a useful report for container-like unknown formats.

**Merge and write.** Never inside a stream: the length prefixes and offsets around it are unknown (SPEC §12.2). With `--binary-merge`, a detected stream is an atomic range for the guard rules of §6: edits by both sides anywhere in one stream conflict as one hunk, and a stream is taken whole from one side. Output is still a byte concatenation of input ranges; no size, checksum, or signature is regenerated, so the `binary-merge-unverified` warning stays.

**Signatures:** none known; authenticity cannot be detected. **Expected churn:** whatever the inputs differ by; without `--scan-streams` it is all `content` (or `unexplained` in envelope reports), with it the stream share moves to `encoding`.

## 8. Validation

Re-hash the output. For `--binary-merge`, re-run the probe of all registered formats on the output: if the base was unrecognized but the output now probes as something (or vice versa), warn. There is no deeper validation possible.

## 9. Libraries

| Candidate | vcpkg | License | Notes | Verdict |
|---|---|---|---|---|
| bsdiff | no port (verified) | BSD-2-Clause (assumed, upstream) | Suffix-array based delta (O(n log n) memory heavy), output optimized for patch size, not for reporting ranges | Reject. Wrong objective. |
| xdelta3 | no port (verified) | Apache-2.0 for 3.1 and later, GPL for older releases (assumed) | VCDIFF (RFC 3284) encoder | Reject for diff. Possible later export format. |
| open-vcdiff | not checked | Apache-2.0 (assumed) | VCDIFF encoder/decoder | Later, only if `--export=vcdiff` is requested. |
| xxHash | `xxhash` 0.8.4 (verified; library BSD-2-Clause, CLI GPL-2.0-or-later) | BSD-2-Clause for the library | Already a core dependency | Use. Do not build or ship the `xxhsum` CLI. |

**Recommendation: own implementation**, about 600 LOC (gear CDC 120, anchor and refinement glue 200, dump 120, byte merge guard and size rule 160). It reuses `seqdiff` and xxHash. Relation to delta tools: bsdiff, xdelta, and VCDIFF answer "smallest patch to transform A into B" for storage and transmission. Splinter answers "where and how much did it change" and must be deterministic and readable, so the CDC plus histogram approach is a better fit. Git's binary patch format (`GIT binary patch`, literal/delta, zlib plus base85) is worth *reading* in `patch` mode later, because users receive such patches.

## 10. Hard cases and explicit non-goals

- **Compressed or encrypted payloads**: any change re-randomizes all following bytes. Reported as low anchor coverage. This is the honest answer; the remedy is a format module.
- **Same-size in-place edits in huge files** (save games, disk images): CDC boundaries do not move, so the result is precise and fast.
- **Periodic data** (tables of identical records): many identical chunks. Histogram's lowest-occurrence rule plus the Myers fallback handles repeats; the cost limit bounds the worst case.
- **Inputs over `--max-input`**: refused with a limit error, not truncated.
- Non-goals: entropy analysis, structure inference (file carving, magic scanning for embedded formats), and binary patch generation optimized for size.

## 11. Test plan

- Generated fixtures: random buffers of 0 B to 64 MiB with seeded random edits (in-place, insert, delete, block move, truncation, append). Assert that the reported ranges reconstruct B from A exactly (apply the change set), and that `bytes_*` metrics add up.
- Determinism: same inputs on Windows, Linux, macOS produce identical reports and dumps (golden hash of the gear table and of reports).
- Resynchronization property: a single k-byte insertion into a 16 MiB random file yields one reported range and a dump diff of at most a few chunk sections.
- GNU message corpus: `-q`, plain, `-a`, mixed text/binary pairs, compared with GNU diff on CI.
- Merge: disjoint edits merge, guard band triggers, size-change rule triggers, warning always present.
- Fuzz: `fuzz_binary_diff` (split input into two buffers), `fuzz_binary_patch_apply`.
- Performance: 1 GiB pair with 100 scattered edits under 5 s release build.

## 12. Phases

| Phase | Content | Effort |
|---|---|---|
| MVP | Fast paths, CDC, anchor diff, refinement, range report, GNU messages, chunked hex dump, OpaqueLeaf merge | S |
| v2 | `--binary-merge` with guard band and size rule, binary patch payload and apply with fuzz | S |
| later | Byte-map visual, reading Git binary patches, VCDIFF export | S |

## 13. Open questions

- **Q-BIN-1**: Should plain `splinter diff` on binaries print only the GNU line or the range summary? **Provisional:** GNU line by default; range report with `--json`, `-u --binary-detail`, or in Git diff-driver mode. **Why:** scripts parse GNU output, while Git diff users want detail.
- **Q-BIN-2**: CDC parameters fixed or adaptive? **Provisional:** two fixed tiers (8 KiB and 64 KiB average) switched at 256 MiB input. **Why:** determinism matters more than optimal granularity, and two tiers bound memory.
- **Q-BIN-3**: Is `--binary-merge` worth shipping at all? **Provisional:** yes, opt-in, with guard band, size-change refusal, and an unconditional warning. **Why:** some users have fixed-layout binaries (save data, ROM patches) where it is exactly right, and refusing everywhere pushes them to worse tools.
- **Q-BIN-4**: Default `--gap` for coalescing reported ranges? **Provisional:** 8 bytes. **Why:** merges edits to adjacent fields of one record without hiding genuinely separate edits.
- **Q-BIN-5**: Hex dump with or without offsets for textconv? **Provisional:** without offsets, chunk-sectioned; offsets only with `--dump-offsets`. **Decided 2026-10-08:** chunk-relative offsets on every row; `--dump-offsets` switches to absolute. **Why:** absolute offsets make every line after an insertion differ, which defeats textconv.
- **Q-BIN-6**: Support applying `GIT binary patch` hunks in `patch` mode? **Provisional:** later phase, read-only (apply, never generate). **Why:** useful interop, but no user of the MVP needs it.
- **Q-BIN-7**: Should `--scan-streams` be on by default in diff mode? **Provisional:** off by default, suggested in the "no common content found" message, and on by default in `splinter inspect`. **Why:** trial decoding costs time on large files and can produce false positives in random data; users opt in when the report says nothing useful.
