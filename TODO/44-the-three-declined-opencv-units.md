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
