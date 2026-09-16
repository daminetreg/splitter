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

On the cluster, `benchmarks/boost-spirit-rbe-summary-16-Sep-2026.md`, remote split at
`-j500`: the body row 38.8s with 51 actions against 129.8s with 267 the day before and
136.2s plain; the full row 1096.6s (4878 actions, every piece compiled without a PCH on
a worker) against 2080.2s; the no-op 24.3s against 31.7s.

## Reconsidered (16 September 2026): the anchor piece gives up what a header piece is for

The store's piece includes the **original header, whole**, and anchors one function:

```cpp
#include "/abs/path/to/mylib.h"
__attribute__((used)) static const auto cpp_splitter_anchor = &multiply;
```

That is not the header piece TODO/09 and TODO/48 built. A per-unit piece includes the
unit's preamble and the *rewritten* header -- declarations only -- and then one body;
every other body of the header is a declaration to it. The anchor piece parses every
body of the header to emit one, and its key is the header's content, so:

- **An edit to one body invalidates every shared object of the header.** The header's
  content changed, every `<key>-<flags>.o` records it as a prerequisite, every one is
  rebuilt. The Spirit body row says so: one edit to `toucs4()` is 51 shared compiles --
  the header's 15 sharable functions and 36 from three headers whose closure reaches it.
  Per unit, the same edit was one piece per includer, each compiling *only that body*
  behind a copy TODO/48 keeps byte-identical. The store traded "one body × N includers"
  for "N bodies × 1 header", and the tool's promise -- an edit compiles the body that
  changed -- holds for neither the count nor the work.
- **Every piece pays for every body, on every compile.** A header with one body that is
  long to compile -- a heavy instantiation, a large `constexpr` evaluation -- charges it
  to each of its N pieces, and to each of them again when any other body is edited. The
  rewritten header spared the pieces exactly that.
- **macOS.** `nm` prints Mach-O weak definitions as `T`, so the store refused every
  shared piece there and the per-unit pieces ran (TODO/54). The fix that read `nm -m`
  (`0474cf1`) was reverted (`a557d6f`) rather than bring the anchor piece to macOS: the
  per-unit pieces, for all their duplication, keep the edit to one body. TODO/54 stays
  open until the design below lands, and then applies to it.

## Revised design: share the per-unit piece, not the header

The piece that is shared should be the per-unit piece with the unit taken out of it:
the rewritten header and one body, nothing of the includer.

- **What the store holds per header** (keyed on the *rewritten* header's content and the
  flags): `R`, the declarations-only copy -- the same text every includer already
  generates from the same original, and byte-identical across body edits (TODO/48) --
  and its PCH, built once per (header, flags) as `<hkey>.h.gch` is today.
- **What the store holds per function** (keyed on `R`'s hash, the body text's hash and
  the flags): the piece as the twin writes it minus the preamble include:

  ```cpp
  #include "<store>/<hkey>.h"          // R: the rewritten header, declarations only
  __attribute__((used))
  #line 12 "/abs/path/to/mylib.h"
  inline int multiply(int a, int b) { return a * b; }
  ```

  and its object. A body edit changes one body's hash: one key changes, one compile;
  the other functions' keys are unchanged and their objects current. Two bodies edited,
  two compiles. `R` does not change, so its PCH does not either.
- **Context.** The unit's preamble is what a per-unit piece includes first, because a
  header may need its includer's context (TODO/09: Boost.System's
  `std_category_impl.hpp`). A shared piece has no unit, so it has to be self-contained:
  the store tries the piece as above and, when it does not compile, marks the key
  `.fail` and the units compile their own -- the fallback that exists today, reached
  by the same headers as the anchor's `.fail` (those with no include guard, those that
  read a macro the includer sets). A header with an include guard and its own includes,
  which is nearly every library header, compiles.
- **Freshness** as today, by content: the object's record names `R`, the piece text and
  the include closure `R` reached. The header's own includes changing rebuilds every
  piece of the header, correctly; a body changing rebuilds its piece.
- **Strong symbols.** The piece emits exactly the one body and what `R` declares;
  `R` carries no non-inline definition (they moved to their own pieces or to the unit's
  definitions header), so the `nm` gate becomes a check that something unexpected did
  not slip in rather than the thing that decides sharing. On Mach-O it must read
  `nm -m` (TODO/54).
- **Overloads and members**: the piece is the body as written, so the typed anchor
  goes; a member is emitted in its out-of-line form as the twin already does.
- **Remote execution**: the action's inputs are `R`, the piece and `R`'s closure -- one
  key per function per flag set, as now; the parse on a worker writes `R` and the piece
  the same way the launcher does here.
- **Cost model**, Spirit, `toucs4()` edited: 1 shared compile instead of 51 (and
  instead of 267 per unit); no-op unchanged, one record per function checked; full
  unchanged in count, cheaper per piece since each parses declarations, not N bodies.

### Tests

- `launcher.shared_piece_body_isolation`: a header with `f()` and `g()`, two units
  including it. After the first build the store has one object each. Edit `f()`'s body:
  exactly one new object, `g()`'s object untouched (same content hash and mtime), no
  per-unit compile, the program prints the new value. Edit both: two.
- `launcher.shared_piece_heavy_neighbour`: a header with a body that is long to compile
  (a deep recursive `constexpr` or a large instantiation) beside a trivial one; an edit
  to the trivial body must not recompile the heavy one -- asserted on the store's
  objects, and on the wall time of the edit build being a fraction of the cold one.
- `launcher.shared_header_piece` as today, on the new shape; the context-needing
  header still goes per unit.
- Then, in CLAUDE.md's order: Boost.Filesystem 0/0, the Spirit suite through
  `SpiritTestsFromJamfiles.cmake` 0/0 with 268 programs passing, and the body row of
  `benchmark-spirit-split.sh` counted: one shared compile.

### Acceptance Criteria

- The three fixtures pass on Linux and on `macos-brew-llvm`; the macOS run needs the
  `nm -m` read from TODO/54 on top.
- Spirit's body row: 1 shared compile for `toucs4()`, the wall time at or below the
  28.7s / 38.8s of the anchor design here and on the cluster; the full row's per-piece
  cost below the anchor design's, since no piece parses more than one body.
- `benchmarks/` gets the rows, and TODO/54 is closed against this design.
