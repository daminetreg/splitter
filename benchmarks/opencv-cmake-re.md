# A minimal OpenCV through CMake RE, on the cluster

Three measurement sets, each from a single run of
`BUILD_TYPE=Release CMAKE_RE_JOBS=500 REMOTE_SPLIT=1 ./benchmark-opencv-cmake-re.sh --distributed`
on 13 September 2026: before TODO/42, after it, and after TODO/44. The corpus, the scenarios and the body target are those of
[`opencv-local-split.md`](opencv-local-split.md): OpenCV 4.11.0, `core` and `imgproc`, static,
nothing optional, 158 C++ units; one line added to the body of `SparseMat::nzcount()` in
`mat.inl.hpp`, a header all 158 units include and one emits.

| | |
|---|---|
| Machine | AMD EPYC-Milan, 32 cores, driving the build |
| Compiler | clang 13.0.0 (tipi toolchain), `-std=c++17`, **Release** |
| Build system | CMake RE v0.0.88, `cmake-re --host --distributed`, `-j500` |
| Execution | EngFlow RBE at `opal.cluster.engflow.com:443` over mTLS |
| Timing | the build phase only |

Three configurations: plain; split, with the split produced on this machine; split, with the
split itself produced on the cluster (`--remote-split`, TODO/35). `remote / cached` are
reclient's per-action records — actions executed on the cluster and actions served from its
cache. The last column is the splitter's own report of what the units did.

## Results

### After TODO/44

| scenario | splitter | build | fallbacks | declined | remote | cached | what the 158 units did |
|---|---|---:|---:|---:|---:|---:|---|
| full | no | 123.8s | 0 | — | 1 | 516 | served from cache |
| full | split here | 74.9s | 0 | 0 | 289 | 25575 | 158 split here; pieces served from cache |
| full | split on cluster | 69.3s | 0 | 0 | 160 | 26436 | 158 split on the cluster; pieces served from cache |
| no-op | any | 3.9–8.6s | 0 | 0 | 0 | 0–474 | reused |
| one source / one header | any | 3.8–3.9s | 0 | 0 | 0 | 0 | not re-mirrored |
| **one body** | **no** | **139.1s** | 0 | — | **158** | 0 | all 158 recompiled |
| **one body** | **split here** | **13.9s** | 0 | 0 | **23** | 510 | 158 re-sliced, 23 pieces recompiled |
| **one body** | **split on cluster** | **14.5s** | 0 | 0 | **23** | 510 | 158 re-sliced, 23 pieces recompiled |

All 158 units split (TODO/44); no unit is compiled whole on any row. The pieces of the full
split builds were served from the cluster's cache, being the pieces the run after TODO/42
had executed: 289 and 160 executions are the three units' new pieces and the splits
themselves. The plain rows took three times what they took in the run after TODO/42 with the
same action counts (123.8s against 11.7s for 516 cache hits; 139.1s against 40.9s for 158
executions): the cluster was slower on this run, and the split rows are to be read against
the plain rows of the same run.

### After TODO/42

| scenario | splitter | build | fallbacks | declined | remote | cached | what the 158 units did |
|---|---|---:|---:|---:|---:|---:|---|
| full | no | 11.7s | 0 | — | 1 | 516 | served from cache |
| full | split here | 398.4s | 0 | 3 | 8618 | 234 | 155 split here, 3 compiled whole; every piece executed |
| full | split on cluster | 123.0s | 0 | 3 | 204 | 25941 | 155 split on the cluster; pieces served from cache |
| no-op | any | 3.9–8.5s | 0 | 0 | 0 | 0–465 | reused |
| one source / one header | any | 3.9s | 0 | 0 | 0 | 0 | not re-mirrored |
| **one body** | **no** | **40.9s** | 0 | — | **158** | 0 | all 158 recompiled |
| **one body** | **split here** | **16.8s** | 0 | 3 | **38** | 465 | 155 re-sliced, 3 compiled whole, 35 pieces recompiled |
| **one body** | **split on cluster** | **22.5s** | 0 | 3 | **26** | 501 | 155 re-sliced, 3 compiled whole |

A *fallback* is a defect: the splitter tried and something failed. A *decline* is a limit it
stated before writing anything (`TODO/44`). The run that produced these numbers reported the
three declines in the fallback column; the columns have been separated since.

### Before TODO/42

| scenario | splitter | build | fallbacks | remote | cached | what the 158 units did |
|---|---|---:|---:|---:|---:|---|
| full | no | 115.3s | 0 | 1 | 516 | served from cache |
| full | split here | 322.6s | 61 | 7248 | 0 | 97 split here, 61 compiled whole |
| full | split on cluster | 179.0s | 61 | 160 | 21738 | 158 split on the cluster |
| **one body** | **no** | **109.5s** | 0 | **158** | 0 | all 158 recompiled |
| **one body** | **split here** | **35.6s** | 61 | **66** | 291 | 64 re-sliced, 78 re-parsed here, 61 compiled whole |
| **one body** | **split on cluster** | **276.0s** | 61 | **4043** | 300 | 64 re-sliced, 94 re-split on the cluster, 61 compiled whole |

## Reading the tables

**The body edit, after TODO/44: 139.1s plain against 13.9s with the split produced here and
14.5s with it produced on the cluster** — 158 remote compiles against 23 and 23. After
TODO/42 the same rows read 40.9s against 16.8s and 22.5s — 158 compiles against 38 and 26 —
the 3 declined units being compiled whole on every header edit. Now every unit re-slices.

**The split-on-cluster body row's 4043 executions are gone.** They read 26 now, with 501
cache hits. The cause was the libclang parse: built around a prefix PCH that could not find
`precomp.hpp`, it degraded differently on the worker than here, so the pieces the worker
wrote differed from the local ones and none was a cache hit. With the parse clean (TODO/42
(5)) the worker's pieces are the local ones. That was TODO/42 (6).

**A cold full plain build is served from cache here** (11.7s); the plain build of this corpus
takes 163.8s cold through the cluster, 16s locally. 158 units do not amortise a cluster's
per-action cost. The split cold build executed every piece (8618 actions, 398.4s); the run
that followed found them cached.

## Caveats

- Everything in `opencv-local-split.md`'s caveats applies: the split archives are not
  symbol-identical to the plain ones.
- The plain `full` row is served from cache because a plain build of the same tree preceded
  the run; its cold figure, 163.8s, is from a smoke test. The split `full` rows after
  TODO/44 are served from cache too, from the run before.
- Wall times are from one run, and the cluster's speed differs between runs: the plain rows
  after TODO/44 are three times the plain rows after TODO/42 for identical action counts.

## Reproducing

```sh
BUILD_TYPE=Release CMAKE_RE_JOBS=500 REMOTE_SPLIT=1 ./benchmark-opencv-cmake-re.sh --distributed
BUILD_TYPE=Release CMAKE_RE_JOBS=500 MODES="plain split" ./benchmark-opencv-cmake-re.sh --distributed
```

Credentials are mTLS, read from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
