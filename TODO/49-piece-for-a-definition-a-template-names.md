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
