# Boost.Filesystem: split build vs plain build — 5 September 2026

Measured at `262eb53`, immediately after the caching work in `TODO/14`.

## Method

`./benchmark-boost-split.sh 2` configures Boost.Filesystem twice from the same source tree,
identically apart from `CMAKE_CXX_COMPILER_LAUNCHER`, so the only variable is the splitting.
Both trees are settled before any incremental scenario is timed — a build that always has
something to do would make those numbers meaningless, which is what `TODO/13` was about.

Five scenarios, chosen because they answer different questions rather than to produce a
flattering number:

| scenario | what it asks |
|---|---|
| full | the cost. One object file per function is strictly more work than one per source. |
| no-op | should be free. Anything else is a bug, not a measurement. |
| one source | touch `libs/filesystem/src/portability.cpp`. |
| one header | touch `boost/filesystem/path.hpp`, included by seven of the twelve units. |
| one function body | change the body of `is_regular_file` in `operations.hpp`. |

The last two differ in a way that turns out to matter: a *touch* moves a timestamp without
changing content, whereas the body edit changes the text. Real edit-build loops contain both,
and the splitter now handles them very differently.

Environment: AMD EPYC-Milan, 32 cores, 122 GiB RAM; clang 13.0.0 as the host compiler for
the splitter, the tipi clang `4f846ee` toolchain for the Boost build; CMake 3.31.9,
ninja 1.12.1; `-j32`; Debug, `BUILD_SHARED_LIBS=OFF`, `BOOST_INCLUDE_LIBRARIES=filesystem`,
which pulls in Boost.System, Boost.Atomic, Boost.SmartPtr, Boost.Iterator, Boost.Scope and a
large slice of Boost.Config — twelve translation units, 5148 generated pieces.

## Results

Two runs, to show the spread:

| scenario | plain | split (run 1) | split (run 2) | ratio |
|---|---:|---:|---:|---:|
| full | 0.7s | 7.4s | 7.3s | 10.8x slower |
| no-op | 0.2s | 0.2s | 0.2s | parity |
| one source | 0.5s | 0.2s | 0.2s | **2.0x faster** |
| one header | 0.7s | 0.3s | 0.3s | **2.5x faster** |
| one function body | 0.7s | 1.8s | 1.8s | 2.7x slower |

Run-to-run variation is under 2% on every row.

| artefact | plain | split |
|---|---|---|
| `libboost_filesystem.a` | 2.2M | 12M |
| build tree | 6.5M | 269M |
| generated pieces | — | 5148 |

## Analysis

### The two edit scenarios the splitter now wins

Both are timestamp-only changes, and both are handled without parsing anything. The split of
a translation unit is a pure function of the source, the flags and the contents of every file
it includes; that dependency list is written beside the pieces as `depfile.cache`, so hashing
those contents and comparing against a stored hash turns re-splitting into a comparison.

This is not a niche case. A build system re-runs the launcher whenever a prerequisite's
*timestamp* moves, and a great deal of what moves timestamps does not change content:
touching a file, checking out the same commit again, a generator rewriting output
identically, a tool that rewrites in place. Every one of those previously cost a full
libclang parse of each affected unit, whose output then came out byte-for-byte identical.

### The scenario it still loses, and why

Changing a function body is a genuine content change, so three translation units must be
re-split. Instrumenting that edit:

```
translation units rebuilt:   3
splits reused from cache:    0     (correctly — the input really did change)
pieces found up to date:     6
pieces recompiled:           0
```

**No compilation happens at all.** The splitter correctly determines that no piece's text
changed and compiles nothing. All 1.8s is the splitting itself.

That cost is no longer dominated by parsing. Precompiling the include prefix took the row
from 2.7s to 2.3s, and removing the preamble's unused PCH took it to 1.8s. What remains is
the rest of the split: walking the AST twice — once to harvest definitions, once for
`collect_emitted()`'s reachability — and writing several hundred output files per unit whose
contents usually turn out unchanged.

So the row needs the *per-piece* work skipped the way the whole split now is. That is a finer
grain of the caching already added rather than a new mechanism.

### The full build

