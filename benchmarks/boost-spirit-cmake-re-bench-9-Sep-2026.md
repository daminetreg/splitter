# Boost.Spirit's test suite on Remote Build Execution — 9 September 2026

Measured at `fad365c` with `./benchmark-spirit-cmake-re.sh`, against EngFlow's cluster at
`opal.cluster.engflow.com:443` over mTLS.

Every other benchmark here measures one machine, where cutting a translation unit into one
object per function is strictly more work. The argument for doing it anyway is that the pieces
are independent and a farm can take them all at once — so this is the benchmark meant to settle
it.

## Method

`example/boost-to-split/cmake-re` builds the Boost superproject configured for Spirit plus all
277 test programs read out of Spirit's Jamfiles. `./build-spirit-cmake-re.sh --distributed`
runs it through CMake RE; `--split` adds `cpp-splitter` as `CMAKE_CXX_COMPILER_LAUNCHER`, which
cmake-re composes with its own driver as `<cpp-splitter>;tipi-compiler-driver` — so the
splitter runs outermost and every piece it emits becomes its own distributed action.

Environment: AMD EPYC-Milan, 32 cores, 122 GiB RAM; clang 13.0.0 (tipi toolchain `4f846ee`);
cmake-re v0.0.88; `-j64`; Debug; C++17.

### Which function the body row edits, and why it decides everything

`standard_wide::toucs4()`, in `boost/spirit/home/support/char_encoding/standard_wide.hpp`.
**194 of the units include that header; exactly 1 emits a piece for it.**

That gap is the whole point. A plain build must recompile all 194 units, because the header
they include changed. A split build re-runs the launcher for all 194 too — but only the unit
that actually emits the function has a piece to recompile. The other 193 keep the definition in
their rewritten copy, and piece recompilation is decided from the piece source and the
preamble, neither of which moved.

An earlier version of this benchmark edited `utf8_put_encode()`, which is emitted in **178 of
the 180** units that include it. Both builds therefore did the same amount of work and the row
came out a wash — which was read, wrongly, as the design failing. It was the target failing to
discriminate. Reach and use have to differ for this row to say anything at all.

## Results

`remote/cached/local` is `REMOTE_EXECUTION / CACHE_HIT / LOCAL_FALLBACK`, taken from reclient's
own per-action records rather than inferred from the clock.

| scenario | splitter | wall | fallbacks | remote/cached/local |
|---|---|---:|---:|---|
| full | no | 32.5s | 0 | 0 / 1641 / 0 |
| no-op | no | 12.0s | 0 | 0 / 0 / 0 |
| one source | no | 12.0s | 0 | 0 / 0 / 0 |
| one header | no | 11.9s | 0 | 0 / 0 / 0 |
| one body | no | **117.8s** | 0 | **271** / 762 / 0 |
| full | yes | 384.3s | 5 | 0 / 16137 / 0 |
| no-op | yes | 21.2s | 0 | 0 / 1605 / 0 |
| one source | yes | 14.4s | 0 | 0 / 0 / 0 |
| one header | yes | 14.4s | 0 | 0 / 0 / 0 |
| one body | yes | **65.3s** | 2 | **3** / 1569 / 0 |

Run twice, the second time after the cluster had executed and cached everything the first pass
produced. The body row read 139.6s against 67.4s on the first pass and 117.8s against 65.3s on
this one: the split side is stable and the plain side carries the variance, which is what one
would expect of the side doing 271 real compiles.

## The body edit: 1.8x faster, and 90x less work sent to the cluster

**117.8s against 65.3s**, and the action counts say why rather than leaving it to be guessed:
the plain build executed **271** compiles on the cluster, the split build **3**.

This row is a real comparison on both sides, unlike the full rows below. A content change
produces new action keys, so nothing the plain build needs can come out of the cache — it
genuinely recompiles 271 units, because 194 of them include the header that changed and the
rest follow from it. The split build genuinely recompiles the one piece that changed.

That is the design working as described. One function changed, so one function's object needs
rebuilding, not the translation units that happen to include the header it lives in. The farm
is handed 3 jobs instead of 271.

