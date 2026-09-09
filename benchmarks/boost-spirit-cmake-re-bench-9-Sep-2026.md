# Boost.Spirit's test suite on Remote Build Execution — 9 September 2026

Measured at `e6a6248` with `./benchmark-spirit-cmake-re.sh`, against EngFlow's cluster at
`opal.cluster.engflow.com:443` over mTLS.

Every other benchmark here measures one machine, where cutting a translation unit into one
object per function is strictly more work. The argument for doing it anyway is that the pieces
are independent, cache separately, and a farm can take them all at once.

## Method

`example/boost-to-split/cmake-re` builds the Boost superproject configured for Spirit plus all
277 test programs read out of Spirit's Jamfiles. `--split` adds `cpp-splitter` as
`CMAKE_CXX_COMPILER_LAUNCHER`, which cmake-re composes with its own driver as
`<cpp-splitter>;tipi-compiler-driver`, so every piece becomes its own action.

Environment: AMD EPYC-Milan, 32 cores, 122 GiB RAM; clang 13.0.0 (tipi toolchain `4f846ee`);
cmake-re v0.0.88; C++17; **Release**.

Release matters beyond the compile flags: the build type is part of every RBE action key, so
switching to it emptied the cluster's cache from its point of view. That is the only way from
here to get a **cold measurement on both sides**, which the earlier Debug runs never had.

### Which function the body row edits, and why it decides everything

`standard_wide::toucs4()`, in `boost/spirit/home/support/char_encoding/standard_wide.hpp`.
**194 of the units include that header; exactly 1 emits a piece for it.**

A plain build must recompile every unit that includes the header. A split build re-runs the
launcher for all of them too, but only the one unit that emits the function has a piece whose
content changed — the other 193 keep the definition in their rewritten copy, and piece
recompilation is decided from the piece source and the preamble, neither of which moved.

An earlier version edited `utf8_put_encode()`, emitted in **178 of the 180** units that include
it. Both builds then did the same work and the row came out flat, which was read as the design
failing. It was the target failing to discriminate.

## Distributed, `-j500`, cold on both sides

| scenario | splitter | wall | fallbacks | remote/cached/local |
|---|---|---:|---:|---|
| full | no | **154.8s** | 0 | **547** / 0 / 0 |
| no-op | no | 12.1s | 0 | 0 / 0 / 0 |
| one source | no | 11.9s | 0 | 0 / 0 / 0 |
| one header | no | 12.0s | 0 | 0 / 0 / 0 |
| one body | no | **159.1s** | 0 | **271** / 762 / 0 |
| full | yes | **537.4s** | 5 | **5249** / 0 / 0 |
| no-op | yes | 18.5s | 0 | 0 / 1605 / 0 |
| one source | yes | 14.5s | 0 | 0 / 0 / 0 |
| one header | yes | 14.5s | 0 | 0 / 0 / 0 |
| one body | yes | **71.8s** | 2 | **3** / 1569 / 0 |

### The cold full build costs 3.5x, not 11.8x

Both full rows executed everything — zero cache hits on either side. This is the
cold-against-cold comparison the previous write-ups said they could not make.

**154.8s against 537.4s**: 547 compiles against 5249, and 3.5x the wall time. Measured with
both sides *cache-served* the same rows read 11.8x, because then the split build's only cost
was looking up 9.8x more actions with no compiling to hide it. Under real load the extra
actions are absorbed by 500-way parallelism and the penalty is a third of what the cached
comparison suggested.

### The body edit is 2.2x faster, executing 3 compiles against 271

**159.1s against 71.8s.** The plain build re-executed **271** compiles, because a content
change gives every affected unit a new action key and nothing it needs can come from cache. The
split build executed **3** and took 1569 cache hits: its pieces did not change, so the cluster
already had them.

That is the whole claim of per-function splitting, and it is what a content-addressed cache
rewards. One function changed, so one function's object needed building.

## The same benchmark on one machine, `-j16`

| scenario | splitter | wall | fallbacks |
|---|---|---:|---:|
| full | no | 63.6s | 0 |
| no-op | no | 11.9s | 0 |
| one source | no | 11.7s | 0 |
| one header | no | 11.7s | 0 |
| one body | no | 63.0s | 0 |
| full | yes | 78.5s | 2 |
| no-op | yes | 18.0s | 0 |
| one source | yes | 14.3s | 0 |
| one header | yes | 14.3s | 0 |
| one body | yes | 78.3s | 2 |

**A cold full split build costs only 1.23x here** — 63.6s against 78.5s — where the Debug
single-machine benchmark measured 10.9x. Most of that difference is debug information: a Debug
piece carries the debug info of the preamble it includes, and on Spirit that preamble is most
of Boost. In Release the split tree is still 38G against 85M, but the compiling itself is
cheap enough that splitting nearly pays for itself even without a farm.

### The incremental rows on one machine are not incremental

`one body` reads 63.0s and 78.3s — the same as the full rows, and for the same reason: it
**rebuilt 525 edges, the entire graph**, in both configurations. A content change under cmake-re
re-executes the whole build; on the cluster the cache absorbs the parts that did not really
change, and on one machine there is no cache to absorb anything.

So the host `one body` row is a second full build wearing an incremental label, and should not
be read as an edit-build-loop number. `./benchmark-spirit-tests.sh` measures that loop with
ordinary CMake and ninja, where it means what it says.

The two touch rows are genuinely free in both modes: cmake-re mirrors by content, so a file
whose bytes did not change is not re-mirrored and nothing downstream runs.

## Sending the relocatable link to the cluster too

A distributed split build compiles pieces remotely and links each unit locally: one `ld -r` over
every object that unit produced. The splitter hands that to `tipi-linker-driver` when it is
chained behind `tipi-compiler-driver`, making the link a cacheable action.

| `one body`, Debug, distributed + split | wall | remote / cached |
|---|---:|---|
| link through `tipi-linker-driver` (default) | 66.6s | 3 / 1569 |
| local `ld -r` (`CPP_SPLITTER_NO_LINKER_DRIVER=1`) | 65.6s | 3 / 771 |

A wash on time, and the links become cacheable: the ~800 extra cache hits are 193 units
relinking identically to last time and the cluster saying so. Measured against the *previous*
body target this looked like a 16-second penalty — that edit changed a piece in 178 units, so
no link could be a cache hit. Which picture you get depends on how much the edit invalidates.

## Fallbacks

5 on the cold distributed split build and 2 elsewhere, all one defect: the splitter inserts
`inline` into the middle of a template argument list in `boost/mp11/algorithm.hpp`, corrupting
an alias template. Filed as `TODO/34`. A fallback writes no split cache, so the unit re-does its
whole split and compiles plain on *every* build (`TODO/31`).

## What this still cannot say

- **Whether the body row generalises.** It measures one edit whose reach and use differ 194:1.
  An edit to something used everywhere looks like the old `utf8_put_encode` number instead.
- **Where the split build's 537.4s goes.** Splitter parse, round trips and linking are not
  separated, and the cold-build argument turns on which dominates.
- **Whether `-j500` is the right width.** It was not tuned; the splitter still parses every
  unit locally, and that is the part 500-way remote parallelism cannot help.

## Reproduction

```sh
BUILD_TYPE=Release CMAKE_RE_JOBS=500 ./benchmark-spirit-cmake-re.sh --distributed
BUILD_TYPE=Release CMAKE_RE_JOBS=16  ./benchmark-spirit-cmake-re.sh --host
```

Credentials are mTLS, from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
