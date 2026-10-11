# SQLite — Implementation Document

**Tier:** T2 · **Module:** `semantic-splinter-sqlite` · **vcpkg feature:** `sqlite` · **Depends on modules:** `text` (dump diff), `binary` (BLOB fallback)
**Extensions / magic:** `.db`, `.sqlite`, `.sqlite3`, `.db3`, `.gpkg` (GeoPackage), `.mbtiles`; magic `"SQLite format 3\0"` (16 bytes at offset 0). Companion files `-wal`, `-shm`, `-journal` are detected and refused as standalone inputs.
**Status (v1 = MVP, #13):** schema diff (KeyedMerge on `(type, name)` over normalized SQL), row diff keyed by declared primary key (composite, `WITHOUT ROWID`) or rowid (`identity-rowid` warning), cell diff and three-way merge per column (`both-modified`, `modify-delete`, `add-add`), header fields (`user_version`, `application_id` merged; a `page_size`/`encoding` change taken from theirs is an `unsupported` conflict that keeps ours), add-add of SQLite-allocated row ids (rowid or `INTEGER PRIMARY KEY`) renumbered after `max(all sides)` (`auto: renumbered`, SPEC §2.3) unless a foreign key points at the table, tables whose schema one side changed (or added, or dropped) taken whole from that side, FTS5/R*Tree tables merged as one unit (their shadow tables copied whole from the winning side), triggers dropped and recreated around the merged DML (Q-SQLITE-5), `constraint` conflicts that keep ours' row, `sqlite_sequence` as `max(ours, theirs, result)`, `derived-stale` for `sqlite_stat1`, `foreign-key` warnings from `PRAGMA foreign_key_check`, `integrity_check` on the result, dump/textconv (§5), WAL sidecar refusal, WAL-mode files kept in WAL mode, cksumvfs checksums kept valid (vendored `src/formats/sqlite/cksumvfs.c`, Q-SQLITE-7), envelope map and `inspect`. An untouched database, or a merge in which one side is unchanged, writes that side's bytes unchanged. Deviations: inputs and the output are opened with `sqlite3_deserialize` on a private in-memory copy, not `immutable=1` files, so container members and Git temp files need no file and nothing is created beside them; the output is that in-memory copy of ours, so untouched pages still keep ours' bytes; cksumvfs's checksum function is applied to the pages that differ from ours instead of registering the VFS (an in-memory database has no VFS to shim, and registration would change the process-wide default VFS); rowid addresses are `rows/rowid:N`; key segments escape `~`, `/`, `,` as `~0`, `~1`, `~3`; the writer is SQLite itself, without the Emitter (§7.1). Not yet (v2 or later): ALTER/12-step rebuild for row edits on a table whose schema the other side changed (a conflict on the table today), FK `dependency` conflicts, `--key`, `--renumber-theirs` with FK rewriting, `--ignore-table`, `--analyze`, `--vacuum`, `--wal=checkpoint`, `--detect-renames`, `--dump-max-rows`, `--conflict-table`, visible-row diff of virtual tables (the dump shows a content hash), generated columns in the dump, BLOB recursion, patch payloads, the session changeset export, streaming (rows are loaded into memory, charged to `--max-memory`), and fuzz targets.

## 1. Why it matters

SQLite files are committed as game data (item tables, dialog, level metadata), mobile app seed databases,
GeoPackage map layers, MBTiles tile sets, and test fixtures. Git sees an opaque blob, and two people adding
rows to different tables collide on every merge even though the edits are independent. A page-level byte
diff is useless: one INSERT can move pages, rewrite the freelist, and bump the file change counter.

Users should be able to:

- See a schema diff (tables, columns, indexes, views, triggers) and a row diff per table keyed by primary key.
- Merge two branches that added, edited, or deleted different rows, with conflicts only on the same row and column.
- Get a `textconv` dump that is stable and diffable as text.

## 2. Format anatomy

- File = array of pages (512..65536 bytes). Page 1 holds the 100-byte header: page size, file change counter
  (offset 24), schema cookie (offset 40), text encoding (offset 56: UTF-8/UTF-16le/UTF-16be), user_version
  (offset 60), application_id (offset 68), and others.
- `sqlite_schema` (alias `sqlite_master`) lists every table, index, view, and trigger with its `CREATE` SQL.
- Tables are rowid B-trees (implicit 64-bit `rowid`, or `INTEGER PRIMARY KEY` aliasing it) or
  `WITHOUT ROWID` tables (clustered index on the declared PRIMARY KEY).
- Internal tables: `sqlite_sequence` (AUTOINCREMENT counters), `sqlite_stat1/4` (ANALYZE statistics).
- Virtual tables (FTS5, R*Tree) store data in shadow tables (`x_data`, `x_idx`, `x_content`, `x_node`, ...).
- WAL mode: committed pages may live in `name-wal` until checkpoint. The main file alone can be stale.

We never parse pages ourselves. All access goes through the SQLite library opened read-only
(`SQLITE_OPEN_READONLY`, `immutable=1` URI parameter for inputs so no `-shm` or `-wal` is created next to Git temp files).

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Header fields (user_version, application_id, encoding, page_size) | field name | `/header/user_version` | KeyedMerge of scalars |
| Schema object | `type:name` | `/schema/table:items`, `/schema/index:items_by_name` | KeyedMerge, value = normalized SQL (OpaqueLeaf) |
| Table | table name | `/tables/items` | KeyedMerge of rows |
| Row | primary key tuple (see below) | `/tables/items/rows/pk:42`, `/tables/loc/rows/pk:en,menu.start` | KeyedMerge of columns |
| Cell | column name | `/tables/items/rows/pk:42/price` | OpaqueLeaf (value + storage class) |
| BLOB cell (optional recursion) | column name | `/tables/tiles/rows/pk:3,1,2/tile_data` | recurse via registry, else OpaqueLeaf |

Row identity, in order of preference:

1. Declared `PRIMARY KEY` (single or composite), from `PRAGMA table_xinfo` (`pk` > 0 ordered by `pk`).
   Composite keys are rendered as comma-joined values with `~0`, `~1`, `~2` escaping (JSON Pointer) and commas escaped as `~3`.
2. `rowid` for rowid tables without a declared PK. This matches sqldiff's default pairing. Rowid identity is
   weaker: `VACUUM` may renumber rowids for tables without `INTEGER PRIMARY KEY`, so a diff across a VACUUM shows churn.
   The report flags these tables with `identity: rowid` and a warning.
3. Tables with neither (no rowid access, no PK; only possible for odd virtual tables) fall back to
   SequenceMerge over rows ordered by all columns, matched by row content hash. Reported as `identity: content`.

Option `--key=TABLE:COL[,COL]` overrides identity per table (useful for UNIQUE natural keys such as `slug`).

Values carry the SQLite storage class: `NULL`, `INTEGER`, `REAL`, `TEXT`, `BLOB`. `1` (INTEGER) and `1.0` (REAL)
are different values. REAL equality is bitwise on the IEEE double, except that `-0.0` and `0.0` compare equal only with `--ignore-float-sign-zero` (off by default).

Dependencies (`depends_on`):

- Row in table T depends on `/schema/table:T`.
- Index, trigger depends on its table. View depends on the tables named in its SQL (best-effort parse of `FROM`/`JOIN` identifiers; unknown names ignored).
- Foreign-key child row depends on the parent row addressed by the FK value (from `PRAGMA foreign_key_list`).
  Deleting a parent row on one side while the other side inserts a child pointing at it is a `dependency` conflict.

Derived data (never merged, regenerated or dropped):

- Index contents (rebuilt by SQLite when rows change).
- `sqlite_stat1/4`: excluded from the row diff by default. On merge, if either side changed rows, the output
  runs nothing and keeps ours' stats; the report warns `derived-stale` at `/tables/sqlite_stat1`. `--analyze` runs `ANALYZE` on the result.
- FTS5/R*Tree shadow tables: never diffed directly (sqldiff warns that applying shadow-table diffs corrupts
  FTS/rtree). The virtual table's visible rows are diffed instead, keyed by rowid, and writes go through the virtual table.
- `sqlite_sequence`: treated as derived. Output value = max(ours, theirs, max(rowid) in result).
- Header file change counter and `version-valid-for` (volatile), schema cookie, freelist, and page layout
  (framing and encoding): never reported as changes (7.1).

## 4. Normalization

- Schema SQL: whitespace collapsed outside string literals, identifier quoting (`"x"`, `[x]`, `` `x` ``)
  normalized for comparison only, keyword case folded. Output always uses the original text of the side that wins.
- `IF NOT EXISTS` is dropped by SQLite from stored SQL, so no work there.
- Row order is never significant (rows are compared by key).
- Text values in UTF-16 databases are compared as UTF-8 after conversion by SQLite (`sqlite3_column_text`).
- Never normalized: column order in a table (significant for `SELECT *` users; reported as a schema change),
  storage class differences, trailing spaces in TEXT, collations.
- `--ignore-table=GLOB` (repeatable) skips tables, for caches and logs.

## 5. Diff

Algorithm (own SELECT-based, default):

1. Attach both inputs to one connection: `ATTACH 'file:a?immutable=1' AS a`, same for `b`. Read-only.
2. Schema diff: KeyedMerge-style comparison of `sqlite_schema` rows by `(type, name)`; `internal` objects
   (`sqlite_autoindex_*`) are reported only as part of their table.
3. For each table present on both sides with compatible keys: stream both sides with
   `SELECT <pk cols>, <all cols> FROM t ORDER BY <pk cols>` and do a sorted merge-join in C++.
   Memory is O(1) per table plus one row; cost O(n) after SQLite's index scan. For column sets that differ,
   the common columns are compared and added or dropped columns are reported as schema changes, not per-row noise.
4. Paired rows that differ produce one `update` per changed column (kind `cell`). Unpaired rows produce `insert`/`erase` (kind `row`).

Session-extension alternative: `sqlite3session_diff()` produces a changeset between an attached table and the
main one, and the changeset can be applied with `sqlite3changeset_apply()` with a conflict callback. It is
available through the vcpkg `sqlite3[session]` feature. It only works on tables with a PRIMARY KEY and needs
identical schemas, so it cannot be the sole engine. We use it for nothing in v1. See Q-SQLITE-1.

Metrics: per table `rows_inserted`, `rows_deleted`, `rows_updated`, `cells_changed`, plus totals, and
`identity` (`pk`, `rowid`, `content`). `-q` prints `Databases a and b differ` style output through the GNU-compatible brief path.

`dump()` layout (textconv), deterministic and one unit per line:

```text
sqlite 3 page_size=4096 encoding=UTF-8 user_version=7 application_id=0
schema table items CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT, price REAL)
schema index items_by_name CREATE INDEX items_by_name ON items(name)
table items pk=(id) rows=3
row items [1] {"name":"Sword","price":12.5}
row items [2] {"name":"Shield","price":8.0}
```

Rows are sorted by key (SQLite's ORDER BY with BINARY collation on key columns), values are rendered as JSON
with storage-class tags for non-obvious cases (`{"$blob":"xxh3:...","len":4096}`, `{"$real":"1.0"}`).
BLOBs over 64 bytes are hashed in the dump. Tables over `--dump-max-rows` (default 1 000 000) are summarized.

Visual output: none. Large files: a 1 GB database diffs at SQLite scan speed; peak memory stays bounded.

## 6. Merge

- Schema: KeyedMerge on `(type, name)`. A one-sided schema change wins. Two-sided changes to the same
  object with different normalized SQL: conflict `both-modified`. Column added on one side and rows edited
  on the other: allowed (new column gets its DEFAULT for theirs' new rows, reported `auto: dependency-touched`).
  Table dropped on one side while the other side changed rows: conflict `modify-delete`. Table renamed:
  detected when a dropped and an added table have the same column list and >= 90 percent of row keys
  shared (heuristic, carries `confidence`; opt-in `--detect-renames`).
- Rows: KeyedMerge by key. Cells: KeyedMerge by column, so ours editing `price` and theirs editing `name`
  on the same row merge cleanly. Same cell, different values: `both-modified`. Row deleted vs edited: `modify-delete`.
  Both inserted the same key with different values: `add-add`, recursed per column first (identical
  columns merge, only differing columns conflict).
- Auto-increment collision: both sides inserted new rows with rowid-identity or INTEGER PRIMARY KEY values
  above base's max. Same key, different content is `add-add`. Option `--renumber-theirs` (opt-in) re-keys
  theirs' inserted rows above `max(ours)` and rewrites FK references within theirs' inserted rows only;
  any reference from elsewhere makes it refuse. Reported as heuristic.
- Constraints: after applying, UNIQUE/CHECK/NOT NULL violations reject the statement. Each failure becomes a
  conflict `constraint` at the row address, and that row takes the policy side (ours by default, which never
  violates since ours was valid).
- `--conflict=mark`: falls back to ours with a warning. An optional `--conflict-table` writes a
  `splinter_conflicts(id, address, reason, ours_json, theirs_json)` table into the result, opt-in because it changes the schema.
- Patch payload: the hunk carries per-row before/after as canonical JSON (the dump row format), zstd+base85 for large BLOBs.
  Applying a patch is the standard merge path.

## 7. Serialization and preservation

Write path: copy ours to the temp output (byte copy of the file, after checkpointing nothing, because inputs
are opened immutable; if `ours-wal` exists see §10), open it read-write, `PRAGMA foreign_keys=OFF`,
`BEGIN IMMEDIATE`, apply schema changes (CREATE/DROP; ALTER TABLE ADD/DROP/RENAME COLUMN where possible,
else the 12-step table rebuild documented by SQLite), then row DELETE, UPDATE, INSERT with bound parameters
in key order, then `COMMIT`. Triggers are disabled during apply by dropping and recreating them inside the
transaction (`PRAGMA recursive_triggers` does not suppress them); their stored SQL is preserved verbatim.

What is byte-exact: pages that SQLite does not touch keep ours' bytes, because the output starts as a copy of
ours; page 1 always changes (header counters). Logical content of untouched tables is exact. Page layout, freelist, file change counter, and possibly page order differ; the report notes
`info: page-layout-changed` (not a warning). `--vacuum` runs `VACUUM` (deterministic output for the same logical
content and SQLite version, at the cost of rowid renumbering in tables without INTEGER PRIMARY KEY; refused for those tables unless `--force-vacuum`).

Determinism: the same SQLite version, same inputs, and same statement order yield identical bytes. We pin
the SQLite version through the vcpkg baseline and record `sqlite_version()` in the report.

### 7.1 Envelope fields

**Layer stack:** `file → pages (page size from the header) → page 1 header (100 bytes) + B-tree pages (page header, cell pointer array, cells, overflow chains, optional reserved bytes at each page end) + freelist trunk/leaf pages → records (varint header, serial types) → rows and schema`.

**Writer delegation (SPEC §12.4).** SQLite writes the file; splinter never emits a page byte itself and the
Emitter is not used. Verification on load is SQLite's (`PRAGMA quick_check`, §10); splinter only reads the header,
plus the cksumvfs checksums when byte 20 is 8. The only bytes splinter sets are bytes 18/19 (put back to ours' WAL
mode after the in-memory write) and the cksumvfs checksums of changed pages.

| Field (header offset) | Class | Verify on load | On write |
|---|---|---|---|
| magic (0..16) | framing | exact match | kept from ours |
| page size (16, u16 BE, 1 = 65536), write/read version (18, 19: 1 legacy, 2 WAL) | framing | SQLite | delegated; kept from ours (18/19 restored after the in-memory write, which needs 1) |
| reserved bytes per page (20) | framing | 8 suggests cksumvfs (below) | delegated |
| file change counter (24), `version-valid-for` (92), SQLITE_VERSION_NUMBER (96) | volatile | none | delegated |
| in-header database size in pages (28) | framing | valid only when (24) equals (92); else SQLite uses the file size | delegated |
| freelist trunk page and count (32, 36), largest root page (52), incremental vacuum (64) | framing | SQLite | delegated |
| schema cookie (40), schema format (44) | framing | SQLite | delegated (bumped by schema changes) |
| text encoding (56), user_version (60), application_id (68, `GPKG` for GeoPackage) | content | none | KeyedMerge (section 3); written with PRAGMAs |
| B-tree page headers, cell pointers, overflow pointers, varint record headers | framing, encoding | SQLite | delegated |
| cksumvfs 8-byte per-page checksum in the reserved bytes | integrity | when (20) is 8: page 1 decides whether the file uses cksumvfs; other mismatching pages are listed by `inspect` | recomputed with cksumvfs's function on every page that differs from ours, when ours uses cksumvfs (Q-SQLITE-7) |
| `-wal` header (32 bytes: magic `0x377f0682`/`0x377f0683` selecting checksum byte order, salts, header checksum) and frame headers (24 bytes: page number, commit size, salts, cumulative checksum) | framing, integrity | not read: a `-wal` next to an input is refused or checkpointed (section 10) | never written; the output is checkpointed and has no WAL |

**Raw copy:** the output starts as a byte copy of ours, so pages that SQLite never touches keep their bytes. `--vacuum` gives up that property for determinism.

**Signatures:** none in SQLite. SQLCipher and SEE encrypt pages (SQLCipher adds a keyed per-page HMAC); such files are detected as binary and are opaque (D-41).

**Expected churn:** one changed cell rewrites its leaf page, possibly its parent and index pages, and page 1
(change counter and `version-valid-for`). The report attributes page 1 header bytes per field and every other
changed page byte to `encoding` (pages are the B-tree encoding of the logical rows), so `unexplained` stays 0.
Churn is summarized as "N pages rewritten", never listed byte by byte.

## 8. Validation

On the temp output before rename:

- `PRAGMA integrity_check` must return `ok`.
- `PRAGMA foreign_key_check` (reported per violating row as conflicts `dependency` if new, warnings if they existed in ours already).
- Re-diff result vs the planned merge model: every planned change present, nothing else changed.
- FTS5 tables: `INSERT INTO t(t) VALUES('integrity-check')`.

CI external validator: the `sqlite3` shell from the `sqlite3[tool]` feature, running `.dump` on result and comparing to golden dumps.

## 9. Libraries

| Library | vcpkg port | License | Status | Use |
|---|---|---|---|---|
| SQLite | `sqlite3` 3.53.4 (verified) | `blessing` (public domain) (verified) | Very active | Required. Features `session`, `dbstat`, `fts5`, `json1`, `rtree`, `math`, `tool` exist in the port (verified) |
| SQLiteCpp | `sqlitecpp` 3.3.3 (verified) | MIT (verified) | Active | Not used: a 200-line RAII wrapper of our own is enough |
| sqldiff | ships with SQLite sources | public domain | Reference only | Behavior reference (rowid pairing by default, `--primarykey`, skips views/triggers, shadow-table hazard; all verified on sqlite.org/sqldiff.html) |

Recommendation: `sqlite3` port with features `session` (for Q-SQLITE-1 experiments), `fts5`, `rtree`, `json1`
so that databases using those virtual tables can be opened. Load extension disabled
(`SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION` off), `SQLITE_DBCONFIG_DEFENSIVE` on, `trusted_schema=OFF`.
Assumed, not verified: that the port compiles `SQLITE_ENABLE_SESSION` and `SQLITE_ENABLE_PREUPDATE_HOOK` together under the `session` feature.

Own code estimate: ~2.5k LOC (schema reader, merge-join diff, apply engine, ALTER rebuild, dump).

## 10. Hard cases and explicit non-goals

- WAL: if `name-wal` sits next to an input (non-Git use), we refuse unless `--wal=checkpoint` (copy both to temp, checkpoint the copy) is given. Git never commits `-wal` correctly anyway; the report says so.
- Malicious databases: schema SQL can contain triggers or views calling functions. Inputs are opened read-only with `trusted_schema=OFF`; we never run triggers on inputs. Corrupt files are rejected via `PRAGMA quick_check` on load (bounded by `--max-input`).
- Custom collations or application functions referenced by indexes or CHECK constraints: opening fails or apply fails. Reported as `unsupported` naming the collation. Non-goal: loading extensions (sqldiff's `--lib`).
- Generated columns: excluded from comparison (derived), included in dump with a marker.
- Schema migrations that rename columns on both sides: conflict, never inferred.
- Encrypted databases (SQLCipher, SEE): non-goal, detected as binary.
- Non-goal: diffing page-level structure or freelist content (`dbstat` exists for `inspect` only).

## 11. Test plan

- Fixtures generated at test time from SQL scripts (no checked-in .db files except two legacy-format samples).
- Golden reports for: one-sided insert/update/delete; disjoint cell edits on same row; same-cell conflict;
  modify-delete; add-add with identical and different content; column add vs row edit; table drop vs edit;
  composite PK; WITHOUT ROWID; rowid-only table across VACUUM (expect churn warning); FK dependency conflict; FTS5 table edits.
- Property test: for random row edit scripts E1, E2 on disjoint keys, merge(base, E1(base), E2(base)) equals E2(E1(base)) logically (compare dumps).
- Idempotence: merge(b, x, x) dump equals x dump; merge(b, b, x) equals x.
- Cross-check: our two-way row counts match `sqldiff --summary --primarykey` on PK tables.
- Fuzz: SQLite is fuzzed upstream; our harness fuzzes the dump and diff on mutated small databases opened via `sqlite3_deserialize`.

## 12. Phases

- MVP (M): schema + row diff, dump/textconv, row/cell KeyedMerge, apply into copy of ours, integrity checks.
- v2 (M): ALTER/rebuild for schema merges, FK dependency conflicts, `--key`, `--renumber-theirs`, FTS5/R*Tree via visible rows.
- Later (S each): BLOB recursion through the registry (MBTiles PNG tiles, GeoPackage geometry as OpaqueLeaf), conflict table, session-changeset export.

## 13. Open questions

- Q-SQLITE-1: Use the session extension (`sqlite3session_diff` / changeset apply) instead of our own SELECT merge-join? **Provisional:** No for v1; own merge-join, session only as an optional `--changeset` export later. **Why:** session requires a PK and identical schemas, and three-way logic lives in our KeyedMerge anyway.
- Q-SQLITE-2: Default row identity for rowid tables without INTEGER PRIMARY KEY? **Provisional:** rowid, flagged with a warning, matching sqldiff. **Why:** content matching on large tables is costly and ambiguous.
- Q-SQLITE-3: Should BLOB cells recurse into other formats by default? **Provisional:** Only when a probe returns confidence >= 90 and the BLOB is <= 16 MiB. **Why:** MBTiles/PNG BLOBs benefit, but probing every BLOB is slow.
- Q-SQLITE-4: Run `VACUUM` on merged output by default? **Provisional:** No, opt-in `--vacuum`. **Why:** it can renumber rowids and costs time; logical content is what matters.
- Q-SQLITE-5: How to handle triggers during apply? **Provisional:** Drop and recreate within the transaction so they do not fire. **Why:** merged rows already contain both sides' trigger effects.
- Q-SQLITE-6: Treat `sqlite_stat1/4` and `sqlite_sequence` as derived? **Provisional:** Yes. **Why:** they are caches or counters recomputable from table contents.
- Q-SQLITE-7: How to keep cksumvfs checksums valid in the output? **Decided (amended 2026-10-11, #93):** when ours has 8 reserved bytes per page and page 1's checksum verifies, recompute the checksum with cksumvfs's own function on every output page that differs from ours; otherwise leave the reserved bytes alone. The output connection does not register the VFS: the merge runs on an in-memory copy, so fixing the changed pages after `serialize` is simpler and gives the same bytes. **Why:** without the VFS, touched pages keep stale checksums and a later reader with cksumvfs fails with `SQLITE_IOERR_DATA`.
