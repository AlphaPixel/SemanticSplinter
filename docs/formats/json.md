# JSON — Implementation Document

**Tier:** T0 · **Module:** `semantic-splinter-json` · **vcpkg feature:** `json` · **Depends on modules:** `text` (core seqdiff only)
**Extensions / magic:** `.json`, `.jsonc`, `.json5`, `.geojson`, `.webmanifest`, `.babelrc`, `tsconfig.json`, `package.json`, `composer.json`, `*.code-workspace`. Content probe: after optional UTF-8 BOM and whitespace, first byte is `{` or `[` and the first 64 KiB tokenizes without error (confidence 60), or `{`/`[` followed by `"key":` (confidence 80). A bare scalar document is valid JSON but probes at confidence 10 so `text` wins.

## 1. Why it matters

JSON is the most common structured data file in repositories: package manifests, lockfiles, editor and CI
settings, data tables, localization bundles, API fixtures, and the container format for many other file types. Line-based merge fails on JSON in
predictable ways:

- Two sides append an element to the same array or a key to the same object. The text merge either conflicts
  on the closing bracket or produces a missing or doubled trailing comma, so the result does not parse.
- Reformatting (indent change, key sort) turns every line into a change and hides the one real edit.
- Arrays of records (`[{ "id": 7, ... }]`) reordered on one side and edited on the other conflict everywhere.

Users should see per-key and per-element changes addressed by JSON Pointer, merge independent edits to
different keys and different identified array elements automatically, and get a file whose untouched parts
are byte-identical to ours.

## 2. Format anatomy

- RFC 8259 / ECMA-404: object, array, string, number, `true`, `false`, `null`. UTF-8 is the only
  interchange encoding (RFC 8259 §8.1). A leading BOM is tolerated on read and preserved on write.
- Object member names SHOULD be unique (RFC 8259 §4). Duplicates exist in the wild and behaviour differs between
  parsers (first wins, last wins, error). We detect and report them.
- Numbers have no defined precision. Integers above 2^53 (IDs, Snowflake timestamps) and decimals like `0.10`
  vs `0.1` are textually distinct, and we must never rewrite them.
- **JSONC** (VS Code settings, tsconfig): `//` and `/* */` comments plus trailing commas.
- **JSON5**: JSONC plus unquoted identifier keys, single-quoted strings, hex numbers, `+`/leading/trailing
  decimal points, `Infinity`/`NaN`, multi-line strings with escaped newlines.
- **JSON Lines / NDJSON** (`.jsonl`, `.ndjson`): one value per line. Treated as a SequenceMerge over line records,
  each record recursing as JSON (Phase v2).

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Document root | singleton | `` (empty pointer) | composite |
| Object member | member name (escaped per RFC 6901) | `/config/timeout` | KeyedMerge |
| Array element with identity rule | value of key field(s) | `/items/id:42` | SequenceMerge (ID-matched) |
| Array element without rule | content hash, then position | `/tags/3` (base index) | SequenceMerge (hash-matched) |
| Scalar | parent address | `/config/timeout` | OpaqueLeaf (compared by normalized value) |
| Long string (> 1 line or > 200 chars) | parent address | `/desc#range=line,count` | SequenceMerge over lines (opt-in `--json-string-lines`) |
| Comment (JSONC/JSON5) | attached to following member | n/a | carried through spans, never diffed in v1 |

**Array identity rules.** Arrays are where JSON merge quality is decided. Resolution order for an array at
pointer `P`:

1. Explicit rule from options or `.splinter.toml`: `--json-key='/items/*=id'`, `--json-key='/**/layers=name'`,
   composite keys `--json-key='/rows=table,id'`. Path patterns use `*` (one segment) and `**` (any depth).
2. Dialect profile supplied by a dependent format (for example, index remapping or a fixed key on one array).
3. Auto-detection: if every element is an object and one of `id`, `uuid`, `guid`, `key`, `name`, `_id`, `@id`,
   `slug` (in that order) is present in all elements with unique scalar values in base, ours, and theirs,
   use it. The chosen key is reported in `stats.format.array_keys` so the user can pin it.
