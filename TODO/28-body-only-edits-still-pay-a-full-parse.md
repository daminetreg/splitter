# 28 — A body-only edit pays a full parse, and the parse is the whole cost

**Severity:** Medium. Nothing is wrong; the one row the design exists to win is lost by a
factor the parse alone accounts for.

## Motivation

`TODO/14` added `split_inputs_hash()`: the source, the flags and the contents of every
prerequisite are hashed, and on a match the parse is skipped and the previous pieces reused.
That is what makes a *timestamp* change free.

It is all-or-nothing. A one-line edit inside one function body changes content, the hash
misses, and the unit is re-parsed from scratch — even though the edit cannot have changed the
set of definitions, their order, their linkage, or where any of them belongs.

Measured on `boost_geometry_algorithms_area` alone, in the six-target tree:

| | wall |
|---|---:|
| split, `touch` only (cache hit, no parse) | **2.2s** |
| split, body edit (full re-split) | **12.2s** |
| plain, body edit (full compile) | 8.1s |

The cache-hit path already does everything except parse: CMake's re-check, the launcher,
hashing ~2200 prerequisites, and the `ld -r` of ~270 objects. So **the parse and the emission
it feeds are ~10 of the 12.2 seconds**, against 8.1 to just compile the unit.

That is why the six-target body row reads 1.14x slower
(`benchmarks/boost-geometry-bench-7-Sep-2026.md`) while the piece-level result underneath it
is perfect: the edit recompiles exactly five piece objects, all of them
`side_info.hpp_6_collinear.o`, out of 2338 pieces. The splitter finds the minimal work and
then spends more discovering it than doing it.

`TODO/14` predicted this wall in its own outcome:

> Making that row win needs the per-piece work to be skipped for pieces whose text cannot have
> changed, which is a finer-grained version of the caching added here rather than a new
> mechanism.

## Description

A change confined to the interior of one definition's body cannot change:

- which definitions exist, or their identity, signature, linkage or order;
- which of them are kept and why (`should_keep_in_header`, `keep_reason`);
- the preamble, for any definition that was moved out;
- the text of any other piece.

It changes exactly two things: the bytes of that one definition, and the byte offsets of
everything after it **in that one file**. Both are recoverable arithmetically if the harvest is
on disk, and neither needs libclang.

Nothing of the harvest is persisted today. `split.cache` stores only the resulting file lists;
`<tag>.keeps` (TODO/27) stores kept definitions but only line, reason and signature; and
`CPP_SPLITTER_DUMP_HARVEST` writes to stderr for a human. A harvest cache is new.

## Why not just reload a serialized AST

Because it does not save enough. `clang_createTranslationUnit2()` opens an `.ast` in 0.02s but
deserializes lazily, so the cost reappears in the first walk: 1.21s to load and walk twice
against 1.95s to parse and walk twice, plus 0.61s to write the `.ast` on every cold split. That
is 1.6x on one component of a ~10s re-split.

The spec below caches the *conclusions* instead of the AST — which definitions exist, their
extents, and where each belongs — so the fast path touches no libclang and walks no cursors.
That is the only version that can approach zero. See `TODO/29` for the measurements and for why
`clang_reparseTranslationUnit()` is not available here.

## Implementation spec

1. **Persist the harvest.** Write `<tag>.harvest` beside the pieces: one record per definition
   with file, byte extent, `start_line`, signature, mangled name, and every flag placement
   depends on — `keep_in_header`, `shares_extent`, `is_template`, `is_virtual`,
   `is_ctor_or_dtor`, `is_specialization`, `in_unnamed_ns`, `is_conversion`,
   `uses_undefined_macro`, the always-inline ranges — plus the variables, plus the byte ranges
   that make up the preamble. Version the format and refuse a record written by another build.

2. **Record what each prerequisite looked like.** `split_inputs_hash()` already hashes every
   prerequisite's content. Keep the per-file hashes rather than only the combined one, so a
   miss can say *which* files changed instead of only *that* something did.

3. **Classify the miss before falling back to a parse.** When exactly the files that changed
   can be diffed against their cached content — cache the content, or a per-definition hash
   plus the inter-definition gaps — and every difference falls strictly inside the body of one
   harvested definition, take the fast path. Anything else, parse.

4. **Reject anything that could move a decision.** The fast path is refused when the changed
   region contains a preprocessor directive, touches a signature or the text between
   definitions, is not brace-balanced, or lies in a file whose `undefined_macros()` result
   changed — that last one is a text scan and cheap, so re-run it rather than assuming.

5. **Re-slice instead of re-harvesting.** Shift every extent in the edited file that begins
   after the edit by the byte delta, rewrite only the affected piece, and leave every other
   piece untouched so the pruner and the build system both see them as unchanged.

6. **Leave a kill switch and prove it is not needed.** `CPP_SPLITTER_NO_INCREMENTAL_SPLIT`
   forces the full parse. The acceptance criterion below is what says the fast path agrees with
   it.

## The risk this has to answer

`collect_emitted()` is the one decision that a body edit genuinely can change: a body that
gains a call can cause a symbol to be emitted that was not, and one that loses a call can stop
emitting a symbol a piece was written for. The second direction is the dangerous one — a piece
that emits a definition the translation unit no longer emits.

Do not hand-wave it. Either re-derive the emitted set without a full parse, or restrict the
fast path to edits that only *add* text, or accept a parse whenever the edited body's
identifier set shrinks. Whichever is chosen, say so in the code, because it is the reason this
optimisation is not simply "hash less".

## Acceptance Criteria

- A body edit to `boost/geometry/strategies/side_info.hpp` rebuilds
  `boost_geometry_algorithms_area` in appreciably less than the 8.1s plain compile, where it
  now takes 12.2s.
- The pieces produced by the fast path are **byte-identical** to those produced with
  `CPP_SPLITTER_NO_INCREMENTAL_SPLIT` set, checked on Boost.Geometry's test suite and on the
  four-library harness.
- Every guard in step 4 has a fixture that proves it takes the slow path: an edit that adds a
  `#define`, one that changes a signature, one that adds a use of a macro the file undefines.
- The `one body` row in both geometry benchmarks becomes faster than plain, and the filesystem
  and Spirit body rows improve.
- No new fallbacks anywhere, and the filesystem example still scores 9/9.
