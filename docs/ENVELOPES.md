# Envelope Catalog

Per-format inventory of the bytes that are **not content**: framing (sizes, offsets, counts, padding,
indexes), integrity (CRCs, checksums, unkeyed digests, FEC), authenticity (signatures, MACs), encoding
layers (compression, filters, transport encodings), derived data, and volatile fields. The rules for
handling each class are in [SPEC.md §12](SPEC.md). Each format document repeats its own row in
section 7.1, with more detail. If the two disagree, the format document wins and this table gets fixed.

Status of facts: drafted from format specifications and the format research on 2026-10-08. Entries
marked *(verify)* still need to be confirmed against the specification or a fixture.

Legend: **L** = layer stack (outermost → content). **F** = framing. **I** = integrity. **A** = authenticity.
**D/V** = derived and volatile. **Sig default** = `--signatures=auto` behaviour. **Watch** = traps.

---

## Images

### PNG
- **L:** file → chunks → IDAT concatenation → zlib → deflate → per-row filter → pixels. `iCCP`, `zTXt`, and `iTXt` (compressed flag) are zlib-wrapped too.
- **F:** chunk length (u32 BE, ≤ 2³¹−1). The IDAT split into chunks is arbitrary and is re-chunked on write (keep ours' chunk size when that is cheap). APNG animation chunks (`acTL`/`fcTL`/`fdAT`) are copied verbatim as one unit (D-48), so their sequence numbers stay valid.
- **I:** CRC-32 per chunk over type and data. Adler-32 at the end of each zlib stream.
- **A:** C2PA manifest in a `caBX` chunk (JUMBF). **Sig default:** keep with `signature-invalidated`, as for JPEG (Q-PNG-12, which overrides PNG's drop rule for unsafe-to-copy chunks). The proposed `dSIG` chunk is stripped.
- **D/V:** `tIME` (volatile, but merged as an ordinary key per Q-PNG-11).
- **Watch:** a single changed pixel re-encodes the entire IDAT stream unless the encoder is restarted per row band. Re-encode with the original zlib FLEVEL and window size, and reuse the per-row filter types for unchanged rows. Raw-copy the IDAT if the pixels are unchanged, even when ancillary chunks changed.

### JPEG
- **L:** markers → entropy-coded segments (byte stuffing `FF00`, restart markers) → Huffman → quantized DCT coefficients (the merge domain, see jpeg.md) → pixels.
- **F:** segment length (u16, includes itself). `RSTn` markers cycle mod 8 and are re-sequenced. APP1 Exif is a TIFF structure with **internal offsets** (IFD0, ExifIFD, GPS, IFD1 thumbnail `JPEGInterchangeFormat`/`Length`). MPF (APP2) holds offsets relative to the MP header, which point at later images in the same file. ICC profiles are split across APP2 segments with sequence numbers and counts.
- **I:** none in baseline JPEG. Photoshop IRB 0x0425 (1061) is the MD5 of IPTC 0x0404. It is recomputed only when it matched on input (Q-JPEG-9); in v1 it is copied, because digests need the crypto feature (D-50). **Extended XMP:** `xmpNote:HasExtendedXMP` is the MD5 of the full extended packet (integrity), and each segment carries the full length and an offset.
- **A:** C2PA/JUMBF manifests (APP11) are signed content credentials. **Sig default:** keep the manifest and report `signature-invalidated`, because C2PA defines how an edited asset should reference its parent *(verify)*.
- **D/V:** Exif thumbnail (derived), Exif `DateTime` (volatile), `PixelXDimension`/`PixelYDimension` (derived).
- **Watch:** after a pixel change the scan data changes length, so MPF offsets (relative to the MP header, pointing past EOI) move. Exif offsets do not: they are relative to the TIFF header inside APP1. Changing Exif can shift MPF targets. `FF` fill bytes before markers are an encoding choice.

### TIFF (and DNG, multipage, BigTIFF)
- **L:** header → IFD chain → strips or tiles → compression (none/LZW/Deflate/PackBits/JPEG/ZSTD/WebP) → predictor (horizontal or float) → samples.
- **F:** **absolute** offsets everywhere: IFD offsets, `StripOffsets`/`TileOffsets` with their byte counts, out-of-line tag values (more than 4 bytes, or 8 in BigTIFF), SubIFDs, EXIF/GPS IFD pointers, and `NextIFD`. Word alignment applies. BigTIFF uses 8-byte variants.
- **I:** none. DNG has `NewRawImageDigest` (MD5, integrity) *(verify)*. **A:** C2PA embeddings are possible *(verify)*, and follow the JPEG rule.
- **D/V:** reduced-resolution IFDs and previews are derived. `DateTime` and `Software` are volatile.
- **Watch:** private tags with offsets that splinter does not understand (MakerNotes, Photoshop tag 37724) silently break when data moves. Keep their data at the same absolute offset when possible (stable layout), and otherwise warn `opaque-offsets-moved`.

### GIF
- **L:** blocks → image data sub-blocks → LZW → palette indices.
- **F:** sub-block lengths (≤ 255) with a zero terminator, packed-field sizes, and the logical screen.
- **I/A:** none. **Watch:** LZW re-encode is deterministic in our own writer (Q-RAST-4).

### WebP
- **L:** RIFF → chunks (VP8/VP8L/VP8X/ALPH/ANIM/ANMF/ICCP/EXIF/XMP) → VP8L bitstream or VP8 → pixels. `ALPH` is optionally VP8L-compressed.
- **F:** RIFF size, chunk sizes, pad byte to even length, and VP8X canvas size and flags (flags are derived from which chunks are present).
- **I:** none. **A:** a C2PA chunk is possible *(verify)*, and follows the JPEG rule. **Advisory:** writers get the RIFF pad and size wrong in the wild, so a mismatch warns rather than failing (Q-RAST-9). **Watch:** the VP8X feature flags must match the chunks present after a merge.

### BMP / TGA / QOI
- **F:** BMP `bfSize`, `bfOffBits`, `biSizeImage` (may be 0), and row padding to 4 bytes. TGA ID length, color-map spec, and the optional extension and developer areas with offsets in the footer. QOI's end marker.
- **I:** none. TGA footer signature is a constant. **L:** BMP RLE4/RLE8, TGA RLE, and QOI ops. **Advisory:** BMP `bfSize` and `biSizeImage` are often wrong in the wild, so a mismatch warns rather than failing (Q-RAST-9).

### SVG / SVGZ
- **L:** `.svgz` = gzip → XML. `data:` URIs in `href` are base64 (images recurse).
- **I:** gzip CRC-32 and ISIZE. **V:** gzip mtime and editor metadata. **D:** Illustrator's `i:pgf` private data (a derived, compressed copy of the document). **A:** none standard (XMLDSig is possible, and follows the generic XML rule).

## Archives and documents

### ZIP family (generic ZIP, JAR, APK, NUPKG)
- **F:** local header compressed and uncompressed sizes and name/extra lengths, data descriptors, central directory entries with **absolute local-header offsets**, EOCD (entry counts, CD size, **CD offset**), ZIP64 extra, ZIP64 EOCD and locator. Alignment: APK zipalign (4-byte stored entries, 4 KiB `.so`), and USDZ 64-byte data alignment via extra-field padding.
- **I:** CRC-32 of the *uncompressed* data per entry (local, central, and descriptor copies must agree).
- **A:** **JAR v1:** `MANIFEST.MF` per-entry digests (*integrity*, recomputed), `*.SF` digests of the manifest and its sections (*integrity*, recomputed), and `*.RSA`/`*.DSA`/`*.EC` PKCS#7 over the `.SF` (**authenticity**). **APK v2/v3/v3.1:** the APK Signing Block between the entries and the central directory signs a digest over 1 MiB chunks of the ZIP sections (**authenticity**, with the whole layout in scope). APK v4 uses an external `.idsig` Merkle tree. **WinZip AES:** HMAC-SHA1 authentication code per entry, keyed by the password (authenticity plus encryption).
- **L:** stored, deflate, deflate64, bzip2, LZMA, zstd, and xz per entry. ZipCrypto/AES encryption.
- **D/V:** DOS date/time and extended timestamps (volatile).
- **Sig default:** JAR: **strip** `META-INF/*.SF|RSA|DSA|EC`, but recompute and keep the `MANIFEST.MF` per-entry digests (Q-ZIP-12), because an invalid signed JAR fails to load while an unsigned one loads. APK: **strip** the v1 files and the v2/v3 signing block *together*, because apksigner's `X-Android-APK-Signed` check detects partial stripping. Warn `signature-stripped`. APK is unusable either way, and stripping makes `apksigner sign` straightforward. NuGet/VSIX `.signature.p7s`: strip.
- **Watch:** APK signatures cover *ZIP layout*, so even raw-copying every entry while changing one still invalidates them. Signing must happen after the final layout.

### PDF
- **L:** file → objects (and object streams) → stream filters (Flate, LZW, ASCIIHex, ASCII85, RunLength, DCT, JPX, JBIG2, CCITT) with **predictors** (PNG/TIFF) → content operators, fonts, images.
- **F:** **absolute byte offsets** in the xref table (fixed 10-digit fields) or in xref streams (`/W`, `/Index`), `startxref`, `/Prev` chains across incremental updates, `/Length` (may be an indirect object, decimal width varies), object stream `/N` and `/First` with an internal offset header, trailer `/Size`, and linearization hint tables and `/L` file length.
- **I:** none for integrity as such. In `/ID [permanent changing]`, `/ID[0]` is identity *and feeds encryption key derivation*, so it is always kept. `/ID[1]` is volatile.
- **A:** **signatures:** `/ByteRange` (the whole file except the `/Contents` hex) plus CMS/PKCS#7 `/Contents` (reserved fixed width), DocMDP/FieldMDP permissions, UR3 usage-rights signatures, and DSS/LTV data. **Encryption:** RC4/AES with owner and user passwords, and `/Perms`. Permission-only encryption (empty user password) is look-through-able (SPEC §12.2).
- **D/V:** appearance streams (derived, Q-PDF-7), linearization (derived, dropped when rewritten), `/ModDate` and `/ID[1]` (volatile), and XMP metadata `xmp:ModifyDate`.
- **Sig default:** write merges as an **incremental update** (Q-PDF-3), so earlier signatures stay valid for their signed revision and the new revision is unsigned (`signature-not-covering-changes`). With `--layout=compact`, `auto` strips signature values (`signature-stripped`). When an incremental update is impossible and the user did not ask for compact layout, the merge reports conflict `signature` (exit 1, policy side written) unless `--signatures=strip|keep` is explicit (pdf.md §7.1).
- **Watch:** DocMDP "no changes allowed" certification makes *any* incremental update a policy violation, even though the signature bytes stay valid. Report `certification-violated`.

## 3D and data

### OBJ / PLY / STL
- **F:** PLY header element counts, list-length prefixes, and binary layout. The binary STL triangle count. **OBJ positive face indices are absolute pool indices**, so growing an earlier pool shifts every later index (framing churn).
- **D/V:** STL facet normals are derived (Q-MESH-4). The 80-byte binary STL header is volatile. The u16 attribute word is content when a tool uses it for color.
- **I/A:** none.

### JSON / XML / CSV / text
- No binary envelope. **Encoding layers:** character encoding and BOM (UTF-16/32), and embedded base64 (data URIs).
- **External integrity references** live inside *content*: `package-lock.json` `integrity`, `yarn.lock` `checksum` `Cargo.lock` `checksum`, `go.sum`, `poetry.lock`, `uv.lock`, `Chart.lock`, and `flake.lock` hashes. They are coupled to their version (SPEC §12.5). By default a lockfile is taken whole from one side (Q-JSON-5). `--lockfile=merge` merges per entry, keeping each entry's version, `resolved` URL, and hash as one unit from one side.
- **Signed JSON:** JWS, TUF, and in-toto metadata, and SOPS `sops.mac` (keyed). Kept with `signature-invalidated` under `auto`.
- **XML signatures (XMLDSig)** in generic XML (SAML, signed configs): each `DigestValue` (over a C14N subtree) is recomputable **integrity**, and only `SignatureValue` is keyed **authenticity**. Kept with `signature-invalidated` under `auto` (Q-XML-10). Verification needs C14N, which xml.md schedules for its later phase, so verification reports `unverifiable` until then.

### SQLite
- **F:** header page size, **file change counter**, in-header database size in pages, freelist trunk and count, **schema cookie**, `version-valid-for`, B-tree page headers, cell pointers, and overflow chains.
- **I:** none by default. Optional `cksumvfs` adds 8-byte per-page checksums, which go stale unless the output connection registers that VFS (Q-SQLITE-7). The **WAL** has frame checksums and salts, and its magic (`0x377f0682` or `0x377f0683`) selects the checksum byte order. SQLCipher's per-page HMAC is opaque (D-41).
- **Writer delegation:** SQLite writes the file (SPEC §12.4), so splinter never touches framing. Page-level byte churn is expected and is attributed as `framing`/`encoding`. **V:** file change counter and `version-valid-for`.

## Generic

### TAR / gzip / zstd / xz
- **TAR:** octal size fields, the **header checksum** (unsigned byte sum with the checksum field treated as spaces), 512-byte block padding, two zero blocks at the end, and pax extended records whose **decimal length includes its own digits** (a fix-point). GNU long names (`././@LongLink`).
- **gzip:** header (with a **volatile mtime**, OS byte, and optional name and comment) and a trailer with **CRC-32** and **ISIZE**. Multi-member streams are possible.
- **zstd:** frame header (with an optional content size), an optional **XXH64 content checksum**, and skippable frames.
- **xz:** block headers, **CRC-32/CRC-64/SHA-256 check** per block, and an index with its own CRC-32.

### Binary fallback
- Unknown envelope. After v1 (D-49), `--scan-streams` finds embedded zlib, gzip, zstd, and LZ4 streams and diffs their decoded content, which explains most `unexplained` churn in container-ish unknown formats. It never merges inside them.

---

## Cross-format rules restated

1. Envelope fields are **never content**. They never appear in a change set except under `--show`.
2. A changed field is explained when its new value equals the value **recomputed** from the new content.
   Otherwise it is reported as `integrity-mismatch-input` (for integrity fields) or `framing-inconsistent`.
3. Untouched units are copied raw, *with* their envelope bytes, whenever the layout allows.
4. Signatures are verified when possible and are never forged. Re-signing happens only with user-supplied keys.
5. Every new format document fills in its section 7.1 *before* writer code is written, because the writer is
   built on the Emitter from that inventory.
