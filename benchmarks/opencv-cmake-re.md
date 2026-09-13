# A minimal OpenCV through CMake RE, on the cluster

One measurement set, from a single run of
`BUILD_TYPE=Release CMAKE_RE_JOBS=500 REMOTE_SPLIT=1 ./benchmark-opencv-cmake-re.sh --distributed`
on 13 September 2026. The corpus, the scenarios and the body target are those of
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

| scenario | splitter | build | fallbacks | remote | cached | what the 158 units did |
|---|---|---:|---:|---:|---:|---|
| full | no | 115.3s | 0 | 1 | 516 | — (served from cache: a plain build of the same tree preceded the run) |
| full | split here | 322.6s | 61 | 7248 | 0 | 97 split here, 61 compiled whole; every piece executed |
| full | split on cluster | 179.0s | 61 | 160 | 21738 | 158 split on the cluster; pieces served from cache |
| no-op | no | 3.9s | 0 | 0 | 0 | — |
| no-op | split here | 7.8s | 0 | 0 | 291 | all reused their split |
| no-op | split on cluster | 7.7s | 0 | 0 | 291 | all reused their split |
| one source | any | 3.9s | 0 | 0 | 0 | not re-mirrored: content unchanged |
| one header | any | 3.9s | 0 | 0 | 0 | not re-mirrored: content unchanged |
| **one body** | **no** | **109.5s** | 0 | **158** | 0 | all 158 recompiled |
| **one body** | **split here** | **35.6s** | 61 | **66** | 291 | 64 re-sliced, 78 re-parsed here, 61 compiled whole |
| **one body** | **split on cluster** | **276.0s** | 61 | **4043** | 300 | 64 re-sliced, 94 re-split on the cluster, 61 compiled whole |

The two `full` split rows are not comparable with each other: the split-here row executed
every piece (a cold cache for this build type), and the split-on-cluster row that followed it
found those pieces cached.

## Reading the table

**The body edit is where distribution changes the picture.** On one machine the split build
loses this row, 20.7s against 16.1s, because 94 units re-parse and 61 are compiled whole. On
the cluster the same row reads **35.6s against 109.5s**: the plain build executes 158
compiles remotely and the split build 66, of which 61 are the fallback units compiled whole
and the rest the changed piece and its dependents. The 78 units that re-parse do so locally
and produce byte-identical pieces, which are not resent. A content-addressed cache converts
"re-parsed but unchanged" into "free".

**The 61 fallbacks are still the floor.** They are 61 remote compiles on every header edit,
whatever the splitter does with the other 97 units. `TODO/42` lists their causes.

**Producing the split on the cluster loses on this corpus.** 276.0s and 4043 executions on the
body row: the 78 units that refuse the re-slice are re-split on a worker, the returned trees
replace the local ones, and their pieces are executed rather than served from cache — only
300 hits. On Boost.Spirit the same path served the regenerated pieces from cache, because the
regenerated pieces were byte-identical. Here they were not. The difference is consistent
with the libclang parse errors `opencv-local-split.md` reports on 96 of these units: a parse
that degrades differently on the worker than on this machine writes different pieces. That
attribution is not established; it is what the counts allow.

**The touch rows are free by construction**, here as for Spirit: CMake RE mirrors by content,
and a file whose bytes did not change is not re-mirrored. They confirm that no configuration
invents work.

**A cold full plain build is 7x the local one** — 115.3s against 16.6s, and the run before it,
with nothing cached, 163.8s. A corpus of 158 units does not amortise the cluster's per-action
cost; it is included for the incremental rows, not for this one.

## Caveats

- Everything in `opencv-local-split.md`'s caveats applies: 61 fallbacks, 78 unexplained
  re-slice refusals, libclang parse errors in 96 units. The cluster rows measure the tool as it
  is on this corpus.
- The plain `full` row was served from cache because the driver was smoke-tested with a plain
  build immediately before the run; its cold figure, 163.8s, is from that smoke test.
- The split-on-cluster body row's cache misses are attributed by consistency with the parse
  errors, not by inspection of the differing pieces.
- Wall times are from one run.

## Reproducing

```sh
BUILD_TYPE=Release CMAKE_RE_JOBS=500 REMOTE_SPLIT=1 ./benchmark-opencv-cmake-re.sh --distributed
BUILD_TYPE=Release CMAKE_RE_JOBS=500 MODES="plain split" ./benchmark-opencv-cmake-re.sh --distributed
```

Credentials are mTLS, read from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
