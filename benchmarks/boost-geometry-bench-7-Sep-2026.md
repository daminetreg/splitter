# Boost.Geometry: split build vs plain build — 7 September 2026

**Re-measured 8 September at `27eb418`**, after `TODO/28` -- a body-only edit is now re-sliced
from the recorded harvest instead of re-parsing the unit. An earlier pass the same day, at
`6850486`, followed the conversion-operator harvest, `TODO/26` and `TODO/27`; the tables keep
that column beside the new one, because what changed between them is the interesting part.
Originally measured at `a54b555`.

Two measurements, because "compiling Boost.Geometry" means two different things and they give
opposite answers. The first is a **consumer** of the library: code someone writes against it.
The second is the library's **own test suite**: the 24 programs in `libs/geometry/test`, built
on Boost.Test in header-only mode.

Boost's libraries are ordinary C++ compiled into a static library; Spirit is expression
templates; Geometry is tag dispatch over a large concept hierarchy, where the cost is in
instantiating that dispatch for each geometry type a function touches rather than in one
enormous grammar.

## Method

Both benchmarks run `benchmark-boost-split.sh`'s five scenarios, on trees configured
identically apart from `CMAKE_CXX_COMPILER_LAUNCHER`, settled before any incremental scenario
is timed.

| scenario | what it asks |
|---|---|
| full | the cost. One object per function, each re-instantiating the dispatch its function needs. |
| no-op | should be free. Anything else is a bug, not a measurement. |
| one source | touch one `.cpp`. |
| one header | touch the shared header — timestamp only, content identical. |
| one body | change the body of one ordinary inline function in a header the splitter emits a piece for. |

`./benchmark-geometry-split.sh` measures `example/geometry-bench/`: four Geometry translation
units (area, distance, overlay, hull) and a driver behind one shared header. It deliberately
avoids Boost.Test, and it **aborts if anything falls back** — a fallback would make every row
a plain build with the splitter's overhead in front of it, which is the mistake the Spirit
benchmark made once before anyone checked.

`./benchmark-geometry-tests.sh` measures six of Boost.Geometry's own test targets across four
directories. Here fallbacks are the finding rather than a disqualification, so the count is
reported on every row instead of aborting. `-j8`, not `-j32`: at 32 this corpus exhausts
122 GiB and the OOM killer's victims look exactly like splitter defects (`TODO/24`).

Environment: AMD EPYC-Milan, 32 cores, 122 GiB RAM; clang 13.0.0 (tipi toolchain `4f846ee`);
CMake 3.31.9, ninja 1.12.1; Debug; `ld` for the relocatable link.

## Consuming Boost.Geometry

| scenario | plain | split | ratio | earlier 8 Sep | 7 Sep |
|---|---:|---:|---:|---:|---:|
| full | 4.5s | 59.9s | 13.2x slower | 11.8x | 13.2x |
| no-op | 0.2s | 0.2s | parity | parity | parity |
| one source | 4.7s | 0.7s | **6.7x faster** | 6.0x | 6.0x |
| one header | 4.5s | 0.7s | **6.4x faster** | 6.7x | 6.4x |
| one body | 4.5s | 6.4s | 1.4x slower | 2.0x slower | 1.9x |

No fallbacks, and the split binary prints exactly what the plain one prints.

| artefact | plain | split | 7 Sep |
|---|---|---|---|
| `geometry_bench` | 7.8M | 15M | 11M |
| build tree | 21M | 989M | 1.2G |
| generated pieces | — | **245** | 54622 |

The piece count fell by **99.6%** -- 54622 to 245 -- which is `TODO/27`: a piece used to be
written for every definition including the ones that can never be compiled, and on this
project 99.6% of them were templates and their members. Every piece written now is one that is
compiled.

The body row is the only timing that moved, from 2.0x slower to **1.4x slower**, and it moved
because of `TODO/28`: all four units re-slice `bench_weight()`'s piece from the recorded
harvest rather than re-parsing. It still loses. Four units and one driver is a small enough
tree that what remains -- rebuilding the preamble PCH and relinking 245 pieces -- outweighs a
parse that is no longer happening. The same change wins the test-suite body row below, where
there is more work for it to save.

The full row moved from 11.8x to 13.2x, which is run-to-run noise on a 4.5s baseline rather
than a regression: the split side is 59.0s against 59.9s, and it is the plain side that moved.

### The trend across three kinds of code

