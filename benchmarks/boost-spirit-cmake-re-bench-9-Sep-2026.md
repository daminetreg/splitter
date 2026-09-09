# Boost.Spirit's test suite on Remote Build Execution — 9 September 2026

Measured at `9b9ab75` with `./benchmark-spirit-cmake-re.sh`, against EngFlow's cluster at
`opal.cluster.engflow.com:443` over mTLS.

Every other benchmark in this directory measures one machine, where cutting a translation unit
into one object per function is strictly more work. The argument for doing it anyway is that
the pieces are independent and a build farm can take them all at once — so this is the
benchmark that was supposed to settle it.

**It does not settle it in the splitter's favour, and the reason is worth more than the
verdict.**

## Method

`example/boost-to-split/cmake-re` builds the Boost superproject configured for Spirit plus all
277 test programs read out of Spirit's Jamfiles. `./build-spirit-cmake-re.sh --distributed`
runs it through CMake RE; `--split` adds `cpp-splitter` as `CMAKE_CXX_COMPILER_LAUNCHER`, which
cmake-re composes with its own driver as `<cpp-splitter>;tipi-compiler-driver` — so the
splitter runs outermost and each piece it emits becomes its own distributed action.

Scenarios are `./benchmark-spirit-tests.sh`'s, unchanged, so the two can be read side by side.
The edited body is `utf8_put_encode()` in `boost/spirit/home/support/utf8.hpp`: an ordinary
non-template inline function the splitter emits a real piece for in 269 of the 277 units.

Environment: AMD EPYC-Milan, 32 cores, 122 GiB RAM; clang 13.0.0 (tipi toolchain `4f846ee`);
cmake-re v0.0.88; `-j64`; Debug; C++17.

## Results

`remote/cached/local` is `REMOTE_EXECUTION / CACHE_HIT / LOCAL_FALLBACK`, taken from reclient's
own per-action records rather than inferred.

| scenario | splitter | wall | fallbacks | remote/cached/local |
|---|---|---:|---:|---|
| full | no | 32.3s | 0 | 0 / 1641 / 0 |
| no-op | no | 11.9s | 0 | 0 / 0 / 0 |
| one source | no | 12.0s | 0 | 0 / 0 / 0 |
| one header | no | 12.0s | 0 | 0 / 0 / 0 |
| one body | no | **110.8s** | 0 | **213** / 594 / 0 |
| full | yes | 377.8s | 5 | 0 / 15321 / 0 |
| no-op | yes | 19.0s | 0 | 0 / 789 / 0 |
| one source | yes | 14.5s | 0 | 0 / 0 / 0 |
| one header | yes | 14.5s | 0 | 0 / 0 / 0 |
| one body | yes | **103.9s** | 2 | **210** / 603 / 0 |

## Only one row compiled anything

Read the action column before the wall column. **Both `full` rows are entirely cache hits** —
zero remote executions. By the time these were measured the cluster had already seen both
configurations, and there is no way from here to evict its cache. So 32.3s against 377.8s is
not plain-versus-split compilation. It is 1641 cache lookups against 15321, plus the work that
never leaves this machine: parsing 277 translation units with libclang and relinking each
unit's pieces with `ld -r`.

That comparison is still worth something, just not what it looks like. It says the split build
carries **9.3x more actions** and a large local cost, and that when nothing needs compiling
those two things cost 11.7x the wall time. It says nothing about throughput under real load.

**`one body` is the only scenario where both sides actually executed remotely**, 213 actions
against 210 — and there the two builds are within 7% of each other, 110.8s against 103.9s.
On a corpus this hostile, with a farm, per-function splitting comes out a wash.

## Why it is a wash, which is the useful part

The design's claim is that splitting raises the ceiling on parallelism: a conventional build
cannot go faster than its slowest translation unit, and pieces can be spread. The claim is
true, and this benchmark shows two reasons it does not cash out here.