4. Otherwise elements are matched by content hash through core seqdiff (text.md §5, histogram by default).
   Unmatched elements in the same gap are paired by structural similarity (shared key set and equal values,
   score ≥ 0.6) so an edited record is an `update`, not `erase`+`insert`. These pairings carry `confidence`.

Identity key text in addresses: `id:42`, `name:Player%20One` (percent-encoding for `/`, `~`, `:`, `%`, and space),
composite `table,id:items,42`. Keys must be scalars; an object-valued candidate key disqualifies the rule.

**Invariants after merge:** the result parses in the input's dialect; no duplicate member names are introduced;
array key uniqueness holds for arrays that had a key rule (two sides adding the same key is `add-add`).

**Dependencies:** generic JSON has none. Dialects (index references, JSON Schema `$ref`) declare
`depends_on` through a profile hook `references(const Node&) -> vector<Address>`.

**Derived data:** none generic. Lockfiles (`package-lock.json`, `composer.lock`) are derived from manifests; a
profile marks them so `splinter` reports `derived-stale` and recommends regenerating rather than merging
(Q-JSON-5). Their hashes are external integrity references coupled to their versions (7.1).

## 4. Normalization

Insignificant, normalized for comparison only:

- Whitespace and indentation between tokens, trailing newline, CRLF vs LF.
- Object member order (KeyedMerge compares by key). Order changes alone are reported as one `kind: "order"`
  change per object only with `--json-order` (default: ignored).
- String escapes: `"é"` equals `"é"`, `"\/"` equals `"/"`. Comparison is on decoded UTF-8.
- Number spelling, with care: values are compared by **canonical decimal text**, not by `double`. Canonical
  form strips leading `+` (JSON5), lowercases the exponent, removes leading zeros of the exponent and trailing
  zeros of a fraction, so `1.50` equals `1.5`, `1e2` equals `100` only if `--json-numeric` is set (default:
  `1e2` and `100` differ, since some consumers distinguish integer and float). Integers of any length compare
  exactly as digit strings.
- JSONC/JSON5 comments and trailing commas: ignored for comparison.

Never normalized: string content, number value, key case, array order (unless the array has a key rule and
the dialect declares order insignificant), `null` vs missing member, duplicate member names (reported as
`invalid-duplicate-key` warning, and the last occurrence is the logical value, matching most JavaScript engines).

Options: `--ignore-order` (arrays without key rules compared as multisets), `-w` (ignore whitespace inside
string values, rarely useful), `--json-numeric`, `--json-order`.

## 5. Diff

**Algorithm.** Recursive tree diff on the `SpanJson` model:

1. Hash every node bottom-up (XXH3-128 over type tag plus canonical value; object hash is order-independent
   over sorted member hashes). Equal hashes short-circuit whole subtrees, so diff cost is proportional to the
   changed area.
2. Objects: key set diff. Same key with different hash recurses.
3. Arrays: identity-rule match or core seqdiff over element hashes. Matched pairs recurse; position changes of
   ID-matched elements become `move`.
4. Type change (object to array, number to string) is one `update` of the whole node.

**Change kinds:** `value`, `type`, `member` (add/remove key), `element` (insert/erase/move), `order`, `comment` (JSONC, report only).

**Metrics:** counts of added/removed/changed members and elements, numeric delta for number updates
(`"delta": 0.25`, `"rel": 0.02`), string edit distance for short strings.

**dump() layout (textconv).** One line per scalar leaf, sorted by address, values in canonical JSON:

```text
/name = "splinter"
/version = "0.1.0"
/items/id:7/label = "Sword"
/items/id:7/stats/dmg = 12
/tags/0 = "rpg"
/empty = {}
```

Arrays with identity rules use identity addresses, so reordering does not churn the dump. Empty containers
appear as `= {}` / `= []`. Strings longer than 120 bytes are emitted as `= "<first 100>…" xxh3:…` with
`--dump-full` restoring them. This is the textual comparison input when users run `splinter -u a.json b.json`
(SPEC §7.2).

