# XML (generic) — Implementation Document

**Tier:** T0 · **Module:** `semantic-splinter-xml` · **vcpkg feature:** `xml` (dep: `expat`) · **Depends on modules:** none (uses the shared text encoding helper from text.md §2). Used by: svg; recursion from containers.md for XML members.
**Extensions / magic:** `.xml .xsd .xsl .xslt .plist .xliff .xlf .tmx .resx .csproj .vcxproj .props .targets .config .manifest .storyboard .xib`; probe: optional BOM, then `<?xml` or, after whitespace and comments, a `<` followed by a name start character, and a successful well-formedness scan of the first 64 KiB. Dialect modules (SVG, ...) probe at higher confidence by root element and namespace.
**Status (v1, #7):** MVP plus the v1 part of v2: comment-based `--conflict=mark`, the dialect hooks (`probe`, `id_rules`, `unordered`, `significant_whitespace`, `mark`; `make_format(info, dialect)` for dialect modules), and similarity pairing with confidence. Added for SVG (#9), all inert for generic XML: `accept_entity` (the bounded entity exception), `attr_kind` (`id`, `token_list`, `style_list`), `normalize` (comparison forms; nullopt marks volatile editor state), `merge_attr` (typed two-sided merge), `text_mode` (word-level text merge), `is_derived` (no merging below the element; conflict `dependency`), `rewrite_ids` (references, used for dependency checks and renumbering), `renumber` (SPEC §2.3 add-add ids, noted `renumbered`), a `MarkSite` for `mark`, `nodes()` for dialect modules, and the invariant warnings `id-duplicate` and `dangling-ref`; two insertions at one place that share an id conflict `add-add` even where the dialect says order is free. Patch payloads (§6) came with #44; `@@ ADDRESS` hunk headers work, `--annotate` metrics do not yet. Not yet: cross-parent moves, the TMX/XLIFF/MSBuild/plist dialects, a `number` attribute kind, the EnvelopeMap, XMLDSig `strip` and `--sign-command` (a changed signed document only warns `signature-invalidated`, #121), `--xml-internal-entities` (#119), `--dump=c14n`, and encodings other than UTF-8, UTF-16, ISO-8859-1, and US-ASCII. Deviations: a prefix bound differently in the output is redeclared on the fragment root instead of renamed (§7); addresses use `[@attr=value]` with the id attribute's name, a bare name for a name unique among its siblings, else `[N]` (no `[#hash]` steps); same-name siblings unique on both sides pair by name before similarity, and the text of a text-only element is its value, not a similarity feature (§3.1); namespace declaration changes are reported only under `--xml-strict-prefixes`; theirs' change to the XML declaration is dropped with warning `decl-kept-ours`; gaps over 256 siblings stay unpaired without `match-capped`.

## 1. Why it matters

XML files are everywhere in repositories: build files (`.csproj`, `.vcxproj`, Maven `pom.xml`), UI layouts (Android, XAML, `.storyboard`), localization (XLIFF, TMX, `.resx`), data exports, and XML members inside ZIP packages. Line-based merge on XML fails in predictable ways: attributes reflowed by an editor, elements reordered, two people adding siblings at the end of the same list, indentation changes. The generic module must:

- Diff by elements, attributes, and text, ignoring attribute order and formatting whitespace.
- Merge with element identity (ids where they exist, structure and content otherwise), so two people editing different attributes of the same element, or adding different children, merge cleanly.
- Write back **byte-exact** for everything untouched, including the original attribute quoting and indentation, so the Git diff of the merged file stays small.
- Be the base layer that dialects (SVG, TMX, XLIFF, MSBuild) configure instead of reimplement.
- Never be an XXE or entity-expansion vector.

## 2. Format anatomy

XML 1.0 (5th edition) plus Namespaces in XML 1.0. XML 1.1 is parsed as 1.0 with a warning (Q-XML-8).

Node kinds kept in the model: XML declaration (version, encoding, standalone), DOCTYPE (kept as an opaque raw span including any internal subset), element, attribute, namespace declaration (`xmlns`, `xmlns:p`), text (character data, possibly split by entity and character references), CDATA section, comment, processing instruction. Whitespace outside the root element is kept as raw spans.

```cpp
struct Span { uint32_t off, len; };            // into the side's original bytes
struct Attr  { QName name; std::string value; /* decoded */ Span raw; Span lead_ws; char quote; };
struct Node {
    enum Kind : uint8_t { element, text, cdata, comment, pi, doctype, decl } kind;
    QName name;                      // element / PI target; QName = {ns_uri, local, prefix}
    std::vector<Attr> attrs;         // in source order, xmlns declarations separate
    std::vector<NsDecl> ns_decls;
    std::vector<uint32_t> children;  // node indices
    std::string text;                // decoded text for text/cdata/comment/pi
    Span whole, start_tag, end_tag;  // end_tag.len == 0 for self-closing
    Hash128 subtree_hash;            // normalized (see §4), computed bottom-up
    uint32_t lead_ws = npos;         // formatting whitespace text node attached as "leading" (§7)
};
```

Spans are byte offsets in the **original input encoding**, which is what the splice writer needs. Expat reports byte index and byte count for each event (`XML_GetCurrentByteIndex`, `XML_GetCurrentByteCount`), so spans are exact even for UTF-16 input.

## 3. Semantic units and identity

| Unit | Identity key | Address pattern | Primitive |
|---|---|---|---|
| Element (child list) | dialect id rule, else content/structure match (§3.1) | `/<qname>[@id=VALUE]`, `/<qname>[#h1a2b3c]`, `/<qname>[N]` steps, e.g. `/svg/g[@id=layer1]/rect[@id=r7]` | SequenceMerge (ordered children), or KeyedMerge where the dialect says children are unordered |
| Attribute | (namespace URI, local name) | `.../rect[@id=r7]/@x`, `.../@{http://www.w3.org/1999/xlink}href` written as `@xlink:href` when the prefix is unambiguous | KeyedMerge, values OpaqueLeaf (or typed by dialect) |
| Namespace declaration | prefix | `.../@xmlns:inkscape` | KeyedMerge |
| Text (mixed content) | position among siblings | `.../p/text()[2]` | SequenceMerge element of the child list; content OpaqueLeaf or word-level SequenceMerge (§6) |
| Text-only element value | the element | `.../title/text()` | OpaqueLeaf or word-level SequenceMerge |
| Comment, PI | position, plus content hash | `.../comment()[1]`, `.../processing-instruction(target)[1]` | SequenceMerge element |
| DOCTYPE, XML declaration | singleton | `/#doctype`, `/#decl` | OpaqueLeaf |

Address steps use XPath-like predicates inside a JSON-Pointer-style path (SPEC §3.1); `~0`/`~1` escaping applies inside values. `[@id=v]` means identity from an id rule, `[#hex]` a content-derived identity (first 6 bytes of the subtree hash at matching time), `[N]` a 1-based position used only when nothing better exists. Q-XML-1.

### 3.1 Identity heuristics (matching children of a matched parent)

1. **Explicit ids.** The dialect's id rules, in priority order. Default generic rules: `xml:id`, then `id`, then `name`, `key`, `uid`, `guid`, `Include` (MSBuild items), but an attribute only counts as an id for a sibling group if its values are **unique within that sibling group in base, ours, and theirs**. Otherwise the rule is skipped for that group (recorded in the report as `id-rule-skipped`).
2. **Exact subtree match.** Remaining children are compared by `subtree_hash` (normalized, §4). This is the `seqdiff` histogram run over the child sequence where each child's token is `hash(qname, id)` for id'd children and `subtree_hash` otherwise.
3. **Similarity pairing** inside the gaps that the sequence diff leaves (delete/insert regions between anchors): children with the same qname are paired greedily in order when their similarity is at least 0.5. Similarity = Dice coefficient over the multiset of (attribute name, value) pairs plus the multiset of text tokens and child qnames. A paired element becomes an `update` (recursed) with `confidence` set to the score. Unpaired children are inserts and deletes.
4. **Moves.** Only id'd elements can move across parents (an id match anywhere in the document). Within one parent, an id'd or hash-identical element at a different position is a `move` by SequenceMerge rules.

Heuristic pairings below 0.8 confidence never auto-merge two-sided changes; the merge reports conflict `low-confidence-match` instead (SPEC: no silent best effort).

### 3.2 Dependencies and invariants

- Namespace scoping: every prefix used in the output must be bound in scope to the same URI it had in its source side.
- Id uniqueness: for attributes declared as ids by the dialect (always for `xml:id`), values must stay unique in the result.
- References (dialect-declared): `href="#x"`, `url(#x)`, `xlink:href`, IDREF attributes put the referenced id into `depends_on`. Deleting an id on one side that the other side newly references is conflict `dependency`.
- Derived data is a dialect concept (for example cached values or generated thumbnails), marked through the dialect hook `is_derived(element)`.

### 3.3 Dialect profiles

Dialects are compiled-in C++ objects that the dialect's module registers with the XML module:

```cpp
struct XmlDialect {
    virtual std::string_view name() const = 0;
    virtual int probe(const QName& root, std::span<const NsDecl>) const = 0;   // 0..100
    virtual std::span<const IdRule> id_rules(const Node& parent) const;      // per sibling group
    virtual ChildMode child_mode(const Node& el) const;                      // ordered | unordered | keyed_by(attr)
    virtual WsMode whitespace(const Node& el) const;                         // formatting | significant
    virtual AttrKind attr_kind(const Node& el, const QName& attr) const;     // opaque | token_list | number | reference(id_space) | style_list
    virtual TextMode text_mode(const Node& el) const;                        // opaque | words
    virtual void normalize(Node& el, NormalizeCtx&) const {}                 // e.g. drop editor-generated attributes
    virtual bool is_derived(const Node& el) const { return false; }
    virtual void rewrite_ids(Doc&, const IdRemap&) const {}                  // after id collisions
    virtual MarkResult mark_conflict(Doc&, const Conflict&) const;           // --conflict=mark
};
```

| Dialect | Where | Identity and rules (summary) |
|---|---|---|
| Generic | here | rules of §3.1, ordered children, comment-based marks |
| SVG | svg.md | `id`; `class` as token list; `d`, `points`, `transform` typed values; `style` as keyed `prop:value` list; `href`/`url(#)` references; marks as `<g id="splinter-conflict-...">` |
| TMX / XLIFF | in this module (small) | TMX `<tu tuid>`, else source segment text; `<tuv xml:lang>` keyed; XLIFF `<trans-unit id>` / `<unit id>`, `<target>` text in words mode |
| MSBuild | in this module (small) | items keyed by (`qname`, `Include`/`Update`/`Remove`); `PropertyGroup` children keyed by qname with `Condition` |
| plist | in this module (small) | `<dict>` as KeyedMerge over `<key>` + value pairs; `<array>` ordered |

## 4. Normalization

For comparison only (the `subtree_hash` and value equality). Output never changes because of normalization.

| Item | Default | Option |
|---|---|---|
| Attribute order | ignored | never significant (XML says so) |
| Attribute quote style, whitespace inside tags, `<a/>` vs `<a></a>` | ignored | none |
| Namespace prefixes | ignored (names compared by URI + local) | `--xml-strict-prefixes` |
| Redundant namespace declarations | ignored | `--xml-strict-prefixes` |
| Entity and character references | decoded before comparison (`&#xA9;` equals the literal character) | none |
| CDATA vs escaped text | equal if decoded text is equal | `--xml-strict-cdata` |
| Whitespace-only text between elements (formatting) | ignored when the element is not mixed content and `xml:space` is not `preserve` | `--xml-ws=significant` |
| Line endings in text | normalized to LF (XML spec end-of-line handling, done by the parser) | none |
| Comments | compared | `--xml-ignore-comments` |
| PIs | compared | `--xml-ignore-pis` |
| XML declaration | compared as a unit, encoding name case-insensitive | none |

Never normalized: element and attribute names, non-whitespace text, whitespace inside mixed content or under `xml:space="preserve"`, attribute values (except dialect-typed values such as number formatting or `class` token order, which the dialect declares).

## 5. Diff

Detectable changes: element insert/erase/move/update; attribute insert/erase/update; text update (with word-level detail when `text_mode = words`); namespace declaration changes; comment/PI changes; declaration and DOCTYPE changes.

Metrics: per change, `attrs_changed`, `text_chars_changed`, `subtree_nodes`; per document: elements matched by id / by hash / by similarity (with the minimum confidence), counts by op.

**`dump()` layout**: Splinter canonical pretty form, one node per line, deterministic:

- 2-space indentation per depth; elements as `<qname attr="v" ...>` with namespace declarations first (sorted by prefix), then attributes sorted by (namespace URI, local name); prefixes as in the source (or `ns1`, `ns2` if unbound).
- Self-closing for empty elements; text content on its own line, escaped with XML escaping, internal newlines kept so that multi-line text diffs line by line; formatting whitespace dropped (per §4).
- Comments and PIs as single lines (newlines escaped as `&#10;`) unless `--xml-ignore-comments`.
- Long attribute lists stay on one line by default; `--dump-attr-lines` puts each attribute on its own line, which is better for textconv of SVG and MSBuild files.
- `--dump=c14n` emits W3C Canonical XML 1.1 instead (later phase; not needed for diff quality).

Every dump line maps to the Address of its node, so `-u` hunk headers show element paths (text.md §5.3).

**Visual output:** none for generic XML (dialects like SVG render).

**Cost:** parsing with expat runs at hundreds of MB/s; the model uses about 60 to 100 bytes per node plus decoded text. Subtree hashing is O(n). Matching is histogram per sibling list plus similarity pairing that is O(k²) per gap of k unmatched same-qname siblings, capped at k = 256 per gap (larger gaps fall back to position order, reported `match-capped`). A 50 MB XML file with 1 M nodes diffs in about a second.

## 6. Merge

Recursive, top-down from matched roots (roots must have the same qname, else whole-document conflict `both-modified` at `/`):

1. **Attributes**: KeyedMerge by (URI, local). Typed values merge by type: `token_list` (e.g. `class`) as a set merge (add/remove tokens independently), `style_list` as KeyedMerge over properties, `number` with optional tolerance, others OpaqueLeaf.
2. **Namespace declarations**: KeyedMerge by prefix; then the writer ensures scoping (§7).
3. **Children**: SequenceMerge with the identity of §3.1 (or KeyedMerge for unordered/keyed dialect modes). Insertions by both sides at the same anchor: conflict `add-add` unless identical; dialects can declare order insignificant for an element (MSBuild item groups, plist dicts), then both are kept, ours first, reported `auto: both-inserted`.
4. **Matched child elements** changed on both sides recurse. Element qname changes are erase + insert.
5. **Text**: one-sided change wins. Two-sided: if `text_mode = words`, three-way merge on word tokens (whitespace-preserving tokenization, `seqdiff` on tokens), conflict `both-modified` on overlap; if `opaque`, conflict.
6. **Id collisions**: both sides add different elements with the same id: the dialect's `rewrite_ids` may rename theirs (`id` → `id-2`, references inside theirs' added content rewritten), reported `auto: id-renamed`; generic default is conflict `add-add` on the id.
7. **Dependencies** (§3.2) checked after merge.

