# Containers (ZIP, TAR) — Implementation Document

> **Decided (D-27, amended 2026-10-10):** the Phase 6 spike rejected minizip-ng (its writer rebuilds every local header from the central record and models neither prefix data nor the APK Signing Block), so the **own reader and writer** of §9 is used, over zlib and zstd. Q-ZIP-1 is superseded. Status: §12.1.

**Tier:** T0 · **Module:** `semantic-splinter-zip` (ZIP, JAR/APK/NUPKG, mimetype-first profile) and `semantic-splinter-tar` (TAR family) · **vcpkg feature:** `zip` (deps: `zlib`, `zstd`), `tar` (deps: `zlib`, `zstd`, `liblzma`, `bzip2`); both share the helper `semantic-splinter-container` (member layer and codecs) · **Depends on modules:** core only.
**Extensions / magic:** ZIP `PK\x03\x04` at 0, or an end-of-central-directory record `PK\x05\x06` found in the last 64 KiB + 22 bytes (covers self-extracting stubs and empty archives `PK\x05\x06` at 0). `.zip .jar .war .ear .apk .aab .nupkg .whl .xpi .vsix .crx` (CRX has a header before the ZIP). TAR: `ustar` at offset 257 (`ustar\0` POSIX or `ustar  \0` GNU), or a valid header checksum in block 0. `.tar .tar.gz .tgz .tar.zst .tzst .tar.xz .txz .tar.bz2`. gzip `1F 8B`, zstd `28 B5 2F FD`, xz `FD 37 7A 58 5A 00`.
**Status (v1 = MVP + v2 without similarity renames, Phase 6):** done: ZIP and TAR (gzip, zstd, xz, bzip2) diff and
merge with recursion into members, and patch payloads (§6.5, #44). Not yet: the EnvelopeMap (#45), `--zip-strict`,
`--layout`, and other §4/§5/§8 options (#49), ZIP64 promotion and extended-timestamp merge (#50), ZIP prefix and APK
Signing Block patches (#109), `@@ ADDRESS` hunk headers and `--annotate` (#102), similarity renames
(`--find-renames`). Details: §12.1.

## 1. Why it matters

Containers are leverage, not destination. Many document, asset, and distribution formats are ZIP packages (EPUB, APK, JAR, NuGet, VSIX) and many deliverables are tarballs. Today Git shows all of them as "Binary files differ", and merge drivers that unzip-merge-rezip produce byte-different archives that break signatures and alignment and create noise.

The container modules must:

- Show which members were added, removed, renamed, or changed, with member metadata (method, size, time, mode), and recurse into members by format: a changed PNG in a ZIP is reported as pixel regions, a changed XML part as XML changes.
- Merge member sets (KeyedMerge by path) and recurse into changed members through the registry.
- Write archives that keep every untouched member **byte-exact** (local header, extra fields, compressed data, data descriptor) and are deterministic for touched ones.
- Defend against ZIP bombs and parser-differential tricks before allocating.
- Provide a mimetype-first profile for packages such as EPUB.

## 2. Format anatomy

### 2.1 ZIP (PKWARE APPNOTE 6.3.10)

```text
[prefix data: SFX stub, CRX header]           optional, preserved as opaque
{ local file header  PK\3\4 (30 B + name + extra)
  file data (compressed)
  [data descriptor  (PK\7\8)? crc32 csize usize]   when flag bit 3; 32- or 64-bit sizes }*
[APK Signing Block: size, pairs, size, "APK Sig Block 42"]   APK v2+ only
central directory: { PK\1\2 (46 B + name + extra + comment) }*
[ZIP64 end of central directory record PK\6\6, ZIP64 locator PK\6\7]
end of central directory PK\5\6 (22 B + comment <= 65535 B)
```

Key fields and their treatment:

| Item | Notes |
|---|---|
| General purpose flags | bit 0 encrypted (member becomes OpaqueLeaf, no recursion), bit 3 data descriptor, bit 11 UTF-8 name/comment. |
| Methods | 0 stored, 8 deflate: read and write. 93 zstd (APPNOTE 6.3.8+; ID 20 is the deprecated early zstd ID, read only): read and write when the `zstd` feature is on. 12 bzip2, 14 LZMA, 95 xz, 9 deflate64, 98 PPMd: OpaqueLeaf (compared by compressed bytes and CRC, not recursed) unless a decoder is available. 99 (WinZip AES): OpaqueLeaf. |
| Times | DOS date/time in local time, 2-second resolution. Extra fields: 0x5455 extended timestamp (Unix seconds, UTC), 0x000A NTFS (100 ns FILETIME). |
| Names | CP437 unless bit 11. 0x7075 Info-ZIP Unicode Path overrides when its CRC of the raw name matches. Names use `/`. A trailing `/` is a directory entry. |
| Attributes | `version made by` host byte, external attributes (Unix mode in the high 16 bits when host is 3, DOS attributes otherwise). |
| ZIP64 | Extra 0x0001 holds 64-bit usize, csize, offset, disk number, only the fields whose 32-bit slot is 0xFFFFFFFF (local header: usize and csize both, if present). EOCD fields at 0xFFFF/0xFFFFFFFF redirect to the ZIP64 EOCD. |
| Other extras | 0x7875 Unix UID/GID, 0xCAFE JAR marker (empty, first entry), 0xD935 Android zipalign alignment padding, 0x9901 AES. Unknown extras are preserved byte-exact. |

The central directory is authoritative. The local header must agree with it on name, method, and (if no descriptor) sizes and CRC, otherwise the input is rejected as `invalid_input` with code `zip-header-mismatch` (parser-differential defence; `--zip-lenient` downgrades to a warning and trusts the central directory).

### 2.2 TAR (POSIX.1-2001 pax, ustar, GNU)

512-byte header blocks with octal ASCII numeric fields (GNU base-256 for large values: high bit of the first byte set), header checksum, typeflag: `0`/`\0` file, `1` hard link, `2` symlink, `5` directory, `3`/`4` devices, `6` FIFO, `x` pax extended header for the next member, `g` pax global header, `L`/`K` GNU long name/link name, `S` GNU sparse (old), pax `GNU.sparse.*` (new). Data padded to 512, archive ends with two zero blocks, total padded to the blocking factor (default 20 blocks = 10240 bytes). Path = pax `path` > GNU `L` > ustar `prefix + "/" + name`.

Outer compression: gzip (RFC 1952; header has mtime, OS byte, optional original name, possibly multiple members), zstd frames, xz, bzip2. These are stream wrappers, not part of member identity.

### 2.3 Mimetype-first packages (EPUB OCF and similar)

First entry is `mimetype`, method 0 (stored), no extra field, no data descriptor, no encryption, content is the media type without newline. Plus a manifest such as `META-INF/container.xml` (EPUB).

### 2.4 Signed archives

- JAR v1 / APK v1: `META-INF/MANIFEST.MF` (per-entry `SHA-256-Digest`), `META-INF/*.SF` (digests of manifest sections), `META-INF/*.RSA|*.DSA|*.EC` (PKCS#7).
- APK v2/v3/v3.1: APK Signing Block between the last member and the central directory. It signs the whole file except itself (with the EOCD offset field normalized), so any byte change elsewhere invalidates it. v4: separate `.idsig` file.
- NuGet: `.signature.p7s` member. VSIX: `.signature.p7s`, `package/services/digital-signature/`.

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Archive properties | singleton | `/archive/comment`, `/archive/prefix`, `/archive/apk-signing-block` | OpaqueLeaf each |
| Member | logical path (UTF-8, `/` separated, as stored; for TAR with occurrence suffix if duplicated) | `/entries/<escaped path>` | KeyedMerge (member set) |
| Member content | the member's own format | `/entries/<path>/content` + the member format's address, e.g. `/entries/img~1a.png/content/pixels#rect=0,0,8,8` | recursion via registry, else OpaqueLeaf |
| Member metadata | field name | `/entries/<path>/meta/mtime`, `/meta/mode`, `/meta/method`, `/meta/comment`, `/meta/uid`, `/meta/xattr/<name>` | KeyedMerge of scalar leaves |

`<escaped path>` uses RFC 6901 escaping (`~0`, `~1`). The ZIP module uses `/entries/...` (Q-ZIP-7).

**Identity and renames.** Member identity is the path. After KeyedMerge pairs paths, unmatched deletions and additions are paired as renames (`move`) when their uncompressed content hashes (XXH3-128) are equal. Similarity-based rename detection (Git-style, at least 50% similar by the member format's diff) is a v2 option `--find-renames[=N]` (not implemented yet). A rename plus a content edit on the other side merges (edit applies at the new path), per SPEC SequenceMerge move rules adapted to keys.

**Duplicates.** ZIP files with duplicate names in the central directory are rejected (parser-differential vector). TAR allows duplicates (later entries overwrite on extraction); identity becomes `path` for the last occurrence and `path#N` for earlier ones, reported as warning `tar-duplicate-path`.

**Case collisions.** Two members whose paths differ only in case are legal but break checkout on Windows and macOS; the merge warns `case-collision` if the result contains such a pair that neither input had.

**Dependencies and invariants.**

- Mimetype-first: `mimetype` first, stored, no extra field.
- JAR `MANIFEST.MF` and `.SF` digests are integrity (recomputed), and the PKCS#7 blocks and the APK Signing Block are authenticity (SPEC §12.1, §7.1 here).

## 4. Normalization

For comparison only, never for output:

| Item | Default | Option |
|---|---|---|
| Member order | ignored | `--zip-strict-order` reports reordering (not implemented yet, #49) |
| Compression method/level for equal content | ignored (no change) | `--zip-strict` reports method changes (not implemented yet, #49, nor in the rows below) |
| DOS time, extended timestamp, NTFS times | **reported as `meta` changes of kind `metadata`, excluded from "files differ" status** | `--zip-times=ignore` hides them; `--zip-times=strict` counts them as differences |
| Extra field ordering, presence of 0x5455/0x000A/0x7875/0xD935 padding | ignored | `--zip-strict` |
| Data descriptor present or not, ZIP64 used when not needed | ignored | `--zip-strict` |
| Name encoding (CP437 vs UTF-8 flag) | names compared after decoding | none |
| gzip header mtime/OS/name, TAR blocking padding | ignored | `--zip-strict` (applies to tar too) |

Never normalized: member content bytes, member paths, Unix modes (an executable bit change is a real change), symlink targets, encryption state.

**Equality shortcut.** Two members are equal without decompression when method, CRC-32, uncompressed size, and the XXH3-128 of the compressed bytes are equal. Different compressed bytes with equal CRC and size are decompressed and compared by content hash. Most members of a typical edited package hit the shortcut, so a 200 MB package with one changed part decompresses one part.

## 5. Diff

Detectable changes: member added, removed, renamed (by content hash), content changed (recursed), metadata changed; archive comment, prefix, signing block changed; signature invalidation.

Metrics per member: `usize` before/after, `csize` before/after, method, `ratio`; per archive: member counts, total sizes, members recursed vs opaque.

**Recursion.** A changed member is loaded through the registry with logical name = member path (extension drives detection, SPEC §11 step 2), within the same `Limits` budget and depth + 1 (default max depth 8). If the member format has no structural support, it falls back to `text` or `binary` exactly like a top-level file. Members of encrypted or unsupported methods are OpaqueLeaf.

**`dump()` layout** (textconv), deterministic, sorted by path:

```text
# zip 14 entries, comment ""
entry assets/config.json  deflate  1312 -> 377  crc=6f1d02aa
entry assets/layout.xml  deflate  745 -> 372  crc=0b9e17c4  mtime=1980-01-01T00:00:00
entry assets/media/image1.png  stored  48211  crc=91aa3b05
=== assets/layout.xml (xml)
<member's own dump lines, prefixed by nothing>
=== assets/media/image1.png (png)
<png dump>
```

Times are shown only with `--zip-times=strict`. Member dumps are included for members whose format supports `dump()`, bounded by `--max-dump` per member (default 1 MiB of dump text; the option is not implemented yet, #49), after which the member shows only its `entry` line. The text renderer's hunk header Address (text.md §5.3) then reads `/entries/assets~1layout.xml/content/...`, which is exactly what the user needs to see (not yet: the container dump has no line addresses, #102).

**Visual output:** delegated to members (`--visual` renders the first changed member that supports it, or all into a directory with `--visual-dir`). Not implemented yet (#49).

**Cost:** central directory parse is O(entries). Hashing compressed bytes is fast I/O. Decompression only for changed members, streaming with limit counters.

## 6. Merge

### 6.1 Member set

KeyedMerge over paths with SPEC §5.1 rules. Per paired member:

1. If content equal on all sides, metadata merges as KeyedMerge of scalars.
2. If content changed on one side, take that side's member **including its raw bytes**, so the result can raw-copy it.
3. If changed on both sides: identical content → take ours' raw bytes; else recurse into the member format's `merge()`. If the member format has no merge, conflict `both-modified` at `/entries/<path>`, resolved per policy.
4. Metadata: mtime conflicts (both sides changed it differently) auto-resolve to the later time (`auto: newest-mtime`), because times carry no content meaning and the value is taken from one side. Mode, uid/gid, method, comment conflicts are real conflicts `both-modified` on `/entries/<path>/meta/<field>`.
5. Add/add of the same path with different content: recurse with an empty base if the format allows (JSON/XML can merge two additions when their keys do not collide), else conflict `add-add`.
6. Modify/delete: conflict `modify-delete`.

Output member order: SPEC KeyedMerge order rule (ours' order, theirs-only members after their nearest surviving predecessor), with profile constraints applied last (mimetype first, JAR `META-INF/MANIFEST.MF` stays first after `META-INF/`).

### 6.2 Recursion budget

The member merge runs with the remaining depth and memory budget. Exceeding it in a member becomes conflict `limit` for that member (OpaqueLeaf fallback), not a failure of the whole merge, unless `--strict-limits` (not implemented yet, #49).

### 6.3 Signatures (JAR/APK/NuGet/VSIX)

Signatures follow SPEC §12.5 (`--signatures=auto|keep|strip|resign`); the exact keyed boundaries and per-scheme defaults are in §7.1. In short, for any content change to a signed archive:

- `auto` (default) **strips** JAR v1 / APK v1 signature files (`META-INF/*.SF`, `*.RSA`, `*.DSA`, `*.EC`), the APK Signing Block, and NuGet/VSIX `.signature.p7s`, and warns `signature-stripped` per removal, because an invalidly signed JAR, APK, or package is rejected where an unsigned one loads or can simply be re-signed (Q-ZIP-5).
- `keep` keeps ours' signature members and the APK Signing Block byte-exact and warns `signature-invalidated` (the result fails verification until re-signed).
- `MANIFEST.MF` per-entry digests are integrity: they are recomputed for changed and added entries under every policy (and removed for deleted entries), so the manifest stays correct and re-signing only has to add the keyed part (Q-ZIP-12). **v1 (D-50):** without the `crypto` feature, digest lines of changed or added entries are removed with a warning instead of recomputed.
- MANIFEST.MF itself is a text member and merges like text when both sides changed main attributes.

### 6.4 `--conflict=mark`

The container has nothing to mark; it delegates to members (text members get markers, XML members get comments, and so on). For OpaqueLeaf members, writing both alternatives as sibling members (`<path>.splinter-theirs`) was considered and rejected, because extra members break package manifests and signatures. Such members fall back to `ours` with a warning (Q-ZIP-4); the alternatives are in the conflict sidecar.

### 6.5 Patch payload

Per member: add (full raw member: local header fields + compressed bytes, so it can be raw-copied on apply), remove (path + content hash), rename (from, to, hash), metadata (field before/after), content (nested payload of the member format). Base verification uses the member's content hash.

**Implemented (#44):** a member's payload is an archive of the same format (bare: no prefix, comment, or outer compression) holding that member alone, raw-copied on apply; a `/meta/` unit's is the same with empty content, its fields taken from that record; `/content` is the member's bytes; a nested address is the member format's own payload when that format has patches, else the member's bytes. A rename is two units, `erase` at the old path and `move` at the new one (the diff writes the extra unit for any move with a `from` elsewhere). Base verification is the envelope's whole-file hash, then identity application as a three-way merge (SPEC §6). Added members go last (#108); the ZIP comment is a unit of its own; `/archive/prefix` and `/archive/apk-signing-block` changes have no payload (`--binary` exits 2, 3 with `--exit-codes`; #109).

## 7. Serialization and preservation

### 7.1 Envelope fields

This subsection comes first in section 7 because §7.2 and §7.3 are built on it (TEMPLATE §7.1).

**Layer stacks.**

- ZIP: `file → [prefix] → { local header → member data → [descriptor] }* → [APK Signing Block] → central directory → [ZIP64 EOCD + locator] → EOCD`; member data `→ [encryption] → method (stored/deflate/zstd/…) → member bytes → member format` (recursion).
- TAR: `file → [gzip | zstd | xz | bzip2 stream] → 512-byte blocks → { [pax x / GNU L,K] → ustar header → data + pad }* → end blocks → blocking pad`; member data `→ member format` (recursion).

**ZIP fields.**

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| Local header `csize`, `usize`, name and extra lengths | framing | must equal the central record (§2.1) and the descriptor | `field(u32, LE, size_of(data_g))`, `size_of(name_g)`, `size_of(extra_g)`; raw-copied for untouched members |
| Data descriptor (`PK\7\8`, 32/64-bit sizes) | framing | equals the central record | not written for touched members (sizes known); kept inside raw spans |
| Central record `local header offset` | framing (absolute) | points at a `PK\3\4` whose name matches; overlap check (§8.1) | `offset_of(local_g)`; the only field rewritten in an otherwise verbatim central record |
| EOCD entry counts, CD size, CD offset | framing | consistent with the parsed directory | `count_of(cd_g)`, `size_of(cd_g)`, `offset_of(cd_g)` |
| ZIP64 extra (0x0001), ZIP64 EOCD and locator | framing | fields present exactly for the 0xFFFF/0xFFFFFFFF slots | promotion when a value exceeds 32 bits is a variable-width fix-point (SPEC §12.4); kept where the source had it (§7.2) |
| zipalign padding (0xD935 extra, or zero padding in the local extra) | framing (alignment) | records whether the source was aligned to 4 B / 4 KiB / 16 KiB | `align(n, 0)` realised as extra-field length, reported `realigned` |
| CRC-32 of the uncompressed data (local, central, descriptor) | integrity | `ok`/`mismatch` for every decompressed member; others `not-checked` (Q-ZIP-11) | `crc32(content_g)` for touched members; raw copy keeps input CRCs (SPEC §12.7) |
| Info-ZIP Unicode Path extra (0x7075): CRC-32 of the raw name | integrity | extra ignored if the CRC does not match | `crc32(raw_name_g)` when the name changed |
| Member compression (deflate, zstd, …) | encoding | inflate within limits (§8.1) | raw copy if the content is unchanged; else `encode(deflate, inferred)` |
| DOS date/time, 0x5455 and 0x000A timestamps | volatile | none | from the winning side (§7.2) |
| `version made by`, `version needed`, flag bits 1-2 | volatile (provenance) | none | copied from the source side |
| WinZip AES (method 99): salt, password verifier, HMAC-SHA1 code | authenticity + encryption | `unverifiable` (no password, D-41) | member is OpaqueLeaf, raw-copied or taken whole from one side |
| ZipCrypto 12-byte encryption header | encryption | none | OpaqueLeaf, raw copy |

**Deflate parameter inference.** General-purpose flag bits 1-2 record the compressor option (0 normal, 1 maximum, 2 fast, 3 super fast); Info-ZIP and most zlib-based writers set them. They select zlib levels 6, 9, 2, 1. For members up to 1 MiB, a trial compression of the decoded content at levels 1-9 picks the level whose output size is closest, which often reproduces zlib-written bytes exactly. Office, Java, and .NET deflaters never match zlib byte-for-byte, so their members always show `encoding` churn when touched.

**TAR and outer stream fields.**

| Field | Class | Verify on load | Regenerate on write |
|---|---|---|---|
| Header `size` (octal or GNU base-256), pax `size` | framing | bounded by the remaining input | `varfield(octal, size_of(data_g))`; pax when it no longer fits |
| Header `chksum` (unsigned byte sum, the field counted as 8 spaces) | integrity | `ok`/`mismatch` per header; the historic signed-sum variant is accepted with a warning | `custom(tar_chksum)` over the 512-byte header |
| pax record `"<len> key=value\n"` | framing | decimal length matches | variable-width fix-point (the length counts its own digits) |
| Data padding to 512, two end blocks, blocking-factor padding | framing | zero-filled; counts recorded | `align(512, 0)`; end and blocking padding reproduced from ours (§7.3) |
| gzip header `MTIME`, `OS`, `XFL`, `FNAME` | volatile | none | copied from ours |
| gzip trailer CRC-32 and ISIZE | integrity | `ok`/`mismatch` per gzip member | `crc32(tar_g)`, `size_of(tar_g) mod 2^32` |
| zstd frame content size, XXH64 content checksum | framing, integrity | checksum verified when the frame flag is set | `xxh64(tar_g)` and content size, each only if ours' frame had it |
| xz block check (CRC-32/CRC-64/SHA-256), index and its CRC-32 | integrity, framing | verified by liblzma | writer delegation to liblzma with ours' check type |
| Outer compression stream | encoding | decode within limits | re-encoded whenever any member changed (a single stream cannot be raw-copied in part) |

Outer re-encode parameters: gzip `XFL` (2 = maximum, 4 = fastest) narrows the deflate level, then a trial on the first 1 MiB picks it. zstd frames do not record the level, so it is guessed from the window descriptor plus a trial, else level 3. xz keeps the preset implied by the LZMA2 dictionary size in the block header (verify).

**Raw copy and layout stability.** Untouched ZIP members are copied as one span (local header to end of descriptor). Local headers hold no offsets, so the only bytes that change for them are the 4-byte (or ZIP64 8-byte) offsets in their central records when an earlier member changed size. Untouched TAR members are copied verbatim inside the decoded TAR stream. `--layout=stable` keeps ours' member order, so members before the first touched one keep their offsets (Q-ZIP-14). `--layout` is not implemented yet (#49); the order is always ours'.

**Signatures and the keyed boundary** (`--signatures=auto`, §6.3):

- **JAR v1 / APK v1.** `MANIFEST.MF` per-entry `*-Digest` (integrity, `sha256(entry_content_g)` or the algorithm the manifest uses) and `.SF` `*-Digest-Manifest`, `*-Digest-Manifest-Main-Attributes`, and per-section digests (integrity, digests of manifest bytes) are recomputable. The keyed boundary is the PKCS#7 `SignedData` in `*.RSA|*.DSA|*.EC`, which signs the `.SF` bytes. `auto`: strip `.SF` and PKCS#7 files, recompute manifest digests (Q-ZIP-12). `resign` with `--sign-key`/`--sign-cert`: recompute the manifest and `.SF`, then sign the `.SF` (an implemented scheme, SPEC §12.5).
- **APK v2/v3/v3.1.** The Signing Block signs digests of 1 MiB chunks of three regions: the member section, the central directory, and the EOCD with its CD-offset field replaced by the Signing Block's offset. Everything except the block itself is inside the keyed boundary, including every offset and alignment byte, so even pure raw copy with one changed member invalidates it. `auto`: strip the block and the v1 files together; v1 files whose `.SF` says `X-Android-APK-Signed: 2` (or 3) without a matching block trip apksigner's anti-stripping check. `resign`: only through `--sign-command=apksigner …` after the final layout. A v4 `.idsig` is a separate file and becomes stale (reported).
- **NuGet / VSIX.** `.signature.p7s` (CMS over a hash of the package without itself) is keyed as a whole. `auto`: strip and warn `signature-stripped` (verify that an unsigned package passes NuGet's default `accept` validation mode while a tampered signed one fails with NU3008).
- **WinZip AES HMAC.** Keyed by the password; such members are opaque in v1 and never recomputed.

**Expected churn.** One changed 30 KB XML member in a 2 MB package: the member's compressed bytes (`encoding`, with the decoded change itself as `content`), its local CRC and sizes (`integrity`, `framing`), its central record (`integrity`, `framing`), and the offsets of every later central record plus the EOCD CD offset (`framing`). Typically over 95% `encoding`, the rest framing and integrity, 0% `unexplained`. A signed JAR adds the removed signature members (`authenticity`) and the changed manifest digest lines (`integrity`). For `.tar.gz`, the compressed stream differs from the first changed deflate block onward (`encoding`); the header checksum and size changes inside the decoded TAR are `integrity` and `framing`.

### 7.2 ZIP writer

- **No-op shortcut**: if the merged result equals ours member for member and archive properties are unchanged, output is ours' bytes verbatim.
- **Raw copy of untouched members**: the local header, name, extra field, compressed data, and data descriptor are copied as one byte span from the source side. Local headers contain no offsets, so this is exact. The central directory record is copied from the source side verbatim except the relative-offset field (and the offset in the ZIP64 extra, adding a ZIP64 extra only if the new offset exceeds 0xFFFFFFFF, which is reported).
- **Touched members** (content changed by merge): recompressed with the method of the side whose structure the member came from (ours on conflict), with parameters inferred per §7.1 (deflate level from general-purpose flag bits 1-2 and trial compression, falling back to zlib level 6, Q-ZIP-2; zstd level 3 unless inferred). Compressed bytes generally differ from what the original application produced; this is reported as info `recompressed` (not a warning; content is identical).
- **Determinism for touched members**: timestamp = the winning side's member timestamp (never wall-clock), extra fields carried over from that side except sizes, `version made by` / `version needed` copied, flag bit 3 not used for new data (sizes are known), UTF-8 flag set when the name is not pure ASCII.
- **ZIP64**: written only when a value requires it, or when the source member already had it (preservation beats minimality).
- **Prefix data, archive comment**: preserved from the winning side.
- **Alignment (APK/zipalign)**: if the source archive had all stored members' data aligned to 4 bytes (and `.so` members to 4 KiB or 16 KiB), the writer keeps that property by adjusting the 0xD935 padding extra (or, when absent, zero padding in the local extra field as zipalign does). This changes local header bytes of moved stored members and is reported as info `realigned`.
- **APK Signing Block**: removed under `--signatures=auto|strip`; copied verbatim from ours under `keep` and written between the last member and the central directory (§7.1).
- **Profile constraints**: mimetype-first enforced (stored, no extras, first).
- Written to a temp file, validated, renamed (SPEC §1 invariant).

### 7.3 TAR writer

- Untouched members: header blocks (including preceding `x`, `L`, `K` blocks) and data blocks copied verbatim.
- Touched members: header regenerated in the source member's flavour (pax stays pax, GNU stays GNU, ustar stays ustar unless a field no longer fits, then pax, reported). mtime from the winning side.
- End blocks and blocking-factor padding reproduced from ours (count of trailing zero blocks preserved).
- Outer compression: same codec as ours. gzip header fields (mtime, OS, original name, XFL) copied from ours. The compressed stream is necessarily re-encoded and is not byte-identical to any input; reported as info `recompressed`. Multi-member gzip and zstd multi-frame layouts are not preserved (single member/frame written).

### 7.4 What is lost

Nothing semantic. Compressed byte identity of touched members and of the outer gzip/zstd/xz stream is lost and reported. Signatures become invalid and are reported.

## 8. Validation

After writing, the writer re-opens the archive with the same reader and checks: central directory parses, every local header matches, CRC-32 of every touched member (and every member with `--validate=full`, not implemented yet, #49), no overlaps, entry count and sizes within limits, profile invariants (mimetype first and stored). Member-level validation is delegated to each member format's `validate()` for touched members.

Optional external validators for CI (not runtime dependencies): `zip -T`/`unzip -t`, `zipalign -c -v 4`, `apksigner verify` (expected to fail after merge unless re-signed), `jarsigner -verify`, EPUB: epubcheck.

### 8.1 ZIP-bomb and hostile-input defences (applied before allocation)

| Threat | Defence |
|---|---|
| Huge entry counts | EOCD count checked against `--max-zip-entries` (65 536) before reading the central directory; central directory size must be at most 46 bytes × count + names, and fit in the file. |
| Declared size lies | Streaming decompression with a hard counter per member: stop at the declared usize (error `zip-size-mismatch` if exceeded) and at the remaining global budget. Declared sizes are used for pre-checks only. |
| Expansion ratio | Per member and total, `--max-zip-ratio` (200:1) evaluated against the actual decompressed bytes, with a floor (members under 1 MiB uncompressed are exempt, since small, highly compressible XML is normal). |
| Total uncompressed size | `--max-zip-size` (4 GiB) across the archive, *and* a single budget shared across recursion levels (nested ZIPs cannot multiply it). |
| Overlapping members (Fifield's non-recursive bomb) | Members sorted by local header offset; each `[offset, offset + header + csize + descriptor)` must not overlap the next; violation is `invalid_input` `zip-overlap`. |
| Recursion (42.zip) | `--max-depth` (8) plus the shared budget. Members are decompressed lazily, only when needed. |
| Duplicate or conflicting names | Rejected (ZIP). Local/central mismatch rejected. |
| Path tricks (`../`, absolute, NUL, backslashes) | Paths are never used for file system access (no extraction). They are addresses only; still reported as warning `suspicious-path`. |
| TAR | Octal and base-256 parsing with overflow checks, size bounded by remaining input, pax record lengths validated, sparse maps bounded. |

## 9. Libraries

| Candidate | vcpkg (verified) | License (verified) | Raw copy of members | Notes |
|---|---|---|---|---|
| libzip | `libzip` 1.11.4 | BSD-3-Clause | Yes: `zip_source_zip_file(..., ZIP_FL_COMPRESSED, 0, -1, ...)` copies compressed data without decompression (verified in docs) | Mature, fuzzed. Regenerates local headers and central records itself, so unknown local extras, ordering details, prefix data, APK Signing Block, and alignment are not under our control (assumed from API design). zstd support since 1.8 (assumed). |
| minizip-ng | `minizip-ng` 4.1.0 | Zlib | Yes: `mz_zip_writer_copy_from_reader`, raw mode via `mz_zip_reader_set_raw` / `mz_zip_writer_set_raw` (verified in docs) | Many compression backends, ZIP64. Heavier API, global build options. |
| miniz | `miniz` 3.1.2 | MIT | `mz_zip_writer_add_from_zip_reader` (assumed raw copy) | Small, includes its own deflate. ZIP64 support partial (assumed). |
| libarchive | `libarchive` 3.8.9 (vcpkg license field empty) | Mostly BSD-2-Clause per its COPYING, a few files BSD-3 (UC Regents), public domain, or CC0/Apache/OpenSSL (verified) | No raw copy between archives (assumed; streaming read/write API decompresses) | Excellent multi-format reader (TAR variants, 7z, RAR, cpio, ISO). Large dependency tree via features. |
| zlib | `zlib` 1.3.2 | Zlib | n/a | Deflate/inflate. Shared with PNG (libpng). |
| zstd | `zstd` 1.5.7 | BSD-3-Clause OR GPL-2.0-only (choose BSD) | n/a | Method 93 and `.tar.zst`; also the patch payload compressor (SPEC §6). |
| liblzma | `liblzma` 5.8.4 | 0BSD (library; tools LGPL, not built) | n/a | `.tar.xz`, ZIP method 95 read. Optional. |

**Recommendation: own ZIP and TAR readers/writers on top of zlib, zstd, and liblzma.** Reasons: (1) byte-exact preservation needs control over every record (raw local-header span copy, prefix data, APK Signing Block placement, zipalign padding, ZIP64 only where it was); (2) the limit checks of §8.1 belong before allocation and inside the decompression loop, which is awkward to inject into libzip or minizip-ng; (3) parser-differential checks (local vs central, overlaps, duplicates) are policy we must own anyway; (4) the format is small and stable. Estimate: ZIP reader 900 LOC, ZIP writer 600, profiles (mimetype-first, JAR/APK signatures, zipalign) 400, TAR reader/writer 700, gzip/zstd/xz stream wrappers 250. About 2.8k LOC, fuzzed from day one. libzip (BSD-3) is the fallback if the own reader proves too costly to harden; its read side could replace ours without touching the writer. libarchive is reserved for a possible later read-only `archive-misc` module (7z, RAR, ISO) in T4.

## 10. Hard cases and explicit non-goals

- **Encrypted members**: OpaqueLeaf only; no password support (non-goal).
- **Multi-disk/split archives** (`.z01`): rejected as unsupported.
- **Members whose compressed bytes changed but content did not** (re-saved by another app): no content change; reported as metadata `recompressed-by-side` only in `--zip-strict`.
- **Huge member counts with tiny edits** (asset bundles): handled by the equality shortcut; memory is O(entries).
- **TAR hard links and symlinks**: link target is metadata; content is not followed.
- **Sparse TAR members**: read; written back only if untouched (raw copy); a touched sparse member is written dense with warning `sparse-lost`.
- **CRX / SFX prefix changes**: OpaqueLeaf on `/archive/prefix`.
- Non-goals: extracting to disk, creating archives from directories, repairing corrupt archives, 7z/RAR merge.

## 11. Test plan

- **Generated fixtures** (at test time, via our writer and via Python `zipfile`/`tarfile` scripts committed under `tests/fixtures/zip/gen/`): stored/deflate/zstd, data descriptors with and without signature, ZIP64 forced on small files, UTF-8 and CP437 names, Unicode path extra, comments, prefix data, zipalign'ed APK-like layout with a fake signing block, JAR with MANIFEST/SF, mimetype-first EPUB skeletons, TAR ustar/pax/GNU with long names, sparse, duplicates, gzip/zstd/xz wrappers.
- **Checked-in fixtures** (small): one real EPUB, APK (unsigned debug, tiny), JAR, `.nupkg` from known tools, to catch real-world quirks.
- **Byte-exact properties**: merge(x, x, x) == x bytes; merge(b, o, b) == o bytes; untouched members' byte spans identical in the output; output re-opens with Info-ZIP `unzip -t` and Python `zipfile` (CI).
- **Golden reports** for diff of edited packages (member added, renamed, changed, mtime-only).
- **Hostile corpus**: 42.zip-style nesting, Fifield overlap bomb, declared-size lies, entry-count bombs, local/central mismatch, duplicate names, truncated archives, TAR with malformed octal; every case must fail fast with a limit or invalid-input error and bounded memory (measured in CI).
- **Fuzz targets**: `fuzz_zip_read`, `fuzz_zip_roundtrip` (read, write unchanged, compare), `fuzz_tar_read`.

## 12. Phases

| Phase | Content | Effort | State |
|---|---|---|---|
| MVP | ZIP reader with all defences, writer with raw copy and deterministic touched members, deflate/stored, member KeyedMerge with recursion, rename by hash, dump, mimetype-first profile | L | done |
| v2 | zstd method, TAR family with gzip/zstd/xz, signatures handling (`--signatures=`, JAR v1 manifest handling per D-50; JAR v1 re-sign with `--sign-key` is post-v1, D-36), zipalign preservation, similarity renames | M | partly done: all but similarity renames (and `resign`, #21) |
| later | libarchive-based read-only module for 7z/RAR/ISO, Git binary patch interop for members | M | not started |

### 12.1 Implementation status (Phase 6)

Done (v1 = MVP + v2 without similarity renames): the ZIP reader with every §8.1 defence (strict end record
search, ZIP64, local/central agreement with `--zip-lenient`, duplicates, overlaps, entry count, declared size and
ratio checked before decoding, one `--max-zip-size` budget and `--max-depth` across recursion); stored, deflate,
and zstd members (id 20 read, written as 93); CP437 and Info-ZIP Unicode Path names; data descriptors; prefix
data and its offset shift; APK Signing Block detection. The writer copies untouched members as raw spans and
rewrites only central-record offsets, writes ours verbatim when nothing changed, and writes touched members
deterministically (deflate level from flag bits 1-2 and a trial up to 1 MiB, zstd level 3), keeping ZIP64 where
the source had it and zipalign 4-byte (`.so`: 4 or 16 KiB) alignment (`realigned`). Mimetype-first order,
`--signatures=auto|strip|keep` (`resign` exits 2, 3 with `--exit-codes`; #21), and D-50 manifest digest removal. TAR: ustar, pax, and GNU
(base-256, signed checksums, `L`/`K`, `path#N` duplicates), gzip/zstd/xz/bzip2 wrappers re-encoded with ours'
header and parameters, verbatim member copy, touched headers in the source flavour. Shared: member diff and
KeyedMerge with renames by content hash, per-field metadata merge (`newest-mtime`), recursion through the
registry with nested changes and conflicts under `/entries/<path>/content`, nested conflict ids accepted by
`--resolve`, `zip-times=ignore|strict`, dump with member dumps.

Patch payloads (§6.5) came with #44. Not yet: the EnvelopeMap (churn line, `inspect --layout`; #45); `--zip-strict`,
`--zip-strict-order`, `--max-dump` (fixed at 1 MiB), `--validate=full`, `--strict-limits`, `--visual`, and
`--layout` (the order is always ours') (#49); `--integrity=strict` forcing every CRC (untouched members' CRCs stay
`not-checked`); `--find-renames`; line addresses in the dump (#102); prefix and APK Signing Block patches (#109).
Member times are DOS times; 0x5455/0x000A extras follow the content side, and a value that needs ZIP64 in a record
that had none exits 2 (3 with `--exit-codes`) (#50). The zstd and xz re-encode levels are fixed (3, preset 6)
instead of inferred (#49); TAR blocking padding is ours' tail verbatim. Fuzz targets, the checked-in
EPUB/APK/JAR/NuGet fixtures, and the measured hostile corpus are Phase 8 work (#16, #17).

## 13. Open questions

- **Q-ZIP-1**: Own ZIP implementation or libzip/minizip-ng? **Superseded by D-27 (amended 2026-10-10):** own reader/writer (about 1.5k LOC for ZIP core) over zlib and zstd; libzip as fallback. **Why:** byte-exact record preservation and pre-allocation limits need control that the libraries do not expose.
- **Q-ZIP-2**: Which deflate implementation and level for touched members? **Decided:** zlib 1.3.x at level 6 (the level most apps use) when nothing better is inferred (§7.1: flag bits 1-2 plus a trial); zlib-ng or libdeflate only if profiling demands it. **Why:** zlib is already needed by PNG, and a single implementation keeps output deterministic across builds.
- **Q-ZIP-3**: Default treatment of timestamp-only changes in diff? **Decided:** listed as metadata changes but not counted for exit status 1; `--zip-times=strict` counts them. **Why:** re-saved packages differ only in times constantly, which is noise for users and for Git.
- **Q-ZIP-4**: What does `--conflict=mark` do for an opaque member? **Decided:** fall back to ours with a warning; do not add sidecar members. **Why:** extra members break package manifests and signatures.
- **Q-ZIP-5**: Keep or strip invalidated signatures by default? **Decided:** under `--signatures=auto`, strip (JAR v1/APK v1 files, APK Signing Block, NuGet/VSIX `.signature.p7s`) and warn `signature-stripped` per removal; `--signatures=keep` keeps ours' bytes with `signature-invalidated`. This replaces the earlier "keep by default" answer, per SPEC §12.5 and D-35. **Why:** an invalidly signed JAR throws `SecurityException` on class load and an invalid NuGet signature fails restore, while unsigned ones load; the change of type (signed to unsigned) is never silent because each removal is warned, and the user must re-sign anyway.
- **Q-ZIP-6**: Should both-sides mtime changes auto-resolve? **Decided:** yes, to the later time, reported as `auto: newest-mtime`. **Why:** the value comes from one side (no invention) and times carry no content meaning.
- **Q-ZIP-7**: Address root for members? **Decided:** `/entries/<path>` with `/content` and `/meta` children. **Why:** separating content from metadata avoids key clashes with member formats' own addresses.
- **Q-ZIP-9**: Similarity-based rename detection in v1? **Decided:** no, exact content hash only; similarity in v2 behind `--find-renames`. **Why:** exact renames cover tool-driven renames (media renumbering), and similarity needs per-format scoring.
- **Q-ZIP-10**: Reject or tolerate local/central header mismatches? **Decided:** reject by default (`zip-header-mismatch`), `--zip-lenient` trusts the central directory with a warning. **Why:** mismatches are the main parser-differential attack and are rare in legitimate files.
- **Q-ZIP-11**: Verify every member's CRC-32 on load (SPEC §12.3 step 2)? **Decided:** only members that are decompressed anyway (changed members, recursion, `--validate=full`); `--integrity=strict|repair` forces all. The code checks decompressed members only; neither forcing option is implemented yet (§12.1). Unchecked members are reported as a count `not-checked`, which SPEC §12.3 does not yet list as a status. **Why:** the equality shortcut (§4) exists so that a 200 MB package with one changed part decompresses one part.
- **Q-ZIP-12**: On `--signatures=strip`, recompute or drop the `MANIFEST.MF` per-entry digests? **Decided:** recompute (integrity) and keep them; drop only the `.SF`/PKCS#7 files. Overridden in v1 by D-50: without the `crypto` feature (#21) the digest lines of changed and added entries are removed with a warning. **Why:** digests in an unsigned JAR are ignored by the JDK and by apksigner, keeping them means fewer manifest lines change, and `jarsigner` re-signing reuses them.
- **Q-ZIP-13**: Preserve the non-signature pairs of an APK Signing Block (verity padding `0x42726577`, SourceStamp, Play dependency metadata) when stripping? **Decided:** no, the whole block goes and each dropped pair ID is listed in the warning. The code drops the whole block but its `signature-stripped` warning does not list the pair IDs yet. **Why:** those pairs are written by signing tools and are regenerated when the APK is re-signed (verify for Play metadata).
- **Q-ZIP-14**: Under `--layout=stable`, move a touched member that grew to the end of the archive so that all other members keep their offsets? **Decided:** no, keep ours' order (the only order the code writes; `--layout` is #49). **Why:** local headers carry no offsets, so shifting members costs only the central-record offset bytes, while reordering is visible in listings and conflicts with profile order rules (mimetype first, JAR manifest first).
- **Q-TAR-1**: Write gzip as single-member even when the input was multi-member? **Decided:** yes, reported as `recompressed`. **Why:** multi-member layout carries no meaning and reproducing it adds complexity for no user benefit.
- **Q-TAR-2**: Duplicate paths in TAR: merge per occurrence or collapse? **Decided:** identity `path` for the last occurrence, `path#N` for earlier ones, warning `tar-duplicate-path`. **Why:** matches extraction semantics (last wins) while keeping all bytes.
