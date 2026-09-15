# C++20 named modules, split and built on one machine

Two runs of `./benchmark-cpp20-modules.sh 2` on 14 September 2026, local plain against local
split, on the project the launcher's module support (TODO/43) was written against. The
second run's row is quoted where the two differ; they never differ by more than 0.3s.

## What was built

`example/cpp-20-modules/bench`, written by its `generate.py`: one module interface unit,
`mathlib.cppm`, with **40 exported non-inline functions** (each a loop over a `std::vector`,
a `std::map<std::string, int>` and a `std::sort`, through `<vector>`, `<string>`, `<map>`
and `<algorithm>` in the global module fragment) and **4 exported inline** ones; **30
importers** `use_N.cpp`, each with a static helper, a function calling five of the module's
functions and one of the inline ones, and a `describe_N()`; and a `main.cpp`. 32
translation units. The bench program prints `314` either way.

Each configuration is built twice, plain and with `cpp-splitter` as
`CMAKE_CXX_COMPILER_LAUNCHER`.

| | |
|---|---|
| Machine | Apple M2 Max, 12 cores, 32 GB |
| Compiler | Homebrew clang 21.1.7, `-std=c++20`, `-fmodules-reduced-bmi`, Debug |
| libclang | Homebrew's, the same 21.1.7 (`environments/macos-brew-llvm.cmake`) |
| Build system | CMake 4.2.1, ninja 1.13.2, `-j12`; CMake's own module scan (`clang-scan-deps`, dyndep) |
| Timing | the build phase only; configuration is not timed |

## The rows

**full** — configure from nothing, then build.

**no-op** — build again with nothing changed.

**one source** — `touch use_1.cpp`; timestamp only.

**one module** — `touch mathlib.cppm`; timestamp only. CMake rescans the unit and re-runs
its compile, and every importer's after it.

**one body** — one line added to the body of `compute_17()`, an exported non-inline function
in the interface unit. The row the design is for: plain, the BMI changes through the
definition's ODR hash and every importer recompiles; split, one implementation unit
recompiles and the interface text is byte-identical, so the BMI is too.

**one body =** — the same function's `return` statement changed in place, no line added. The
re-slice then has no `#line` to renumber in the pieces that follow, which is the difference
between this row and the one above.

**inline body** — one statement added to `fast_1()`, an exported inline function. The
control: the body lives in the BMI in both builds, so the BMI must change in both and every
importer recompile in both.

**importer body** — one line added to `use_1()` in `use_1.cpp`: the ordinary split, one
piece of one importer.

`objs/cc` is objects ninja rebuilt / compiler invocations made. Plain, the two are equal;
split, the launcher runs once per object and compiles what it must. `BMI` says whether
`mathlib.pcm`'s bytes changed across the edit.

## Results

| scenario | plain | split | ratio | plain objs/cc | split objs/cc | BMI |
|---|---:|---:|---:|---|---|---|
| full | 2.7s | 13.2s | 4.81x | 32/32 | 32/132 | |
| no-op | 0.0s | 0.0s | — | 0/0 | 0/0 | |
| one source | 0.6s | 0.4s | 0.64x | 1/1 | 1/0 | |
| one module | 2.2s | 1.1s | 0.51x | 31/31 | 31/0 | plain same, split same |
| one body | 2.3s | 2.7s | 1.16x | 31/31 | 31/23 | plain changed, **split same** |
| one body = | 2.3s | 1.8s | 0.77x | 31/31 | **31/1** | plain changed, **split same** |
| inline body | 2.5s | 10.8s | 4.30x | 31/31 | 31/131 | plain changed, split changed |
| importer body | 0.6s | 1.5s | 2.43x | 1/1 | 1/2 | |

Fallbacks in the split build: 0. Programs agree (`314`).

| | plain | split |
|---|---|---|
| binary | 188 KB | 788 KB |
| build tree | 21 MB | 153 MB |
| `mathlib.pcm` | 12 MB | 3.9 MB |
| pieces generated | — | 131 |

