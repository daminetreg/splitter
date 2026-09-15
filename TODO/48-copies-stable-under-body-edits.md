# 48 — copies of a header stable under an edit to a body the unit does not emit

## Motivation

`benchmarks/p4c-unity.md`: one line added to the body of `cstring::size()`, an inline
member every one of p4c's 218 units includes and 14 emit, costs the split build 683s
against 103s plain. The 14 units re-slice one piece each. The other 204 keep `size()`
defined in class in their copy of `cstring.h`; the copy changes, the preamble PCH built
from it is rebuilt, and every piece compiled against that PCH is recompiled -- 9689 of the
10381 pieces. That is correct: a piece that inlined the old body must be rebuilt. It is
also almost the full build.

The same edit on OpenCV (`opencv-local-split.md`, one body 4.6s) and Boost.Spirit
(`boost-spirit-rbe-summary-9-Sep-2026.md`) was measured before the pieces loaded the PCH at
all (TODO/47 12.), when the up-to-date check looked only at a piece's own text and the
preamble: the pieces that had inlined the old body were kept, stale. Those rows have to be
measured again, and will read like p4c's until this TODO is done.

## Implementation Proposal

A unit that does not emit an inline function has no use for its body. Its copy of the
header can declare the function instead of defining it:

- `prepare_functions()` already knows, per unit, which header definitions the unit emits
  (`collect_emitted()`); a kept definition that is inline, not a template, not `constexpr`,
  not defined in a class template, and not emitted by the unit, is replaced in the copy by
  its declaration (`generate_forward_decl_inplace()` for a free function; for a member the
  in-class declaration that `member_decl` already produces), and no piece is written for it.
- A body edit then changes the copy only in the units that emit the function, and their
  re-slice already handles it (TODO/28). The copies of the other units are unchanged, their
  PCHs stay valid, nothing recompiles.
- A unit that emits the function later (an edit adds a call) sees its copy change -- the
  function is now emitted, so the copy gets the piece's declaration instead of the
  definition, which is textually the same declaration -- and re-splits as it does today
  when the emitted set changes.
- Excluded, and left defined in the copy: `constexpr`, templates and members of class
  templates (instantiated on use), functions the unit's own preamble text uses in a
  constant expression, and anything `collect_emitted()` cannot decide.

Then measure again: p4c's body row, OpenCV's and Spirit's, all with the PCH in use.

## Acceptance Criteria

- A fixture with two units including one header of two inline functions, each unit
  calling one of them: an edit to the body of the function a unit does not call leaves that
  unit's copy byte-identical and recompiles none of its pieces; the launcher's verbose log
  shows the PCH kept and no piece compile for that unit.
- `benchmark-p4c.sh` one body: the 204 non-emitting units recompile nothing; the row is
  under plain's 103s.
- `benchmark-opencv.sh` and the Spirit benchmark re-run with the PCH in use; the documents
  gain a set measured after TODO/47 and TODO/48, with the earlier body rows marked as
  measured with stale objects.
- Boost.Filesystem and Boost.Spirit: 0 fallbacks, 0 declined; the split programs print
  what the plain ones print.