Still 10.8x, and this is the honest cost of the approach: 5148 compilations instead of 12,
each paying process spawn, preamble include and PCH load, to produce a library six times
larger. Halving it (from 16.3s) came entirely from deleting a precompiled header that nothing
read — an artefact `TODO/08` had orphaned and that was quietly costing more than half the
build.

A cold build is also the case with the most obvious remedy and the least urgency: 5148
independent compilations is close to an ideal workload for a shared cache or a remote
executor, which is what `test-boost-split-reninja.sh` exists to explore. Nothing about the
local numbers here will tell you whether that pays off.

### On the artefacts

12M against 2.2M is not overhead in the usual sense. The splitter emits every function as its
own object with its own copy of whatever inline and template code it needs, and `ld -r`
combines them without deduplicating. It came down from 22M during this work as the
reachability and preamble-layering rules stopped forcing definitions nobody needed into
existence, and it would come down further if pieces shared their vague-linkage definitions.

The 269M build tree, from 1.7G before the PCH removal, is what makes repeated runs of this
benchmark a disk-space problem; it is worth a `df` check before a long session.

## What changed to get here

Measured on the same example, in order:

| change | full | one body | build tree |
|---|---:|---:|---|
| before `TODO/14` | 16.3s | 2.5s | 1.7G |
| + skip split when inputs unchanged | 16.3s | 2.7s | 1.7G |
| + precompile the include prefix | 16.4s | 2.3s | 1.7G |
| + stop precompiling the preamble | **7.4s** | **1.8s** | **269M** |

The input hashing does not show up in either column here because both scenarios in this table
involve genuine content changes; its effect is entirely in the two touch rows, which it took
from roughly 2x slower to roughly 2x faster.

The largest single win was a deletion.

## Correctness

Timing is worthless if the output is wrong, so alongside every measurement:

- 12 of 12 translation units split, no fallbacks to plain compilation;
- the resulting library links a program with **no undefined references** and passes nine
  Boost.Filesystem assertions — path decomposition, `lexically_normal`, `create_directories`,
  `file_size`, `directory_iterator`, `remove_all` — identically to a library built without the
  splitter;
- a second build reports no work;
- editing a header rebuilds exactly the seven translation units that include it, the same set
  a build without the launcher rebuilds;
- the eleven `ctest` fixtures pass.

## Relocatable linker: `ld` vs `mold`

Splitting turns one link of twelve objects into twelve links of several hundred, so the
linker doing the combining stops being an incidental choice. `CPP_SPLITTER_LINKER` selects
it; it defaults to `ld` and is passed `-r` either way.

End to end, with mold 2.30.0:

| scenario | `ld` | `mold` |
|---|---:|---:|
| full | 7.4s | 7.2s |
| no-op | 0.2s | 0.2s |
| one source | 0.2s | 0.2s |
| one header | 0.3s | 0.3s |
| one function body | 1.8s | 1.8s |
| **library** | **12M** | **9.3M** |
| build tree | 269M | 265M |

The timings are within noise; the library is 22% smaller.

That the total barely moves is not evidence that the linker does not matter — it is evidence
that the link is not where the time goes. Timing the step in isolation, on the largest unit
(72 objects):

| linker | per link | output |
|---|---:|---:|
| `ld` | 40 ms | 11M |
| `mold` | **20 ms** | **7.7M** |

**mold is twice as fast at the relocatable link and produces an object 30% smaller.** Across
twelve units that saves about 0.24s of a 7.4s build — roughly what the end-to-end numbers
show, and about 3%. The link is around 5% of a full split build; halving it cannot do more
than that.

The size reduction is the more interesting result, and it compounds: the split library is
large because every piece carries its own copy of whatever inline and template code it needs,
and mold evidently merges more of that duplication than GNU ld does when producing a
relocatable object. The library it produces links and passes the same nine assertions.

Worth revisiting if the compile side gets faster — at which point 5% becomes a larger share —
or on a project whose units split into thousands of pieces rather than hundreds, where the
link would scale up faster than the compilation.

## Reproducing

```sh
./benchmark-boost-split.sh [runs]
CPP_SPLITTER_LINKER=mold ./benchmark-boost-split.sh [runs]
```

No environment variables are needed; the splitter splits by default since the server was
removed. Each split build writes a few hundred megabytes, so check `df` first.