**`--conflict=mark`** (generic): ours' version is kept in place, and theirs' alternative (serialized from theirs' raw span) is inserted immediately before it inside an XML comment: `<!-- splinter-conflict x-1a2b3c theirs: ... -->`, with `--` sequences in the payload escaped as `-\-`. Attribute and text conflicts get one comment before the owning element. This keeps the file well-formed and visible in any editor. Dialects override (SVG wraps both in a group).

**Patch payload**: per change, the Address plus the raw XML fragment before and after (UTF-8), so patches are human-readable. Fragments of more than 64 KiB use the default zstd+base85 encoding.

**Implemented (#44):** a node's payload is its source as UTF-8, with declarations for the prefixes it uses but does not declare; an attribute's is its value; `/#decl`'s is the declaration's pseudo-attributes. Every payload is zstd+base85 encoded (the envelope's one codec). `implant()` splices over the target's bytes: replaced nodes and attributes in place, removed ones with their leading whitespace, new nodes in the target's encoding without the declarations its scope already makes. A positional insert (`item[3]`) goes at that position among the surviving same-label siblings (the new side's position), any other insert after the last same-label sibling, else last. Keyed moves apply as content changes (the order is not patched), and a unit whose parent the target lacks is dropped. SVG wraps the XML format and does not forward `payload()`/`implant()`, so `svg` keeps `caps.patch` off.

## 7. Serialization and preservation

**Splice writer.** The merged tree's nodes each remember which side and which source node they came from, and whether they are *pristine* (identical to that source node including the whole subtree). The writer walks the tree:

- Pristine subtree: copy `whole` span bytes from its source side's buffer. Byte-exact, including quoting, attribute order, entity references, and inner formatting.
- Element with changed attributes but pristine children: rebuild only the start tag. Unchanged attributes are emitted from their raw spans including their leading whitespace (`lead_ws`), so `x="1"   y='2'` stays exactly so; new attributes are appended after the last surviving one with the leading whitespace of the previous attribute (or a single space) and the quote character most used in that tag.
- Element with changed children: start tag as above, children recursively, end tag from the raw span.
- **Formatting whitespace** is attached to the following sibling as its leading whitespace (`lead_ws`) when parsing, so an inserted element brings its own indentation from its source side, and a deleted element takes its indentation with it. The whitespace before the parent's end tag stays with the end tag.
- Nodes contributed by theirs are spliced from theirs' buffer. If theirs' encoding differs from the output encoding, the fragment is transcoded and characters not representable in the output encoding become character references.
- **Namespaces**: before emitting a node from another side, the writer checks that every prefix the fragment uses is bound to the same URI in the output scope. If not bound, the declaration is added on the fragment's root element; if bound to a different URI, the fragment's prefix is renamed (`ns1`, `ns2`, ... deterministic) inside the fragment and reported `ns-prefix-renamed`. This is the only case where pristine-from-theirs bytes are rewritten.
- XML declaration, DOCTYPE, and whitespace outside the root come from ours.

Lost: nothing in pristine regions. In rebuilt start tags, the exact whitespace between attributes that were deleted is lost (harmless). If the encoding is not UTF-8/UTF-16/ISO-8859-1/US-ASCII and not covered by our single-byte tables, the document is OpaqueLeaf for merging (diff still works on decoded text).

### 7.1 Envelope fields

**Layer stack:** `[gzip for .svgz via containers.md, or a ZIP member] → bytes → BOM and declared encoding (UTF-8, UTF-16, ISO-8859-1, US-ASCII, single-byte tables) → EOL → markup (entity and character references, CDATA, attribute quoting) → tree`. Text and attribute values may carry base64 (`xs:base64Binary`, plist `<data>`, `.resx` `<data mimetype="...base64...">`, SVG `data:` URIs), decoded only by dialects.

| Field | Class | Verify on load | On write |
|---|---|---|---|
| BOM, XML declaration `encoding` | encoding | BOM and declaration agree | ours' (section 7) |
| EOL, references, CDATA vs escaped, quoting, empty-element form | encoding | none | splice keeps source bytes |
| base64 values | encoding | decodable (dialects) | copied verbatim unless the decoded bytes changed |
| XMLDSig `Reference` `DigestValue` (digest of the referenced data after its `Transforms`, usually enveloped-signature plus C14N or exclusive C14N) | integrity (unkeyed) | recomputed (needs C14N, phase "later"; until then `unverifiable`) | recomputed only under `resign`; otherwise left as is with the signature |
| XMLDSig `SignatureValue` over canonicalized `SignedInfo` | authenticity | `valid` / `invalid` with an embedded `KeyInfo` certificate or `--verify-key`; else `unverifiable` | `auto`: kept, see below |
| `Reference` to another file (`URI="file.bin"`) | external integrity reference | not verifiable from one file | never recomputed; coupled to the URI |

**Signatures (generic XML).** `ds:Signature` elements (namespace `http://www.w3.org/2000/09/xmldsig#`) in SAML
assertions, ClickOnce `.manifest`/`.application` files, and signed configs are authenticity, never diffed as content.
A signature whose every `Reference` covers only unchanged nodes stays valid and is reported `valid`. When a
covered node changed, `--signatures=auto` **keeps** the signature and warns `signature-invalidated`: consumers of
these files reject an unsigned document as well as a badly signed one, and an intact `Signature` template is what
external re-signers (`--sign-command`, for example `xmlsec1 --sign` or `mage -sign`) expect (Q-XML-10). `strip`
removes the `ds:Signature` element and its leading whitespace. `resign` with a user key needs an XMLDSig signer and is later (Q-XML-10); until then `resign` on generic XML fails with trouble unless
`--sign-command` is given. The keyed boundary is `SignatureValue`; `DigestValue`s are recomputable.

*Implemented today:* only the warning. When the output differs from ours and the document holds a `ds:Signature`,
the merge warns `signature-invalidated`; references are not checked (no C14N), so a signature covering only
unchanged nodes is warned about too, and none is reported `valid`. `strip` and `--sign-command` are not applied to
XML yet (#121).

**Churn:** an attribute edit changes only that attribute (`content`). An editor re-save that reflows attributes or
changes quoting is all `encoding`. In a signed document, the invalidated `SignatureValue` is reported, not counted as content.

## 8. Validation

- Re-parse the output with expat (well-formedness, namespace well-formedness).
- Check id uniqueness for dialect id attributes and dangling references (warning `dangling-ref`, error only if the dialect says so).
- Check that pristine spans round-trip: in debug builds, assert that merge(x, x, x) output equals x byte for byte.
- Optional CI validators: `xmllint --noout --schema` for XSD-backed dialects, dialect-specific validators (SVG via resvg rendering).

**Security (SPEC §9)**: expat is configured with `XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER)`, no external entity reference handler (external entities are never loaded), and an `EntityDeclHandler` that stops the parser with `xml-entity-declared` whenever the DOCTYPE declares an entity, unless the active dialect's entity hook accepts it (used by SVG for the bounded Illustrator exception, Q-SVG-2). Billion-laughs protection (expat 2.4+) stays at its defaults as a second line of defence. Limits: nesting depth 512, attributes per element 4096, name length 64 KiB, total nodes bounded by `--max-memory`.

## 9. Libraries

| Candidate | vcpkg (verified) | License (verified) | Fit |
|---|---|---|---|
| **expat** | `expat` 2.8.5 | MIT AND CC0-1.0 | Streaming parser, heavily fuzzed (OSS-Fuzz), exact byte index and count per event, namespace processing, UTF-8/UTF-16/ISO-8859-1/US-ASCII built in plus an unknown-encoding hook for single-byte encodings, billion-laughs protection. **Recommended.** |
| pugixml | `pugixml` 1.16 | MIT | Fast DOM with good API. Does not expand DTD entities ("the only allowed entities are predefined ones", verified in manual), so XXE-safe. But it gives at most a start offset per node (`offset_debug`, assumed; not in the manual excerpt read), decodes references and rewrites quoting and empty-element spelling on save (assumed), so byte-exact splicing would need end offsets we do not have. Rejected for the core; acceptable for dialect-internal scratch parsing if ever needed. |
| libxml2 | `libxml2` 2.15.4 | MIT | Complete (XSD, C14N, XPath), but large, with a long CVE history and a history of maintainer turnover (assumed). Rejected for runtime; `xmllint` is fine as a CI tool. |
| tinyxml2 | `tinyxml2` 11.0.0 | Zlib | Small, but no namespace support and whitespace handling is lossy. Rejected. |

**Recommendation:** expat for tokenization and well-formedness, plus our own tree with spans, matcher, merge, splice writer, and dump. Estimate: tree builder over expat 600 LOC, normalization and hashing 300, matcher 700, merge 700, splice writer with namespace fix-up 700, dump 300, dialect API and generic/TMX/XLIFF/MSBuild/plist dialects 800. About 4k LOC.

## 10. Hard cases and explicit non-goals

- **Illustrator-style internal entities** (`<!ENTITY ns_svg "http://www.w3.org/2000/svg">` used in `xmlns`). Rejected by default per SPEC §9. `--xml-internal-entities` (Q-XML-3, not implemented yet, #119) would allow internal general entities with expat's amplification limits and keep the raw references in output.
- **Mixed content with inline markup** (XHTML-like `<p>text <b>bold</b> more</p>`): sequence of text and element children; word-level merge applies per text node, so edits that cross inline element boundaries conflict. Better handling is dialect work.
- **Elements without ids in long homogeneous lists** (e.g. 10 000 `<row>` children) whose content changes on both sides: similarity pairing is capped; results may report many low-confidence pairs. Dialects should provide keys.
- **Reordered lists with no ids**: reported as moves only when subtrees are identical; otherwise erase plus insert.
- **Schema-dependent semantics** (default attribute values, ID types from a DTD, whitespace facets): ignored. Non-goal: schema-aware merging.
- **XInclude, external entities, XSLT**: never processed.
- Non-goals: validating against arbitrary schemas at runtime, XML 1.1 specific character rules, preserving non-standard encodings in merges.

## 11. Test plan

- Generated fixtures: random trees (seeded) with random edit scripts (attribute edits, insertions, deletions, moves, text edits, reformatting), checked by properties: merge(b, o, b) == o bytes; merge(x, x, x) == x bytes; merge(b, o, t) contains every one-sided change; reformat-only changes yield no diff.
- Checked-in fixtures: real `.csproj`, `pom.xml`, Android layout, `.resx`, XLIFF 1.2 and 2.0, TMX, plist, an Inkscape SVG, UTF-16 `.resx`/`.vcxproj.filters` style files, files with CRLF, CDATA, comments, DOCTYPE.
- Golden reports and dumps for each fixture pair.
- Security corpus: billion laughs, quadratic blowup, external entity (file and URL), parameter entities, deeply nested elements, huge attribute counts. Each must fail fast with the right error and bounded memory.
- Interop: `xmllint --noout` on every merge output in CI.
- Fuzz: `fuzz_xml_load`, `fuzz_xml_roundtrip` (load, write unchanged, compare bytes), `fuzz_xml_merge` (three inputs from one fuzz buffer).

## 12. Phases

| Phase | Content | Effort |
|---|---|---|
| MVP (done) | expat tree with spans, normalization and hashing, id rules + hash matching, KeyedMerge attributes, SequenceMerge children, opaque text, splice writer with lead_ws and namespace fix-up, dump, security config, generic dialect | L |
| v2 (partly done) | Similarity pairing with confidence, cross-parent moves, word-level text merge, typed attributes, comment-based `--conflict=mark`, TMX/XLIFF/MSBuild/plist dialects, dialect API frozen for SVG. Done: similarity pairing, typed attributes, `mark`, the dialect API (used by SVG). Open: cross-parent moves and word-level text merge for generic XML (#43), the dialects (#42) | M |
| later (not started) | `--dump=c14n` and XMLDSig verification on top of it, internal entities option, single-byte encoding tables beyond ISO-8859-1, schema-informed id detection from DTD ATTLIST | S |

## 13. Open questions

- **Q-XML-1**: Address step syntax for XML (`rect[@id=r7]`, `[#hash]`, `[N]`) versus a compact `qname:key` form? **Decided:** XPath-like predicates for the generic module; dialects may use compact `p:3f9a1c` steps where their identity is synthetic. **Why:** readable for users who know XPath, and unambiguous about where the identity came from. *Implemented without `[#hash]` steps: an element without an id or a unique name is addressed by position `[N]` (see Status).*
- **Q-XML-2**: expat plus own tree, or pugixml DOM? **Decided:** expat plus own tree. **Why:** byte-exact splicing needs start and end byte spans for every node, which expat provides and pugixml does not.
- **Q-XML-3**: Allow internal DTD entities (Illustrator SVG)? **Decided:** off by default per SPEC §9; `--xml-internal-entities` (not implemented yet, #119) enables internal general entities only, under expat's amplification limits, keeping references raw in output. **Why:** real files need it, but the default must be safe.
- **Q-XML-4**: Default id attribute list for generic XML? **Decided:** `xml:id`, `id`, `name`, `key`, `uid`, `guid`, `Include`, each used only when unique within the sibling group on all three sides. **Why:** covers common dialects without a profile, and the uniqueness check prevents false identity.
- **Q-XML-5**: Similarity threshold for pairing unmatched siblings, and the auto-merge cut-off? **Decided:** pair at 0.5 or more; auto-merge two-sided edits only at 0.8 or more, else `low-confidence-match` conflict. **Why:** follows the SPEC rule of no silent best effort while still giving useful diffs.
- **Q-XML-6**: Generic `--conflict=mark` via XML comments? **Decided:** yes, theirs' alternative in a comment before ours' element. **Why:** stays well-formed and visible; consumers ignore comments.
- **Q-XML-7**: Treat whitespace-only text as formatting by default? **Decided:** yes, unless mixed content or `xml:space="preserve"`; dialects can override per element. **Why:** reindentation is the most common XML noise; mixed content is where whitespace matters.
- **Q-XML-8**: Support XML 1.1? **Decided:** parse as 1.0 with a warning; reject if 1.1-only characters cause expat errors. **Why:** XML 1.1 is almost unused and expat does not implement it.
- **Q-XML-9**: Should TMX/XLIFF/MSBuild/plist dialects live in the XML module or in their own modules? **Decided:** in the XML module as small built-in dialects. **Why:** each is under 200 LOC and needs no extra dependency; separate modules would be scaffolding.
- **Q-XML-10**: What does `--signatures=auto` do with an invalidated XMLDSig signature in generic XML, and do we re-sign it ourselves? **Decided:** keep it with `signature-invalidated` (signatures whose references cover only unchanged nodes stay valid); `strip` on request; `resign` uses an XMLDSig signer once C14N exists, and before that only `--sign-command` re-signs. **Why:** SAML and ClickOnce consumers reject unsigned and badly signed files alike, so stripping gains nothing and loses the template external signers fill in. *Only the warning is implemented (#121).*
