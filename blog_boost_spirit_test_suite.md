# Compiling Boost.Spirit's own test suite, one function per object file

`cpp-splitter` is a `CMAKE_CXX_COMPILER_LAUNCHER` that takes a C++ translation unit, cuts it
into one object file per function, and links the pieces back together with `ld -r`. The build
system then sees the same object it asked for — but when you change one function body, only the
piece holding it needs recompiling.

That is the promise. This post is what happened when it was pointed at Boost.Spirit's own test
suite, which is about as unkind a corpus as C++ has: header-only, expression-template dense,
276 small programs each pulling in most of Boost.

## What was measured

Spirit ships no CMakeLists for its tests — they are driven by a Boost.Build Jamfile — so
`BUILD_TESTING` produces no Spirit targets at all. So the targets are read straight out of
those Jamfiles: every `run` and `compile` rule the `qi`, `karma`, `lex`, `x3` and `support`
suites declare becomes a CMake target.

**277 targets. All 277 build. Nothing is excluded**.

The four `compile-fail` sources are the only omission, for the obvious reason: a build that is
supposed to fail is not interesting in benchmarking compilation performance.

Two trees are configured identically apart from `CMAKE_CXX_COMPILER_LAUNCHER`, and both are
settled before any incremental scenario is timed.

## The machine

| | |
|---|---|
| CPU | AMD EPYC-Milan, 32 cores |
| RAM | 122 GiB |
| Compiler | clang 13.0.0, for both the splitter and the Boost build (tipi toolchain `4f846ee`) |
| Build | CMake 3.31.9, ninja 1.12.1, `CMAKE_BUILD_TYPE=Debug`, C++17 |
| Link | `ld -r` for the relocatable link |
| Parallelism | **`-j8`**, not `-j32` |

The `-j8` is not modesty. A Spirit test unit is a very large template instantiation, and the
splitter holds a libclang AST of that unit in memory *alongside* the compile. At `-j32` a
corpus like this exhausts 122 GiB.

## The five benchmarked scenarios

| scenario | what it asks |
|---|---|
| **full** | the cost. One object per function, each re-instantiating the grammar its function needs. |
| **no-op** | rerunning on an existing build tree. Should do nothing. Anything else is a bug, not a measurement. |
| **one source** | touch one test `.cpp` — `libs/spirit/test/qi/char1.cpp`. |
| **one header** | touch a widely-included header, timestamp only, content identical. |
| **one body** | change the body of one ordinary inline function in a header. |

The last two are the interesting pair, and *which* header gets edited decides whether the
number means anything.

### `one header` — a touch that changes nothing

The header is:

```
boost/spirit/home/support/char_encoding/standard.hpp
```

Its timestamp moves; its content does not. Every build system re-runs the launcher on every
unit that includes it, so this row asks a specific question: can the splitter recognise that
its inputs are unchanged and reuse the previous split, instead of re-parsing 277 translation
units to produce byte-identical output?

### `one body` — a real edit, to a function chosen with care

The body target is a different file, and *which function* it is decides whether the row means
anything at all:

```
boost/spirit/home/support/char_encoding/standard_wide.hpp
```

```cpp
        static ::boost::uint32_t
        toucs4(wchar_t ch)
        {
            (void)42;   // <- the benchmark inserts one line here
            return static_cast<make_unsigned<wchar_t>::type>(ch);
        }
```

A different marker is inserted on each run, so successive edits are genuinely different text
and no content hash can short-circuit one of them.

**Two properties matter, and the second one took two attempts to get right.**

First, the definition has to be one the splitter can move out of the header at all. Nearly all
of Spirit is templates, which stay in the preamble with no piece emitted — there is nothing to
gain from splitting out a function that cannot be instantiated ahead of time. Editing one could
never recompile "one piece", because there is no piece; it would measure the splitter's worst
case and report it as its design case.

Second — and this is the one that matters — **the header has to be included far more widely
than the function is used.** `toucs4` is included by 194 of the units and emitted by exactly 1.
A plain build must recompile all 194, because a header they include changed. A split build
re-runs the launcher for all 194 too, but only the one unit that actually emits the function
has a piece to recompile.

The first version of this benchmark edited `utf8_put_encode()`, which is emitted in 178 of the
180 units that include it. Both builds therefore did the same amount of work, the row came out
flat, and it was read as the design failing. It was the target failing to discriminate.

