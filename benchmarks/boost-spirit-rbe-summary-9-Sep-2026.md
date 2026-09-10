# Boost.Spirit's test suite, split and built on Remote Build Execution

Two measurement sets, each from a single run, each labelled with what it measured. Nothing is
carried over from a pass older than those, and no row is combined with a row from the other
set: comparing configurations across runs compares the cluster's cache state as much as
anything the splitter did.

## What was built

All **277** test programs Boost.Spirit's Jamfiles declare — every `run` and `compile` target
across `qi`, `karma`, `lex`, `x3` and `support` — plus the Boost superproject configured for
Spirit. The targets are read out of the Jamfiles rather than listed by hand, so the set cannot
drift from the suite.

Each configuration is built twice over, once normally and once with `cpp-splitter` in front of
the compiler, cutting every translation unit into one object file per function and linking the
pieces back with `ld -r`.

| | |
|---|---|
| Machine | AMD EPYC-Milan, 32 cores, 122 GiB RAM |
| Compiler | clang 13.0.0 (tipi toolchain `4f846ee`), C++17, **Release** |
| Build system | CMake RE v0.0.88 |
| Execution | EngFlow RBE at `opal.cluster.engflow.com:443` over mTLS (`-j500`), and this machine alone (`-j16`) |

## The five scenarios

**full** — a cold build of all 277 programs. `--clean` is used, because removing the build
directory is not enough on its own: CMake RE keys its real build directory on the
configuration, so an identical configure lands back on the same one and reports nothing to do.

**no-op** — build again with nothing changed. Should do nothing at all; anything else is
overhead being measured rather than work.

**one source** — `touch` one test `.cpp` (`qi/char1.cpp`). The timestamp moves, the content does
not.

**one header** — `touch` a widely included header
(`boost/spirit/home/support/char_encoding/standard_wide.hpp`). Again timestamp only.

Both touch rows are expected to be free here: CMake RE mirrors sources by content, so a file
whose bytes did not change is not re-mirrored and nothing downstream runs. They confirm that
neither configuration invents work.

**one body** — change one line inside the body of `standard_wide::toucs4()`, an ordinary
non-template function in that same header.

### Why that function, specifically

This row is the one that distinguishes the two builds, and only because of how the target was
chosen. Two properties are required.

*It must be a definition the splitter can move out of a header.* Almost all of Spirit is
templates, which stay in the preamble with no piece emitted — a template cannot be instantiated
ahead of time. Editing one could never recompile "one piece", because there is no piece.

*Its reach must exceed its use.* **194 of the units include that header; exactly one emits a
piece for the function.** An ordinary build must recompile every unit that includes the header.
The split build re-runs the launcher for all of them too, but only the single unit that emits
the function has a piece whose content changed — the other 193 keep the definition in their
rewritten copy, and piece recompilation is decided from the piece source and the preamble,
neither of which moved.

A function used as widely as it is included makes both builds do the same work and the row says
nothing.

## Results

`remote / cached` are reclient's own per-action records: actions genuinely executed on the
cluster, and actions served from its cache. Wall time alone cannot tell those apart, and they
differ by an order of magnitude.

### On the cluster, `-j500`

| scenario | splitter | wall | fallbacks | remote | cached |
|---|---|---:|---:|---:|---:|
| full | no | 32.1s | 0 | 0 | 1641 |
| full | yes | 456.0s | 0 | 0 | 15885 |
| no-op | no | 12.0s | 0 | 0 | 0 |
| no-op | yes | 17.7s | 0 | 0 | 1635 |
| one source | no | 11.9s | 0 | 0 | 0 |
| one source | yes | 13.9s | 0 | 0 | 0 |
| one header | no | 12.0s | 0 | 0 | 0 |
| one header | yes | 13.8s | 0 | 0 | 0 |
| **one body** | **no** | **136.2s** | **0** | **271** | 762 |
| **one body** | **yes** | **74.9s** | **0** | **1** | 1575 |

### With the split itself produced on the cluster, `-j500`

