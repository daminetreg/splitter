# Boost.Spirit: split build vs plain build — 6 September 2026

Measured at `c620c20`, after `TODO/17`–`TODO/23` and the prefix-PCH fix those benchmarks
turned up.

Boost's libraries are ordinary C++ compiled into a static library. Spirit is template
metaprogramming: a single translation unit costs seconds, one shared header reaches across
most of Boost, and one edit re-instantiates thousands of templates. Splitting is expensive in
proportion to how much a unit costs to parse, and it saves in proportion to how much of that
cost an edit repeats — so Spirit is where both sides of the trade are largest, and the
filesystem numbers do not carry over.

## Method

Two things were measured, and they answer different questions.

**Correctness across a wider slice of Boost.** `./test-boost-libraries.sh
'filesystem;spirit;system;core'` builds four libraries *and their test suites* twice, once
through the splitter and once without, and compares only the difference. A test that fails to
build normally is not the splitter's fault and is not counted against it.

**Cost, on Spirit.** `./benchmark-spirit-split.sh` runs `benchmark-boost-split.sh`'s five
scenarios against `example/spirit-bench/`: four Spirit translation units and a driver behind
one shared header, carved from `example/spirit_example.cpp`. Both trees are configured
identically apart from `CMAKE_CXX_COMPILER_LAUNCHER` and settled before any incremental
scenario is timed.

| scenario | what it asks |
|---|---|
| full | the cost. One object per function, and on Spirit each of those re-instantiates the grammar. |
| no-op | should be free. Anything else is a bug, not a measurement. |
| one source | touch `xml.cpp`. |
| one header | touch `bench_common.hpp` — timestamp only, content identical. |
| one body | change the body of `bench_weight()`, an inline function in `bench_common.hpp`. |

The last two differ in the way that matters: a touch moves a timestamp without changing
content, so the split cache recognises the inputs and reuses the previous split (`TODO/14`).
The body edit changes the text, so the split has to run again — and on Spirit that re-parse is
the number that decides whether any of this pays.

Environment: AMD EPYC-Milan, 32 cores, 122 GiB RAM; clang 13.0.0 for both the splitter and the
Boost build (tipi toolchain `4f846ee`); CMake 3.31.9, ninja 1.12.1; `-j32`; Debug;
`ld` for the relocatable link.

## Correctness: filesystem, spirit, system, core

| | plain | split |
|---|---:|---:|
| wall time | 6.3s | 81.1s |
| objects built | 457 | 457 |
| **failed edges** | **0** | **0** |
| translation units split | — | 434 |
| **fallbacks to plain compilation** | — | **0** |

No target fails that would not also fail without the splitter.

The Spirit consumer has no CMake test suite, being header-only, so it is compiled directly.
It is the densest single translation unit available here:

| | time | result |
|---|---:|---|
| plain | 4.0s | compiles |
| split | 31.6s | 4525 pieces, no fallback, links and prints what the plain build prints |

### How that got there

| | failed edges | units split | fallbacks | Spirit consumer |
|---|---:|---:|---:|---|
| when `TODO/16` was filed | 40 | 373 | 62 | falls back |
| after `TODO/16` | 0 | 373 | 62 | splits |
| after `TODO/17`–`TODO/23` | 0 | **434** | **0** | splits |

The split count and the fallback count do not move by the same amount, and the difference is
accounted for rather than assumed. The splitter is invoked on 456 translation units either
way; each one either splits, falls back, or turns out to have nothing of its own to split —
a unit whose whole body arrives through an included file, which is neither a success nor a
failure and is reported as neither:

| | split | fell back | nothing of its own | invoked |
|---|---:|---:|---:|---:|
| before | 373 | 62 | 21 | 456 |
| after | 434 | 0 | 22 | 456 |

One unit crossed from the middle column to the right one rather than to the left: it no longer
fails, and it has no functions of its own to split either.

## Cost: the five scenarios on Spirit

| scenario | plain | split | ratio |
|---|---:|---:|---:|
| full | 3.7s | 42.3s | 11.6x slower |
| no-op | 0.2s | 0.2s | parity |
| one source | 2.9s | 0.6s | **4.5x faster** |
| one header | 3.6s | 0.7s | **5.1x faster** |
| one body | 3.7s | 8.1s | 2.2x slower |

| artefact | plain | split |
|---|---|---|
| `spirit_bench` | 12M | 17M |
| build tree | 31M | 1.4G |
| generated pieces | — | 22548 |

The split binary prints exactly what the plain one prints.

## Analysis

