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

## The five benchamkred scenarios

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

### The cold build costs an order of magnitude

10.9x, and that is inherent. One object per function is strictly more work than one object per
source, and on Spirit each of those objects re-instantiates the grammar templates its function
needs. Remote Execution of the build with Bazel or CMake RE fixes that, and it is one of the main reason for the splitter: break the most atomic unit of the builds to maximize cacheability and distributabilty of the build.

However a cold build is not where an edit-build loop spends its time and therefore even if we make this slower on single machine builds, the goal is to serve very active edit loop.

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
touch into an **8-second** one, and a real body edit from 73 seconds into 59. It costs an
order of magnitude on the cold build and 34x the disk.

Whether that trade is worth taking depends entirely on how often you do a cold build versus how
often you change one function — which is to say, it is worth taking exactly when you are doing
the thing this is for.
