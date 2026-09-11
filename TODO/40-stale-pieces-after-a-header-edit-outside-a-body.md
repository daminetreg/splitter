# 40 — a header edit outside any function body leaves stale piece objects

A correctness defect, reproducible in three files, present before this entry's neighbours
(the binary built from `45b3033a` reproduces it). Not yet fixed.

## Motivation

```cpp
// lib.h
#pragma once
inline constexpr int OFFSET = 1;
inline int add(int a, int b) { return a + b + OFFSET; }
// m.cpp
#include "lib.h"
#include <cstdio>
int main() { std::printf("%d\n", add(3, 4)); return 0; }
```

Split, link, run: prints `8`. Change `OFFSET` to `100`, split again: the launcher re-parses,
reports every piece and the preamble PCH up to date, compiles nothing, exits 0. Link, run:
prints `8`. A plain compile prints `107`.

Removing a function the unit calls has the same shape with a different symptom: the piece
that carries the call is skipped, the launcher exits 0 although libclang reported `use of
undeclared identifier`, and the failure arrives at the final link as an undefined reference.

### Mechanism

The pieces' preamble PCH is `<unit>_preamble.h.gch/<hash>.gch` where `<hash>` is the content
hash of the preamble *text*. That text is a list of includes and declarations, and it does not
change when a header behind it does. Nothing records which headers the PCH compile read, so
`pch_is_current()` — which does that for the libclang prefix PCH, since `c620c206` — has no
`.deps` to check here, and the PCH is reported up to date whenever a file of that name exists.

`needs_recompile()` then decides a piece is current from three timestamps: the piece source,
the preamble, and the PCH. The rewritten header copies under `<split>/include/` are inputs of
every piece and appear in none of the three. So an edit that changes a copy without changing
any piece's text or the preamble's text recompiles nothing: a constant, a type, a default
argument, a macro, a template, a removed declaration.

### Why the benchmarks did not show it, and what they show because of it

No benchmark row edits a header outside a body: `one header` touches without changing
content, and `one body` is handled by the re-slice. Every CTest fixture starts from an empty
tree.

The same mechanism is what makes the `one body` rows read one compile. For the 193 Spirit
units that keep `toucs4()` in their header copy, TODO/36 patches the copy — the copy changes,
the PCH is not rebuilt, the pieces are skipped. The objects are correct because those units
never reference the function, but the decision was not made on that basis. A fix that
invalidates the PCH when a copy changes, and nothing else, makes those 193 units recompile
every piece on a body edit: about 1575 compiles, which on one machine is more than the
plain build's 194 (57.5s) and on the cluster is 1575 executions in place of cache hits. The
row's result depends on the defect and has to be re-earned with it fixed.

## Implementation Proposal

1. **Correctness.** Build the preamble PCH with `-MD -MF` and keep the prerequisite list as
   `<gch>.deps`, as the prefix PCH already does; extend `pch_is_current()` to it, or share it.
   A PCH whose prerequisites changed is rebuilt, its timestamp moves, and `needs_recompile()`
   already recompiles every piece behind a newer PCH. Separately: when the libclang parse
   reported errors, skip no piece, so the real compiler reports the error and the build fails
   where a plain one would.
2. **Cost.** Make a unit's rewritten copy of a header contain only what that unit uses: the
   header's non-definition text as now, declarations for the definitions the unit emits as
   now, and — the change — **nothing** for a definition the unit neither emits nor references,
   in place of the definition it keeps today. The copy is then a function of the source and of
   the unit's reference set, both known to a full split, so it is reproducible and stays
   byte-identical between a fast path and a full split. An edit to an unreferenced definition
   leaves the copy unchanged, the PCH current, and every piece skipped — on the basis that the
   unit does not use it, which is the right basis. This is the deterministic form of "leave
   the declaration in the header": never include what the unit does not use, rather than
   retain what it once had (see TODO/39).
   To settle before writing it: the reference set must count uses in unevaluated contexts —
   `decltype`, `sizeof`, `noexcept`, `requires` — which do not odr-use; whether
   `clang_getCursorReferenced()` over the whole AST covers them; overload sets where an
   unreferenced overload took part in resolution; ADL; friend definitions. One fixture each.
3. **Measure** `one body` after (1) alone and after (1)+(2), on one machine and on the cluster,
   and replace the rows in `benchmarks/boost-spirit-rbe-summary-9-Sep-2026.md` with the
   result — whichever way it goes.

## Acceptance Criteria

- A fixture, `launcher.header_edit_outside_a_body`: the three files above; after the constant
  change the program prints `107`; after removing `add()` the launcher exits non-zero with the
  compiler's `use of undeclared identifier`, not a link error. Both must fail today.
- `launcher.incremental_body_edit` keeps its byte-identity check, and gains a phase in which
  a body edit to a definition the unit does not reference changes no file under the split
  directory except the harvest.
- The `one body` rows are re-measured with the fix in and the numbers replaced, not kept.
- Boost.Filesystem and Boost.Spirit build with 0 fallbacks.
