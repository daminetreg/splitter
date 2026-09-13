# 44 — the three OpenCV units the splitter declines

Design only. Filed from TODO/42's outcome: after it, 3 of OpenCV's 158 units are still
compiled whole, each declined by the splitter before any piece is written, with the reason.
Two shapes.

## Motivation

| unit | reason given | shape |
|---|---|---|
| `core/src/alloc.cpp` | `static AllocatorStatistics allocator_stats;` has a type no other translation unit can name | a static variable whose type is a class in an unnamed namespace (defined in `allocator_stats.impl.hpp`) |
| `core/src/array.cpp` | `static struct { ... } CvIPL;` has a type no other translation unit can name | a static variable of an unnamed struct type |
| `core/src/arithm.dispatch.cpp` | `arithm.simd.hpp` is included more than once with no include guard and defines a function with external linkage | a header included twice under two macro states, whose second expansion defines the `cv::hal` dispatchers |

Both shapes are declined because the alternatives the splitter had were wrong, not slow:
kept in the preamble, a static of such a type is one object per piece — `array.cpp` split
that way before TODO/42 and the program was wrong; moved out, its `extern` declaration names
a type the pieces cannot name. And a pair header left unsplit reaches every piece through the
preamble with its external definitions.

`*.dispatch.cpp` is a pattern across OpenCV, not one file: every module's SIMD-dispatched
kernels are written this way, and `arithm` is only the one whose second expansion defines
functions rather than declaring them. Corpora beyond OpenCV write `static struct {...} x;`
for file-local state as a matter of course.

## Implementation Proposal

### A static of a type no other translation unit can name — keep its users together

The variable cannot be shared across translation units, so everything that touches it has to
be in one. The splitter already has one translation unit per unit that may exist only once:
`<unit>_0_definitions.cpp`, which includes the definitions header. The rule:

1. In `prepare_variables()`, where the decline is decided today, do not decline. Mark the
   variable `move_out` into the definitions header, unrenamed, `static` kept.
2. In `prepare_functions()`, mark every function whose body references the variable — the
   emit graph already records `calls[definition] = {USRs referenced}`, and a variable is a
   USR — as `keep_in_header` with a new reason, *uses a static no other translation unit can
   name*, so that it goes to the definitions header with the variable rather than to a piece.
   Transitively: a function that references such a function need not follow, since it calls
   through a declaration.
3. Nothing else changes: the pieces never name the variable or its type, the definitions
   object holds the only instance, and `getAllocatorStatistics()` returns a reference to it
   from there.

Cost: those functions are not split; in `alloc.cpp` that is the five allocation functions,
in `array.cpp` the IPL allocator setters and the header/data creation functions that read
`CvIPL`. Everything else in the unit still splits.

### A pair header whose second expansion defines external functions — split each inclusion

The header is one file included twice, and one mirrored copy cannot serve both inclusions.
The preamble is generated text, so its two `#include` lines can name two different copies:

1. `header_split_candidates()` treats each inclusion of an unguarded header as its own
   candidate, keyed by the inclusion index from the preprocessing record — the record gives
   the directives in order, so the second `#include "arithm.simd.hpp"` is distinguishable
   from the first.
2. The harvest is by file; it has to become by file *and* inclusion. The functions of the
   second expansion are those whose extent lies in the header and whose USR was not present
   after the first inclusion — the visitor sees them in the order the TU declares them, and
   the preprocessing record says where the second inclusion begins in that order.
3. The second inclusion's copy is mirrored under a distinct path
   (`include/_inclusion/2/arithm.simd.hpp`), rewritten as any header copy is — its external
   definitions replaced by declarations, pieces emitted for them — and the preamble's second
   `#include` line rewritten to that path. The first inclusion's copy stays where it is.
4. The pieces of the second inclusion include the unit's preamble first, as header pieces do,
   which replays both inclusions in order before the piece's own definition.

Cost: the harvest keyed by inclusion touches the harvest map, the `.split` manifests and
`resolve_header_deps()`. It is the larger of the two changes.

### Order

A first, then B. A closes two of the three units with a change inside two existing
decisions; B is the design change.

## Acceptance Criteria

- `split.static_unnamed_type_declines` and a counterpart for the unnamed-namespace class
  become splitting fixtures: the unit splits, the program prints what the plain build prints
  — `42`, with the hook set in one function and read in another — and `.keeps` names the
  functions kept for the variable's sake. Both decline before the change.
