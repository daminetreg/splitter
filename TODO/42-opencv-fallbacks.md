# 42 — what a minimal OpenCV makes the splitter do

Filed from `benchmarks/opencv-local-split.md`. 61 of 158 units fall back, the libclang parse
reports errors in 96, and the body edit re-parses 94. Not yet worked on.

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