**Exports** (`--export=FMT`): RFC 6902 JSON Patch (array positions computed against the evolving document,
`move` emitted for ID moves), RFC 7386 Merge Patch (lossy: cannot express array element edits or setting
`null`; a warning is emitted when the change set is not representable), jsondiffpatch delta format (for web
viewers), and `jd`-style lines. Import of RFC 6902 as a patch is Phase v2.

**Visual:** none (`render_visual` returns false).

**Cost:** parse O(n) with 2 to 3 words of overhead per node plus spans; diff O(n) on hashes plus seqdiff on
changed arrays. A 100 MB lockfile parses in about 1 s; arrays above 1e6 elements switch to histogram
without the similarity pairing step (quadratic) and report `pairing-skipped`.

## 6. Merge

Composition: objects use **KeyedMerge** (SPEC §5.1 table, key order rule included), arrays use
**SequenceMerge**, scalars are **OpaqueLeaf**.

- Independent member edits merge. Both sides adding the same key with different object values recurse
  (`add-add` only at differing leaves).
- Array with identity rule: element-level three-way by key, with moves. Both sides inserting different
  elements at the same anchor: arrays default to **order-significant**, so this is `add-add` unless the array
  matches a `--json-set='/deps'` pattern or a profile declares it, in which case both are kept (ours first,
  reported `auto: both-inserted`). Package `dependencies` objects need no rule (they are objects).
- Array without identity rule: diff3 over element hashes; conflicted chunks whose elements are objects
  recurse only when similarity pairing is unique on all three sides, otherwise the chunk conflicts as a whole.
- Type change on one side and edit on the other: `both-modified`.
- Duplicate key in any input: merge proceeds on the logical (last) value and reports a warning; output keeps
  ours' duplicate structure.

**`--conflict=mark`:** JSON has no comment syntax, so marking is limited:

- JSONC/JSON5 inputs: the conflicted member is written as ours, followed by a comment block
  `/* <<<<<<< splinter conflict x-…  theirs: <json> >>>>>>> */`. The file stays parseable.
- Strict JSON: falls back to `ours` with a warning, unless `--json-mark=text` is given, which writes Git-style
  text markers around the conflicting member lines (file no longer parses; useful only for manual editing
  in Git workflows, mirrors what users expect from `git merge`).

**Patch payload:** per touched unit, `before` and `after` as canonical JSON text in the hunk body
(`- ` / `+ ` lines). No binary encoding needed.

