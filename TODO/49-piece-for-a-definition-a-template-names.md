# 49 — a piece for a header definition the unit does not emit but a template names

## Motivation

`benchmarks/boost-spirit-summary-14-Sep-2026.md`: an edit to the body of
`standard_wide::toucs4()` costs the split build 128.0s against 56.8s plain. 268 units include
the header; one declares `toucs4` only (TODO/48) and compiles nothing; 265 keep its body in
their copy because `support/char_class.hpp` and `qi/char/char.hpp` name it inside templates,
and TODO/48 declares a definition only when nothing in the unit's sources names it. A body
kept in the copy is in the preamble, the preamble is the PCH, and every piece is compiled
against the PCH: those 265 units each rebuilt their PCH and recompiled every piece, 4228 of
the full build's 4408 piece compiles.

On `qi/actions.cpp`, 118 header definitions pass TODO/48's shape rules (inline, external
linkage, not a template, not a member of a class template, not `constexpr`, not virtual, no
directive in the body, ...); 116 are kept only because a template names them. The unit has
15 pieces.

That a template names a function is no reason to keep its body in the copy. The unit may
not emit it at all -- the emit graph does not resolve dependent calls, so the name is the
only evidence -- and if it does, a definition in a piece of its own serves it exactly as a
piece serves an emitted function today: the copy declares, the piece defines, `inline` with
`__attribute__((used))`, and the linker merges the copies across units.

## Implementation Proposal

- `declare_only_candidates()` keeps a second set beside `g_declare_only`: the candidates
  that passed the shape rules and were rejected by the mention fixpoint, `g_split_unemitted`.
- The final pass of `prepare_functions()` for a header leaves such a function split -- no
  `keep_in_header`, no `declare_only` -- so it takes the path of an emitted definition:
  `declare_in_place()` in the copy, a piece written `inline` with `__attribute__((used))`,
  a `.harvest` entry with the piece path, the piece compiled and linked as a header dep.
- A body edit then re-slices that one piece per unit and recompiles it against the
  unchanged PCH (TODO/28's re-slice, unchanged).
- Everything else the shape rules exclude stays as it is: `constexpr` (needed at compile
  time), templates and members of class templates, virtuals, constructors and destructors,
  conversions, specialisations, defaulted and deleted, definitions sharing an extent,
  bodies holding a directive or using a macro the file undefines, `auto` returns,
  operators and the implicitly named (`begin`/`end`/`get`, coroutine hooks) -- the last
  two only because TODO/48 could not declare them; with a piece they need no exclusion,
  but that is a later step.
- Cost, stated up front: on Boost.Spirit about 116 more pieces per unit, some 32000 over
  the suite against 4408 now. The full split build and the no-op validation grow with it.
  Measured, not estimated, before this is accepted.

## Acceptance Criteria

- `launcher.stable_copy` gains a third function `gamma()` in `ops.h`, named by a template
  in the header that no unit instantiates through the emit graph, and called through that
  template by `main.cpp`: every unit's copy declares `gamma()`; a piece
  `ops.h_<n>_gamma.o` exists under each unit's split include directory; after an edit to
  `gamma()`'s body, `b.cpp` rebuilds no PCH and recompiles exactly one header piece, its
  other pieces' objects are untouched, and the program prints the new result. The existing
  `alpha()` case is unchanged.
- Boost.Filesystem: 0 fallbacks, 0 declined; the split programs print what the plain ones
  print.
- Boost.Spirit: 0 fallbacks, 0 declined, 268 programs link and pass; the body row
  measured again with `MODES="plain split" ./benchmark-spirit-cmake-re.sh --host`, with
  the full row's new cost beside it.

## Outcome

Implemented at `040b7ab8`. Two things the rungs found on the way:

- A body that refers to a function the unit declares and does not define cannot have a
  piece: `boost::math::concepts::acosh()` calls the template `boost::math::acosh` with only
  `math_fwd.hpp` read, the plain build never emits it, and a piece forced into existence
  referenced an instantiation nothing defines -- nine Spirit programs failed to link. Such
  definitions keep their body, and so does every vague-linkage caller that would emit them
  (`record_reference()`, `g_lacks_definition`; 1504 across the suite). Callees declared in
  system headers are exempt: libc and libstdc++ define them.
- The include prefix precompiled for the parse was cut inside a `#define` continued with
  backslashes, so Boost.Filesystem's `utf8_codecvt_facet.cpp` parsed with 20 errors that
  a passthrough had hidden; the pieces TODO/49 added exposed it as a fallback.
  `include_prefix_of()` cuts on logical lines and `RunSplitTest` fails on any parse error.

Boost.Spirit, `benchmarks/boost-spirit-summary-14-Sep-2026.md`: the body row goes from
128.0s to 34.1s, 267 piece compiles and no PCH, 1.66x faster than plain; the full split
build from 438.1s to 882.4s and the no-op from 11.8s to 29.2s, on 37538 pieces against
4408. The acceptance criteria hold; the cost stated in the proposal is measured.

On the cluster (`benchmarks/boost-spirit-rbe-summary-14-Sep-2026.md`): the body row is 267
actions of 2.1s each, 83.0s at `-j500` against the plain build's 136.2s, and the full
rows cost 2080s to 2352s on 114249 cache records -- the same 8.5x, paid in transfer.