## Results## Results

| scenario | plain | split | ratio | fallbacks |
|---|---:|---:|---:|---:|
| full | 73.8s | 802.5s | **10.9x slower** | 0 |
| no-op | 0.2s | 0.6s | 2.9x slower | 0 |
| one source | 2.4s | 0.8s | **3.2x faster** | 0 |
| one header | 72.6s | 8.2s | **8.9x faster** | 0 |
| one body | 72.8s | 58.7s | **1.2x faster** | 0 |

| artefact | plain | split |
|---|---|---|
| build tree | 1.4G | 48G |
| generated pieces | — | 4882 |

Nothing falls back on any row, and 210 units re-slice the edited body from a recorded harvest
rather than re-parsing.

## What the numbers say

### The cold build costs an order of magnitude — of *work*, not of wall time

10.9x more work, and that part is inherent: one object per function is strictly more than one
object per source, and on Spirit each of those objects re-instantiates the grammar templates
its function needs.

But 802 seconds is what that work costs **on eight lanes of one machine**, and a single
machine is not where this is meant to run. Remote execution — Bazel, or CMake's — is what the
cold-build column is aimed at, and it is one of the main reasons the splitter exists: break the
build into its most atomic unit, and both halves of remote execution get better at once.

| | independent units of work |
|---|---:|
| plain | 277 translation units |
| split | **4882 pieces** |

**Distributability.** A conventional build cannot go faster than its slowest translation unit,
however many machines you point at it. One heavy Spirit unit is a single job, on a single core,
start to finish — add a hundred workers and it takes exactly as long. Splitting removes that
floor: the unit becomes a parse, then hundreds of small independent compiles that can land on
different machines at once, then a link. Distribution's usual problem is running out of work to
hand out; 277 units cannot occupy 500 workers, and 4882 pieces can.

**Cacheability.** The same cut helps the cache, and for a different reason. A translation
unit's object is invalidated by any change anywhere in the file — edit one function of thirty
and the cache entry for all thirty is gone. A piece is one function: its cache key survives
every edit to its neighbours. The finer the unit, the more of the build a cache can keep across
a change, which is the property remote execution is actually built to exploit.

There is a third, more prosaic way the two fit together, and this benchmark ran straight into
it: the `-j8` above is a **memory** limit, not a CPU one. The splitter holds a libclang AST
alongside the compile, so eight is what 122 GiB will carry. A farm replaces "more lanes on one
box" with "lanes on many boxes", each bringing its own RAM — the constraint that caps this
benchmark does not exist there.

And a cold build is not where an edit-build loop spends its time in any case. Making it slower
on one machine is a deliberate trade in favour of the loop the next two rows measure.

**That was the argument. It has since been measured, and it did not hold up.** The section
below has the numbers.

### The touch rows are where the design pays

`one header` at **8.9x faster** is the clearest result here. The split of a translation unit is
a pure function of its source, its flags, and the contents of everything it includes. That
dependency list is written beside the pieces, so hashing it turns re-splitting into a
comparison. The plain build has to recompile every affected unit; the split build hashes a few
thousand files and relinks.

The work avoided scales with how expensive the code is. The work done to avoid it does not.

### That row was wrong twice, and the reason is worth knowing

| `one header` | |
|---|---|
| six targets, one unit falling back | **2.55x slower** |
| all 277, four units falling back | 4.6x faster |
| all 277, none falling back | **8.9x faster** |

When the splitter cannot split a unit it falls back to compiling it normally — the build
succeeds, and nothing is wrong with the output. But a fallback writes no split cache, so that
unit re-does its *entire* split on every subsequent build, fails again, and then compiles
plain. It never gets cheaper.

Timed apart on the six-target version: the five units that split took 0.5s; the one that fell
back took 9.5s, against 2.8s to just compile it. One unit in six was 95% of the row.

Three fallbacks, three defects, three fixes — and only after the last of them does this row
measure what it claims to. Which is the real argument for running a benchmark over a whole test
suite rather than a sample: it is not that the average is more representative, it is that the
tail is where the defects are.

### The no-op row stopped being free

0.2s against 0.6s. On every smaller corpus this row is parity, and here it is not.

Nothing is rebuilt in either tree. What the split side spends is the launcher starting once per
unit and hashing every prerequisite that unit records — 277 times over, about 2ms each. That is
a per-unit floor, and this is the first corpus large enough to show it. A tree of a few thousand
units would pay several seconds to discover it has nothing to do.

