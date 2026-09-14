---
chapter: Benchmarks
chapter-label: Benchmark set 3 / host −j16
notes: Set 3 is host -j16, build-only. Full cold host costs 57.8 vs 654.6; body edit 57.5 vs 14.2.
---
## On one machine,  
pieces are {split}more work{/split}.

::: legend
- plain | 
- splitter | s
:::

::: bars
- full | 57.8 | 654.6
- no-op | 5.7 | 11.4
- source touch | 5.6 | 6.0
- header touch | 5.7 | 6.0
- one body | 57.5 | 14.2
:::

::: tiny
Each bar is value ÷ this set’s 654.6s maximum, from a zero baseline. Build-only timing; no cluster per-action records.
:::