**The remote action count barely moves on an incremental edit.** Editing `utf8_put_encode()`
invalidates one piece in each of about 210 units, and invalidates the whole translation unit in
each of about 213 without the splitter. The farm is handed roughly the same number of jobs
either way. Splitting makes each job much smaller, but the count is what the scheduler works
with, and the count is the same.

**The splitting itself does not distribute.** Each unit is parsed with libclang, its pieces are
written, and its objects are combined with `ld -r`, all locally, before anything can be
scheduled. That work grows with the number of units and there are 277 of them. It is the
difference between the two `no-op` rows (11.9s against 19.0s) and most of the difference
between the two `full` rows.

Where the ceiling argument should pay is a corpus with *few, enormous* translation units, where
the plain build is pinned to one long pole and the split build is not. Boost.Spirit's tests are
the opposite: 277 units of a few seconds each, which a farm already parallelises perfectly
well without any help.

## The two touch rows are free by construction

`one source` and `one header` are timestamp-only changes, and both come out at the `no-op`
time. cmake-re mirrors sources by content, so a file whose bytes did not change is not
re-mirrored and nothing downstream runs. On a single machine those rows measure the splitter's
own input hashing (`TODO/14`); here they measure the content-addressed mirror, which gets the
same answer for nothing.

Worth knowing for anyone reading the single-machine benchmarks next to this one: a `touch`
cannot be used to force work under cmake-re. Neither can removing `-B` — cmake-re keys its real
build directory on the configuration, so an identical configure lands back on the same one and
ninja reports "no work to do" over the previous run's outputs. `--clean` exists for that, and
an early version of this benchmark reported a confident 0 fallbacks that was a green build
sitting on outputs which had every one of them fallen back.

## Fallbacks

5 on the cold split build and 2 on the body edit, all one defect: the splitter inserts `inline`
into the middle of a template argument list in `boost/mp11/algorithm.hpp`, corrupting an alias
template. Filed as `TODO/34`. `./benchmark-spirit-tests.sh` reports 0 fallbacks on the same
sources, so what differs between the two builds is part of that question.

They matter more than five failures in 277 sounds. A fallback writes no split cache, so the
unit re-does its whole split and then compiles plain on *every* build (`TODO/31`).

## Sending the relocatable link to the cluster too

The `one body` row above says the compiles already distribute and something else is the cost.
The obvious candidate is the one step that stays here: one `ld -r` per translation unit over
every object that unit produced, a few hundred of them. The splitter now hands that to
`tipi-linker-driver` whenever it is chained behind `tipi-compiler-driver`, so the link becomes
a distributed, cacheable action like any other.

**It costs rather than saves, on this corpus:**

| `one body`, distributed + split | wall | remote / cached |
|---|---:|---|
| local `ld -r` | 103.9s | 210 / 603 |
| link through `tipi-linker-driver` | **120.0s** | **482** / 795 |

The 272 extra remote actions are exactly the 272 links. The objects being linked were produced
here, so sending the link away buys a round trip to upload a few hundred of them per unit, and
that costs about 16 seconds more than doing it locally.

It is left on by default regardless, for two reasons that this measurement does not test. A
driven link is *cacheable*, where a local one is not, so a rebuild the cluster has already seen
can skip it entirely. And the trade turns on how much local CPU there is: on a machine with far
fewer than 32 cores, a few hundred local links compete with the splitter's own parsing in a way
they do not here. `CPP_SPLITTER_NO_LINKER_DRIVER=1` turns it off.

## What this benchmark still cannot say

- **Cold against cold.** Both `full` rows were cache-served and the cluster's cache cannot be
  evicted from here. A salt in the action key, or a fresh instance, would fix this and is the
  single most valuable change to make next.
- **Where the split build's time goes.** 377.8s is splitter parse plus cache round-trips plus
  `ld -r`, and the argument stands or falls on which dominates. Only the total is measured.
- **A corpus that would favour it.** Few large units rather than many small ones is the shape
  the design is aimed at, and it is not what was measured.

## Reproduction

```sh
./benchmark-spirit-cmake-re.sh
```

Credentials are mTLS, from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