Where the remaining 65 seconds goes is worth being precise about, because it is not
compilation. The launcher still runs for all 194 units, each re-checks its inputs, and each
relinks; those 1569 cache hits are that work being recognised as already done. The floor on
this row is the per-unit fixed cost, and it is most of what is left.

## The full build, with both sides fully cached

The first pass could not compare these rows: the plain build was entirely cache-served while
the split build still executed 208 actions. Run again, **both are now zero remote executions** —
every action on both sides came out of the cluster's cache.

| `full` | wall | actions |
|---|---:|---|
| plain | 32.5s | 1641 cached |
| split | 384.3s | 16137 cached |

So this is a like-for-like measurement at last, and what it measures is **overhead**: 9.8x more
actions to look up, plus the splitting, which happens locally. **11.8x the wall time when
nothing needs compiling at all.**

That is a real cost, and it is the price paid for the body row above. It is also not the
cold-build number: with the cache empty on both sides the split build would additionally
compile 16137 pieces where the plain one compiles 1641, and that comparison still cannot be
made from here.

## The two touch rows are free by construction

`one source` and `one header` are timestamp-only changes and both come out at the `no-op` time.
cmake-re mirrors sources by content, so a file whose bytes did not change is not re-mirrored
and nothing downstream runs.

Two things follow for anyone running these by hand. A `touch` cannot force work under cmake-re.
Neither can removing `-B`: cmake-re keys its real build directory on the configuration, so an
identical configure lands back on the same one and ninja reports "no work to do" over the
previous run's outputs. `--clean` exists for that, and an early version of this benchmark
reported a confident 0 fallbacks that was a green build sitting on outputs which had every one
of them fallen back.

## Sending the relocatable link to the cluster too

A distributed split build compiles pieces remotely and then links each unit here: one `ld -r`
over every object that unit produced. The splitter hands that to `tipi-linker-driver` whenever
it is chained behind `tipi-compiler-driver`, making the link a distributed, cacheable action.

| `one body`, distributed + split | wall | remote / cached |
|---|---:|---|
| link through `tipi-linker-driver` (default) | 66.6s | 3 / 1569 |
| local `ld -r` (`CPP_SPLITTER_NO_LINKER_DRIVER=1`) | 65.6s | 3 / 771 |

**A wash on time, and the links become cacheable**: the ~800 extra cache hits are the links
being recognised rather than redone. Since only one unit's pieces changed, 193 units relink
identically to last time, and the cluster can say so.

Measured against the *previous* body target this looked much worse — 120.0s against 103.9s,
with 272 extra remote actions — because that edit changed a piece in 178 units, so no link
could be a cache hit and every one had to upload its objects. Which of those two pictures you
get depends entirely on how much the edit actually invalidates.

## Fallbacks

5 on the cold split build and 2 on the body edit, all one defect: the splitter inserts `inline`
into the middle of a template argument list in `boost/mp11/algorithm.hpp`, corrupting an alias
template. Filed as `TODO/34`.

They matter more than five in 277 sounds. A fallback writes no split cache, so the unit re-does
its whole split and then compiles plain on *every* build (`TODO/31`).

## What this benchmark still cannot say

- **Cold against cold.** Both `full` rows are cache-served on both sides now, which makes them
  a fair measure of overhead and no measure at all of compilation. The cluster's cache cannot be
  evicted from here; a salt in the action key, or a fresh instance, is the single most valuable
  change to make next.
- **Where the split build's 384.3s goes.** Splitter parse, cache round-trips and linking are
  not separated, and the cold-build argument turns on which dominates.
- **Whether the body row generalises.** It measures one edit whose reach and use differ by
  194:1. Real edits vary, and an edit to something used everywhere looks like the old
  `utf8_put_encode` number, not this one.

## Reproduction

```sh
./benchmark-spirit-cmake-re.sh
```

Credentials are mTLS, from `~/engflow-mTLS` unless `ENGFLOW_MTLS_DIR` says otherwise.