## Reading it

**The cascade is gone.** On both body rows the plain build's BMI changes and all 31 objects
downstream of it recompile; the split build's BMI is byte-identical and the 30 importers
compile nothing -- their launcher reads the split cache, hashes its 942 prerequisites, finds
them unchanged and touches the object (0.12s each, measured on one alone). That is what
TODO/43 set out to do, and what `-fmodules-reduced-bmi` alone cannot: the reduced BMI is
what both builds use here, and on the plain side it still changes on every body edit.

**Wall time does not show it at this size.** An importer here compiles in 0.6s and there
are twelve cores, so the plain build's 31 recompiles are 2.3s of wall; the split build's
one compile sits behind CMake's rescan of the module, 30 launcher runs, a relink of the
archive and of the program. 0.77x on the row that counts one compile against thirty-one.
What the row measures is *work*: 1 compiler invocation against 31, which is what a content
cache or a cluster charges for. The Boost and OpenCV benchmarks in this directory are where
the wall-time difference appears, because a unit there costs tens of seconds, not 0.6.

**A line inserted costs the pieces after it.** `one body` recompiles 23 pieces, `one body
=` recompiles 1. The re-slice keeps the `#line` directive of every later piece truthful, so
an insertion rewrites 22 pieces' first line and they recompile. Diagnostics point at the
right line in `mathlib.cppm`; the price is paid in an edit that adds or removes lines. That
is TODO/28's design for headers too, and it is the same trade there.

**The inline row is the control, and it costs more than the plain build.** The BMI changes
in both, as it must. Plain recompiles 31 objects; split recompiles the interface, then
*its own 40 implementation units* -- they are compiled against that BMI, and the launcher
cannot tell an inline body edit that reaches them from one that does not -- and then the 90
importer pieces. 131 compiles against 31, 4.3x the wall time. The 40 are the avoidable part:
a piece of the interface unit needs recompiling only when a declaration or inline body it
uses changed, which a finer key than the whole BMI's hash could say.

**Cold is 5x**, 132 compiles for 32 objects, each piece re-parsing the module's global
module fragment and its importer's headers. The importers' preambles are precompiled once
per unit and shared by their pieces; the interface's implementation units each replay the
fragment from source, since a module unit's fragment cannot be a PCH the way a header
preamble is. Same trade as everywhere else in this repository: pay once cold, win on every
edit after -- when the edit is a body.

**The BMI is 3x smaller in the split build** (3.9 MB against 12 MB) with the reduced BMI on
in both: the plain interface carries 40 bodies' worth of declarations and the ODR hashes
that go with them, the split interface carries declarations only.

## What the split tree looks like

```
CMakeFiles/mathlib.dir/mathlib.cppm.o.split/
├── mathlib_interface.cppm          declarations, the 4 inline bodies → mathlib.pcm + mathlib_interface.o
├── mathlib_preamble.h              the global module fragment, replayed by every piece
├── mathlib.cppm_5_compute_0.cpp …  module; #include "mathlib_preamble.h" module mathlib; + the body
├── mathlib.cppm.harvest / modules.hash / inputs.hash / split.cache / depfile.cache
└── *.o → ld -r → mathlib.cppm.o

CMakeFiles/bench.dir/use_3.cpp.o.split/
├── use_3_preamble.h (+ .gch)       import mathlib; the includes; the declarations
├── use_3.cpp_1_helper_3.cpp        import mathlib; #include "use_3_preamble.h" + the body
├── use_3.cpp_2_use_3.cpp · use_3.cpp_3_describe_3.cpp
└── *.o → ld -r → use_3.cpp.o
```

## Not measured here

- The cluster. TODO/43's remaining criterion -- the BMI in reclient's input records for a
  piece and for an importer -- needs the Linux clang ≥ 20 image, which does not exist yet.
- GCC. `bmi-probe/probe.sh` on a `.gcm` is still to be run.
- A project whose units cost seconds rather than 0.6s: the wall-time row that would show
  the 1-against-31 as time and not only as count.
