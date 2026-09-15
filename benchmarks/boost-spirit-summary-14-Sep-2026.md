# Boost.Spirit's test suite, split and built on one machine, 14 and 15 September 2026

One run of `BUILD_TYPE=Release CMAKE_RE_JOBS=16 MODES="plain split" ./benchmark-spirit-cmake-re.sh --host`
at cpp-splitter `a3e256df` (after TODO/44, TODO/47, TODO/48 and the preamble PCH being loaded
with `-include`), Boost at `ab7968a0bb`. Local rows only: no cluster, no remote split. The
scenarios, the machine and the toolchain are those of
`boost-spirit-rbe-summary-9-Sep-2026.md`: 277 test programs, clang 13 (`4f846ee`), C++17
Release, `-j16`, wall is the build phase as CMake RE reports it.

## Results

| scenario | splitter | build | fallbacks | declined | what the splitter did |
|---|---|---:|---:|---:|---|
| full | no | 57.6s | 0 | 0 | — |
| full | yes | 438.1s | 0 | 0 | 279 parsed and split, 557 PCH, 4408 piece compiles |
| no-op | no | 5.6s | 0 | 0 | — |
| no-op | yes | 11.8s | 0 | 0 | none: every unit reused its split |
| one source | no | 5.6s | 0 | 0 | — |
| one source | yes | 5.9s | 0 | 0 | none, not re-mirrored |
| one header | no | 5.6s | 0 | 0 | — |
| one header | yes | 5.9s | 0 | 0 | none, not re-mirrored |
| **one body** | **no** | **56.8s** | 0 | 0 | — |
| **one body** | **yes** | **128.0s** | 0 | 0 | 265 re-sliced, 1 declared only, 2 re-parsed, 268 PCH rebuilt, 4228 piece compiles |

Speed-ups, plain over split: full 0.13x, no-op 0.47x, one source 0.95x, one header 0.95x,
one body **0.44x**.

## Against the 11 September table

| scenario | 11 Sep plain | 11 Sep split | 14 Sep plain | 14 Sep split |
|---|---:|---:|---:|---:|
| full | 57.8s | 654.6s | 57.6s | 438.1s |
| no-op | 5.7s | 11.4s | 5.6s | 11.8s |
| one source | 5.6s | 6.0s | 5.6s | 5.9s |
| one header | 5.7s | 6.0s | 5.6s | 5.9s |
| one body | 57.5s | 14.2s | 56.8s | 128.0s |

**The full split build is 1.5x faster than on 11 September**, 654.6s to 438.1s. The pieces
now load the preamble PCH: before the `-include` fix clang built the `.gch` and never read
it, so every piece re-parsed its preamble from source.

**The body row is 9x slower than on 11 September, and the 11 September figure was wrong.**
That row was measured with the PCH not in use and the pieces' objects left stale: the copy
of `standard_wide.hpp` had changed but the pieces including it were not recompiled. With the
PCH in use the dependency is honoured, and the row measures what the edit costs.

## Why the body edit recompiles almost everything

`standard_wide::toucs4()` is a static member function defined in its class. 268 units include
the header. One (`x3/tst.cpp`) declares it only in its copy (TODO/48), re-slices nothing and
compiles nothing. Two (`lex/regression_matlib_static.cpp`, `lex/regression_matlib_switch.cpp`)
re-parse: the build regenerates their `matlib_static*.h` on every run, and with two changed
inputs the launcher does not re-slice. The other 265 keep `toucs4`'s body in their copy:
TODO/48's rule declares a definition only when nothing in the unit's sources names it outside the candidates' own bodies, and
`toucs4` is named in `support/char_class.hpp` and `qi/char/char.hpp` inside templates, so
the copy must carry a body whether or not the unit instantiates those templates.

A body kept in the copy sits in the preamble, the preamble is the PCH, and every piece is
compiled against the PCH. So each of those 267 units rebuilt its PCH and recompiled every
piece: 4228 of the full build's 4408 piece compiles, and 268 of its 557 PCH builds (the
prefix PCH for libclang's parse is not rebuilt). What the row saves against the full build
is the parse: 128.0s against 438.1s.

The plain build recompiles the same 268 units once each, 56.8s. The split build pays 2.25x
that: per unit, one PCH build plus the parallel piece compiles, of which the piece compiles
are the larger part.

## Two things the run showed

**The launcher prints `Error: cannot open file: .../standard_wide_preamble.h` once per
re-sliced unit** (once per unit that keeps the definition, 264 times in the body row). The re-slice looks for the kept definition in
two candidate files, the copy and a `<stem>_preamble.h` beside it, and `read_file` reports
the absent second candidate before the loop skips it. The re-slice is not affected.

**The touch rows and the no-op row are unchanged**: 5.9s against 5.6s for a touch, 11.8s
against 5.6s for a no-op. The no-op cost is the launcher validating 279 splits.

## After TODO/49, 15 September

Same command, cpp-splitter `040b7ab8`. A header definition the unit does not emit but a
template names is now split like an emitted one: the copy declares it and a piece defines it
(`inline`, `__attribute__((used))`). The Spirit suite goes from 4408 pieces to 37538.

| scenario | splitter | build | fallbacks | declined | what the splitter did |
|---|---|---:|---:|---:|---|
| full | no | 57.7s | 0 | 0 | — |
| full | yes | 882.4s | 0 | 0 | 279 parsed and split, 557 PCH, 37538 piece compiles |
| no-op | no | 5.7s | 0 | 0 | — |
| no-op | yes | 29.2s | 0 | 0 | none: every unit reused its split |
| one source | no | 5.6s | 0 | 0 | — |
| one source | yes | 5.9s | 0 | 0 | none, not re-mirrored |
| one header | no | 5.7s | 0 | 0 | — |
| one header | yes | 5.9s | 0 | 0 | none, not re-mirrored |
| **one body** | **no** | **56.6s** | 0 | 0 | — |
| **one body** | **yes** | **34.1s** | 0 | 0 | 267 re-sliced, 1 declared only, 0 PCH rebuilt, 267 piece compiles |

Speed-ups, plain over split: full 0.07x, no-op 0.20x, one source 0.95x, one header 0.95x,
one body **1.66x**.

**The body edit recompiles one piece per unit and no PCH.** `toucs4` has a piece in each of
the 267 copies that used to keep its body; the edit re-slices that piece and recompiles it
against the unchanged PCH: 267 compiles against 4228 the day before, 34.1s against 128.0s,
and 1.66x faster than plain's 56.6s. `x3/tst.cpp` still declares it only and compiles nothing.

**The full build doubles and the no-op more than doubles.** 37538 pieces against 4408:
882.4s against 438.1s for the full split, 29.2s against 11.8s for the no-op, which is the
launcher checking every piece of every unit. That is the price of option 1 of TODO/49, one
piece per named definition; option 2, all of a unit's named definitions in its one
definitions unit, would have added no piece and cost one compile per unit on the edit.

**Kept with their body, 1504 definitions across the suite:** those whose body refers to a
function the unit declares and does not define, `boost::math::concepts::acosh()` calling
`boost::math::acosh` with only `math_fwd.hpp` read. A piece for one of those carries a
reference the plain build never made and the link fails; the first run of TODO/49 lost
`qi/real1..5`, `karma/real1..3` and `x3/real4` to exactly that.
