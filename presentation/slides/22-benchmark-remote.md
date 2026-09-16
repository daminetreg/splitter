---
chapter: Benchmarks
chapter-label: Benchmark set 2 / what the splitter did, −j500
notes: The same cluster run, read as the splitter's own counts rather than wall time. Full — 279 units parsed and split on workers, 540 shared compiles, 3866 per-unit header pieces, 371 unit pieces: 4878 remote actions, which is why the row is 1096.6s and why it measures transfer, not compilation — the day before, with one piece per includer, 2080.2s over 114249 cache records. No-op — nothing: every unit reused its split and found its shared objects current, 1635 cache hits and 0 executions. One body — 268 units re-sliced their twin, 51 shared compiles, no per-unit piece, no PCH of a unit rebuilt. 0 fallbacks, 0 declined on every row.
---
## What the {split}splitter{/split} did.

::: single-bars
- full | 1096.6 | 279 parses · 540 shared, 3866 per-unit, 371 unit pieces
- no-op | 24.3 | nothing: 279 splits reused, 0 remote
- one body | 38.8 | 268 re-slices, 51 shared compiles, 0 PCH
:::

::: callout
The full row is transfer and parsing on the cluster, not compilation: every piece compiled without the local PCH, every remote split downloaded, through 8 slots per launcher. One shared piece per header function took the same row from 2080.2s (one piece per includer, 15 Sep) to 1096.6s.
:::
