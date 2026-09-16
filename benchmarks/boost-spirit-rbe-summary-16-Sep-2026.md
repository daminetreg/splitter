# Boost.Spirit's test suite after TODO/51: this machine against the cluster, 16 September 2026

One header function, one shared piece (TODO/51): a piece taken out of a header is one
object in a store, compiled from the original header, and linked by every unit that
includes the header. Measured here (`-j16`, `MODES=split --host`) and on the EngFlow
cluster with the split itself produced there (`-j500`, `REMOTE_SPLIT=1 MODES=remote
--distributed`, 8 piece compiles per launcher in flight). cpp-splitter `8ab6e9f3`, Boost
`ab7968a0bb`, clang 13 (`4f846ee`), C++17 Release, 277 programs. Wall is the build phase
as CMake RE reports it; `remote / cached` are reclient's records of actions executed on
the cluster and served from its cache. The plain rows are the ones measured beside the
local run and, for the cluster, the 9 September ones (whole invocation, about 20s of
mirroring and configuring included).

## Full, no-op, one body

| scenario | plain, here | split, here `-j16` | plain, cluster `-j500` (9 Sep) | remote split, cluster `-j500` |
|---|---:|---:|---:|---:|
| full | 57.7s | 460.7s | 32.1s (0 remote, 1641 cached) | 1096.6s (4878 remote, 1791 cached) |
| no-op | 5.7s | 22.2s | 12.0s | 24.3s (0 remote, 1635 cached) |
| **one body** | **56.9s** | **28.7s** | **136.2s** (271 remote) | **38.8s** (51 remote, 1575 cached) |

What the splitter did, both places: full -- 279 units parsed and split, 540 shared
compiles, 3866 per-unit header pieces, 371 unit pieces; no-op -- nothing, every unit
reused its split and found its shared objects current; one body -- 268 units re-sliced
their twin, 51 shared compiles, no per-unit piece, no PCH of a unit rebuilt.

## Reading the rows

**The body edit is 51 compiles, here and there.** `standard_wide::toucs4()` is edited; its
header's 15 sharable functions and 36 of three headers whose include closure reaches it
are recompiled once each, and the 268 units that include it link the store's objects.
On the cluster those are 51 actions of 1.3s mean execution and 1.8s mean queue, 38.8s
against the plain build's 136.2s (3.5x) and against 129.8s the day before with 267
actions (TODO/49, one piece per includer). Here, 28.7s against 56.9s plain (2.0x); the
row is 268 launcher runs plus the compiles.

**The full build on the cluster is 1096.6s against 460.7s here.** 4878 actions: 279
parses on workers, 540 shared and 3866 per-unit piece compiles and the unit pieces, each
compiled without the local PCH (the `.gch` is not an input of the action), the pieces of
the remote splits downloaded, all through 8 slots per launcher. The day before, with
one piece per includer, the same row was 2080.2s on 114249 cache records; on 10 September,
before TODO/49, 357.5s with every piece a cache hit. The row measures transfer and
parsing on the cluster, not compilation.

**The no-op is the launcher's bookkeeping**, 22.2s here and 24.3s there against 5.7s and
12.0s plain: 279 launchers each checking the records of the shared objects they link --
135 records of some 1500 prerequisites, stat'ed once per launcher, hashed when a stat
moved -- before finding nothing to do.

## What changed for the cluster

A shared piece's inputs are the anchor and the original header's include closure, so one
action key serves every includer: 268 units, one compile of `toucs4`, 267 cache hits or
none needed at all. With `--remote-split` the parse runs on a worker and the store piece
it writes stays there; the per-unit twin carries the header and the anchor, and the
launcher writes the piece to the local store again under the same key before compiling
it. 0 fallbacks, 0 declined on every row.
