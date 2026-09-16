# 51 — one piece per header function, shared by every unit that includes the header

## Motivation

A piece taken out of a header belongs to the unit that includes it: it sits in the unit's
split directory, includes the unit's preamble first, and is compiled against the unit's
PCH. That was TODO/09's fix (`37d1879a`): a header is not necessarily self-contained, and a
piece that included only the header's copy failed on Boost.System's
`std_category_impl.hpp`. It made a header function's piece an artefact of each includer.

The cost after TODO/49 (`benchmarks/boost-spirit-summary-14-Sep-2026.md`,
`boost-spirit-rbe-summary-14-Sep-2026.md`): `standard_wide::toucs4()` is one function, and
an edit to its body compiles 267 pieces, one per unit that includes the header, each a
distinct action on the cluster. The suite carries 37538 pieces for a few thousand distinct
header functions; the full split build doubled (438s to 882s here, 2080s to 2352s on the
cluster, all of it transfer) and the no-op tripled. The goal of the tool is one compile
per function; for a header function it is doing one per includer.

## Implementation Proposal

A shared piece does not need the unit's copy of the header, nor its preamble. It includes
the *original* header and forces the function out of it:

```cpp
#include "/abs/path/to/standard_wide.hpp"
__attribute__((used)) static const auto cpp_splitter_anchor = &boost::spirit::char_encoding::standard_wide::toucs4;
```

Taking the address odr-uses the inline function, clang emits it as a weak (`linkonce_odr`)
definition, and `used` keeps the anchor and with it the reference. Every unit's copy still
declares the function (TODO/48, TODO/49: the copy, and so the PCH, do not change under a
body edit), and every unit links the same object. Callees the body needs are in the
original headers the piece reads, so nothing dangles.

- **Store**: `$CPP_SPLITTER_STORE`, default `<cwd>/.cpp-splitter-store` (the build
  directory: ninja runs the launcher there). Per function: `<key>.cpp`, the anchor piece,
  `key` = hash of the anchor text and the header path; per (function, flags):
  `<key>-<flags hash>.o`, `.d`, `.deps` (content hashes of every prerequisite, as
  `gch_is_current()` reads), `.fail` (the header's content hash when the standalone
  compile failed: not retried until the header changes). Per (header, flags): `<hkey>.h`
  holding `#include "<header>"` and its `.gch`, built by `build_pch()`, so the pieces of one
  header parse its include closure once.
- **Freshness** is content: an object is current when every prerequisite hashes as
  recorded. A body edit changes the header, the first launcher to look rebuilds the
  object, the others find it current. Hashes are memoised per launcher run; a unit's
  prerequisites are the same ~1500 files for all its pieces.
- **Concurrency**: a `mkdir` lock per object. A launcher that finds the lock taken skips
  the object, compiles its own batch, then waits for the other's result -- never waiting
  while holding a lock. A lock older than ten minutes is broken.
- **Fallback**: the per-unit piece is still written, as today, with a `// Store: <key>`
  line; it is compiled only when the shared compile failed (a header that needs its
  includer's context, an anchor an overload makes ambiguous). No fallback to a whole
  compile: TODO/09's case costs one failed compile per key, once.
- **Which functions**: the header pieces of TODO/49's shape -- inline, external linkage,
  not static, not in an unnamed namespace, not `extern "C"`, not from a macro invocation,
  not sharing an extent, not always-inline, not overloaded within its scope in the file,
  not a pair inclusion, not a module unit. A non-inline external definition in an
  implementation include stays per unit: it may exist once.
- **Dependencies**: the unit's depfile is unchanged (the copy maps back to the original
  header, which the shared piece reads); when no per-unit compile is left to carry `-MF`,
  the preamble is preprocessed for it.
- **Remote execution**: the shared compile goes through the driver like any piece; its
  inputs are the original headers and the anchor, so 268 includers are one action key.

## Acceptance Criteria

- `launcher.shared_header_piece`: two units include a header with an inline function one
  of them emits; after both compile, the store holds one object for it and the second
  unit's log says it reused it; after an edit to the body, one shared compile happens,
  the other unit reuses it, and the program prints the new result. A header that only
  compiles in its includer's context (`ctx_main`'s shape) still splits, through the
  per-unit piece.
- The existing suite, Boost.Filesystem and Boost.Spirit: 0 fallbacks, 0 declined, the
  programs pass.
- Boost.Spirit body row (`-j16`, here): one compile for `toucs4`, and the full and no-op
  rows back near their pre-TODO/49 figures (438s, 11.8s) or below.

## Outcome

Implemented. Three things the rungs and the benchmark decided on the way:

- Whether a header can be shared is not predictable from the AST -- `.ipp` members, a
  static data member of a class template, a namespace-scope variable under a macro -- so
  the compiler decides: `nm -g --defined-only` on the shared object, and a global that is
  not weak (`T D B R C`) fails the key. `macro_definition_main`'s `.ipp` found it.
- An overloaded name gets a typed anchor, `static_cast<R (*)(P)>(&f)` or
  `static_cast<R (C::*)(P) const>(&C::f)`, from the return type and the display name; 40
  of `qi/actions.cpp`'s 115 header pieces are overloads (`test_output_impl`, `isalnum`...).
- The object's key leaves the include directories out and the record beside it names
  the list the compile searched; a unit with another list checks that every prerequisite
  resolves to the same file through its own. Spirit's test directories differ only by
  their own `-I`, and the 8 groups share one object. The header PCH keeps the full list:
  clang does not check a PCH's include paths against the compile's.

Boost.Spirit, `benchmarks/boost-spirit-summary-14-Sep-2026.md`: the body row 28.7s (51
shared compiles, 0 per unit) against 34.1s after TODO/49 and 56.9s plain; the full split
build 460.7s against 882.4s; the no-op 22.2s against 29.2s, and 11.8s before TODO/49 --
the launcher checking 135 shared records per unit. Boost.Filesystem 0/0; Spirit 0/0, 268
programs pass; 33301 of 36943 header pieces shared from 413 objects.

Still to measure: the cluster rows, where a shared piece is one action key for all its
includers.
