# 39 — removing a function from a header

Design only, revised after measuring. Depends on TODO/40.

## Motivation

The benchmarks cover one edit shape, a body changed in place. A definition **removed** from a
header is a second, and the first version of this entry reasoned about its cost instead of
measuring it. Measured on the fixtures, per unit that includes the header:

| the unit | what happens today |
|---|---|
| does not reference the function | one libclang parse (the re-slice refuses: no "one body changed" hypothesis matches a deleted extent); **no piece recompiled**; object unchanged and correct |
| references the function | one parse, which reports the error; the piece carrying the call is **skipped**; the launcher exits 0; the object has an undefined reference and the failure arrives at link |

Both rows are the mechanism of TODO/40: pieces are judged current from the piece text, the
preamble text and its PCH, none of which the rewritten header copy is part of. The first row
is the right outcome reached for the wrong reason; the second is a defect.

So on Boost.Spirit, removing a function from `standard_wide.hpp` today costs 194 parses and
no compiles for the 193 units that do not use it, and a build that should fail and does not
for the one that does. Once TODO/40 (1) is in, the 193 units recompile every piece — roughly
1575 compiles — unless TODO/40 (2) is in as well, after which they recompile nothing, on the
right basis.

## On leaving the declaration in the copy

Retaining a declaration for a function the source no longer has was the first idea. It is
rejected in that form: the copy would depend on history rather than on the source, so a
`--clean` build, another machine or the cluster would produce a different tree with different
action keys; and it defers the recompile to the next full split rather than avoiding it.
TODO/40 (2) is the same intuition in a reproducible form — the copy never contains what the
unit does not use, so a function the unit does not use can be edited or removed without the
copy changing — and it is where the saving belongs.

## Implementation Proposal

After TODO/40, what remains of the removal case is the parse per unit, and on the cluster the
round trip per unit. Add a second recognised edit shape to `try_incremental_split()`, alongside
"one body changed": **one recorded definition removed**.

1. When no one-body hypothesis matches, test for each recorded definition *k* that its extent
   plus the blank lines after it was deleted, requiring every gap and every other extent to
   hash as recorded, shifted after *k*. Refuse if none or more than one matches.
2. On a match, without parsing: delete *k*'s piece and object if it had one; if the unit
   references *k*, refuse — the compile has to fail with the compiler's diagnostic, which is
   TODO/40 (1)'s job; otherwise the copy does not mention *k* (TODO/40 (2)) and is unchanged;
   renumber `#line` in pieces after *k*; drop *k* from the harvest and `.keeps`; merge its two
   gaps; re-record the harvest.
3. Add a `one removal` scenario to both benchmark scripts: delete an inline function from a
   widely included header that no unit calls. Measure before implementing (1)–(2).

## Acceptance Criteria

- `launcher.incremental_body_edit` gains a removal phase for a definition the unit does not
  reference: the fast path handles it, and its output is byte-identical to a forced full split
  of the same removal. Must fail without the change.
- The `one removal` row: for every unit that does not reference the function, the splitter
  reports the removal handled without a parse and no piece recompiled; for a unit that does,
  the build fails with the compiler's diagnostic.
- No change to the `one body` rows as re-measured under TODO/40.