A separate run, 10 September, measuring only this configuration
(`REMOTE_SPLIT=1 MODES=remote`). TODO/35: with `--remote-split` the launcher shells itself out
through `rewrapper` before parsing anything, so the libclang parse happens on a cluster worker
and the pieces are downloaded. Everything else is unchanged — the pieces still compile as
separate actions, and the final `ld -r` still happens here.

**Wall is the build alone**, as CMake RE reports it, not the whole invocation: copying the
project in, staging the drivers, mirroring the sources and configuring came to about 20s and
used to land in every row. The two tables above still include it and are not comparable with
this one column for column. The last column is what the splitter itself reported, which is the
only way to tell which of its four paths each unit took.

| scenario | build | fallbacks | remote | cached | peak RSS | how the 279 units were split |
|---|---:|---:|---:|---:|---:|---|
| full | 357.5s | 0 | 0 | 16722 | 7.0G | 279 on the cluster |
| no-op | 12.5s | 0 | 0 | 1635 | 0.6G | none: all 279 reused their split |
| one source | 6.1s | 0 | 0 | 0 | 0.3G | none, not re-mirrored |
| one header | 6.1s | 0 | 0 | 0 | 0.3G | none, not re-mirrored |
| **one body** | **33.0s** | **0** | **1** | 1575 | 0.4G | **268 re-sliced here, none on the cluster** |

Peak RSS is the resident memory of this benchmark's own processes, sampled every two seconds
and attributed by process group. It still spans the whole invocation, which costs nothing in
accuracy: mirroring and configuring are a couple of single-threaded processes against several
hundred concurrent launchers. A system-wide figure would be meaningless on this machine, which
has other tenants.

**The body row is the result, and it needed TODO/36 to work at all.** 33.0s, one remote compile,
and **268 of the affected units re-sliced the changed body locally** without touching the
cluster. Before TODO/36 the same row read **606.5s and 4835 remote actions**: a definition the
unit keeps in its copy of the header was left out of the harvest, so an edit to it re-split the
whole unit — on the cluster, once per affected unit. Two defects, both found by asking the
splitter why it refused rather than inferring it. See `TODO/36`.

**The full row's cost is transfer, not parsing.** All 279 splits were produced on the cluster
with no fallback, and every action came from its cache — 0 executions against 16722 hits. The
EngFlow profile of an earlier instance of this row says where the time goes: 279 remote splits
at a mean of **28.9s of worker time** each, and **589911 blob downloads** to bring the pieces
home. Compiling the pieces where they already are, rather than downloading them to compile
here, is the thing to try next.

**Peak memory is still higher, not lower.** 7.0G on the full row against 2.9G when splitting
here. The second half of TODO/35's motivation was that several hundred concurrent libclang
parses are what exhausts this machine at `-j500`; moving the parse away does remove those, but
the launchers stay resident while they wait on the cluster and the downloads are not free. On
this evidence the memory argument for remote splitting does not hold.

**Run-to-run variance is real and worth stating.** The same five rows measured an hour earlier,
timed around the whole invocation, gave a `full` row of 334.6s **with** 279 executions rather
than cache hits, and a `one body` row of 22.6s. A row that did the parsing beat one that did
none, and the body row moved by half again. Wall times here are worth about two significant
figures; the action counts and the split attribution are the stable part of the result.

### On this machine alone, `-j16`

No cluster, so no per-action records exist and the last two columns cannot be filled in.

| scenario | splitter | wall | fallbacks |
|---|---|---:|---:|
| full | no | 64.2s | 0 |
| full | yes | 654.5s | 0 |
| no-op | no | 12.0s | 0 |
| no-op | yes | 17.5s | 0 |
| one source | no | 11.8s | 0 |
| one source | yes | 13.6s | 0 |
| one header | no | 11.8s | 0 |
| one header | yes | 13.7s | 0 |
| one body | no | 63.2s | 0 |
| one body | yes | 90.2s | 0 |

**Nothing falls back to a plain compile on any row, in either mode.**

## Reading the tables