- `split.pair_header_declines` becomes a splitting fixture: the unit splits, the second
  inclusion's external function has a piece, the program prints `31`. It declines before the
  change.
- `benchmark-opencv.sh`: 0 fallbacks, and the `one body` row within 10% of its value with 3
  declined units — 7.3s locally — since the three units add compiles, not parses.
- Boost.Filesystem and Boost.Spirit: 0 fallbacks, unchanged.

## Outcome

Implemented in two commits: A (`85c85a87`) and B. All three units split; OpenCV
`core`+`imgproc` builds with 0 fallbacks and 0 declined.

### A, as designed

`prepare_variables()` marks the variable `anchors_users` instead of declining; `split_unit()`
marks every function whose emit-graph references include the variable's USR
`kept_with_variable`, and `generate_preamble()` sends the variable's whole text and those
functions to the definitions header whatever their linkage, a `static` one renamed and
stripped of the keyword so the pieces call it through the declaration left behind. A
template, a member of a class template or a function defined in a header that references
the variable still declines the unit, with the reason. Fixtures `split.static_unnamed_type`
(prints `42 42`) and `split.static_unnamed_ns_type` (`42 2`); `.keeps` records *uses a
static no other translation unit can name; kept with it*.

### B, with three departures from the design

1. **Attribution by source location, not by visit order.** The design attributed a
   definition to inclusion 2 from the first repeated extent, which fails for a pair whose
   first inclusion holds only declarations. clang's source manager gives every inclusion of
   a file an entry of its own, whose base offset the raw `CXSourceLocation` encodes; the
   base is the raw location minus the offset within the file, and a macro-expansion
   location, which has an entry created while the inclusion was read, falls between the
   base of its inclusion and the next. Entries loaded from the prefix PCH sit at the top of
   the range, so they are ordered before the ones the parse creates. `g_file_bases` collects
   the bases per file in `visitor()`, and `attribute_pair_inclusions()` re-keys the harvest
   as `path` and `path#n`. A pair whose inclusions cannot be told apart, or one of whose
   `#include` lines is in another header rather than in the unit, is left as before: the unit
   declines if the header defines an external function, otherwise the header is not split.
2. **Each inclusion's pieces compile in that inclusion's macro state.** The unit defines and
   undefines macros around each `#include`; a piece that includes the whole preamble and then
   replays the definition's conditionals sees the state at the end of the preamble --
   `#undef PAIR_DEFINITIONS` had run, and the piece was empty. The preamble is therefore cut
   after each inclusion's `#include` line into `<unit>_preamble.<header>.<n>.h`, with a PCH
   of its own (`SplitResult::context_preambles`, persisted in `split.cache`), and a pair
   header's conditionals are replayed as `#if 1`, every one of them being true where the
   harvest found the definition.
3. **Macro-produced definitions are compiled in place, not in the definitions header.**
   `arithm.simd.hpp` redefines `DEFINE_SIMD_FUN` section by section; `DEFINE_SIMD_ALL(add,
   op_add)` moved to the definitions header expanded with the last section's macro and
   called `hal_ni_add8u` with the wrong arity. For a pair inclusion the definitions piece
   includes the preamble cut *before* the `#include` line and then a *definitions variant* of
   the copy -- `generate_preamble()` with no definitions sink, so the pieces' functions are
   declared and everything bound for the definitions header stays where it was written --
   and each definition is expanded once, in the macro state it had.

Two rules found on the way apply to every header, not only pairs: a macro invocation that
produces exactly one free function (`DEFINE_SIMD_U8(or, op_or)`) is moved to the definitions
header like a group of one, with a declaration spelled from what libclang resolved
(`prepare_functions()` no longer restricts the widening to members, and an extent that
already is the whole invocation is recognised as one); and the incremental re-slice refuses a
file with more than one harvest record, since a pair header has one per inclusion.

Fixture `split.pair_header`: the header's second inclusion defines `kernel()` and two
macro-produced functions whose helper macro is redefined between them; the unit splits,
`kernel()` has a piece under `include/_inclusion/2/`, the definitions piece includes the
`.2.before.h` context and the variant, the preamble's second `#include` names the copy, a
second run reuses the split, and the program prints `31 15 6`.

Not done, and the same as before: a pair header included from another header, and a pair
whose inclusions hold no declaration at all, are not split per inclusion.
