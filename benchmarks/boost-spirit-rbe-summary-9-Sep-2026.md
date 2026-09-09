# Boost.Spirit's test suite, split and built on Remote Build Execution

One measurement set, from a single run. Nothing here is carried over from an earlier pass.

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
| Build system | CMake RE v0.0.88, `-j500` |
| Execution | EngFlow RBE, `opal.cluster.engflow.com:443`, mTLS |
| Command | `BUILD_TYPE=Release CMAKE_RE_JOBS=500 ./benchmark-spirit-cmake-re.sh --distributed` |

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

| scenario | splitter | wall | fallbacks | remote | cached |
|---|---|---:|---:|---:|---:|
| full | no | 33.5s | 0 | 0 | 1641 |
| full | yes | 554.7s | 0 | 5178 | 351 |
| no-op | no | 11.9s | 0 | 0 | 0 |
| no-op | yes | 17.8s | 0 | 0 | 1635 |
| one source | no | 11.9s | 0 | 0 | 0 |
| one source | yes | 13.8s | 0 | 0 | 0 |
| one header | no | 11.9s | 0 | 0 | 0 |
| one header | yes | 13.8s | 0 | 0 | 0 |
| **one body** | **no** | **224.2s** | **0** | **271** | 762 |
| **one body** | **yes** | **75.2s** | **0** | **1** | 1575 |

**Nothing falls back to a plain compile on any row**, in either configuration.

## Reading the table

**The body edit is the result.** 224.2s against 75.2s, and the action counts say why: the
ordinary build executes **271** compiles on the cluster, the split build **one**. A content
change gives every affected unit a new action key, so nothing the ordinary build needs can come
from cache; it genuinely recompiles all 271. The split build rebuilds the single piece that
changed and takes 1575 cache hits for everything else.

That is the whole claim of per-function splitting: the work an edit causes is proportional to
what the edit changed, not to the file it was written in.

**The two full rows are not a like-for-like comparison in this run.** The ordinary build was
served entirely from the cluster's cache (0 executed) while the split build executed 5178
actions. They measure different things and their ratio is meaningless.

**The touch rows behave as designed** — both configurations do nothing, and the splitter adds
about 2 seconds of launcher overhead across 277 units.

**The no-op row is not free for the split build**, 11.9s against 17.8s. Nothing is compiled;
the difference is 1635 cache lookups where the ordinary build needs none.

## Caveats

- **The body row is noisy on the ordinary side.** It executes 271 real compiles, and cluster
  load moves that by roughly a factor of two between runs. The action counts, 271 against 1,
  are the stable part of the result.
- **One edit is not a distribution of edits.** This one has a reach-to-use ratio of 194:1. An
  edit to something used as widely as it is included would show both builds doing the same
  work.
- **Disk is not measured here and is not small.** A split build tree for this suite runs to
  tens of gigabytes against tens of megabytes, because every piece carries the debug
  information of the preamble it includes.

## Reproducing

```sh
BUILD_TYPE=Release CMAKE_RE_JOBS=500 ./benchmark-spirit-cmake-re.sh --distributed
```

Credentials are mTLS, read from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