### 48G of build tree

Against 1.4G plain. The tree is dominated by debug information: every piece carries the debug
info of the preamble it includes, and on Spirit that preamble is most of Boost. Cutting the
number of pieces written by two orders of magnitude did not move this number at all, which is
the clearest evidence available that the cost is in the pieces that are *compiled*, not the
count of pieces written. It is a real cost of the approach and nothing here addresses it.

## Then it was measured on a real farm

The same 277 programs, built through CMake RE against an EngFlow Remote Build Execution
cluster, with and without the splitter. Reclient keeps per-action records, so every row can say
whether work was executed on the cluster or served from its cache — a distinction wall time
hides.

| scenario | splitter | wall | remote executions |
|---|---|---:|---:|
| full | no | 32.5s | 0 (all cached) |
| full | yes | 384.3s | 0 (all cached) |
| **one body** | **no** | **117.8s** | **271** |
| **one body** | **yes** | **65.3s** | **3** |

**The body edit is 1.8x faster, and the action counts say why: 271 compiles sent to the cluster
against 3.**

One function changed, so one function's object needed rebuilding — not the 194 translation
units that happen to include the header it lives in. That is the entire claim of per-function
splitting, and on a farm it is the difference between handing out 282 jobs and handing out 5.

The body row is a real comparison on both sides, which the full rows are not. A content change
produces new action keys, so nothing the plain build needs can come from the cache: it
genuinely recompiles 271 units. The split build genuinely recompiles one piece.

Where the remaining 65 seconds goes is worth being precise about, because it is not
compilation. The launcher still runs for all 194 units, each re-checks its inputs, each
relinks. Those are cache hits on the cluster now — 1569 of them — but they are the floor on
this row, and they are most of what is left.

### The full build, measured fairly, is where the cost is

The first run could not compare the full rows: the plain build was entirely cache-served while
the split build still executed 208 actions. Run again, after the cluster had cached everything
that first pass produced, **both come back with zero remote executions** — every action on both
sides served from cache.

That makes it a like-for-like measurement at last, and what it measures is overhead: **1641
cache lookups against 16137, and 32.5s against 384.3s.** Nothing was compiled on either side.
The split build carries 9.8x more actions and does its splitting locally, and when there is no
compiling to be done that costs 11.8x the wall time.

This is the honest price of the body row above, and it is not small. Nor is it the cold-build
number — with an empty cache the split build would additionally compile 16137 pieces against
the plain build's 1641, and that comparison still cannot be made from here, because the
cluster's cache cannot be evicted from outside.

### Linking is distributable too

A distributed split build compiles pieces remotely and then links each unit locally: one
`ld -r` over every object that unit produced. The splitter now hands that to
`tipi-linker-driver` when it is chained behind the compiler driver, which makes the link a
cacheable action like any other — 66.6s against 65.6s doing it locally, so free, with ~800
extra cache hits where 193 units relinked identically to last time and the cluster could say
so.

Against the old body target the same change looked like a 16-second penalty, because that edit
changed a piece in 178 units and no link could be a cache hit. Which picture you get depends
entirely on how much an edit actually invalidates — which is the same lesson as the target
choice, arriving from a different direction.

## The honest summary

On a corpus this hostile, per-function splitting turns a **73-second** rebuild after a header
touch into an **8-second** one on a single machine, and on a build farm turns a **118-second**
rebuild after a one-line function change into **65 seconds**, sending 3 compiles to the cluster
where the ordinary build sends 271. Nothing falls back that is not `TODO/34`.

It costs 10.9x the work on a cold build and 34x the disk, and on the farm it carries 16137
actions where the ordinary build carries 1641 — 11.8x the wall time on a full build where
nothing at all needs compiling. The disk is a real cost and this post has no answer to it. The
truly cold build has still not been measured with a control, because the cluster's cache cannot
be evicted from outside, so the cold column remains a number without one.

What can be said is narrower than "splitting makes builds faster", and more useful. It makes
the work an edit causes proportional to what the edit actually changed, instead of to the file
it happened to be written in. On one machine that shows up as a faster edit-build loop. On a
farm it shows up as far fewer jobs to hand out. And it shows up **only** when the edit is
smaller than the file — which is the normal case, and precisely why the benchmark had to be
built to measure it rather than around it.
