---
chapter: Benchmarks
chapter-label: Benchmark set 5 / generated corpus, host −j16
notes: Set 5, build-only. Full: 16.0 plain, 14.1 unity, 97.3 split -- 8000 pieces, 201 parses, 403 PCHs against 201 compiles; one object per function is six times the work when nothing stays in the preamble. No-op: nothing. One body: plain recompiles the unit, 40 functions, 1.3s; unity recompiles the batch, 8 units and 320 functions, 6.7s; the split re-slices without a parse and compiles 20 pieces, 0.7s -- 20 rather than 1 because the pieces after the edited function carry a #line directive that moved; TODO/53 takes that to one. 2x over plain, 10x over unity.
---
## Generated corpus:  
{split}2x plain, 10x unity{/split} on the edit; 6x cold.

::: legend
- plain | 
- unity | u
- splitter | s
:::

::: bars
- full | 16.0 | 14.1 | 97.3
- no-op | 0.2 | 0.2 | 0.2
- one body | 1.3 | 6.7 | 0.7
:::

::: metrics 3
- 1 unit | | plain: 40 functions recompiled
- 1 batch | violet | unity: 8 units, 320 functions
- 20 pieces | accent | split: re-sliced, no parse; 1 after TODO/53
:::

::: tiny
Each bar is value ÷ this set’s 97.3s maximum. Full: 201 compiles plain, 26 unity batches, 8000 pieces + 201 parses + 403 PCHs split. 0 fallbacks, 0 declined; the three programs print the same number.
:::
