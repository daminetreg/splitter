# 36 — a definition the unit keeps in its header copy cannot be re-sliced

## Motivation

TODO/28 makes a one-line body edit cost a re-slice instead of a parse. It only ever worked for
a definition this unit **emitted a piece for**. A definition the unit keeps — nothing here needs
an out-of-line copy of it — was left out of the harvest, so it sat inside a *gap*, so an edit to
its body read as a change outside every definition and the whole unit was re-split.

That is the common case on a real corpus, not a curiosity. Editing `standard_wide::toucs4()` in
Boost.Spirit's suite reaches 194 units and exactly one of them emits it; the other 193 are this.
Measured on Boost.Filesystem, editing `path::has_relative_path()`:

| | re-sliced | refused |
|---|---:|---|
| before | 1 | 4 × `the change is not confined to one definition`, 2 × `the changed file contributed no split-out definition` |
| after | **7** | none |

Both refusal messages are the ones Boost.Spirit's `one body` row gives — the second is what its
265 units reported. Nothing was ever *wrong*: refusing falls back to a full split, which is
correct and merely pays a parse. What made it expensive was doing that on a cluster, where a
full split is a round trip and every regenerated piece is a new action: 267 re-splits and 4835
remote compiles against 1.

This entry twice blamed something else — a missing prerequisite record, then an interaction
between TODO/28 and TODO/35. Both were wrong, and the second was reached by counting files in a
live build tree. The cause is one line of deliberate filtering in `write_harvest()`, and the
thing that found it was asking the splitter why it refused rather than guessing again.

## Implementation Proposal

1. Record kept definitions in the harvest, with no piece. `HarvestDef::piece` was already
   documented as "the piece emitted for it, **if any**", so the shape was there.
2. Move that recording ahead of TODO/27's short-circuit in `emit_split_files()`, which
   `continue`s for a kept definition before any harvest work is reached.
3. Stop `write_harvest()` dropping pieceless definitions, and serialise the absent piece as
   `-`. Bump the harvest magic to 5: a file written by an older binary records those
   definitions inside a gap, and reading it as though it did not would place an edit wrongly.
   The cost of the bump is one full split per unit on the first build after upgrading.
4. In `try_incremental_split()`, when the edited definition has no piece, patch the copy that
   carries its body — this unit's rewritten copy of the file, or the preamble for a definition
   kept out of the unit's own source. Both sit beside the harvest, so take the directory from
   the harvest that was read rather than from a piece's path.
   Locating the body needs no brace matching: the signature is unchanged (`prefix_hash` proves
   it), and the old body's length is recorded, so the candidate is `[end of prefix, +length)`
   and `body_hash` proves it is the text that was harvested. A wrong guess refuses.

## Acceptance Criteria

- `launcher.incremental_body_edit` grows a header function the unit never calls, edits its
  body, and requires a re-slice whose output is **byte-identical** to a forced full split of the
  same edit. It must fail without the change.
- Boost.Filesystem: an edit to a kept body in `path.hpp` is re-sliced by every affected unit,
  with no refusal and no fallback.
- `launcher.remote_split_on_opal` keeps passing: its body-edit phase now edits a function only
  one of its two units calls, so it exercises the same path through a cluster-produced split.
- No row where the split is produced locally gets slower.

## Outcome

Done, and all four criteria hold. 34/34 tests pass.

The fix is the four steps above. The repro is local and takes 0.6s — deliberately not Boost.
Spirit, which is where the symptom was found but far too slow and far too indirect to develop
against. Reproducing it small also showed it was never about remote splitting at all: the same
refusal appears in a two-unit project with no cluster involved, which is what finally moved the
diagnosis onto `write_harvest()`.

Not addressed: the `full` row's cost on the cluster, which is output transfer rather than
parsing (279 remote splits at ~28.9s of worker time and 589911 blob downloads), and the peak
memory of a distributed split build, which rises rather than falls. Both are in
`benchmarks/boost-spirit-rbe-summary-9-Sep-2026.md`; neither is this entry.
