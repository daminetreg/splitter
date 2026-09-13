# 42 — what a minimal OpenCV makes the splitter do

Filed from `benchmarks/opencv-local-split.md`. 61 of 158 units fall back, the libclang parse
reports errors in 96, and the body edit re-parses 94. Done; the outcome is at the end.

## Motivation

OpenCV is the first corpus here with a C API, a CPU-dispatch macro layer and `-O3` library
sources, and it finds four distinct defects plus one unexplained behaviour. Each has a unit
that reproduces it in the tree `benchmark-opencv.sh` leaves in `/tmp/opencv-split`.

## Implementation Proposal

In order of units affected:

1. **External-linkage definitions kept in the tier-one preamble** (45 units). `CV_IMPL`
   expands to `extern "C"`, and the dispatch macros define functions inside a `cpu_baseline`
   namespace; both are kept in the preamble with a reason, and both have external linkage, so
   every header piece emits them. TODO/10 layered the preamble by linkage; these are the cases
   its linkage test misses. Fixture: one `extern "C"` definition and one inline function in a
   header, in the same unit.
2. **Static renames the macros cannot see** (11 units). `CV_CPU_DISPATCH(name, …)` names a
   static function by token; after the rename the token no longer exists. Either rename through
   macro arguments too, or do not rename a static that a macro invocation names. Fixture: a
   macro that pastes a static function's name.
3. **`inline` inserted before a non-function** (2 units), `filesystem.private.hpp:46`.
4. **A default argument dropped when a member is rewritten out of line** (1 unit),
   `histogram.cpp`; and the two preamble syntax errors (`alloc.cpp`, `cuda_gpu_mat_nd.cpp`).
5. **The libclang parse errors** (96 units). Establish the cause first — the flags the parse
   receives against the flags the compile receives, on one unit — before attributing anything
   to it.
6. **The split-on-cluster body row executes 4043 piece compiles** where the split-here row
   sends 66 (`benchmarks/opencv-cmake-re.md`): pieces regenerated on a worker differ from those
   regenerated here, so none is a cache hit. Diff one unit's tree from each side; if the
   difference tracks the parse errors of (5), that is (5).
7. **The 78 body-row refusals**, *the change is not confined to one definition*, in units
   whose harvest does record the edited extent. `try_incremental_split()` already says which
   hypothesis fails; run it on one refusing unit with the verbose output and read it.

## Acceptance Criteria

- One fixture per defect in 1–4, each failing before its fix.
- `benchmark-opencv.sh` reports 0 fallbacks, and the split archives define the same external
  symbol set as the plain ones, or the difference is listed and each item explained.
- The body row re-parses no unit.

## Outcome

61 fallbacks to 3, 3262 parse-error lines to 0, the body row from 64 re-sliced and 78 refused
to 155 re-sliced and 0 refused, and the cluster's body row from 4043 executions to 26. One
commit per finding, each with a fixture that fails before it.

| | before | after |
|---|---:|---:|
| fallbacks on the full build | 61 | 3, each declined with its reason |
| libclang parse errors (lines / units) | 3262 / 96 | 0 / 0 |
| body edit, local: re-sliced / refused | 64 / 78 | 155 / 0 |
| body edit, local: wall (plain 16.1s) | 20.7s | 7.3s |
| body edit, cluster, split here: remote actions | 66 | 38 |
| body edit, cluster, split on cluster: wall / remote | 276.0s / 4043 | 22.5s / 26 |

What each item turned out to be:

1. **Not kept definitions: unharvested ones.** `CV_IMPL` expands to `extern "C"`, libclang 13
   reports a linkage specification as `CXCursor_UnexposedDecl`, and the visitor never
   descended into one — 42 units. Two more shapes under the same heading: a header included
   at the *end* of a source, which `clang_getInclusions()` does not report under a
   precompiled preamble (it walks the loaded or the local source-location table, not both);
   and one macro invocation producing several external definitions, whose folded range had
   lost its functions and so never reached the rule that moves an external definition out of
   the preamble. Fixtures: `split.extern_c_main`, `split.tail_include_main`,
   `split.macro_group_main`.
2. **The static rename reached a token that named another function.** A static whose
   unqualified name another function in the unit also has is kept in the preamble. A second
   shape: a static whose declaration defines its type got its `extern` with the original
   name. Fixtures: `split.static_name_shared_main`, `split.static_defines_type_main`.
3. **`inline` before a class** was the parse of item 5: `class CV_EXPORTS FileLock` read as a
   variable because `CV_EXPORTS` was not a macro in a parse missing `cvdef.h`. Gone with 5.
4. **A declarator across `#ifdef/#else/#endif`** needed its `;` on its own line and the
   piece not to close the conditional the extent closes itself; **`T::~T() = default;`** needed
   its extent carried through the `;`; the dropped default argument was item 5's parse.
   Fixtures: `split.conditional_declarator_main`, `split.defaulted_out_of_line_main`.
5. **The prefix PCH** was built from a copy of the include block where `#include
   "precomp.hpp"` — a sibling of the unit that nothing puts on `-I` — was not found, and was
   saved and used anyway; and the main parse read the include block a second time from the
   source, so every unguarded header was processed twice. The PCH gets `-iquote` with the
   unit's directory, is not saved when its build fails, and the parse sees the source with the
   prefix blanked. Fixture: `launcher.quoted_include_in_prefix`.
6. **The worker's pieces differed from the local ones** because the degraded parse of 5
   degraded differently there. With the parse clean they match, and the row's executions are
   cache hits.
7. **The 78 refusals** were the harvest of the degraded parse. 0 with 5 in.

Three units are declined by design, before any piece is written, with the reason: a static
variable of a type no other translation unit can name (`alloc.cpp`, `array.cpp`), and a
header included twice with no include guard whose second expansion defines external functions
(`arithm.dispatch.cpp`). `array.cpp` was not a fallback before: it split, and the program it
produced was wrong — a static of unnamed struct type was one object per piece. That is the
finding of this entry worth remembering: a fallback is the safe outcome, and a split that
went ahead is not always the better one. Fixtures: `split.pair_header_declines`,
`split.static_unnamed_type_declines`, each run twice so the decision has to survive its own
cache.

Also found along the way, in the launcher's caching: a split whose relocatable link failed
left its `split.cache` behind and was reused straight into the same failing link on every
later run. Not remembered any more.
