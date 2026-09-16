# 52 — a generated corpus of perfectly splittable code: unity build against the split build

## Motivation

The corpora measured so far -- Boost.Filesystem, Boost.Spirit, OpenCV, p4c -- mix what
the splitter handles well with what it keeps in the preamble: templates, members of class
templates, definitions from macros, statics of unnameable types. The p4c comparison
(`benchmarks/p4c-unity.md`) puts `-DCMAKE_UNITY_BUILD=ON` beside the split build, but on
code where two thirds of the definitions cannot move. What the two techniques do on code
that is entirely splittable -- free functions with external linkage, one declaration in
a header, no templates of the project's own -- has not been measured, and it is the
lower bound of the splitter's overhead and the upper bound of its gain.

## Implementation Proposal

- `example/synthetic/generate.py UNITS FUNCS <dir>` writes a CMake project: `synthetic.h`
  with the standard headers every unit reads and a few inline helpers; per unit
  `unit_<i>.h` declaring `FUNCS` free functions and `unit_<i>.cpp` defining them, each
  body a few lines of `std::vector`, `std::map`, `std::ostringstream` work so that a
  function costs something to compile; `main.cpp` calling one function of every unit;
  a `CMakeLists.txt` building one executable. Deterministic, so a body edit is the
  only difference between runs.
- `benchmark-synthetic.sh`: the p4c benchmark's shape -- plain, `-DCMAKE_UNITY_BUILD=ON`
  and the launcher, locally, `-j16`, Release -- for the full build, the no-op and one
  body: a line added to the body of one function of one unit. The unity batch size is
  CMake's default, 8. `UNITS` and `FUNCS` are parameters, 200 and 40 by default.
- `benchmarks/synthetic-unity.md`: the table with speed-ups as in `p4c-unity.md`, the
  counts (compiles, pieces, re-sliced), and the mechanism behind each row.

## Acceptance Criteria

- The generated project builds in the three configurations, the programs print the same
  number, and the split build has 0 fallbacks and 0 declined.
- The document reports full, no-op and one body for the three, with what each build
  compiled on the body row: one unit plain, one batch of 8 units unity, one piece split.

## Outcome

`benchmarks/synthetic-unity.md`, 200 units × 40 functions: full 16.0s plain, 14.1s unity,
97.3s split; no-op 0.2s each; one body 1.3s plain, 6.7s unity (a batch of 8 units),
0.7s split -- 2.0x plain, 10x unity. The split's body row recompiles 20 pieces, not one:
the pieces after the edited function in the unit change their `#line` directive. That is
the next thing to remove.