**The body edit is the result.** On the cluster, 136.2s against 74.9s — and the action counts
say why: the ordinary build executes **271** compiles, the split build **one**. A content
change gives every affected unit a new action key, so nothing the ordinary build needs can come
from cache; it genuinely recompiles all 271. The split build rebuilds the single piece that
changed and takes 1575 cache hits for the rest.

That is the whole claim of per-function splitting: the work an edit causes is proportional to
what the edit changed, not to the file it was written in.

**The full rows measure overhead, not compilation.** Both are now served entirely from the
cluster's cache — zero executions on either side — so they are directly comparable and what
they compare is bookkeeping: 1641 cache lookups against 15885, 32.1s against 456.0s. That is
the standing cost of carrying 9.7x more actions, paid on a build where nothing needed
compiling.

**On one machine the cold full build costs 10.2x**, 64.2s against 654.5s. One object per
function is strictly more work, and without a farm or a cache there is nothing to absorb it.

**The touch rows behave as designed.** Both configurations do nothing: CMake RE mirrors sources
by content, so a file whose bytes did not change is not re-mirrored. The splitter adds about
two seconds of launcher overhead across 277 units.

**The no-op row is not free for the split build**, 12.0s against 17.7s. Nothing is compiled;
the difference is 1635 cache lookups where the ordinary build needs none.

## Caveats

- **`--host` rows cannot distinguish work from cache.** Without a cluster there are no
  per-action records, so a fast row there cannot be shown to have compiled anything. An earlier
  pass in this same configuration reported the full split build at 78.5s rather than 654.5s;
  the difference is far too large to be the code change between them, and the likely
  explanation is a warm cache the driver could still reach. Treat the `-j16` table as
  indicative and the cluster table as measured.
- **The `one body` row under `--host` is not incremental.** It reads 90.2s against a 654.5s
  full build, but a content change makes CMake RE re-execute the whole graph, and with no cache
  there is nothing to absorb the parts that did not really change. It is a partial rebuild
  wearing an incremental label.
- **The body row is noisy on the ordinary side.** It executes 271 real compiles and cluster
  load moves that substantially between runs. The action counts, 271 against 1, are the stable
  part of the result.
- **One edit is not a distribution of edits.** This one has a reach-to-use ratio of 194:1. An
  edit to something used as widely as it is included would show both builds doing the same
  work.
- **The first two tables' wall times include the configure; the third's do not.** The benchmark
  used to time one whole invocation of `build-spirit-cmake-re.sh` -- copying the project in,
  staging the drivers, mirroring the sources, configuring, and then building -- which added
  about 20s to every row. The driver now reports its own build phase and the benchmark reads
  that, so the cluster-split table is build-only. The two `-j500` and `-j16` tables above
  predate the change: compare within a table, not across.
- **A row is only attributable if the splitter was verbose.** It reports which of its four
  paths each unit took -- reuse, re-slice, cluster, or parse here -- and without that a row
  that quietly declined to use the cluster and split here instead looks exactly like one that
  worked, only slower. The driver now sets `CPP_SPLITTER_VERBOSE`; an earlier pass of the third
  table was discarded because it could not be read.
- **`remote-split.log` is not evidence.** It is written into the split directory by the
  rewrapper redirect and is not there afterwards, most likely because reclient replaces the
  output directory when it downloads it. Counting those files says nothing about whether the
  cluster was used; the build log does.
- **Disk is not measured here and is not small.** A split build tree for this suite runs to
  tens of gigabytes against tens of megabytes, because every piece carries the debug
  information of the preamble it includes.

## Reproducing

```sh
BUILD_TYPE=Release CMAKE_RE_JOBS=500 ./benchmark-spirit-cmake-re.sh --distributed
BUILD_TYPE=Release CMAKE_RE_JOBS=16  ./benchmark-spirit-cmake-re.sh --host

# the third table: split on the cluster, that configuration alone
BUILD_TYPE=Release CMAKE_RE_JOBS=500 REMOTE_SPLIT=1 MODES=remote \
    ./benchmark-spirit-cmake-re.sh --distributed
```

Credentials are mTLS, read from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
