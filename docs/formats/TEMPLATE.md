# <Format name> — Implementation Document

**Tier:** T? · **Module:** `semantic-splinter-<fmt>` · **vcpkg feature:** `<fmt>` · **Depends on modules:** …
**Extensions / magic:** …

## 1. Why it matters
Who keeps this format in version control, and what goes wrong today. What a user should be able to see and do.

## 2. Format anatomy
The structure relevant to diffing and merging. Spec references, versions, and variants.

## 3. Semantic units and identity
Table: unit → identity key → Address pattern → primitive (Keyed/Sequence/Grid/Opaque).
Dependencies between units, invariants that must hold after merge, and derived data.

## 4. Normalization
What is considered insignificant, what is never normalized, and what options control it.

## 5. Diff
Detectable changes, metrics reported, `dump()` (textconv) layout, visual output (`--visual`), and the cost on large files.

## 6. Merge
Auto-merge rules, conflict reasons, dependency handling, `--conflict=mark` behaviour, and patch payload encoding.

## 7. Serialization and preservation
The write path. What is byte-exact, what is re-encoded, what is lost (and is reported as a warning).

### 7.1 Envelope fields (required; see SPEC §12 and ENVELOPES.md)
- **Layer stack:** bytes → … → content.
- Table: field → class (framing / integrity / authenticity / encoding / derived / volatile) → how it is verified on load → how it is regenerated on write (Emitter expression, re-encode parameters, or writer delegation).
- Raw-copy opportunities, which untouched units keep their encoded bytes, and the layout-stability tricks (padding absorption and so on).
- Signature policy under `--signatures=auto`, and exactly where the keyed boundary lies.
- Expected byte churn for a typical small edit, and how it is attributed.

## 8. Validation
Checks after writing. Optional external validators for CI.

## 9. Libraries
Candidate libraries: vcpkg port name, license, maintenance status (as researched), and the recommendation.
Write-it-ourselves estimate where that is the better choice.

## 10. Hard cases and explicit non-goals

## 11. Test plan
Fixtures (generated vs. checked-in), golden reports, property tests, and fuzz targets.

## 12. Phases
MVP → v2 → later, each with a rough effort (S ≤ 1 wk, M ≤ 3 wk, L ≤ 6 wk, XL > 6 wk for one engineer).

## 13. Open questions
`Q-<FMT>-n`: question. **Provisional:** best-guess answer. **Why:** one line.