**Implemented (#44):** each side's payload is the unit's source text as written, its continuation lines
re-indented to column 0. `implant()` places the values through the span-splice writer, like changes from
theirs that keep their own layout, so a patch applied to its exact base reproduces the new file byte for byte.
An insert at an array index makes a new element at that index (the new side's); an inserted identified element
or member goes last. Element moves apply as value updates (the order is not patched), and a unit whose
container the target lacks is dropped.

## 7. Serialization and preservation

**Span splice.** `SpanJson` records for every node `[begin, end)` byte offsets in the source, and for objects the
span of each member including the leading whitespace, key, colon, and trailing comma. The writer walks the
merged tree:

- A node whose subtree hash and source document match an unchanged ours node is copied verbatim from ours'
  bytes (comments, odd spacing, `1.50`, `é` escapes all survive).
- A node taken from theirs is copied verbatim from theirs' bytes and re-indented: each line of the copied
  span gets its indentation adjusted by the difference between theirs' and ours' indentation at that depth.
- New or rebuilt containers are emitted with **detected style**: indent unit (spaces count or tab, from the most
  common first-level indent), newline (LF/CRLF by majority), space after colon, inline vs multi-line
  (a container that was single-line in its source stays single-line if under 120 columns), trailing comma
  (JSONC/JSON5 only, by majority), final newline presence.
- Commas are re-derived per container, never copied, so insertions and deletions cannot produce doubled or
  missing commas. Comments attached to a deleted member are deleted with it; comments attached to a moved
  element move with it.
- Numbers from either side keep their source text. New numbers produced by the tool (only in `--json-numeric`
  arithmetic, which v1 does not do) would use shortest round-trip `std::to_chars`.

Byte-exact: entire file when nothing changed; every unchanged subtree otherwise. Lost: nothing in strict
JSON. In JSON5, comments between a deleted and a kept member may attach to the wrong member (heuristic:
comment belongs to the following member unless on the same line as the previous one); reported as
`comment-reattached` info.

### 7.1 Envelope fields

**Layer stack:** `[gzip, for .json.gz via containers.md] → bytes → character encoding (UTF-8; UTF-8 BOM; UTF-16/32 with BOM, §10) → EOL (LF/CRLF) → JSON tokens (escapes, number spelling) → tree`. Inside values, strings may carry further layers: base64 (data URIs, embedded binary buffers) and base64url (JWS), which dialects decode for diff.

| Field | Class | Verify on load | On write |
|---|---|---|---|
| BOM, encoding | encoding | decodes | ours' BOM and encoding |
| EOL, indentation, escapes, number spelling | encoding | none | span splice keeps source text (section 7) |
| base64 string values | encoding | decodable (dialects) | copied verbatim, never re-encoded unless the decoded bytes changed |
| gzip mtime / CRC-32, ISIZE | volatile / integrity | per containers.md | ours' header; `crc32`, `size_of` |
| lockfile hashes: `package-lock.json` `integrity` (SRI, `sha512-<base64>`), NuGet `packages.lock.json` `contentHash`, `composer.lock` `dist.shasum` and `content-hash`, `Pipfile.lock` `hashes` and `_meta.hash`, `flake.lock` `narHash` | external integrity reference (SPEC §12.5) | not verifiable (the subject is another file or a registry tarball) | never recomputed; coupled to the entry's version and `resolved` (Q-JSON-5) |
| signed JSON: JWS JSON serialization (`protected`, `payload`, `signature`), TUF and in-toto metadata (`signed` + `signatures[].sig` over canonical JSON) | authenticity | `unverifiable` unless `--verify-key` (later phase) | `auto`: kept with `signature-invalidated` when the signed part changed, because consumers reject an unsigned file as firmly as a badly signed one, and keeping the structure lets `--sign-command` re-sign. The keyed boundary is the signature value; everything else is content |

No framing, no volatile fields in plain JSON. **Churn:** a value edit changes only its bytes (`content`). A
re-save that reindents or reorders keys is all `encoding` with zero content changes.

## 8. Validation

Re-parse output in the input dialect; check no duplicate keys were introduced; check array key uniqueness
for keyed arrays; optional `--json-schema=FILE` validation in v2 (via jsoncons, see §9) with failures reported as
warnings. Round-trip test: `save(load(x)) == x` byte-exact for every fixture.

## 9. Libraries

| Library | vcpkg port | License | Status (verified 2026-10) | Notes |
|---|---|---|---|---|
| nlohmann/json | `nlohmann-json` 3.12.0 | MIT | verified port, license, version | `ordered_json` already used by core reports. 3.12.0 adds `JSON_DIAGNOSTIC_POSITIONS` with `start_pos()`/`end_pos()` (verified in docs). |
| simdjson | `simdjson` 5.0.2 | (Apache-2.0 OR MIT) AND BSL-1.0 AND BSD-3 | verified port, license | On-demand read-only API; fast, has raw token access, no mutable tree. |
| jsoncons | `jsoncons` 1.9.0 | BSL-1.0 | verified port, license | JSON Pointer, JSON Patch, JSON Schema, JSONPath. |
| yyjson | `yyjson` 0.13.0 | MIT | verified port, license | Fast C, mutable docs, JSON5-ish non-standard options. |
| glaze | `glaze` 9.0.0 | MIT | verified port, license | Reflection-oriented, wrong fit. |
| RapidJSON | `rapidjson` (2025-02-26) | MIT | verified port, license | Unreleased for years, SAX gives offsets. |
| Boost.JSON | `boost-json` 1.92.0 | BSL-1.0 | verified port, license | Heavy dependency. |

**Recommendation: own `SpanJson` parser** in `src/formats/json/` (about 700 LOC parser plus 400 LOC writer). Why:

- We need, per value, byte spans, raw number text, raw string escapes, member-level comma/whitespace spans,
  and duplicate keys. No library gives all of these. nlohmann's `JSON_DIAGNOSTIC_POSITIONS` gives spans but must
  be defined before every include program-wide because it changes `basic_json`'s layout, which is an ODR
  hazard with core and any third-party code using nlohmann; it also still stores numbers as `double`/`int64`.
- JSONC and JSON5 read modes are about 150 extra LOC in our own tokenizer.
- Limits (depth 512 default, max string 256 MiB, max nodes derived from `--max-memory`) are checked before
  allocation, which is easier in our own code.

Model: arena-allocated `Node { Kind kind; uint32_t begin, end; uint32_t first_child, next_sibling; Hash128 hash; }`
plus a side table for member key spans and decoded keys. Strings stay as spans into the source and are decoded
lazily (with a decode cache) for comparison.

nlohmann `ordered_json` remains the report and `--export` encoder. jsoncons is the candidate for v2 JSON Schema
validation (optional feature, BSL is permissive). Assumed, not verified: none of the alternatives above preserve
comments in a mutable tree (based on documentation of their feature sets, not tested).

## 10. Hard cases and explicit non-goals

- Huge single-line minified files (bundled JSON, 50 MB on one line): text renderers would show one changed line;
  the dump renderer avoids that. Span splice keeps minified style (new containers inline when the source is inline).
- Arrays of scalars with heavy duplication (`[0,0,0,1,0,...]`): content-hash matching is ambiguous; histogram handles
  it, but merges may conflict more than a human expects. Large numeric arrays (embedded buffers, point clouds)
  could use GridMerge via a `--json-grid=/data` rule (v2).
- Keys that look like identity but are not stable (`name` renamed in one side and edited in the other): reported as
  erase plus insert with a `possible-rename` hint when similarity ≥ 0.8.
- Non-UTF-8 input (UTF-16 JSON from Windows tools): accepted with BOM, converted for parsing, written back in the original
  encoding; spans are in the original byte stream via an offset map.
- Non-goal: resolving lockfiles against manifests (regenerate instead; see Q-JSON-5 for the opt-in entry merge), JSON-LD graph semantics, schema-driven identity inference.

## 11. Test plan

- **Generated fixtures** in tests: random JSON trees (Catch2 generators) with seeded edits applied on two sides.
  Properties: merge of disjoint edits equals sequential application; `merge(b,o,o) == o`; `merge(b,b,t) == t`;
  `diff(a,a)` is empty; byte-exact round-trip for every parsed input; export to RFC 6902 then apply equals target.
- **Golden reports** for: key add-add recursion, keyed array move plus edit, unkeyed array insert conflict,
  type change, duplicate keys, big integers (`12345678901234567890123`), `1.50` preservation, JSONC comment
  preservation, CRLF file, tab-indented file, minified file.
- **Conformance**: the JSONTestSuite corpus (nst/JSONTestSuite; y_/n_/i_ cases) for strict mode, json5-tests for JSON5 mode.
- **Fuzz**: libFuzzer target `fuzz_json_parse` (round-trip must be identity), `fuzz_json_merge` (three mutated copies of one seed; no crash, output re-parses).

## 12. Phases

- **MVP (M):** SpanJson strict + JSONC, tree diff, KeyedMerge/SequenceMerge with auto key detection, span-splice writer, dump, unified-over-dump, JSON report.
- **v2 (M):** JSON5, `--json-key` rules file, RFC 6902 / 7386 / jsondiffpatch export, RFC 6902 import as patch, JSON Lines, conflict comments in JSONC.
- **Later (S each):** JSON Schema validation, `--json-grid`, rename detection for keyed elements, dialect profile API polish.

### 12.1 Implementation status (Phase 6)

Done (MVP): own `SpanJson` parser for strict JSON and JSONC (comments, trailing commas; BOM, CRLF), depth limit
512, duplicate names (`invalid-duplicate-key`, last wins); canonical number and string comparison; XXH3 node
hashes; tree diff with JSON Pointer addresses (`member`, `element`, `value`, `type` kinds, `move` for identified
elements, numeric `delta`/`rel`); automatic identity keys from the §3 candidate list (`stats.format.array_keys`);
KeyedMerge on objects and SequenceMerge on arrays through the core primitives, `union` offered on array
conflicts, `--conflict=mark` falling back to ours with `mark-unsupported`; span-splice writer (ours' bytes outside
changed units, commas re-derived, same-line comments kept with their member, other sides' values re-indented
or laid out to match the surrounding inline or multi-line style, EOL and indent unit from ours), reparse
validation; dump; unified-over-dump; JSON report; detection by the extensions and probe of the header (a
`.json` file that parses neither as JSON nor as JSONC falls back to `text`; `.babelrc` is not matched).
Shared helper `src/formats/common/splice.hpp` (SPEC §2.3).

Simplified: unidentified elements pair positionally inside a changed run (equal-length runs in diffs, 1:1:1
conflicting chunks in merges) instead of the §3 similarity scoring, so no `confidence`; comments in other
sides' leading text of an inserted member are dropped; an object that is rebuilt keeps shadowed duplicate
members just before the member that overrides them.

Not yet: JSON5 (`.json5` is not claimed), `--json-key`/`--json-set` rules, `--json-order`, `--json-numeric`,
`--ignore-order`, `--json-string-lines`, `--dump-full`, exports,
JSON Lines, conflict comments in JSONC, lockfile handling (Q-JSON-5), `possible-rename`, `pairing-skipped`,
UTF-16/32 input, and the JSONTestSuite corpus and fuzz targets (Phase 7+ tracks).

Measured (Release, a 10 MB file with a 100 000-record keyed array, on a loaded machine): parse about 0.5 s,
merge about 8 s, most of it in the core SequenceMerge over the keyed array (identity maps and move detection).

## 13. Open questions

- **Q-JSON-1:** Own parser or nlohmann with `JSON_DIAGNOSTIC_POSITIONS`? **Provisional:** own `SpanJson`. **Why:** spans plus raw number text plus duplicate keys, without a program-wide ODR-sensitive macro.
- **Q-JSON-2:** Are arrays order-significant by default when both sides insert at the same place? **Provisional:** yes, `add-add` unless a set rule or profile says otherwise. **Why:** silently ordering inserts changes meaning in ordered lists (steps, layers).
- **Q-JSON-3:** Auto-detect array identity keys or require configuration? **Provisional:** auto-detect from the fixed candidate list, require uniqueness on all three sides, report the choice. **Why:** most record arrays use `id`/`name`, and reporting makes the heuristic visible.
- **Q-JSON-4:** Treat `1e2` and `100` as equal? **Provisional:** no by default, `--json-numeric` opts in. **Why:** integer vs float distinctions matter to typed consumers.
- **Q-JSON-5:** Merge lockfiles? **Provisional:** not by default; detect known lockfile names, report `derived-stale`, take ours whole, exit with conflict only if both sides changed it. Taking one side whole satisfies the coupling rule (SPEC §12.5) trivially. Opt-in `--lockfile=merge` (v2) merges per package entry, where an entry's version, `resolved`, and hash fields are one coupled unit taken whole from one side; both sides changing one entry differently, or a merge that would pair one side's version with the other's hash, is a `dependency` conflict. Top-level manifest digests (`composer.lock` `content-hash`, `Pipfile.lock` `_meta.hash`) are coupled to the manifest and become `derived-stale` after any entry merge. **Why:** lockfiles are derived from manifests and package managers regenerate them, but disjoint dependency bumps are common enough to justify an opt-in that can never produce a version with a foreign hash.
- **Q-JSON-6:** How to mark conflicts in strict JSON? **Provisional:** fall back to ours plus sidecar; `--json-mark=text` writes Git markers on request. **Why:** strict JSON has no place for markers that keeps the file valid.
- **Q-JSON-7:** Is JSON5 written or only read? **Provisional:** read and write via span splice, new values emitted in JSON-compatible syntax. **Why:** span splice preserves JSON5 syntax for free; we never need to invent JSON5-only syntax.
