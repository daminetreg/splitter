# A minimal OpenCV, split and built on one machine

Three measurement sets, each from a single run of `./benchmark-opencv.sh`, all on 13
September 2026: before TODO/42, after it, and after TODO/44. The earlier ones are kept because
each later one is their consequence.

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

### After TODO/44

| scenario | plain | split | ratio | fallbacks | declined |
|---|---:|---:|---:|---:|---:|
| full | 16.3s | 322.2s | 19.8x slower | 0 | 0 |
| no-op | 0.2s | 0.2s | — | 0 | 0 |
| one source | 1.3s | 0.3s | 4.3x faster | 0 | 0 |
| one header | 16.0s | 1.0s | **16x faster** | 0 | 0 |
| one body | 15.9s | 4.6s | **3.5x faster** | 0 | 0 |

**All 158 units split; 0 fall back, 0 are declined.** `alloc.cpp` and `array.cpp` keep the
static variable and every function that uses it together in the definitions object
(TODO/44 A); `arithm.dispatch.cpp` splits each inclusion of `arithm.simd.hpp` on its own,
the second into `include/_inclusion/2/`, with its pieces compiled in that inclusion's macro
state (TODO/44 B). On the header row nothing is compiled whole any more: 158 launcher runs
reuse their split, 1.0s in all. On the body row 158 units re-slice and 0 parse; 35 pieces
recompile, as before.

| artefact | plain | split |
|---|---:|---:|
| `libopencv_core.a` | 6,392,568 bytes | 10,715,094 bytes |
| `libopencv_imgproc.a` | 7,164,534 bytes | 9,727,204 bytes |
| build tree | 30M | 5.7G |
| generated pieces | — | 8641 |

`nm --defined-only -g`: `libopencv_core.a` 4118 external symbols plain, 5231 split, 85 only
in the plain archive, 1198 only in the split one (`imgproc`: 2604 / 3125; 352 / 873) — the
same shape as after TODO/42, 190 more pieces from the three units.

### After TODO/42

| scenario | plain | split | ratio | fallbacks | declined |
|---|---:|---:|---:|---:|---:|
| full | 16.4s | 318.8s | 19.4x slower | 0 | 3 |
| no-op | 0.2s | 0.2s | — | 0 | 0 |
| one source | 1.3s | 0.3s | 4.3x faster | 0 | 0 |
| one header | 16.3s | 5.3s | **3.1x faster** | 0 | 3 |
| one body | 16.1s | 7.3s | **2.2x faster** | 0 | 3 |

**0 units fall back. 3 are declined: the splitter decides before any piece is written that
it cannot split them correctly, compiles them whole, and says why.** A fallback is a defect —
the splitter tried and something failed; a decline is a stated limit (`TODO/44`). The two
were one column until this run. `alloc.cpp` and `array.cpp` hold a static variable of a type no other
translation unit can name (a class in an unnamed namespace; an unnamed struct), and
`arithm.dispatch.cpp` includes `arithm.simd.hpp` twice under two macro states with no include
guard, and the second expansion defines external functions. The libclang parse reports 0
errors. On the body row 155 units re-slice the edited body, 0 refuse, and 3 parses run — the
declined units'. 35 pieces recompile: the edited one, and the pieces of `mat.inl.hpp` after it
whose `#line` moved.

| artefact | plain | split |
|---|---:|---:|
| `libopencv_core.a` | 6,392,568 bytes | 10,527,770 bytes |
| `libopencv_imgproc.a` | 7,164,534 bytes | 9,747,596 bytes |
| build tree | 30M | 5.6G |
| generated pieces | — | 8451 |

`nm --defined-only -g`: `libopencv_core.a` 4118 external symbols plain, 5142 split, 85 only
in the plain archive, 1109 only in the split one (`imgproc`: 2604 / 3125; 352 / 873). The
extra symbols are `static` helpers given external linkage by the rename and
`__attribute__((used))`; the missing ones include namespace-scope tables, template
instantiations no longer emitted, and out-of-line members. Whether a consumer of the split
archive would fail to link on any of them has not been tested.

### Before TODO/42

| scenario | plain | split | ratio | fallbacks |
|---|---:|---:|---:|---:|
| full | 16.6s | 295.4s | 17.8x slower | 61 |
| no-op | 0.2s | 0.2s | — | 0 |
| one source | 1.3s | 2.1s | 1.6x slower | 1 |
| one header | 16.0s | 17.6s | 1.1x slower | 61 |
| one body | 16.1s | 20.7s | 1.3x slower | 61 |

61 of the 158 units fell back; the libclang parse reported 3262 errors across 96 units; on
the body row 64 units re-sliced and 78 refused. What each of those was, and what fixed it, is
in `TODO/42`. In short: functions inside `extern "C"` were never harvested (42 units); a
header included at the end of a source was not a candidate; a macro producing several
external definitions stayed in the preamble; a static whose name a dispatch macro used for
another function was renamed under the macro (11); the prefix PCH was built where a quoted
include of a sibling could not be found and used anyway, which is where the parse errors and
the refusals came from; and two rewrite defects. Two of the three units now declined were
*not* fallbacks before: `array.cpp` split, and the program it produced was wrong — a static of
unnamed struct type became one object per piece.

## Why the rows read as they do

**The full build costs 19.8x**, against 11.3x on Boost.Spirit's suite: `-O3` compiles of
8641 pieces on one machine.

**The header touch is 16x faster than plain.** 158 launcher runs, every one reusing its
split. After TODO/42 the 3 declined units were compiled whole on every touch, and the row
read 5.3s; before it the 61 fallback units were, and it read 17.6s.

**The body edit is 3.5x faster than plain.** 158 re-slices without a parse and 35 piece
compiles, against 158 whole compiles. After TODO/42, with 3 whole compiles among them, the
row read 7.3s.

## Caveats

- The split archives are not symbol-identical to the plain ones; see above.
- `-j16` on 32 cores, as for the Boost measurements.
- Wall times are from one run.

The same corpus through CMake RE on the cluster is in
[`opencv-cmake-re.md`](opencv-cmake-re.md).

## Reproducing

```sh
./benchmark-opencv.sh          # clones opencv 4.11.0 into example/opencv if absent
JOBS=8 ./benchmark-opencv.sh
```
