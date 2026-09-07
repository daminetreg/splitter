# Boost.Geometry: split build vs plain build — 7 September 2026

Measured at `a54b555`.

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
| one body | change the body of one inline function in that header. |

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

| scenario | plain | split | ratio |
|---|---:|---:|---:|
| full | 4.4s | 57.7s | 13.2x slower |
| no-op | 0.2s | 0.2s | parity |
| one source | 4.2s | 0.7s | **6.0x faster** |
| one header | 4.5s | 0.7s | **6.4x faster** |
| one body | 4.5s | 8.7s | 1.9x slower |

No fallbacks, and the split binary prints exactly what the plain one prints.

| artefact | plain | split |
|---|---|---|
| `geometry_bench` | 7.8M | 11M |
| build tree | 21M | 1.2G |
| generated pieces | — | 54622 |

### The trend across three kinds of code

| scenario | filesystem | spirit | geometry |
|---|---:|---:|---:|
| full | 10.2x slower | 11.6x slower | 13.2x slower |
| one source | 2.1x faster | 4.5x faster | **6.0x faster** |
| one header | 2.4x faster | 5.1x faster | **6.4x faster** |
| one body | 2.8x slower | 2.2x slower | **1.9x slower** |

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

| scenario | plain | split | ratio | fallbacks |
|---|---:|---:|---:|---:|
| full | 14.6s | 268.5s | 18.3x slower | **6** |
| no-op | 0.2s | 0.2s | parity | 0 |
| one source | 7.4s | 3.7s | **2.0x faster** | 0 |
| one header | 13.7s | 9.0s | **1.5x faster** | 0 |
| one body | 13.5s | 8.9s | **1.5x faster** | 0 |

| artefact | plain | split |
|---|---|---|
| build tree | 235M | 3.7G |
| generated pieces | — | 75168 |

### They fall back cold and split warm

The fallback column is the interesting part. On the **cold** build all six units fall back:
every Geometry test includes Boost.Test in header-only mode, and `progress_monitor.ipp`
defines a macro, uses it in four out-of-line member functions with external linkage, and
undefines it sixty lines later. Those definitions can go neither to the definitions header —
the macro is gone by the time that is compiled — nor stay duplicated in the preamble, where
`ld -r` rejects the copies. There is no placement that works, and falling back is the right
answer (`TODO/24`).

On every **incremental** build afterwards they split: `4 attempted, 4 split, 0 fallbacks`.
The headers those units share have been rewritten and cached by then, and the unit no longer
has to re-derive the parts that defeated it cold.

So the 18.3x on the full row is the price of *trying and failing* — the split runs, produces
75168 pieces, fails to link them, and compiles the six units normally on top of that. It is
the honest cost of the fallback path, which is worth having a number for rather than
describing as "falls back safely".

And the three incremental rows are genuine splits that beat the plain build, on the library's
own tests, despite the cold build not being able to split them at all.

## What the benchmark found

Both of these were found by writing the benchmark, not by the correctness harness.

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
