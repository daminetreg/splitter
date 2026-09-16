# 50 — a cap on the piece compiles a launcher hands to the cluster at once

## Motivation

With remote execution the launcher hands every piece of a unit to `rewrapper` at once
(`compile_parallel()`: `num_threads = jobs.size()`). After TODO/49 a Boost.Spirit unit has
about 135 pieces, and `-j500` units in flight means tens of thousands of concurrent
`rewrapper` processes against one `reproxy`. Two runs of `benchmark-spirit-cmake-re.sh
--distributed` on 15 and 16 September failed on that: the first lost the proxy 11 minutes
in ("peer is alive, but connection closed", 5909 remote actions done, 55 units), the second
had 125 units fall back on `dial_timeout of 3m0s expired before being able to connect to
reproxy` and nine programs fail to link on the mix. At `-j32`, about 4300 pieces in flight,
the same build went through: 30860 remote executions, 21669 cache hits, 0 fallbacks.

## Implementation Proposal

- `CPP_SPLITTER_REMOTE_JOBS=<n>`: with the tipi compiler driver in use, at most `n` piece
  compiles of one unit in flight at a time. Unset, the behaviour is unchanged: all at once.
- `build-spirit-cmake-re.sh` sets it for `--distributed --split` to `4000 / JOBS` (at least
  1) unless already set: 8 per launcher at `-j500`, 125 at `-j32`, about 4000 pieces in
  flight either way, which is what the 9 September runs had with 16 pieces per unit.

## Acceptance Criteria

- `launcher.remote_jobs_cap`: through the launcher with a fake `tipi-compiler-driver` on
  the PATH that records its concurrency, `CPP_SPLITTER_REMOTE_JOBS=2` compiles a unit of
  five pieces with never more than two compiles at once, and the program prints what it
  printed without the cap.
- `benchmark-spirit-cmake-re.sh --distributed` at `-j500`, split and remote-split, runs
  with 0 fallbacks.

## Outcome

Implemented at `b89cfffb`. `benchmarks/boost-spirit-rbe-summary-14-Sep-2026.md`: with 8
piece compiles per launcher, the `-j500` split and remote-split runs went through with 0
fallbacks (body row 83.0s and 129.8s, full 2352.3s and 2080.2s). `launcher.remote_jobs_cap`
sees 8 compiles at once without the cap on `sample.cpp` and 2 with it.