| scenario | filesystem | spirit | geometry |
|---|---:|---:|---:|
| full | 10.2x slower | 11.6x slower | 13.2x slower |
| one source | 2.1x faster | 5.0x faster | **6.7x faster** |
| one header | 2.4x faster | 5.3x faster | **6.4x faster** |
| one body | 2.8x slower | 1.5x slower | **1.4x slower** |

Spirit and Geometry are both from today, at the same commit; the filesystem column is its own
benchmark's and has not been re-measured since 5 September, so its body row still predates
`TODO/28` and should be expected to improve when it is re-run.

Every column moves the same way, and that is the argument for this approach stated as a
measurement rather than a hope. The two winning rows are the ones where nothing is parsed at
all: the split of a translation unit is a pure function of its source, its flags and the
contents of everything it includes; that list is written beside the pieces as `depfile.cache`,
and hashing it turns re-splitting into a comparison. The plain build has to recompile the
unit. The work avoided scales with how expensive the code is; the work done to avoid it —
hashing a few hundred files — does not.

The body edit is the row where the splitter still loses, and it loses by less on denser code
for the same reason. It recompiles one piece per translation unit, which is the design
working; what it pays for is re-parsing the units whose shared header changed, and on Geometry
that re-parse is a smaller fraction of what the plain rebuild costs.

## Compiling Boost.Geometry's own test suite

Six targets, `-j8`:

| scenario | plain | split | ratio | fallbacks | 7 Sep |
|---|---:|---:|---:|---:|---:|
| full | 15.1s | 265.1s | 17.5x slower | **0** | 18.3x, 6 fallbacks |
| no-op | 0.2s | 0.2s | parity | 0 | parity |
| one source | 7.3s | 2.2s | **3.3x faster** | 0 | 2.0x |
| one header | 14.0s | 4.6s | **3.0x faster** | 0 | 1.5x |
| one body | 14.2s | 7.2s | **2.0x faster** | 0 | not comparable |

| artefact | plain | split | 7 Sep |
|---|---|---|---|
| build tree | 235M | 4.6G | 3.7G |
| generated pieces | — | **2338** | 75168 |

Two things moved: the fallbacks are gone, and the two touch-driven rows got roughly twice as
fast because there are 2338 pieces to consider instead of 75168.

### The body row changed what it edits, and got worse

Until 8 September this row edited `area<Polygon, polygon_tag>::apply` in
`boost/geometry/algorithms/area.hpp`, and reported **1.1x faster**. That number was not worth
having. `apply` is a member of a class template, so the splitter keeps it in the preamble and
emits no piece for it (`TODO/27`). No edit to it can ever recompile "one piece", because there
is no piece -- the row was timing the splitter's worst case and printing it as its design case.

It now edits `side_info::collinear()` in `boost/geometry/strategies/side_info.hpp`: an ordinary
inline member of an ordinary class, the shape splitting exists to serve. The row got *worse*,
and the worse number is the true one.

What the edit actually causes on the split side:

```
$ ninja -j8 <six targets>                      # after the edit
plain:  5 CXX edges          # the 5 units that include side_info.hpp
split:  5 CXX edges
$ find ... -name '*.o' -newer <edit>           # piece objects rebuilt, split tree
      5 side_info.hpp_6_collinear.o
```

**Five piece objects, all of them the edited function.** Piece-level incrementality is exact:
nothing else in 2338 pieces was touched.

For one day this row still read **1.14x slower**, because what the splitter paid for was not
the compile but re-parsing those five units to discover that only one piece had changed. That
is what `TODO/28` removed: the extents and the hashes needed to prove an edit is confined to
one body are now written beside the pieces, so a body-only edit re-slices that one piece and
parses nothing. The row is **2.0x faster** than the plain build, and the split output is
byte-identical to what a full parse produces -- 4642 files checked, which is the assertion the
fixture makes rather than a timing claim.

It took editing a real, non-template function to see any of this. While the row edited a class
template's member the splitter could only lose it, and the loss was invisible because the
number happened to look like a win.

Boost.Geometry makes this hard to measure at all: of the 245 header pieces compiled into every
one of the six units, **not one comes from a Boost.Geometry header** -- they are Boost.Test,
SmartPtr and Multiprecision. Geometry is header-only and almost entirely templates, so it
offers the splitter very little to split. `side_info.hpp` is among the only Geometry headers
contributing a compiled piece; `boost_geometry_util_range` does not include it and so does not
rebuild on this row, equally on both sides.


### They used to fall back cold. They no longer do.

