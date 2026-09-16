---
chapter: Benchmarks
chapter-label: Set 4 / what changed
notes: The body edit as counts. Plain compiles 218 units. Unity compiles 29 batches, each 8 units concatenated, which is why a batch costs more than a unit and why touching one source costs 10s. The split re-slices 2 units and recompiles 50 pieces against 2 rebuilt PCHs; the other 203 units find the edited function declared only in their copy and do nothing but validate. 4.5x over plain, 2.8x over unity, on the edit that the copies can absorb.
---
## {split}203{/split} units change nothing.  
{accent}50{/accent} pieces compile.

::: metrics 3
- 218 |  | plain units recompiled
- 29 | violet | unity batches recompiled
- 50 | accent | split pieces compiled, 2 PCHs
:::

The body edit is 102.6s plain, 63.9s unity, 23.0s split — 

## Split faster {split}4.5x{/split} over plain, {split}2.8x{/split} over unity.
