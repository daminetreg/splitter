# A minimal OpenCV, split and built on one machine

One measurement set, from a single run of `./benchmark-opencv.sh` on 13 September 2026.

## What was built

OpenCV 4.11.0 (`31b0eee`), the `core` and `imgproc` modules only, as static libraries, with
tests, applications, bindings, IPP, OpenCL, threading libraries, image codecs, LAPACK and the
CPU-dispatch variants all disabled (`CPU_DISPATCH=`). That leaves **158 C++ translation units**
— 91 in `core`, 67 in `imgproc` — plus the 15 C units of the bundled zlib, which the launcher
does not touch. `core` alone builds in 7s on this machine, which is too little for the
incremental rows to measure anything; `imgproc` is the module every other one depends on next.

Each configuration is built twice, once plain and once with `cpp-splitter` as
`CMAKE_CXX_COMPILER_LAUNCHER`.

| | |
|---|---|
| Machine | AMD EPYC-Milan, 32 cores |
| Compiler | clang 13.0.0 (tipi toolchain `4f846ee`), `-std=c++17` as OpenCV sets it, **Release** (`-O3`) |
| Build system | CMake 3.31.9, ninja 1.12.1, `-j16` |
| Timing | the build phase only; configuration is not timed |

## The five scenarios

**full** — configure from nothing, then build.

**no-op** — build again with nothing changed.

**one source** — `touch modules/core/src/arithm.cpp`; timestamp only.

**one header** — `touch opencv2/core/cvdef.h`, which every unit includes; timestamp only.

**one body** — one line added to the body of `SparseMat::nzcount()` in
`opencv2/core/mat.inl.hpp`. Chosen from the split tree, not by hand: `mat.inl.hpp` is
mirrored into all **158** unit split directories, and `nzcount()` is emitted as a piece by
**1** of them. A plain build has to recompile every includer; a split build has one piece
whose content changed.

## Results

| scenario | plain | split | ratio | fallbacks |
|---|---:|---:|---:|---:|
| full | 16.6s | 295.4s | 17.8x slower | 61 |
| no-op | 0.2s | 0.2s | — | 0 |
| one source | 1.3s | 2.1s | 1.6x slower | 1 |
| one header | 16.0s | 17.6s | 1.1x slower | 61 |
| one body | 16.1s | 20.7s | 1.3x slower | 61 |

**61 of the 158 units fall back to a plain compile.** The fallback column counts them per row;
a unit that falls back is compiled whole, so every build above is correct and links, and the
rows measure the tool as it is on this corpus.

| artefact | plain | split |
|---|---:|---:|
| `libopencv_core.a` | 6,392,568 bytes | 9,187,850 bytes |
| `libopencv_imgproc.a` | 7,164,534 bytes | 7,737,912 bytes |
| build tree | 30M | 3.7G |
| generated pieces | — | 7827 |

## Why the rows read as they do

**The 61 fallbacks decide the incremental rows.** A unit that fell back has no split tree, so
every time the build system re-runs the launcher on it — every header touch, every header
edit — it is compiled whole again. Measured on the `one header` row: 158 launcher runs, 142
units reused their split, 61 were passed through to the compiler. 61 whole compiles is about
40% of the plain build, before any split unit does anything, and that is the 17.6s.

**The body row re-parsed 94 units.** Of the 158, 64 re-sliced the edited body without a
parse; 78 refused with *the change is not confined to one definition* and re-split from
scratch; the rest are fallback units. In the one refusing unit inspected, the harvest for
`mat.inl.hpp` does record `nzcount()`'s extent (a kept definition, no piece), so the refusal is
not the TODO/36 case; its cause is not established here. The 94 libclang parses are the
difference between 16.1s and 20.7s.

**The full build costs 17.8x**, against 11.3x on Boost.Spirit's suite. `-O3` compiles of
pieces that each re-instantiate what the unit's preamble declares, 7827 of them, on one
machine with nothing to absorb the extra work.

## What fell back, and why

From the full build's log, by first cause:

| units | cause |
|---:|---|
| 45 | `ld -r`: *multiple definition* of a C-API function — `cvMaxS`, `cvResize`, `cvFindContours`, `cvSVBkSb`, … (41 `CV_IMPL` functions), plus `cv::hal::cpu_baseline::recip64f`, `cv::accW_64f`, `cv::plugin::impl::DynamicLib::~DynamicLib()`. A definition with external linkage was kept in the tier-one preamble, which every header piece includes, so every header piece emitted it. TODO/10's layering by linkage does not catch these; `CV_IMPL` expands to an `extern "C"` specifier, and `cpu_baseline` is a namespace the dispatch macros open. |
| 11 | a `static` function renamed by the splitter (`__static_<file>__<name>`) and then referenced by name through OpenCV's CPU-dispatch macros (`CV_CPU_DISPATCH`, `cpu_baseline::`), which the rename does not reach: *no member named … in namespace cv::cpu_baseline*, *no matching function for call to …*, *declaration of reference variable … requires an initializer*. |
| 2 | `inline` inserted in front of something that is not a function: `'inline' can only appear on functions and non-local variables` (`utils/filesystem.private.hpp:46`). |
| 1 | a member rewritten out of line lost a default argument: *too few arguments to function call, expected 11, have 10* (`histogram.cpp`). |
| 1 | *expected ';' after top level declarator* (`alloc.cpp` preamble). |
| 1 | *expected unqualified-id* (`cuda_gpu_mat_nd.cpp` preamble). |

Separately, the libclang parse reported errors in **96 of the 158 units** — 3262 lines, mostly
*no type named … in …* and *no viable conversion* in `imgproc.hpp`, `cuda.hpp`, `mat.hpp` and
libstdc++'s `stl_vector.h`. The compile command's `-std=c++17` does reach the parse (the
command line wins over the probed default). The parse continues past errors and the split
proceeds from what it harvested; how much of the two tables above this accounts for is not
established. Boost.Spirit's suite shows 12 such lines across 279 units.

**The split archives are not symbol-identical to the plain ones.** `nm --defined-only -g` on
`libopencv_core.a`: 4118 external symbols plain, 4836 split; 73 present only in the plain
archive, 791 only in the split one (`imgproc`: 2604 / 2782; 139 / 317). The extra symbols are
`static` helpers given external linkage by the rename and `__attribute__((used))`
(`cv_isalnum`, `hal_ni_lut`, 415 `__static_…`). The missing ones include namespace-scope tables
(`cv::g_8x32fTab`, `cv::hal::popCountTable`), template instantiations no longer emitted
(`cv::normDiffInf_<float, float>`), a function-local static's guard variable, and
`cv::DownhillSolverImpl::createInitialSimplex`. Whether a consumer of the split archive would
fail to link on any of these has not been tested; the benchmark links nothing against them.

## Caveats

- The fallback rate makes this a measurement of the splitter *on a corpus it does not yet
  handle*, and the incremental rows should be read as such. Every cause above is a defect in
  the splitter, not a property of OpenCV; see `TODO/42`.
- The body row's 78 refusals are reported, not explained.
- `-j16` on 32 cores, as for the Boost measurements.
- Wall times are from one run.

## Reproducing

```sh
./benchmark-opencv.sh          # clones opencv 4.11.0 into example/opencv if absent
JOBS=8 ./benchmark-opencv.sh
```
