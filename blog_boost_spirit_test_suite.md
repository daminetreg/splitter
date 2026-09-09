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

### `one body` — a real edit to a real function

The body target is a different file, and deliberately so:

```
boost/spirit/home/support/utf8.hpp
```

```cpp
namespace detail {
    inline void utf8_put_encode(utf8_string& out, ucs4_char x)
    {
        (void)42;   // <- the benchmark inserts one line here
        // https://www.unicode.org/versions/Unicode15.0.0/ch03.pdf D90
        if (BOOST_UNLIKELY(x > 0x10FFFFul || (0xD7FFul < x && x < 0xE000ul)))
            x = 0xFFFDul;
        ...
```

A different marker is inserted on each run, so successive edits are genuinely different text
and no content hash can short-circuit one of them.

**Why this function and not any other.** Nearly all of Spirit is templates, and the splitter
keeps a template in the preamble without emitting a piece for it — there is nothing to gain
from splitting out a function that cannot be instantiated ahead of time. Editing one could
therefore never recompile "one piece", because there is no piece. It could only force every
unit that includes it to be re-split, which measures the splitter's *worst* case and reports it
as its design case.

`utf8_put_encode` is an ordinary non-template free function in Spirit's own header, and the
splitter emits and compiles a real piece for it in **269 of the 277 units**. It is the shape
splitting exists to serve, and of the handful of Spirit headers contributing any compiled piece
at all it is the one reaching the most units.

That distinction is not academic. An earlier version of this benchmark edited a class
template's member and reported the body row as a *win*. It was measuring nothing.

## Results

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

**None of that is measured here.** This is a single-machine benchmark, and the 10.9x is a real
number for a single machine. The distributed and cached cases are the design intent and the
mechanism it rests on, not a result — and they are the next thing to measure.

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

## The honest summary

On a corpus this hostile, per-function splitting turns a **73-second** rebuild after a header
touch into an **8-second** one, and a real body edit from 73 seconds into 59, with nothing
falling back to a plain compile.

It costs 10.9x the work on a cold build and 34x the disk. The disk is a real cost and this post
has no answer to it. The cold build is a different kind of number: 10.9x more work, cut into
18x more independent jobs, each one small enough to cache on its own and to hand to a different
machine. On eight lanes of one box that trade loses, and it is supposed to — the reason to make
it is that a build farm can only go as fast as the largest thing you can give a single worker,
and this makes that thing a function.

Proving it is the next benchmark, on more machines than one.
