# Boost.Spirit's test suite on Remote Build Execution — 9 September 2026

Measured at `4ac30a6` plus the `TODO/34` fix, with `./benchmark-spirit-cmake-re.sh`, against EngFlow's cluster at
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

## Distributed, `-j500`

| scenario | splitter | wall | fallbacks | remote/cached/local |
|---|---|---:|---:|---|
| full | no | 33.5s | 0 | 0 / 1641 / 0 |
| no-op | no | 11.9s | 0 | 0 / 0 / 0 |
| one source | no | 11.9s | 0 | 0 / 0 / 0 |
| one header | no | 11.9s | 0 | 0 / 0 / 0 |
| one body | no | **224.2s** | **0** | **271** / 762 / 0 |
| full | yes | 554.7s | **0** | 5178 / 351 / 0 |
| no-op | yes | 17.8s | 0 | 0 / 1635 / 0 |
| one source | yes | 13.8s | 0 | 0 / 0 / 0 |
| one header | yes | 13.8s | 0 | 0 / 0 / 0 |
| one body | yes | **75.2s** | **0** | **1** / 1575 / 0 |

**Nothing falls back on any row.** The run before this one reported 5 fallbacks on the cold
split build and 2 on the body edit; all were one defect, `TODO/34`.

### The body edit: 3.0x faster, executing 1 compile against 271

**224.2s against 75.2s.** The plain build re-executed **271** compiles: a content change gives
every affected unit a new action key, so nothing it needs can come from cache. The split build
executed **one** — the single piece that changed — and took 1575 cache hits for the rest.

That is the whole claim of per-function splitting, and a content-addressed cache is what
rewards it. One function changed, so one function's object needed building.

This row is also the noisiest in the file, and the ratio should be read with that in mind. The
plain side has read 117.8s, 139.6s, 159.1s and now 224.2s across four passes, all executing the
same 271 compiles; the split side 65.3s, 67.4s, 71.8s, 75.2s. Cluster load moves the plain side
by a factor of two. **The action counts — 271 against 1 — are what carry the argument**, not
any single pair of wall times.

### The cold full build costs 3.5x, from the previous pass

The `full` rows above are not comparable: the plain build came entirely from the cluster's
cache while the split build executed 5178 actions, because fixing `TODO/34` changed every piece
the splitter emits and therefore every action key.

The cold-against-cold measurement is from the pass immediately before, in the same
configuration, where both sides executed everything with no cache hits at all:

| `full`, cold both sides | wall | remote executions |
|---|---:|---:|
| plain | 154.8s | 547 |
| split | 537.4s | 5249 |

**3.5x**, against 11.8x measured with both sides *cache-served* — that comparison had no
compiling in it, so the split build's 9.8x larger action count had nothing to hide behind.
Under real load 500-way parallelism absorbs most of it. Those 5249 included the 5 units that
fell back; the fix removes fallbacks rather than changing what a correct split emits, so the
shape of the comparison holds.

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

## Fallbacks: none

There were 5 on the cold split build and 2 on the body edit until `TODO/34` was fixed. It was
filed as an alias-template defect, because a corrupted alias in `boost/mp11/algorithm.hpp` is
where the broken output appeared. The cause was one line in the launcher: chained behind
another compiler launcher, the splitter probed **the launcher in front of it** for the system
include paths instead of the compiler behind it, and parsed a translation unit the compiler
would not have recognised.

`launcher.chained_behind_driver` covers it, and asserts that a real compiler was probed rather
than that a `<string>`-sized unit happened to survive not probing one — the first version of
that fixture passed without the fix.

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
