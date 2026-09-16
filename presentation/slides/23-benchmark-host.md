---
chapter: Benchmarks
chapter-label: Benchmark set 3 / host −j16
notes: Set 3 is this machine at -j16, build-only, 16 September. Cold, the split build is 460.7s against 57.7s plain: 279 parses, 540 shared compiles, 3866 per-unit header pieces, 371 unit pieces. The no-op is the launcher's bookkeeping — 279 launchers each checking the records of the shared objects they link, 22.2s against 5.7s. The body edit: 268 launcher runs plus 51 shared compiles, 28.7s against 56.9s plain, 2.0x — where the cluster makes it 3.5x, because there the 51 compiles are the whole cost.
---
## On one machine,  
pieces are {split}more work{/split}.

::: legend
- plain | 
- splitter | s
:::

::: bars
- full | 57.7 | 460.7
- no-op | 5.7 | 22.2
- one body | 56.9 | 28.7
:::

::: tiny
Each bar is value ÷ this set’s 460.7s maximum, from a zero baseline. Build-only timing, -j16; no cluster per-action records. The body edit is 2.0x here, 3.5x on the cluster.
:::