### Where splitting wins, it wins by more on Spirit

Against Boost.Filesystem, re-measured today on the same machine for comparison:

| scenario | filesystem | spirit |
|---|---:|---:|
| full | 10.2x slower | 11.6x slower |
| one source | 2.1x faster | **4.5x faster** |
| one header | 2.4x faster | **5.1x faster** |
| one body | 2.8x slower | 2.2x slower |

The two "faster" rows are the ones where nothing is parsed at all: the split of a translation
unit is a pure function of its source, its flags and the contents of everything it includes,
that dependency list is written beside the pieces as `depfile.cache`, and hashing it turns
re-splitting into a comparison. The plain build has to recompile the unit; the split build has
to hash a few hundred files and relink. On filesystem the unit being skipped costs half a
second, so the saving is half a second. On Spirit it costs three, so the saving is three. The
work avoided scales with how expensive the code is, and the work done to avoid it does not.

That is the whole argument for this approach, and Spirit is where it is most visible.

### Where it loses, it loses for a different reason than on filesystem

The full build is 11.6x slower and that is inherent: 22548 objects instead of 5, each
re-instantiating the grammar templates its function needs. Nothing about that is going to
improve much, and it is not supposed to — a cold build is not what an edit-build loop spends
its time on.

The body edit is the interesting row. It recompiles **one piece per translation unit** — the
design working exactly as intended, a change to one function costing one object file. And it
is still 2.2x slower than just rebuilding the units, because of what has to happen before
that: the shared header changed, so all five prefix PCHs are invalidated and rebuilt, and all
five units are re-parsed. On Spirit a PCH of the include prefix *is* most of the cost of the
translation unit, so re-parsing five of them costs about what compiling five of them costs.

    5 x "libclang PCH stale, rebuilding"
    4 x "compiling 1 header dep file"
    8.0s

So the piece-level incrementality is real and complete, and it is currently paying for a
re-parse that eats the entire saving. That is where the next round of work belongs: the split
of a header does not need the *whole* including unit re-parsed to discover that one function
body changed.

### The benchmark found a bug, which is most of what it was worth

The first run of this benchmark reported the body edit at 3.9s — better than the 8.1s above.
That number was wrong, and finding out why is the useful part.

The include prefix is precompiled so the include graph is parsed once per distinct prefix
rather than once per split. The PCH is named after a hash of the prefix file — the include
directives at the top of the source — and that text does not change when one of the headers it
names is edited. The stale PCH was handed back to libclang, which does not degrade gracefully:

    libclang PCH up-to-date: .../roman.cpp_prefix.h.pch/79590a14ead075c7.pch
    Error: failed to parse translation unit (code: 4)     [CXError_ASTReadError]
    splitting failed, falling back to normal compilation

Every header edit fell back. The 3.9s was five translation units compiling normally, plus the
overhead of trying to split them first — a number that *looked* like a good result and was in
fact the splitter not running.

Nothing said so. That message was the only trace and it was gated on `CPP_SPLITTER_VERBOSE`,
so a build that had quietly stopped splitting looked exactly like one that had not. It is
unconditional now: a fallback means the tool did nothing it exists to do, and the build
succeeds either way, so a silent one is a silent regression.

The PCH records its include closure beside itself and is rebuilt when any of it is newer.
`launcher.header_edit_behind_pch` pins it down — and splits twice to the same object path on
purpose, because splitting to a fresh path finds no PCH to be stale and would pass whatever
the splitter does.

This also affects the numbers in `boost-filesystem-bench-5-Sep-2026.md`: re-running that
benchmark's body edit against the pre-fix binary shows 2 of the 12 units falling back on a
parse failure. Its "one function body" row was measuring ten split units and two plain ones.
The re-measurement above (2.8x slower) has no fallbacks in it.

### Build tree size

1.4G of build tree for a 31M plain build, and 22548 pieces for five translation units. The
tree is dominated by per-piece debug information: every piece carries the debug info for the
whole preamble it includes, and on Spirit that preamble is most of Boost. This is the same
ratio the filesystem benchmark shows (275M against 6.5M) applied to code that is forty times
denser. It is a real cost of the approach and nothing here addresses it.

## Reproduction

```sh
./test-boost-libraries.sh 'filesystem;spirit;system;core'
./benchmark-spirit-split.sh
./benchmark-boost-split.sh          # the filesystem comparison
```

`./classify-fallbacks.sh` groups any fallbacks a build produced by their first error, replaying
each on its own — the build log cannot be read for this directly, because at `-j32` the
diagnostics of thirty-two translation units interleave.
