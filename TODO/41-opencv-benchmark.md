# 41 — benchmark a minimal OpenCV, locally and through CMake RE

## Motivation

Every benchmark so far is Boost: header-only, template-dense, hundreds of small programs.
OpenCV is a different corpus — a library of `.cpp` files, each of a few hundred to a few
thousand lines, sharing a set of headers (`cvdef.h`, `base.hpp`, `mat.hpp`, `utility.hpp`)
that every unit includes. It is the shape the splitter's argument is about: units that are
large relative to an edit, and headers whose reach exceeds their use.

## Implementation Proposal

1. `benchmark-opencv.sh`: clone `opencv/opencv` at a pinned tag into `example/opencv` if
   absent; configure the `core` module alone, static, with tests, apps, bindings, IPP, OpenCL,
   TBB and CPU dispatch variants off, so the unit set is the module's own sources; build it
   plain and with the splitter as launcher, Release, `-j16`, timing the build phase only.
2. The five scenarios of the Boost benchmarks: `full`, `no-op`, `one source`, `one header`,
   `one body`. The body target is chosen from the first split tree: a non-template function in
   a header every core unit includes, emitted by as few units as possible. Report which, and
   the include/emit counts.
3. Results to `benchmarks/opencv-local-split.md`, in the form of the Spirit summary: what was
   built, the machine, the five scenarios, the table, fallbacks and what caused them, artefact
   sizes, caveats, reproduction.
4. Commit and push. Then `benchmark-opencv-cmake-re.sh`: the same through
   `cmake-re --host --distributed`, plain and split, reported with reclient's action counts.

## Acceptance Criteria

- The script runs from a clean checkout with no manual step beyond the toolchain.
- Every row reports fallbacks; the document says what fell back and why.
- The body edit's include and emit counts are measured from the split tree, not assumed.
- Both plain and split builds link `libopencv_core.a`; the split one with the same symbol
  set (`nm` on both archives agrees on defined external symbols).
