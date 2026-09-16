# Boost.Spirit's test suite, split and built on Remote Build Execution, 15 and 16 September 2026

The split rows of `benchmark-spirit-cmake-re.sh --distributed` after TODO/49 (a piece for
every header definition a template names: 37538 pieces against 4408), on the EngFlow
cluster at `opal.cluster.engflow.com:443`, Boost at `ab7968a0bb`, clang 13 (`4f846ee`),
C++17 Release. The plain rows were not run again; they are the 9 September ones from
`boost-spirit-rbe-summary-9-Sep-2026.md`, and that table timed the whole invocation where
these time the build phase alone -- about 20s of copying, mirroring and configuring sit in
every plain row and in none of the rows below. The scenarios are unchanged: full (`--clean`),
no-op, one source touched, one header touched, one line in the body of
`standard_wide::toucs4()`.

`remote / cached` are reclient's records: actions executed on the cluster and actions served
from its cache. `-j` is the number of units in flight; how many pieces of each are handed to
the cluster at once is `CPP_SPLITTER_REMOTE_JOBS` (TODO/50, below).

## Split here, pieces compiled on the cluster, `-j500`

cpp-splitter `b89cfffb`, 8 piece compiles per launcher at a time (`4000 / 500`).

| scenario | splitter | build | fallbacks | remote | cached | peak RSS | 9 Sep plain, `-j500`, whole invocation |
|---|---|---:|---:|---:|---:|---:|---:|
| full | yes | 2352.3s | 0 | 0 | 114249 | 3.4G | 32.1s (0 remote, 1641 cached) |
| no-op | yes | 31.7s | 0 | 0 | 1635 | 0.6G | 12.0s |
| one source | yes | 6.0s | 0 | 0 | 0 | 0.3G | 11.9s |
| one header | yes | 6.1s | 0 | 0 | 0 | 0.3G | 12.0s |
| **one body** | **yes** | **83.0s** | 0 | **267** | 1575 | 2.2G | **136.2s (271 remote, 762 cached)** |

## Split on the cluster too, `-j500`

Same commit and cap, `REMOTE_SPLIT=1 MODES=remote`: the launcher runs itself through
`rewrapper` for the parse, downloads the pieces, and compiles them as separate actions.

| scenario | splitter | build | fallbacks | remote | cached | peak RSS |
|---|---|---:|---:|---:|---:|---:|
| full | cluster | 2080.2s | 0 | 279 | 114249 | 7.9G |
| no-op | cluster | 31.7s | 0 | 0 | 1635 | 0.6G |
| one source | cluster | 6.1s | 0 | 0 | 0 | 0.3G |
| one header | cluster | 6.1s | 0 | 0 | 0 | 0.3G |
| **one body** | **cluster** | **129.8s** | 0 | **267** | 1575 | 2.2G |

## Split here, `-j32`, no cap

cpp-splitter `040b7ab8`, the run that populated the cluster's cache: every piece of a unit
handed to the cluster at once, about 4300 in flight.

| scenario | splitter | build | fallbacks | remote | cached | peak RSS |
|---|---|---:|---:|---:|---:|---:|
| full | yes | 2231.7s | 0 | 30860 | 21669 | 6.6G |
| no-op | yes | 33.5s | 0 | 0 | 1635 | 0.4G |
| one source | yes | 6.0s | 0 | 0 | 0 | 0.3G |
| one header | yes | 6.0s | 0 | 0 | 0 | 0.3G |
| **one body** | **yes** | **380.3s** | 0 | **267** | 1575 | 1.7G |

## Reading the tables

**The body edit is 267 actions of two seconds each, and the rest is the cluster's queue.**
Every body row above executed 267 actions -- one piece per unit that keeps `toucs4`, no PCH
rebuilt, as on this machine alone (34.1s at `-j16`,
`boost-spirit-summary-14-Sep-2026.md`). reclient's records for the two `-j500` rows: mean
execution on the worker 2.1s, mean time queued by the server 23.5s and 24.8s, maximum
101.6s and 54.7s. The same 267 actions took 380.3s at `-j32` and, in a run whose full row
was not cold (below), 204.0s with a mean queue of 100.3s. Against the plain build's 271
compiles of whole units at 136.2s, the split build's 83.0s is 1.6x faster with the
invocation overhead counted on the plain side only; the work it sends is two seconds per
action, and the row measures how fast the cluster hands out 267 slots.

**The full rows cost transfer and bookkeeping, not compilation.** 114249 cache records for
37538 pieces, 0 executions in the `-j500` split row, 279 in the remote-split row (the
parses), and 2080s to 2352s either way; the 9 September full row read 456.0s on 15885
records. The `-j32` row that filled the cache, 30860 executions and 21669 hits, took
2231.7s -- no slower than the rows that found everything cached, so the cluster's compile
time is not what these rows measure. This is the cost stated in TODO/49: 8.5x the pieces,
paid on every cold build and on every cache lookup.

**The no-op row triples**, 11.8s on this machine and 31.7s here against 12.0s plain: the
launcher validating 37538 pieces, 1635 cache lookups, nothing compiled.

**Splitting on the cluster changes nothing in the body row and costs memory in the full
row**: 129.8s against 83.0s is the queue again (same 267 actions, same 2.1s execution),
and 7.9G against 3.4G peak RSS is 279 launchers waiting on their remote parse and
downloading pieces at once, as on 10 September (7.0G against 2.9G).

## What did not run

Two earlier runs at `-j500` did not finish, and both failed the same way. With remote
execution the launcher handed every piece of a unit to `rewrapper` at once, and 500 units
of about 135 pieces is tens of thousands of concurrent connections to one `reproxy`. The
first run lost the proxy 11 minutes in (`peer is alive, but connection closed`, 5909
actions done, 55 units built); the remote-split run had 125 units fall back on
`dial_timeout of 3m0s expired before being able to connect to reproxy` and nine programs
fail to link on the mix of split and whole objects. TODO/50 adds
`CPP_SPLITTER_REMOTE_JOBS`, and `build-spirit-cmake-re.sh` sets it to `4000 / JOBS` for a
distributed split build -- about what the 9 September runs had in flight with 16 pieces
per unit. The `-j32` run needed no cap: 4300 in flight.

The first `-j500` run also reported a "full" row of 290.1s that was not a cold build:
`--clean` removes the directory `-B` resolves to, the distributed `-B` did not exist yet,
and the configure landed on the host run's build directory with every object in place, so
the row rebuilt the 268 units the restored header had touched. `--clean` now removes every
build directory of the flavour.
