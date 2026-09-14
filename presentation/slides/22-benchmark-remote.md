---
chapter: Benchmarks
chapter-label: Benchmark set 2 / remote split −j500
notes: Set 2 is distinct: remote split, single configuration, build-only. 279 units. Never compare 33.0 seconds to set 1 136.2 seconds.
---
## Build-only. One configuration.  
Not a speedup denominator.

::: single-bars
- full | 357.5 | 279 cluster splits
- no-op | 12.5 | all 279 reused
- source touch | 6.1 | not re-mirrored
- header touch | 6.1 | not re-mirrored
- one body | 33.0 | 268 re-slices, 1 remote
:::

::: callout
33.0s is not compared to the first set’s 136.2s: setup and timing scope differ. Full: 279 cluster splits; no-op: all 279 reused; body edit: 268 re-slices, 1 remote.
:::