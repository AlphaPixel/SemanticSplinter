# CSV / TSV — Implementation Document

**Tier:** T1 · **Module:** `semantic-splinter-csv` · **vcpkg feature:** `csv` · **Depends on modules:** `text` (core seqdiff, encoding detection)
**Extensions / magic:** `.csv`, `.tsv`, `.tab`, `.psv`. No magic; content probe sniffs the dialect on the first 64 KiB and returns confidence 70 when at least 5 records have a consistent field count ≥ 2 under one dialect, 40 for 2 to 4 records. Extension alone gives 60.
**Status (v1 = MVP, #10):** own tokenizer (RFC 4180, doubled quotes, quoted newlines, CR/LF/CRLF, blank lines, ragged rows, `sep=` line), sniffer over `, \t ; |`, UTF-8 with or without BOM, UTF-16/32 through `src/core/text` (other bytes compared as is and reported `windows-1252`), header and key detection (§2, §3), keyed rows by KeyedMerge and unkeyed rows by SequenceMerge with a three-way merge per cell, column add/delete with `modify-delete` per column, span-splice writer (`src/formats/common/splice.hpp`) in ours' dialect with `dialect-mismatch`, dump, diff with numeric `delta`/`rel`, `--csv-key`, `--csv-header=auto|yes|no`. Deviations: the content probe needs *every* sampled record to share the field count (40 only for 3 or more fields) and rejects text-like samples (a column empty in every record, as in `x = 1;` lines, or most later fields starting with a space); a header is also detected when row 0 names a key column (`id`, `key`, …, `*_id`); column order is ours' (theirs' when only theirs reordered) instead of a SequenceMerge, and a column reorder is one `column-order` change; unkeyed merges pair rows only in equal-length runs; the writer reuses the splice helper rather than the Emitter, since CSV has no framing or integrity fields (§7.1); `--conflict=mark` falls back to ours (`mark-unsupported`). Unified diffs carry `@@ ADDRESS` hunk headers and `--annotate` metrics (#33). Not yet (v2 or later): rename detection, `--csv-order`, sorted-table re-sorting (Q-CSV-4), `--csv-mark`, `--csv-cell-lines`, `--delimiter`/`--quote`/`--encoding`/`--ignore-space`/`--numeric`/`--ignore-columns`/`--trim-empty-columns`, backslash escapes and `'` quotes, comment lines, `possible-rename`, `auto: filled-empty` reporting, the grid and HTML renderers, patch payloads, `.csv.gz`, and fuzz targets.

## 1. Why it matters

CSV is where spreadsheets end up when teams want them in Git: game balance tables, item and quest databases,
localization string tables (one column per language), test vectors, data science samples, configuration matrices.
Line merges fail in typical ways:

- Two people edit different cells of the same row: one physical line, so Git conflicts.
- A quoted field containing a newline spans multiple lines; a line merge can split a record and corrupt the file.
- Someone adds a column (a new language): every line changes, conflicting with every row edit on the other side.
- Re-saving in Excel changes line endings, quoting, or adds a BOM, producing a whole-file diff.

Users should see "row `sword_01`, column `damage`: 12 to 15", merge cell edits independently, and merge column
additions with row edits.

## 2. Format anatomy

RFC 4180 is the baseline: records separated by CRLF, fields by comma, fields optionally enclosed in `"`, embedded
quotes doubled, quoted fields may contain delimiters and line breaks, optional header row. Real files deviate:

- Delimiter `,` `;` (European Excel) `\t` `|`.
- Line endings LF, CRLF, CR (old Mac exports), mixed.
- Encoding UTF-8 with or without BOM, UTF-16LE with BOM (Excel "Unicode text"), Windows-1252.
- Quote character `"` (rarely `'`), escape by doubling or by backslash (MySQL dumps).
- Ragged rows (fewer or more fields than the header), trailing empty fields, final line without newline,
  blank lines, `sep=;` first line (Excel hint), comment lines `#` (some game tools).

**Dialect sniffing** (similar to Python's `csv.Sniffer`, but deterministic):

1. Encoding: BOM decides; else valid UTF-8 means UTF-8; else Windows-1252 (reported as an assumption).
   `--encoding=` overrides. This logic is shared with text.md.
2. For each candidate delimiter in `, ; \t |` and quote `"`, tokenize the sample and score =
   (fraction of records with the modal field count) × log2(modal count), requiring modal count ≥ 2. Highest wins;
   ties prefer `,` then `\t` then `;` then `|`.
3. Header detection: row 0 is a header if all its fields are non-empty, unique, and non-numeric, and at least one
   column has a type in rows 1..N (numeric, boolean, date) that row 0 does not match. `--header` / `--no-header` override.
4. Line ending: majority of record terminators; mixed is recorded per record.

All three files of a merge must sniff to the same dialect, otherwise the dialect of ours is used for all and a
`dialect-mismatch` warning is reported (base re-parse with ours' dialect must succeed, else trouble).

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Column | header name (dedup: `name`, `name#2`); else index | `/columns/damage` | SequenceMerge (column order) + KeyedMerge (membership) |
| Row (keyed) | value(s) of key column(s) | `/rows/id:sword_01` | KeyedMerge, order via SequenceMerge |
| Row (unkeyed) | content hash | `/rows#range=12,1` (base index) | SequenceMerge |
| Cell | (row key, column name) | `/rows/id:sword_01/damage` | OpaqueLeaf (or text lines for multi-line cells) |
| Dialect / encoding / BOM | singleton | `/dialect` | OpaqueLeaf |
| Header row text | singleton per column | `/columns/damage/label` | OpaqueLeaf |

**Key column selection** (stops at first hit):

1. `--key=COL[,COL...]` by name or 1-based index; or a `.splinter.toml` rule per path glob.
2. Auto: first column whose header matches `id`, `key`, `uuid`, `guid`, `name`, `code`, `string_id`, `msgid`
   (case-insensitive, also `*_id` as the first column), and whose values are non-empty and unique in base, ours,
   and theirs.
3. Auto: the first column (only if it is unique and non-empty in all three, and not numeric-sequential 1..N, since
   a row number column is position, not identity).
4. None: rows are unkeyed. Report `"row_identity": "position"`.

Composite keys are written `id,lang:sword_01,de` in addresses. Key values are percent-encoded per SPEC §3.1.

**Column identity:** header name. Rename detection: a column removed and a column added on the same side whose
cell values match on ≥ 90 % of keyed rows is a `move` with `kind: "column-rename"` (confidence = match ratio).
No header: columns are identified by position, and column insert/delete detection uses seqdiff over column
content hashes.

**Invariants:** every output row has the header's field count (unless input was ragged and the row was untouched);
key uniqueness for keyed tables; header names unique.

**Dependencies and derived data:** none in generic CSV. Spreadsheet formulas are not present in CSV (exported values
are already derived); totals rows are a known hazard (Q-CSV-5).

## 4. Normalization

Comparison ignores: quoting of a field when the unquoted value is equal (`"abc"` equals `abc`), line ending
style, BOM, the final newline, trailing empty records, and Excel `sep=` lines. Encoding differences are
compared after decoding.

Never normalized by default: whitespace inside fields (`--ignore-space` / `-b`-like trimming is opt-in),
case, numeric spelling (`1.0` vs `1`, `1,5` vs `1.5`), empty vs missing trailing field (reported as
`kind: "ragged"`), row order for keyed tables (order change reported only with `--csv-order`, default off
for keyed and inherently on for unkeyed).

Options: `--key`, `--header/--no-header`, `--delimiter=C`, `--quote=C`, `--encoding=E`, `--ignore-space`,
`--numeric` (compare numbers by value, `1.0` equals `1`), `--ignore-columns=a,b` (volatile columns like
`last_modified`), `--csv-order`.

## 5. Diff

1. Parse with the dialect into `Table { header, rows: vector<Row{span, fields: vector<Field{span, quoted, value_view}>}> }`.
2. Columns: KeyedMerge-style membership diff by name, then seqdiff over base and target column order for moves.
   Rename detection as above.
3. Rows: keyed tables use hash maps by key (deterministic iteration via sorted key list); unkeyed use core seqdiff
   (text.md §5, histogram) over row hashes computed **only over columns common to both sides**, so adding a
   column does not make every row look changed. Unmatched rows in one gap are paired by cell similarity
   (≥ 50 % equal cells) to report `update` instead of erase plus insert, with `confidence`.
4. Cells: compare per common column.

**Metrics:** rows added/removed/changed/moved, columns added/removed/renamed/moved, cells changed, per-column change
counts, numeric delta for numeric cells (`"delta": 3`, `"rel": 0.25`).

**dump() layout:** one line per cell of keyed rows, so textual diffs read like a cell report:

```text
#columns id,name,damage,weight
[id:sword_01] name = "Sword"
[id:sword_01] damage = 12
[id:shield_02] name = "Shield"
```

Unkeyed tables dump one line per row as canonical CSV (all fields quoted only if needed, LF endings), prefixed
with nothing, so `splinter -u` over dumps looks like a normal CSV diff but with stable quoting.

**Renderers:** `-y` side-by-side shows a cell grid with changed cells highlighted (ANSI color with `--color`).
`--visual=html` (v2) writes a table with per-cell coloring.

**Cost:** O(cells) parse and hash. Tables up to 10^7 cells within default limits; key maps use a sorted
vector of `(key hash, row index)` for memory compactness.

## 6. Merge

- Columns: membership by KeyedMerge (added by one side = added; deleted by one side and cells edited by the other in
  that column = conflict `modify-delete` per affected cell group, reported once per column). Column order by
  SequenceMerge; both inserting different columns at the same position is resolved by keeping both (ours first),
  since column order is rarely semantic, reported as `auto: both-inserted`.
- Rows keyed: KeyedMerge on row key with recursion into cells, row order from ours with theirs-only rows inserted
  after their nearest preceding surviving key (SPEC §5.1 order rule), which keeps sorted tables sorted when both
  inputs were sorted. If base, ours, and theirs are all sorted by key, the output is re-sorted by the same comparator
  (detected: lexicographic or numeric).
- Rows unkeyed: diff3 SequenceMerge over row hashes; a chunk where both sides changed rows that pair 1:1 by
  similarity recurses into cells, otherwise conflict over the row range.
- Cells: OpaqueLeaf three-way. Multi-line cells (quoted with newlines, such as dialogue text) optionally merge by lines
  (`--csv-cell-lines`).
- Key column edit on one side: that is a row identity change, treated as delete plus insert with a
  `possible-rename` hint when the other cells are ≥ 80 % equal; if the other side edited that row: conflict `modify-delete`.
- Localization tables: adding a language column on one side and adding rows on the other merges cleanly; new rows get
  an empty cell in the new column (reported `auto: filled-empty`), never a value copied from anywhere.

**`--conflict=mark`:** CSV has no comments in general. Mark mode writes ours' value in the cell and adds
a trailing column `splinter_conflict` holding `x-<id>: theirs=<value>` on affected rows. This changes the schema, so it
is opt-in via `--csv-mark=column`; plain `--conflict=mark` falls back to ours with a warning. `--csv-mark=text` writes
Git-style line markers around whole conflicting rows (file still tokenizes as CSV but contains junk records).

**Patch payload:** per touched cell, before/after as CSV-encoded field text; rows as full CSV records. Text, not binary.

## 7. Serialization and preservation

The writer emits records in order and, per record:

- **Untouched row from ours:** copy the raw record span from ours' bytes, including its terminator. Byte-exact.
- **Row from theirs, untouched:** copy theirs' raw span, converting only the record terminator to ours' dominant line ending
  and, if delimiters differ, re-tokenizing (only possible after a dialect mismatch warning).
- **Row with merged cell changes:** rebuild the record field by field: untouched fields copy their raw span (keeping
  each field's original quoting), changed fields take the raw span of the winning side's field if its quoting is valid in
  ours' dialect, else are quoted minimally (quote if the value contains delimiter, quote char, CR, LF, or leading/trailing
  space when ours quotes such fields; if ours quotes every field in the column, quote always).
- **New column:** header cell quoted per ours' header quoting style; cells in rows for which no value exists are empty.
- BOM, `sep=` line, and final-newline presence follow ours.

Lost: nothing for untouched rows. A rebuilt row may change from irregular quoting (`"a",b,"c"`) only in the changed fields.

### 7.1 Envelope fields

**Layer stack:** `[gzip for .csv.gz/.tsv.gz via containers.md] → bytes → BOM and encoding (UTF-8, UTF-16LE, Windows-1252) → record terminators (CRLF/LF/CR) → optional `sep=` hint line → dialect (delimiter, quoting, doubled quotes) → table`.

| Field | Class | On write |
|---|---|---|
| BOM, encoding | encoding | ours' (section 7) |
| record terminators, final newline | encoding | ours' dominant style; untouched rows keep their own |
| quoting and quote doubling | encoding | per field, raw span of the winning side when valid (section 7) |
| delimiter, `sep=` line | encoding | ours' |
| blank lines between records | encoding | kept with the following record of ours; dropped from rows of other sides |
| gzip mtime / CRC-32, ISIZE | volatile / integrity | ours' header; recomputed trailer (not in v1: `.csv.gz` is not detected) |

Reviewed for v1 (#10): the table matches the parser except for the two rows amended above. UTF-16BE and
UTF-32 also load (through `src/core/text`), and a diff reports all of these fields as one `encoding` change at
`/dialect`. With no framing or integrity, the writer splices record spans rather than using the Emitter.

No framing, integrity, authenticity, or volatile fields in CSV itself. Checksum sidecars (`SHA256SUMS`, `.sfv`) and
Frictionless `datapackage.json` resource `hash` values are external integrity references held in *other* files,
which become stale and are reported only in `-r` mode. **Churn:** a cell edit changes only that field (`content`);
an Excel re-save (BOM, CRLF, `;`, requoting) is all `encoding` with zero content changes (Q-CSV-6).

## 8. Validation

Re-parse output with ours' dialect: field count per row equals header count (or matches the source row for untouched ragged
rows), key uniqueness, header uniqueness, every record terminated consistently. Compare parsed output to merged model by hash.

## 9. Libraries

| Library | vcpkg port | License | Status | Notes |
|---|---|---|---|---|
| vincentlaucsb/csv-parser | `vincentlaucsb-csv-parser` 5.3.0 | MIT | verified port, version, license | Fast, dialect guessing, but exposes decoded values only, no raw spans or per-field quote state. |
| rapidcsv | `rapidcsv` 9.07 | BSD-3-Clause | verified port, version, license | Whole-table convenience API, rewrites on save. |
| fast-cpp-csv-parser, Python `csv` | n/a | n/a | not evaluated further | No vcpkg port (assumed) / not C++. |

**Recommendation: own tokenizer** (about 400 LOC plus 250 LOC sniffer and 300 LOC writer). A CSV state machine is small,
and we need per-field raw spans, quote flags, per-record terminators, and limit checks (max fields per record 65 536,
max field 64 MiB, max records from `--max-memory`) that no candidate exposes. Encoding conversion reuses the text module
(UTF-16 to UTF-8 with an offset map back to source bytes for span copies).

## 10. Hard cases and explicit non-goals

- Tables whose first row is not a header but looks like one (all text): header detection may misfire; `--no-header` fixes,
  and the sniffer reports its decision.
- Duplicate keys in base (data errors): auto key detection rejects the column; explicit `--key` with duplicates fails with
  `invalid_input` listing the first duplicates.
- Numbers formatted by locale (`1.234,5`), dates in mixed formats: compared as strings unless `--numeric`.
- Excel artifacts: trailing empty columns (`a,b,,,`) that vary between saves; `--trim-empty-columns` treats all-empty
  trailing columns as absent.
- Very wide tables (10k columns, genomics): column seqdiff is O(columns × D), fine.
- Totals or summary rows (derived values): not detected; Q-CSV-5.
- Non-goals: type inference beyond header detection, schema files (CSVW, frictionless) in v1, formulas, merging spreadsheet
  formatting (it does not exist in CSV).

## 11. Test plan

- **Generated:** random tables with random dialects; edits on two sides (cell edits, row add/delete/move, column add/delete/rename/reorder);
  properties: disjoint edits merge to sequential application, idempotence, `merge(b,o,o)==o`, byte-exact round trip.
- **Golden:** Excel-saved CSV (CRLF, BOM, `;`), UTF-16 TSV, multi-line quoted dialogue cells, localization table with new
  language column vs new rows, keyed row reorder plus edit, key edit conflict, ragged rows.
- **Compat:** text renderer output for unkeyed tables with `--csv=off` equals GNU diff output on the same file (falls back to text module).
- **Fuzz:** `fuzz_csv_tokenize` (round trip identity) and `fuzz_csv_sniff` (no crash, deterministic result).

## 12. Phases

- **MVP (M):** tokenizer, sniffer, keyed/unkeyed row merge, cell merge, column add/delete, span-preserving writer, dump.
- **v2 (S–M):** column rename detection, sorted-table reordering, side-by-side grid renderer, HTML visual, `--csv-mark=column`.
- **Later (S):** CSVW/frictionless schema hints for keys and types, JSON Lines interop.

## 13. Open questions

- **Q-CSV-1:** Auto-detect key column or require `--key`? **Provisional:** auto-detect with strict uniqueness on all three sides, report choice, `--key` overrides. **Why:** most game and localization tables lead with a unique ID column.
- **Q-CSV-2:** Is column insertion order significant when both sides add columns at the same place? **Provisional:** no, keep both ours-first. **Why:** consumers address columns by header name.
- **Q-CSV-3:** What does `--conflict=mark` do for CSV? **Provisional:** fall back to ours plus sidecar; `--csv-mark=column` adds a conflict column on request. **Why:** markers break consumers; an extra column is visible but changes the schema.
- **Q-CSV-4:** Keep a sorted table sorted on merge? **Provisional:** yes, when base, ours, and theirs are all sorted by the same key comparator. **Why:** sorted files are a common convention and the order rule alone can break it.
- **Q-CSV-5:** Detect totals/derived rows? **Provisional:** no in v1. **Why:** no reliable signal in plain CSV; document the hazard.
- **Q-CSV-6:** Dialect mismatch between inputs (Excel resave changes `,` to `;`)? **Provisional:** parse each with its own sniffed dialect, compare decoded values, write in ours' dialect, warn. **Why:** a resave is not a semantic change.
- **Q-CSV-7:** Should a numeric-sequential first column (`1..N`) be a key? **Provisional:** no, treat as position. **Why:** row-number columns renumber on insertion and would turn inserts into mass edits.