On 7 September all six units fell back on the cold build, and split only on the second: every
Geometry test includes Boost.Test in header-only mode, and an out-of-line `operator bool()`
there was copied into every piece because conversion operators were not harvested at all. That
was the last thing standing between these units and a cold split, and merging the harvest
removed it. The fallback column is zero on every row now, cold included.

The `18.3x` on that row was therefore the price of *trying and failing*. The `17.3x` today is
the price of actually doing it, which is a different number that happens to look similar: the
split runs, produces 2338 pieces, links them, and there is no plain compile underneath.

**This does not mean the tests work.** The benchmark builds them; it does not run them. They
still fail at run time:

```
$ boost_geometry_algorithms_area          # plain
$ echo $?  ->  0
$ boost_geometry_algorithms_area          # split
Test setup error: There is no argument provided for parameter color_output
$ echo $?  ->  200
```

That is `TODO/25` defect 3, open and not understood. A row reading "0 fallbacks" says the
splitter produced an object without giving up; it says nothing about whether the object is
right. Reading it as the latter is exactly the mistake this benchmark's first run made in the
other direction, when a fallback was timed as if it were a split.

## What the benchmarks found

Four defects so far, none of them by the correctness harness.

### Written on 8 September, from the piece counts above

`TODO/27` began with a question about one of these files: why is there a `.cpp` for a function
template, when a template cannot be instantiated ahead of time? It could not be, and it never
was -- the piece carried a note saying it would not be compiled, and nothing compiled it. It
was written anyway. On one test translation unit that was 11515 pieces written against 273
compiled, 12 MB of `.cpp` that nothing reads, 8667 of them templates and their members. The
reason a definition was kept is now recorded once per unit in a `.keeps` file instead of at the
top of eleven thousand files nobody opens.

Fixing that immediately exposed a defect in `TODO/26`, which had shipped an hour earlier: a
`static` variable was being renamed and moved without asking whether it was `const`.
Boost.Filesystem writes `BOOST_CONSTEXPR_OR_CONST`, so the text says nothing, and a compile-time
constant was moved out from under its users. The const-ness is asked of the type now, which is
what the code already did for constexpr *functions* and for the same written-down reason.

### Written on 7 September

### An explicit specialization losing the `inline` its macro carried

Boost.QVM, which Geometry pulls in, writes

```cpp
template <> BOOST_QVM_INLINE_TRIVIAL long double floor<long double>( long double x )
```

where the macro carries an always-inline attribute *and* the `inline`. Stripping the attribute
— which has to happen, because an always-inline function is never emitted out of line — took
the `inline` with it. The text was then judged to open a template and left alone, so what
remained was a strong definition in a header every piece includes:

```
multiple definition of `long double boost::qvm::floor<long double>(long double)'
```

`template <>` introduces a function, not a template. Fixed, with a fixture; the first attempt
at the fix produced `inline template <>`, which is not a declaration, so the keyword now goes
after the prefix.

### A fallback erasing its own dependencies

The first run of the test-suite benchmark reported the body edit at **0.2s against 13.5s**,
and the split build faster than the plain one on three rows. With every unit falling back that
is impossible, and it was: the split tree had quietly stopped rebuilding.

```
$ ninja -t deps ... area.cpp.o
  plain:  #deps 2232
  split:  #deps 0
$ ninja -d explain ... area.cpp.o
  plain:  output ... older than most recent input ... area.hpp
  split:  no work to do.
```

The build system consumes the depfile and deletes it, so its absence means "already read",
not "no dependencies" — but ninja does not keep what it read if the edge runs again and writes
nothing. An edge that runs and produces no depfile is recorded with **zero** dependencies, and
the object then survives every later edit to every header it includes.

The split path caches the depfile it rewrites so an idle rerun can restore it. The fallback
path never did, and these units fall back cold and split warm — exactly the sequence that
loses everything. All three passthrough paths cache it now; the same object records 2232
dependencies again and the header edit is detected.

There is no regression fixture for this one. Reproducing it needs a unit that falls back on
one run and splits with nothing to recompile on the next, which I could not build small and
deterministically — the fixture I tried exercised the link-failure path, which already cached,
and passed with and without the fix. A test that cannot fail is worse than none, so the
evidence is the before/after above rather than a green check.

## Reproduction

```sh
./benchmark-geometry-split.sh      # a consumer of the library
./benchmark-geometry-tests.sh      # the library's own test suite
```

Both print the fallback count. If it is not zero where the row claims a split, the row is
measuring a plain build with overhead in front of it.
